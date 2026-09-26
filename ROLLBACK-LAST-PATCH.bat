@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Rollback-FlashNextVelocity-v1.0.5.ps1" %*
set "EC=%ERRORLEVEL%"
echo.
if not "%EC%"=="0" echo Rollback did not complete successfully. Exit code %EC%.
pause
exit /b %EC%
