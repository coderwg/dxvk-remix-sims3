@echo off
rem Builds and runs the hook's unit test (sims3_camera_hook.h pure functions) with the
rem x64 MSVC toolchain. Output goes to run_tests.log next to this script.
call :main > "%~dp0run_tests.log" 2>&1
exit /b %errorlevel%
:main
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
where cl >nul 2>&1
if errorlevel 1 ( echo VCVARS_FAILED: cl not on PATH & exit /b 1 )
cd /d "%~dp0"
echo === compile ===
cl /nologo /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS /I"%~dp0..\..\src\client" "%~dp0test_sims3cam.cpp" /Fe:"%~dp0test_sims3cam.exe" /Fo:"%~dp0test_sims3cam.obj"
if errorlevel 1 ( echo COMPILE_FAILED & exit /b 2 )
echo === run ===
"%~dp0test_sims3cam.exe"
set rc=%errorlevel%
echo === test exit code %rc% ===
exit /b %rc%
