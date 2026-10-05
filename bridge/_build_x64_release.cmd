@echo off
call "%~dp0_vcvars.cmd" x64
if errorlevel 1 exit /b 1
cd /d "%~dp0"
echo === meson setup x64 RELEASE ===
meson setup --buildtype release --backend ninja _compRelease_x64
if errorlevel 1 exit /b 2
copy /Y Directory.Build.Props _compRelease_x64\ >nul
cd _compRelease_x64
echo === meson compile x64 RELEASE ===
meson compile
if errorlevel 1 exit /b 3
echo === BUILD_OK ===