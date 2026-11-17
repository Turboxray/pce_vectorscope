@echo off
rem Builds build\pce_vectorscope.exe with the w64devkit toolchain (gcc, cmake, make).
setlocal
if not defined W64DEVKIT set "W64DEVKIT=C:\w64devkit"
if not exist "%W64DEVKIT%\bin\g++.exe" (
  echo w64devkit not found at %W64DEVKIT%. Set W64DEVKIT to its folder.
  exit /b 1
)
set "PATH=%W64DEVKIT%\bin;%PATH%"
cd /d "%~dp0"
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build --parallel 8 || exit /b 1
echo.
echo Built %~dp0build\pce_vectorscope.exe
