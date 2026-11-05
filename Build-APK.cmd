@echo off
rem Builds the Android APK from your own Battlefield 2: Modern Combat (USA) Xbox 360 disc image. See README.md.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
pause
