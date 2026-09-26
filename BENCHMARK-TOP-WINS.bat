@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\benchmark-top-wins.ps1" %*
set RC=%ERRORLEVEL%
if not "%RC%"=="0" echo Benchmark failed with exit code %RC%.
exit /b %RC%
