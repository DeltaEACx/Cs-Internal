@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\updater\update_build.ps1" -Mode Update %*
exit /b %errorlevel%
