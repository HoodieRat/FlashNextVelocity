@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1"
set EC=%ERRORLEVEL%
echo.
if "%EC%"=="0" echo Build and runtime validation passed. Run RUN.bat.
pause
exit /b %EC%
