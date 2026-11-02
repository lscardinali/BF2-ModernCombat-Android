# Runs out/windows/bf2mc.exe, takes screenshots of its window and, with -Stacks, has the debugger write every thread's
# stack at the end (cdb, non-invasive: the game keeps its state). Everything lands in out/runs/<Name>.
#   tools/run_windows.ps1 [-Name run] [-Seconds 60] [-Script pads/menus.txt] [-Split] [-ScriptPlayer 1] [-Shots 10,30,60] [-Stacks] [-Validate]
param([string]$Name = 'win', [int]$Seconds = 60, [string]$Script = '', [switch]$Split, [int]$ScriptPlayer = -1,
      [int[]]$Shots = @(), [switch]$Stacks, [switch]$Validate, [switch]$Debugger, [string]$CdbExtra = '')
$Root = Split-Path -Parent $PSScriptRoot
Get-Process bf2mc, cdb -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process bf2mc -ErrorAction SilentlyContinue | Wait-Process -Timeout 20 -ErrorAction SilentlyContinue
$Exe = "$Root\out\windows\bf2mc.exe"
$User = "$Root\out\windows\user"
$Out = "$Root\out\runs\$Name"
New-Item -ItemType Directory -Force $Out, $User | Out-Null
Remove-Item -Recurse -Force "$User\logs", "$User\script.txt", "$User\split_script_player" -ErrorAction SilentlyContinue
if ($Script) { Copy-Item "$Root\$Script" "$User\script.txt" }
if ($ScriptPlayer -ge 0) { Set-Content "$User\split_script_player" $ScriptPlayer -Encoding ascii }
Set-Content "$User\settings.toml" "split_screen = $($Split.IsPresent.ToString().ToLower())" -Encoding ascii

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@ -ReferencedAssemblies System.Drawing

function Shot($process, $path) {
    $process.Refresh()
    $h = $process.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return }
    $r = New-Object Win+RECT
    [Win]::GetClientRect($h, [ref]$r) | Out-Null
    if ($r.R -le 0) { return }
    # asks the window itself: never picks up what is on top of it, and takes no focus
    $bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [Win]::PrintWindow($h, $dc, 3) | Out-Null  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

if ($Validate) {  # needs the Vulkan SDK
    $env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
    $env:VK_LAYER_DEBUG_ACTION = 'VK_DBG_LAYER_ACTION_LOG_MSG'
    $env:VK_LAYER_REPORT_FLAGS = 'error'
    $env:VK_LAYER_LOG_FILENAME = "$Out\validation.txt"
    Remove-Item "$Out\validation.txt" -ErrorAction SilentlyContinue
}
$env:BF2_NO_DIALOG = '1'
if ($Debugger) {
    # under cdb from the start: what ended the process lands in cdb.txt. First-chance access violations stay quiet, the
    # runtime handles those itself.
    # rdi is the guest's PPCContext in the recompiled functions; guest memory starts at 0x100000000
    $commands = 'sxd -c2 ".lastevent; r; dq @rdi L28; db 0x18284F9B8+0n320 L10; db 0x18284F7EC L4; kn 60; ~* kn 16; qd" av; ' +
        'sxd -c2 ".lastevent; r; kn 60; qd" c0000409; bp ntdll!NtTerminateProcess "kn 40; g"; ' + $CdbExtra + ' g'
    $cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
    Start-Process $cdb -WorkingDirectory "$Root\out\windows" -WindowStyle Hidden -ArgumentList @(
        '-G', '-y', "`"$Root\out\windows`"", '-logo', "`"$Out\cdb.txt`"", '-c', "`"$($commands -replace '"', '\"')`"", "`"$Exe`"")
    while (-not ($process = Get-Process bf2mc -ErrorAction SilentlyContinue | Select-Object -First 1)) { Start-Sleep -Milliseconds 100 }
} else {
    $process = Start-Process $Exe -WorkingDirectory "$Root\out\windows" -PassThru -RedirectStandardError "$Out\stderr.txt"
}
$null = $process.Handle  # or ExitCode is lost when it ends
$start = Get-Date
$pending = [System.Collections.Generic.List[int]]::new()
$Shots | Sort-Object | ForEach-Object { $pending.Add($_) }
while (((Get-Date) - $start).TotalSeconds -lt $Seconds -and -not $process.HasExited) {
    Start-Sleep -Milliseconds 250
    while ($pending.Count -and ((Get-Date) - $start).TotalSeconds -ge $pending[0]) {
        Shot $process "$Out\s$($pending[0]).png"
        $pending.RemoveAt(0)
    }
}
if ($process.HasExited) {
    "exited after $([int]((Get-Date) - $start).TotalSeconds) s with code $('{0:X8}' -f $process.ExitCode)"
} else {
    "alive after $Seconds s, cpu $([int]$process.TotalProcessorTime.TotalSeconds) s"
    Shot $process "$Out\end.png"
    if ($Stacks) {
        $cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
        & $cdb -pv -p $process.Id -y "$Root\out\windows" -c '~* kn 40; qd' > "$Out\stacks.txt" 2>&1
    }
    Stop-Process -Id $process.Id -Force
}
Copy-Item "$User\logs\*" $Out -ErrorAction SilentlyContinue
