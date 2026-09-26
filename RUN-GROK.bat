@echo off
setlocal
cd /d "%~dp0"

set GROK_RESPECT_GITIGNORE=1

where grok >nul 2>nul
if errorlevel 1 (
  echo ERROR: grok is not on PATH.
  pause
  exit /b 1
)

grok