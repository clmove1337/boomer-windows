@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if not errorlevel 1 if defined INCLUDE if defined LIB goto compile
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" goto missing
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 goto missing
:compile
if not exist build mkdir build
if not exist build exit /b 1
cl /nologo /O2 /W3 /WX src/boomer.c /Fe:build\boomer.exe /Fo:build\ user32.lib gdi32.lib shell32.lib d3d11.lib dxgi.lib dxguid.lib dwmapi.lib /link /SUBSYSTEM:WINDOWS
exit /b %errorlevel%
:missing
echo Microsoft C compiler not found. Install Visual Studio with the Desktop development with C++ workload. 1>&2
exit /b 1
