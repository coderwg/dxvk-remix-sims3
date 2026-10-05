@echo off
call "%~dp0_vcvars.cmd" x86
if errorlevel 1 exit /b 1
cd /d "%~dp0"
echo === meson setup x86 ===
meson setup --buildtype debugoptimized --backend ninja _compDebugOptimized_x86 --debug
if errorlevel 1 exit /b 2
copy /Y Directory.Build.Props _compDebugOptimized_x86\ >nul
cd _compDebugOptimized_x86
echo === meson compile x86 ===
meson compile
if errorlevel 1 exit /b 3
echo === BUILD_OK ===