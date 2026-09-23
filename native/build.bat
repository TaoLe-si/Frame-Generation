@echo off
setlocal enabledelayedexpansion

rem ---------------------------------------------------------------------------
rem 构建 dlssmc_native.dll
rem   cl.exe 直接编译，不依赖 cmake（本机没装 cmake）
rem ---------------------------------------------------------------------------

set "VS=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
set "SL_SDK=D:\Backup\Downloads\streamline-sdk-v2.14.1"
set "JDK=D:\Java21"
set "ROOT=%~dp0"
set "OUT=%ROOT%build"

if not exist "%OUT%" mkdir "%OUT%"

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

echo [1/1] compiling...
cl /nologo /std:c++17 /O2 /MD /EHsc /DNDEBUG /DUNICODE /D_UNICODE /D_WINDOWS ^
   /I"%JDK%\include" /I"%JDK%\include\win32" /I"%SL_SDK%\include" ^
   /Fo"%OUT%\\" /Fd"%OUT%\dlssmc_native.pdb" ^
   "%ROOT%src\dlssmc_native.cpp" ^
   /link /DLL /MACHINE:X64 /OUT:"%OUT%\dlssmc_native.dll" ^
   "%SL_SDK%\lib\x64\sl.interposer.lib" d3d11.lib dxgi.lib

if errorlevel 1 (
    echo [ERROR] compile failed
    exit /b 1
)

echo [OK] %OUT%\dlssmc_native.dll
exit /b 0
