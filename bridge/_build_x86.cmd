@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1
cd /d "C:\Users\William Gallyot\Documents\rtx-remix-investigation\xoxor4d-bridge-remix"
echo === meson setup x86 ===
meson setup --buildtype debugoptimized --backend ninja _compDebugOptimized_x86 --debug
if errorlevel 1 exit /b 2
copy /Y Directory.Build.Props _compDebugOptimized_x86\ >nul
cd _compDebugOptimized_x86
echo === meson compile x86 ===
meson compile
if errorlevel 1 exit /b 3
echo === BUILD_OK ===