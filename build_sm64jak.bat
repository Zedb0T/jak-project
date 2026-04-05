@echo off
setlocal enabledelayedexpansion

echo ============================================================
echo   SM64-Jak Build Script
echo   Builds libjakopengoal DLL + SM64EX with Jak integration
echo ============================================================
echo.

:: Save the repo root
set "REPO_ROOT=%~dp0"
set "REPO_ROOT=%REPO_ROOT:~0,-1%"

:: ---------------------------------------------------------------
:: Step 0 — Prerequisite checks
:: ---------------------------------------------------------------
echo [Step 0] Checking prerequisites...
echo.

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake not found. Install CMake and add it to PATH.
    goto :fail
)
echo   cmake ......... OK

if not exist "C:\msys64\usr\bin\make.exe" (
    echo ERROR: MSYS2 make not found at C:\msys64\usr\bin\make.exe
    echo   Install MSYS2 and run: pacman -S mingw-w64-x86_64-toolchain make
    goto :fail
)
echo   msys2 make .... OK

if not exist "C:\msys64\mingw64\bin\gcc.exe" (
    echo ERROR: MinGW64 gcc not found. Run: pacman -S mingw-w64-x86_64-toolchain
    goto :fail
)
echo   mingw64 gcc ... OK

if not exist "%REPO_ROOT%\sm64-jak\baserom.us.z64" (
    echo WARNING: sm64-jak\baserom.us.z64 not found.
    echo   SM64EX build will fail without a US baserom.
    echo.
)

if not exist "%REPO_ROOT%\iso_data\jak1" (
    echo WARNING: iso_data\jak1\ not found.
    echo   Jak asset extraction will fail without extracted PS2 ISO data.
    echo.
)

echo.
echo Prerequisites look good.
echo.

:: ---------------------------------------------------------------
:: Step 1 — Configure CMake (if needed)
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 1] Configure CMake build
echo ============================================================
echo.

if exist "%REPO_ROOT%\build\CMakeCache.txt" (
    echo   Build directory already configured, skipping.
    echo.
) else (
    echo   This will run cmake to configure the project.
    choice /c YN /m "Proceed?"
    if errorlevel 2 goto :skip_cmake_config
    echo.
    echo   Running cmake configure...
    cmake -B build -DCMAKE_BUILD_TYPE=Release "%REPO_ROOT%"
    if errorlevel 1 (
        echo ERROR: CMake configure failed.
        goto :fail
    )
    echo   CMake configure complete.
    echo.
)
:skip_cmake_config

:: ---------------------------------------------------------------
:: Step 2 — Build libjakopengoal DLL
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 2] Build libjakopengoal DLL (MSVC / Release)
echo ============================================================
echo.
echo   This compiles the GOAL VM into a DLL that SM64 loads at runtime.
choice /c YN /m "Proceed?"
if errorlevel 2 goto :skip_dll

echo.
echo   Building...
cmake --build build --target jakopengoal --config Release
if errorlevel 1 (
    echo ERROR: DLL build failed.
    goto :fail
)
echo.
echo   DLL build complete.
echo.
:skip_dll

:: ---------------------------------------------------------------
:: Step 3 — Copy DLLs to SM64 build directory
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 3] Copy DLLs to sm64-jak\build\us_pc\
echo ============================================================
echo.
echo   jakopengoal.dll and its dependencies need to be next to sm64.exe.

if not exist "%REPO_ROOT%\build\bin\Release\jakopengoal.dll" (
    echo ERROR: build\bin\Release\jakopengoal.dll not found. Build the DLL first (Step 2).
    goto :fail
)

if not exist "%REPO_ROOT%\sm64-jak\build\us_pc" (
    echo   Creating sm64-jak\build\us_pc\ directory...
    mkdir "%REPO_ROOT%\sm64-jak\build\us_pc"
)

choice /c YN /m "Copy DLLs?"
if errorlevel 2 goto :skip_copy

echo.
echo   Copying DLLs...
copy /Y "%REPO_ROOT%\build\bin\Release\*.dll" "%REPO_ROOT%\sm64-jak\build\us_pc\" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy DLLs.
    goto :fail
)
echo   DLLs copied successfully.
echo.
:skip_copy

:: ---------------------------------------------------------------
:: Step 4 — Build SM64EX with Jak integration
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 4] Build SM64EX with Jak integration (MinGW64)
echo ============================================================
echo.
echo   This compiles SM64EX with JAKOPENGOAL=1 using MSYS2 MinGW64.
choice /c YN /m "Proceed?"
if errorlevel 2 goto :skip_sm64

echo.
echo   Building SM64EX...
C:\msys64\usr\bin\env.exe MSYSTEM=MINGW64 /usr/bin/bash -lc "cd '%REPO_ROOT:\=/%/sm64-jak' && export PATH='/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH' && TMPDIR=/tmp TMP=/tmp TEMP=/tmp OS=Windows_NT make -j$(nproc) JAKOPENGOAL=1 WINDOWS_BUILD=1"
if errorlevel 1 (
    echo ERROR: SM64EX build failed.
    goto :fail
)
echo.
echo   SM64EX build complete.
echo.
:skip_sm64

:: ---------------------------------------------------------------
:: Step 5 — Copy DLLs again (in case SM64 build recreated the directory)
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 5] Ensure DLLs are in place
echo ============================================================
echo.
copy /Y "%REPO_ROOT%\build\bin\Release\*.dll" "%REPO_ROOT%\sm64-jak\build\us_pc\" >nul 2>&1
echo   DLLs verified in sm64-jak\build\us_pc\.
echo.

:: ---------------------------------------------------------------
:: Done
:: ---------------------------------------------------------------
echo ============================================================
echo   BUILD COMPLETE
echo ============================================================
echo.
echo   To run the game:
echo.
echo     cd sm64-jak\build\us_pc
echo     set JAK_DATA_PATH=%REPO_ROOT%
echo     sm64.us.f3dex2e.exe --skip-intro
echo.
echo   Or just run: launch_sm64jak.bat
echo.

:: Create a launch script too
(
echo @echo off
echo cd /d "%REPO_ROOT%\sm64-jak\build\us_pc"
echo set "JAK_DATA_PATH=%REPO_ROOT%"
echo start "" sm64.us.f3dex2e.exe --skip-intro
) > "%REPO_ROOT%\launch_sm64jak.bat"
echo   (launch_sm64jak.bat has been created in the repo root)
echo.
pause
exit /b 0

:fail
echo.
echo   Build failed. See errors above.
echo.
pause
exit /b 1
