@echo off
if "%~1"=="" (
  echo Usage: flash.cmd COM7
  exit /b 2
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash.ps1" -Port "%~1"
exit /b %ERRORLEVEL%

