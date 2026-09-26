@echo off
setlocal
cd /d "%~dp0"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0MAKE-GROK-HANDOFF-V3.ps1"
if errorlevel 1 (
  echo.
  echo ERROR: Grok handoff bundle creation failed.
  pause
  exit /b 1
)

echo.
pause
