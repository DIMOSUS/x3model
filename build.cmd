@echo off
rem Configures and builds x3model (Release, x64) with the installed Visual Studio (2019 or later, C++ workload, which
rem brings CMake and Ninja); output build\x3model.exe. Run it from any prompt: it sets up the x64 compiler itself.
setlocal
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
  echo vswhere.exe not found: install Visual Studio with the "Desktop development with C++" workload
  exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (
  echo no Visual Studio with the C++ x64 tools found
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
cd /d "%~dp0"
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
