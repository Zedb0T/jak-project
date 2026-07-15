@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

echo ============================================================
echo   SM64-Jak Setup
echo ============================================================
echo.
echo This builds SM64-Jak from YOUR OWN copies of both games.
echo Nothing from either game is distributed with this mod.
echo.
echo You will need:
echo   1. A Super Mario 64 (US) ROM  -  baserom.us.z64 (~8 MB)
echo   2. A Jak and Daxter: The Precursor Legacy (NTSC-U) ISO
echo   3. An internet connection (first run installs the compiler)
echo.
pause
echo.

:: ----------------------------------------------------------------
:: [1/4] Mario ROM
:: ----------------------------------------------------------------
if exist "%ROOT%\sm64-jak\baserom.us.z64" (
    echo [1/4] Mario ROM already in place, skipping.
    goto :rom_done
)
set /p "ROM=[1/4] Path to your SM64 US .z64 ROM: "
set "ROM=!ROM:"=!"
if not exist "!ROM!" (
    echo ERROR: file not found: "!ROM!"
    pause
    exit /b 1
)
copy /Y "!ROM!" "%ROOT%\sm64-jak\baserom.us.z64" >nul
echo   ROM copied.
:rom_done
echo.

:: ----------------------------------------------------------------
:: [2/4] Compiler environment (MSYS2)
:: ----------------------------------------------------------------
if exist "C:\msys64\usr\bin\bash.exe" goto :have_msys
echo [2/4] MSYS2 (the compiler environment) is not installed.
choice /C YN /M "      Install it now with winget"
if errorlevel 2 (
    echo Please install MSYS2 from https://www.msys2.org and re-run Setup.
    pause
    exit /b 1
)
winget install -e --id MSYS2.MSYS2 --accept-source-agreements --accept-package-agreements
if not exist "C:\msys64\usr\bin\bash.exe" (
    echo ERROR: MSYS2 install did not complete. Install it from msys2.org and re-run.
    pause
    exit /b 1
)
:have_msys
echo [2/4] Installing build tools (quick if already installed)...
C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm make mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-glew python"
if errorlevel 1 (
    echo ERROR: failed to install build tools.
    pause
    exit /b 1
)
echo.

:: ----------------------------------------------------------------
:: [3/4] Build SM64-Jak from source + your ROM
:: ----------------------------------------------------------------
echo [3/4] Building SM64-Jak (5-15 minutes, one time)...
C:\msys64\usr\bin\env.exe MSYSTEM=MINGW64 /usr/bin/bash -lc "cd \"$(cygpath -u '%ROOT%')/sm64-jak\" && export PATH=/mingw64/bin:/usr/bin:$PATH && TMPDIR=/tmp TMP=/tmp TEMP=/tmp OS=Windows_NT make -j$(nproc) JAKOPENGOAL=1 WINDOWS_BUILD=1"
if errorlevel 1 (
    echo ERROR: SM64 build failed. See errors above.
    pause
    exit /b 1
)
copy /Y "%ROOT%\dlls\*.*" "%ROOT%\sm64-jak\build\us_pc\" >nul
echo   Build complete.
echo.

:: ----------------------------------------------------------------
:: [4/4] Jak game data from your ISO
:: ----------------------------------------------------------------
if exist "%ROOT%\data\out\jak1" (
    echo [4/4] Jak data already built, skipping.
    goto :jak_done
)
set /p "ISO=[4/4] Path to your Jak 1 .iso: "
set "ISO=!ISO:"=!"
if not exist "!ISO!" (
    echo ERROR: file not found: "!ISO!"
    pause
    exit /b 1
)
echo   Extracting and building Jak data (several minutes)...
"%ROOT%\extractor.exe" "!ISO!" --proj-path "%ROOT%\data" -e -v -d -c
if errorlevel 1 (
    echo ERROR: Jak data setup failed. See errors above.
    pause
    exit /b 1
)
:jak_done
echo.
echo ============================================================
echo   Setup complete!  Run "Play SM64-Jak.bat" to play.
echo ============================================================
pause
exit /b 0
