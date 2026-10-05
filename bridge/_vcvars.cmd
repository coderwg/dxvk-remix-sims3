@echo off
rem Sets up the MSVC toolchain for %1 (x86 or x64) in the calling script: finds Visual Studio
rem 2017 or later, or its Build Tools, with the C++ tools through vswhere (every install carries
rem it) and calls its vcvarsall.
set "VCVARSALL="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find VC\Auxiliary\Build\vcvarsall.bat`) do set "VCVARSALL=%%i"
if not defined VCVARSALL ( echo VCVARS_FAILED: no Visual Studio with the C++ tools found & exit /b 1 )
call "%VCVARSALL%" %1
