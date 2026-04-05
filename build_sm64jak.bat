@echo off
setlocal

echo ============================================================
echo   SM64-Jak Build Script
echo   Builds libjakopengoal DLL + SM64EX with Jak integration
echo ============================================================
echo.

:: Save the repo root (strip trailing backslash)
set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

:: ---------------------------------------------------------------
:: Step 0 — Prerequisite checks
:: ---------------------------------------------------------------
echo [Step 0] Checking prerequisites...
echo.

where cmake >nul 2>&1
if errorlevel 1 goto :no_cmake
echo   cmake ......... OK
goto :check_make

:no_cmake
echo ERROR: cmake not found. Install CMake and add it to PATH.
goto :fail

:check_make
if not exist "C:\msys64\usr\bin\make.exe" goto :no_make
echo   msys2 make .... OK
goto :check_gcc

:no_make
echo ERROR: MSYS2 make not found at C:\msys64\usr\bin\make.exe
echo   Install MSYS2 and run: pacman -S mingw-w64-x86_64-toolchain make
goto :fail

:check_gcc
if not exist "C:\msys64\mingw64\bin\gcc.exe" goto :no_gcc
echo   mingw64 gcc ... OK
goto :check_rom

:no_gcc
echo ERROR: MinGW64 gcc not found. Run: pacman -S mingw-w64-x86_64-toolchain
goto :fail

:check_rom
if exist "%REPO%\sm64-jak\baserom.us.z64" goto :check_iso
echo WARNING: sm64-jak\baserom.us.z64 not found.
echo   SM64EX build will fail without a US baserom.
echo.

:check_iso
if exist "%REPO%\iso_data\jak1" goto :prereqs_done
echo WARNING: iso_data\jak1\ not found.
echo   Jak asset extraction will fail without extracted PS2 ISO data.
echo.

:prereqs_done
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

if exist "%REPO%\build\CMakeCache.txt" goto :cmake_already_done
echo   This will run cmake to configure the project.
choice /c YN /m "Proceed?"
if errorlevel 2 goto :step2
echo.
echo   Running cmake configure...
cmake -B "%REPO%\build" -DCMAKE_BUILD_TYPE=Release "%REPO%"
if errorlevel 1 goto :cmake_fail
echo   CMake configure complete.
echo.
goto :step2

:cmake_already_done
echo   Build directory already configured, skipping.
echo.
goto :step2

:cmake_fail
echo ERROR: CMake configure failed.
goto :fail

:: ---------------------------------------------------------------
:: Step 2 — Build libjakopengoal DLL
:: ---------------------------------------------------------------
:step2
echo ============================================================
echo [Step 2] Build libjakopengoal DLL (MSVC / Release)
echo ============================================================
echo.
echo   This compiles the GOAL VM into a DLL that SM64 loads at runtime.
choice /c YN /m "Proceed?"
if errorlevel 2 goto :step3

echo.
echo   Building...
cmake --build "%REPO%\build" --target jakopengoal --config Release
if errorlevel 1 goto :dll_fail
echo.
echo   DLL build complete.
echo.
goto :step3

:dll_fail
echo ERROR: DLL build failed.
goto :fail

:: ---------------------------------------------------------------
:: Step 3 — Copy DLLs to SM64 build directory
:: ---------------------------------------------------------------
:step3
echo ============================================================
echo [Step 3] Copy DLLs to sm64-jak\build\us_pc\
echo ============================================================
echo.
echo   jakopengoal.dll and its dependencies need to be next to sm64.exe.

if not exist "%REPO%\build\bin\Release\jakopengoal.dll" goto :no_dll

if not exist "%REPO%\sm64-jak\build\us_pc" mkdir "%REPO%\sm64-jak\build\us_pc"

choice /c YN /m "Copy DLLs?"
if errorlevel 2 goto :step4

echo.
echo   Copying DLLs...
copy /Y "%REPO%\build\bin\Release\*.dll" "%REPO%\sm64-jak\build\us_pc\" >nul
if errorlevel 1 goto :copy_fail
echo   DLLs copied successfully.
echo.
goto :step4

:no_dll
echo ERROR: jakopengoal.dll not found in build\bin\Release\. Build the DLL first (Step 2).
goto :fail

:copy_fail
echo ERROR: Failed to copy DLLs.
goto :fail

:: ---------------------------------------------------------------
:: Step 4 — Build SM64EX with Jak integration
:: ---------------------------------------------------------------
:step4
echo ============================================================
echo [Step 4] Build SM64EX with Jak integration (MinGW64)
echo ============================================================
echo.
echo   This compiles SM64EX with JAKOPENGOAL=1 using MSYS2 MinGW64.
choice /c YN /m "Proceed?"
if errorlevel 2 goto :step5

echo.
echo   Building SM64EX...
set "SM64_DIR=%REPO:\=/%"
C:\msys64\usr\bin\env.exe MSYSTEM=MINGW64 /usr/bin/bash -lc "cd '%SM64_DIR%/sm64-jak' && export PATH='/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH' && TMPDIR=/tmp TMP=/tmp TEMP=/tmp OS=Windows_NT make -j$(nproc) JAKOPENGOAL=1 WINDOWS_BUILD=1"
if errorlevel 1 goto :sm64_fail
echo.
echo   SM64EX build complete.
echo.
goto :step5

:sm64_fail
echo ERROR: SM64EX build failed.
goto :fail

:: ---------------------------------------------------------------
:: Step 5 — Copy DLLs again (in case SM64 build recreated the directory)
:: ---------------------------------------------------------------
:step5
echo ============================================================
echo [Step 5] Ensure DLLs are in place
echo ============================================================
echo.
if exist "%REPO%\build\bin\Release\jakopengoal.dll" copy /Y "%REPO%\build\bin\Release\*.dll" "%REPO%\sm64-jak\build\us_pc\" >nul 2>&1
echo   DLLs verified in sm64-jak\build\us_pc\.
echo.

:: ---------------------------------------------------------------
:: Create launch script
:: ---------------------------------------------------------------
echo @echo off> "%REPO%\launch_sm64jak.bat"
echo cd /d "%REPO%\sm64-jak\build\us_pc">> "%REPO%\launch_sm64jak.bat"
echo set "JAK_DATA_PATH=%REPO%">> "%REPO%\launch_sm64jak.bat"
echo start "" sm64.us.f3dex2e.exe --skip-intro>> "%REPO%\launch_sm64jak.bat"

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
echo     set JAK_DATA_PATH=%REPO%
echo     sm64.us.f3dex2e.exe --skip-intro
echo.
echo   Or just run: launch_sm64jak.bat
echo.
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
