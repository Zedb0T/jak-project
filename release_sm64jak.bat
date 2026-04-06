@echo off
setlocal enabledelayedexpansion

echo ============================================================
echo   SM64-Jak Release Builder
echo   Builds everything and packages into a distributable zip
echo ============================================================
echo.

:: Save the repo root
set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

set "RELEASE_DIR=%REPO%\release_sm64jak"
set "RELEASE_NAME=SM64-Jak"
set "ZIP_NAME=%RELEASE_NAME%.zip"

:: ---------------------------------------------------------------
:: Kill running game (DLLs are locked while in use)
:: ---------------------------------------------------------------
tasklist /FI "IMAGENAME eq sm64.us.f3dex2e.exe" 2>nul | find /i "sm64.us.f3dex2e.exe" >nul
if not errorlevel 1 (
    echo [!] SM64-Jak is running. Closing it...
    taskkill /IM sm64.us.f3dex2e.exe /F >nul 2>&1
    timeout /t 2 /nobreak >nul
    echo   Closed.
    echo.
)

:: ---------------------------------------------------------------
:: Prerequisite checks
:: ---------------------------------------------------------------
echo [Check] Prerequisites...

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake not found. Install CMake and add it to PATH.
    goto :fail
)
echo   cmake ......... OK

if not exist "C:\msys64\usr\bin\make.exe" (
    echo ERROR: MSYS2 make not found at C:\msys64\usr\bin\make.exe
    goto :fail
)
echo   msys2 make .... OK

if not exist "C:\msys64\mingw64\bin\gcc.exe" (
    echo ERROR: MinGW64 gcc not found.
    goto :fail
)
echo   mingw64 gcc ... OK

if not exist "%REPO%\sm64-jak\baserom.us.z64" (
    echo ERROR: sm64-jak\baserom.us.z64 not found. Cannot build SM64EX.
    goto :fail
)
echo   baserom ....... OK

if not exist "%REPO%\iso_data\jak1" (
    echo ERROR: iso_data\jak1\ not found. Need extracted PS2 ISO data.
    goto :fail
)
echo   iso_data ...... OK

if not exist "%REPO%\out\jak1" (
    echo ERROR: out\jak1\ not found. Need compiled GOAL output.
    echo   Run the decompiler/compiler first to generate out\jak1\.
    goto :fail
)
echo   out\jak1 ...... OK
echo.

:: ---------------------------------------------------------------
:: Step 1 — Configure CMake
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 1/5] Configure CMake
echo ============================================================

if not exist "%REPO%\build\CMakeCache.txt" (
    echo   Running cmake configure...
    cmake -B "%REPO%\build" -DCMAKE_BUILD_TYPE=Release "%REPO%"
    if errorlevel 1 goto :fail
) else (
    echo   Already configured, skipping.
)
echo.

:: ---------------------------------------------------------------
:: Step 2 — Build DLL
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 2/5] Build libjakopengoal DLL
echo ============================================================

cmake --build "%REPO%\build" --target jakopengoal --config Release
if errorlevel 1 (
    echo ERROR: DLL build failed.
    goto :fail
)
echo   DLL built OK.
echo.

:: ---------------------------------------------------------------
:: Step 3 — Copy DLLs to SM64 build dir
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 3/5] Copy DLLs
echo ============================================================

if not exist "%REPO%\sm64-jak\build\us_pc" mkdir "%REPO%\sm64-jak\build\us_pc"
copy /Y "%REPO%\build\bin\Release\*.dll" "%REPO%\sm64-jak\build\us_pc\" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy DLLs.
    goto :fail
)
echo   DLLs copied OK.
echo.

:: ---------------------------------------------------------------
:: Step 4 — Build SM64EX
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 4/5] Build SM64EX with Jak integration
echo ============================================================

set "SM64_DIR=%REPO:\=/%"
C:\msys64\usr\bin\env.exe MSYSTEM=MINGW64 /usr/bin/bash -lc "cd '%SM64_DIR%/sm64-jak' && export PATH='/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH' && TMPDIR=/tmp TMP=/tmp TEMP=/tmp OS=Windows_NT make -j$(nproc) JAKOPENGOAL=1 WINDOWS_BUILD=1"
if errorlevel 1 (
    echo ERROR: SM64EX build failed.
    goto :fail
)

:: Copy DLLs again in case make recreated the directory
copy /Y "%REPO%\build\bin\Release\*.dll" "%REPO%\sm64-jak\build\us_pc\" >nul
echo   SM64EX built OK.
echo.

:: ---------------------------------------------------------------
:: Step 5 — Package release zip
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 5/5] Package release
echo ============================================================

:: Clean old release
if exist "%RELEASE_DIR%" rmdir /s /q "%RELEASE_DIR%"
mkdir "%RELEASE_DIR%\%RELEASE_NAME%"

set "OUT=%RELEASE_DIR%\%RELEASE_NAME%"

echo   Copying SM64 executable and DLLs...
copy /Y "%REPO%\sm64-jak\build\us_pc\sm64.us.f3dex2e.exe" "%OUT%\" >nul
copy /Y "%REPO%\sm64-jak\build\us_pc\*.dll" "%OUT%\" >nul

echo   Copying SM64 sound data...
if exist "%REPO%\sm64-jak\build\us_pc\sound" (
    xcopy /E /I /Q /Y "%REPO%\sm64-jak\build\us_pc\sound" "%OUT%\sound" >nul
)

echo   Copying Jak game data (iso_data)...
xcopy /E /I /Q /Y "%REPO%\iso_data" "%OUT%\jak_data\iso_data" >nul

echo   Copying Jak compiled output (out)...
xcopy /E /I /Q /Y "%REPO%\out" "%OUT%\jak_data\out" >nul

echo   Copying OpenGOAL settings...
if exist "%REPO%\sm64-jak\build\us_pc\OpenGOAL" (
    xcopy /E /I /Q /Y "%REPO%\sm64-jak\build\us_pc\OpenGOAL" "%OUT%\OpenGOAL" >nul
)

:: Create launcher script
echo   Creating launcher...
(
echo @echo off
echo cd /d "%%~dp0"
echo set "JAK_DATA_PATH=%%~dp0jak_data"
echo start "" sm64.us.f3dex2e.exe --skip-intro
) > "%OUT%\Play SM64-Jak.bat"

:: Create README
(
echo SM64-Jak  -  Jak and Daxter in Super Mario 64
echo ================================================
echo.
echo HOW TO PLAY:
echo   Double-click "Play SM64-Jak.bat"
echo.
echo REQUIREMENTS:
echo   - Windows 10/11 64-bit
echo   - GPU with OpenGL 3.3+ support
echo.
echo CONTROLS:
echo   Standard SM64 controls. Jak replaces Mario for most actions.
echo   Mario handles: swimming, cannons, wing cap, shell, poles.
echo.
echo NOTES:
echo   - jak_data\ contains the Jak game assets (do not delete^)
echo   - All DLLs must remain next to the exe
) > "%OUT%\README.txt"

:: Create zip
echo   Creating zip archive...
if exist "%REPO%\%ZIP_NAME%" del "%REPO%\%ZIP_NAME%"

:: Try PowerShell zip (available on all modern Windows)
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath '%REPO%\%ZIP_NAME%' -Force"
if errorlevel 1 (
    echo WARNING: PowerShell zip failed. Release folder is at:
    echo   %RELEASE_DIR%\%RELEASE_NAME%\
    echo You can manually zip it.
) else (
    echo   Created: %ZIP_NAME%
)

:: Show sizes
echo.
echo   Release contents:
for %%f in ("%REPO%\%ZIP_NAME%") do echo   Zip size: %%~zf bytes
echo   Folder:   %RELEASE_DIR%\%RELEASE_NAME%\
echo.

:: ---------------------------------------------------------------
:: Done
:: ---------------------------------------------------------------
echo ============================================================
echo   RELEASE BUILD COMPLETE
echo ============================================================
echo.
echo   Zip:    %REPO%\%ZIP_NAME%
echo   Folder: %RELEASE_DIR%\%RELEASE_NAME%\
echo.
echo   Share the zip. Recipients just unzip and run "Play SM64-Jak.bat"
echo.
pause
exit /b 0

:fail
echo.
echo   Release build failed. See errors above.
echo.
pause
exit /b 1
