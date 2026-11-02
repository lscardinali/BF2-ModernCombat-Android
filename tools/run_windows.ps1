# Runs out/windows/bf2mc.exe, takes screenshots of its window and, with -Stacks, has the debugger write every thread's
# stack at the end (cdb, non-invasive: the game keeps its state). Everything lands in out/runs/<Name>.
#   tools/run_windows.ps1 [-Name run] [-Seconds 60] [-Script pads/menus.txt] [-Split] [-ScriptPlayer 1] [-Shots 10,30,60] [-Stacks]
param([string]$Name = 'win', [int]$Seconds = 60, [string]$Script = '', [switch]$Split, [int]$ScriptPlayer = -1,
      [int[]]$Shots = @(), [switch]$Stacks)
$Root = Split-Path -Parent $PSScriptRoot
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
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@ -ReferencedAssemblies System.Drawing

function Shot($process, $path) {
    $process.Refresh()
    $h = $process.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return }
    [Win]::SetForegroundWindow($h) | Out-Null
    $r = New-Object Win+RECT
    [Win]::GetClientRect($h, [ref]$r) | Out-Null
    $p = New-Object System.Drawing.Point 0, 0
    [Win]::ClientToScreen($h, [ref]$p) | Out-Null
    if ($r.R -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($p, [System.Drawing.Point]::Empty, $bmp.Size)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

$env:BF2_NO_DIALOG = '1'
$process = Start-Process $Exe -WorkingDirectory "$Root\out\windows" -PassThru -RedirectStandardError "$Out\stderr.txt"
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
