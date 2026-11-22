@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
title PanoramaEdit - Windows Release Build

echo =====================================================
echo  PanoramaEdit - local Windows x64 Release build
echo =====================================================
echo.

where cmake >nul 2>&1
if errorlevel 1 (
  echo [ERROR] CMake was not found in PATH.
  echo Install Visual Studio C++ tools and CMake, then retry.
  goto :fail
)

set "QT_PREFIX="
if defined CMAKE_PREFIX_PATH (
  if exist "%CMAKE_PREFIX_PATH%\lib\cmake\Qt6\Qt6Config.cmake" set "QT_PREFIX=%CMAKE_PREFIX_PATH%"
)
if not defined QT_PREFIX if defined QTDIR (
  if exist "%QTDIR%\lib\cmake\Qt6\Qt6Config.cmake" set "QT_PREFIX=%QTDIR%"
)
if not defined QT_PREFIX (
  for %%R in (C D E) do (
    for /d %%V in ("%%R:\Qt\6.*") do (
      if exist "%%~fV\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake" set "QT_PREFIX=%%~fV\msvc2022_64"
    )
  )
)
if not defined QT_PREFIX (
  echo [ERROR] Qt 6 MSVC 2022 64-bit was not found.
  echo Set QTDIR to your Qt directory, e.g. C:\Qt\6.8.3\msvc2022_64
  echo Or set CMAKE_PREFIX_PATH to the same directory.
  goto :fail
)
echo [OK] Qt: !QT_PREFIX!

set "GENERATOR=Visual Studio 17 2022"
cmake -S . -B build -G "%GENERATOR%" -A x64 -DCMAKE_PREFIX_PATH="!QT_PREFIX!"
if errorlevel 1 goto :fail

cmake --build build --config Release --parallel
if errorlevel 1 goto :fail

set "EXE=build\Release\PanoramaEdit.exe"
if not exist "!EXE!" (
  echo [ERROR] Built EXE not found: !EXE!
  goto :fail
)
set "DIST=dist\PanoramaEdit"
if not exist "!DIST!" mkdir "!DIST!"
copy /Y "!EXE!" "!DIST!\PanoramaEdit.exe" >nul
if errorlevel 1 goto :fail

set "DEPLOY=!QT_PREFIX!\bin\windeployqt.exe"
if not exist "!DEPLOY!" (
  echo [ERROR] windeployqt.exe was not found in Qt bin directory.
  goto :fail
)
"!DEPLOY!" --release --compiler-runtime "!DIST!\PanoramaEdit.exe"
if errorlevel 1 goto :fail

echo.
echo [SUCCESS] Ready: %CD%\!DIST!\PanoramaEdit.exe
echo.
pause
exit /b 0

:fail
echo.
echo [FAILED] Build did not complete. See errors above.
pause
exit /b 1
