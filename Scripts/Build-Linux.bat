@echo off

echo [1] Input path: %~1

:: Convert path using wslpath
for /f "delims=" %%P in ('wsl.exe wslpath -a "%~1"') do set "WSL_PATH=%%P"

echo [2] WSL path: %WSL_PATH%

:: Check wsl.exe is available
where wsl.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] wsl.exe not found in PATH
    exit /b 1
)

:: Run the build
echo [3] Running Setup-Linux.sh...
wsl.exe bash -lc "cd \"%WSL_PATH%\" && bash Scripts/Setup-Linux.sh"

echo [4] Exit code: %errorlevel%
exit /b %errorlevel%