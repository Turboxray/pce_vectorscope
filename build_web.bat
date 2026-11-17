@echo off
rem Builds the browser version into build_web\site (index.html, index.js, index.wasm) with the Emscripten SDK.
rem To try it: python -m http.server -d build_web\site 8000, then open http://localhost:8000
setlocal
if not defined EMSDK set "EMSDK=C:\emsdk"
if not exist "%EMSDK%\emsdk_env.bat" (
  echo Emscripten SDK not found at %EMSDK%. Set EMSDK to its folder.
  echo To get it: git clone https://github.com/emscripten-core/emsdk.git, then "emsdk install 4.0.10" and "emsdk activate 4.0.10".
  exit /b 1
)
call "%EMSDK%\emsdk_env.bat" >nul 2>&1
rem Emscripten has no make of its own: use w64devkit's, as build.bat does, or ninja if it is there.
if not defined W64DEVKIT set "W64DEVKIT=C:\w64devkit"
if exist "%W64DEVKIT%\bin\make.exe" set "PATH=%PATH%;%W64DEVKIT%\bin"
cd /d "%~dp0"
call emcmake cmake -S . -B build_web -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build_web --parallel 8 || exit /b 1
if not exist build_web\site mkdir build_web\site
copy /y build_web\index.html build_web\site\ >nul
copy /y build_web\index.js build_web\site\ >nul
copy /y build_web\index.wasm build_web\site\ >nul
echo.
echo Built %~dp0build_web\site
