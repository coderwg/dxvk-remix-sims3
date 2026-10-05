@echo off
call "%~dp0_vcvars.cmd" x64
if errorlevel 1 exit /b 1
cd /d "%~dp0"
echo === meson setup x64 ===
meson setup --buildtype debugoptimized --backend ninja _compDebugOptimized_x64 --debug
if errorlevel 1 exit /b 2
copy /Y Directory.Build.Props _compDebugOptimized_x64\ >nul
cd _compDebugOptimized_x64
echo === meson compile x64 ===
meson compile
if errorlevel 1 exit /b 3
echo === BUILD_OK ===