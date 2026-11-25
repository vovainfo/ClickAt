@echo off
setlocal
cd /d "%~dp0"
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" if defined VCToolsInstallDir goto compile
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo Install Visual Studio Build Tools with Desktop development with C++.
    exit /b 1
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

:compile
if not exist build mkdir build
pushd build
set "FLAGS=/nologo /std:c++17 /W4 /WX /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
rc /nologo /fo ClickAt.res ..\src\ClickAt.rc
if errorlevel 1 exit /b 1
cl %FLAGS% /c ..\src\scheduler.cpp
if errorlevel 1 exit /b 1
cl %FLAGS% ..\src\main.cpp scheduler.obj ClickAt.res user32.lib gdi32.lib wtsapi32.lib advapi32.lib comctl32.lib shell32.lib /link /MANIFEST:NO /SUBSYSTEM:WINDOWS /OUT:ClickAt.exe
if errorlevel 1 exit /b 1
cl %FLAGS% ..\src\benchmark.cpp scheduler.obj ClickAt.res user32.lib wtsapi32.lib advapi32.lib /link /MANIFEST:NO /OUT:ClickAtBenchmark.exe
exit /b %errorlevel%
