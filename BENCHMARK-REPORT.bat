@echo off
setlocal
cd /d "%~dp0"
where /q py.exe
if not errorlevel 1 (
  py -3 "%~dp0scripts\benchmark_report.py" %*
) else (
  where /q python.exe
  if errorlevel 1 (
    echo Python 3.10 or newer is required. Install Python and enable its PATH option.
    pause
    exit /b 1
  )
  python "%~dp0scripts\benchmark_report.py" %*
)
set "BENCHMARK_RC=%ERRORLEVEL%"
echo.
if not "%BENCHMARK_RC%"=="0" echo See the report for incomplete tests or failures. Exit code: %BENCHMARK_RC%
if "%~1"=="" pause
exit /b %BENCHMARK_RC%
