@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer.ps1" %*
echo.
pause
