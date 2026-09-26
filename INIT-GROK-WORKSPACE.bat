@echo off
setlocal
cd /d "%~dp0"

where git >nul 2>nul
if errorlevel 1 (
  echo ERROR: git is not on PATH.
  pause
  exit /b 1
)

if not exist ".git" git init

git add -A
git diff --cached --quiet
if errorlevel 1 (
  git -c user.name="FlashNextVelocity Handoff" -c user.email="handoff@local" commit -m "FlashNextVelocity Grok handoff baseline"
)

echo.
echo Fresh Git baseline ready.
pause