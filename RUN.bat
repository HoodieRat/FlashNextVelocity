@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0dist\FlashNextVelocity.exe" (
  echo FlashNextVelocity is not built yet. Run BUILD.bat first.
  pause
  exit /b 1
)
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\ensure-local-rocm.ps1"
if errorlevel 1 (
  echo.
  echo ROCm local-runtime repair failed.
  pause
  exit /b 1
)
start "FlashNextVelocity" "%~dp0dist\FlashNextVelocity.exe"
