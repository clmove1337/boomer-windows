@echo off
setlocal
cd /d "%~dp0"
call build.bat
if errorlevel 1 exit /b 1
start "" /wait build\boomer.exe --self-test
if errorlevel 1 goto failed
rem Verify checks remain active in release builds and run MSVC's static analyzer.
set "CL=%CL% /DNDEBUG /analyze"
call build.bat
if errorlevel 1 exit /b 1
start "" /wait build\boomer.exe --self-test
if errorlevel 1 goto failed
echo All self-tests and static analysis passed.
exit /b 0
:failed
echo Self-test failed at source line %errorlevel%. 1>&2
exit /b 1
