@echo off
call :main > "%~dp0_rebuild_server_release.log" 2>&1
exit /b %errorlevel%
:main
call "%~dp0_vcvars.cmd" x64 >nul 2>&1
where cl >nul 2>&1
if errorlevel 1 ( echo VCVARS_FAILED: cl not on PATH & exit /b 1 )
cd /d "%~dp0_compRelease_x64"
echo === incremental meson compile: x64 server RELEASE ===
meson compile
if errorlevel 1 ( echo BUILD_FAILED & exit /b 3 )
echo === BUILD_OK ===
