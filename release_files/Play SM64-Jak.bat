@echo off
cd /d "%~dp0"
if not exist "sm64-jak\build\us_pc\sm64.us.f3dex2e.exe" (
    echo SM64-Jak has not been built yet -- run "Setup.bat" first.
    pause
    exit /b 1
)
if not exist "data\out\jak1" (
    echo Jak game data has not been built yet -- run "Setup.bat" first.
    pause
    exit /b 1
)
set "JAK_DATA_PATH=%~dp0data"
cd sm64-jak\build\us_pc
start "" sm64.us.f3dex2e.exe --skip-intro
