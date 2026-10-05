@echo off
call :main > "%~dp0_rebuild_client_release.log" 2>&1
exit /b %errorlevel%
:main
call "%~dp0_vcvars.cmd" x86 >nul 2>&1
where cl >nul 2>&1
if errorlevel 1 ( echo VCVARS_FAILED: cl not on PATH & exit /b 1 )
cd /d "%~dp0_compRelease_x86"
echo === incremental meson compile: x86 client RELEASE ===
meson compile
if errorlevel 1 ( echo BUILD_FAILED & exit /b 3 )
echo === BUILD_OK ===