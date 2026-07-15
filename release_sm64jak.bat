@echo off
setlocal enabledelayedexpansion

echo ============================================================
echo SM64-Jak Release Builder
echo Builds everything and packages into a distributable zip
echo ============================================================
echo.

:: Save the repo root
set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"
set "RELEASE_DIR=%REPO%\release_sm64jak"
set "RELEASE_NAME=SM64-Jak"

:: Date-stamped zip
for /f "usebackq tokens=*" %%d in (`powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd"`) do set "STAMP=%%d"
set "ZIP_NAME=%RELEASE_NAME%_%STAMP%.zip"

:: ---------------------------------------------------------------
:: Kill running game
:: ---------------------------------------------------------------
tasklist /FI "IMAGENAME eq sm64.us.f3dex2e.exe" 2>nul | find /i "sm64.us.f3dex2e.exe" >nul
if not errorlevel 1 (
    echo [!] SM64-Jak is running. Closing it...
    taskkill /IM sm64.us.f3dex2e.exe /F >nul 2>&1
    timeout /t 2 /nobreak >nul
    echo Closed.
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
echo cmake ......... OK

if not exist "C:\msys64\usr\bin\make.exe" (
    echo ERROR: MSYS2 make not found.
    goto :fail
)
echo msys2 make .... OK

if not exist "C:\msys64\mingw64\bin\gcc.exe" (
    echo ERROR: MinGW64 gcc not found.
    goto :fail
)
echo mingw64 gcc ... OK

if not exist "%REPO%\sm64-jak\baserom.us.z64" (
    echo ERROR: baserom.us.z64 not found.
    goto :fail
)
echo baserom ....... OK
echo.

:: ---------------------------------------------------------------
:: Step 1 — CMake
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 1/3] Configure CMake
echo ============================================================
if not exist "%REPO%\build\CMakeCache.txt" (
    echo Running cmake configure...
    cmake -B "%REPO%\build" -DCMAKE_BUILD_TYPE=Release "%REPO%"
    if errorlevel 1 goto :fail
) else (
    echo Already configured, skipping.
)
echo.

:: ---------------------------------------------------------------
:: Step 2 — Build
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 2/3] Build DLL + extractor
echo ============================================================
cmake --build "%REPO%\build" --target jakopengoal --config Release
if errorlevel 1 (
    echo ERROR: DLL build failed.
    goto :fail
)
echo DLL built OK.

cmake --build "%REPO%\build" --target extractor gk goalc --config Release
if errorlevel 1 (
    echo ERROR: extractor/gk/goalc build failed.
    goto :fail
)
echo Extractor + gk + goalc built OK.
echo.

:: ---------------------------------------------------------------
:: Step 3 — Package
:: ---------------------------------------------------------------
echo ============================================================
echo [Step 3/3] Package release
echo ============================================================

if exist "%RELEASE_DIR%" rmdir /s /q "%RELEASE_DIR%"
mkdir "%RELEASE_DIR%\%RELEASE_NAME%"
set "OUT=%RELEASE_DIR%\%RELEASE_NAME%"

echo Copying DLLs and controller support files...
mkdir "%OUT%\dlls" 2>nul
copy /Y "%REPO%\build\bin\Release\*.dll" "%OUT%\dlls\" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy DLLs.
    goto :fail
)
if exist "%REPO%\sm64-jak\extras\gamecontrollerdb.txt" copy /Y "%REPO%\sm64-jak\extras\gamecontrollerdb.txt" "%OUT%\dlls\" >nul
if exist "%REPO%\controllers.txt" copy /Y "%REPO%\controllers.txt" "%OUT%\dlls\" >nul

:: ---------------------------------------------------------------
:: Export sm64-jak source - FIXED
:: ---------------------------------------------------------------
echo Exporting sm64-jak source (git-clean, no ROM-derived files)...

set "REPO_FWD=%REPO:\=/%"
set "OUT_FWD=%OUT:\=/%"

C:\msys64\usr\bin\env.exe MSYSTEM=MINGW64 /usr/bin/bash -lc "cd '%REPO_FWD%' && git ls-files -z --cached --others --exclude-standard sm64-jak ':(exclude)sm64-jak/build' | tar --null -T - -cf - | tar -xf - -C '%OUT_FWD%'"

if errorlevel 1 (
    echo ERROR: sm64-jak source export failed.
    goto :fail
)

if not exist "%OUT%\sm64-jak\Makefile" (
    echo ERROR: source export incomplete ^(no Makefile^).
    goto :fail
)

:: ---------------------------------------------------------------
:: Jak data
:: ---------------------------------------------------------------
echo Copying Jak toolchain and data skeleton...

copy /Y "%REPO%\build\bin\Release\extractor.exe" "%OUT%\" >nul
if errorlevel 1 (
    echo ERROR: extractor.exe missing.
    goto :fail
)
:: gk.exe is the mod-launcher entry point; it and extractor need their
:: companion DLLs co-located (MSVC shared build) or they die with
:: "common.dll was not found".
copy /Y "%REPO%\build\bin\Release\gk.exe" "%OUT%\" >nul
copy /Y "%REPO%\build\bin\Release\goalc.exe" "%OUT%\" >nul
copy /Y "%REPO%\build\bin\Release\*.dll" "%OUT%\" >nul

mkdir "%OUT%\data\launcher" 2>nul
mkdir "%OUT%\data\decompiler" 2>nul
mkdir "%OUT%\data\game\graphics\opengl_renderer" 2>nul
mkdir "%OUT%\data\log" 2>nul

copy /Y "%REPO%\.github\scripts\releases\error-code-metadata.json" "%OUT%\data\launcher\" >nul
xcopy /E /I /Q /Y "%REPO%\decompiler\config" "%OUT%\data\decompiler\config" >nul
xcopy /E /I /Q /Y "%REPO%\goal_src" "%OUT%\data\goal_src" >nul
xcopy /E /I /Q /Y "%REPO%\game\assets" "%OUT%\data\game\assets" >nul
xcopy /E /I /Q /Y "%REPO%\game\graphics\opengl_renderer\shaders" "%OUT%\data\game\graphics\opengl_renderer\shaders" >nul
xcopy /E /I /Q /Y "%REPO%\custom_assets" "%OUT%\data\custom_assets" >nul

:: Setup files
echo Copying Setup, Play and README...
copy /Y "%REPO%\release_files\Setup.bat" "%OUT%\" >nul
copy /Y "%REPO%\release_files\Play SM64-Jak.bat" "%OUT%\" >nul
copy /Y "%REPO%\release_files\README.txt" "%OUT%\" >nul

if not exist "%OUT%\Setup.bat" (
    echo ERROR: Setup.bat missing.
    goto :fail
)

:: ---------------------------------------------------------------
:: Create zip
:: ---------------------------------------------------------------
echo Creating zip archive...
if exist "%REPO%\%ZIP_NAME%" del "%REPO%\%ZIP_NAME%"

powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath '%REPO%\%ZIP_NAME%' -Force"
if errorlevel 1 (
    echo WARNING: PowerShell zip failed. You can zip the folder manually.
    echo Folder: %RELEASE_DIR%\%RELEASE_NAME%\
) else (
    echo Created: %ZIP_NAME%
)

echo.
echo Release contents:
for %%f in ("%REPO%\%ZIP_NAME%") do echo Zip size: %%~zf bytes
echo Folder: %RELEASE_DIR%\%RELEASE_NAME%\
echo.

echo ============================================================
echo RELEASE BUILD COMPLETE
echo ============================================================
echo.
pause
exit /b 0

:fail
echo.
echo Release build failed. See errors above.
pause
exit /b 1