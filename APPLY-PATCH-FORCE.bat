@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Apply-FlashNextVelocity-v1.0.5.ps1" -Force %*
set "EC=%ERRORLEVEL%"
echo.
if not "%EC%"=="0" echo Forced patch did not complete successfully. Exit code %EC%.
pause
exit /b %EC%
