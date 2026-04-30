@echo off
setlocal

where cl.exe >nul 2>nul
if %ERRORLEVEL%==0 goto build_msvc

set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" (
    call "%VCVARS%" >nul
    goto build_msvc
)

set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" (
    call "%VCVARS%" >nul
    goto build_msvc
)

set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" (
    call "%VCVARS%" >nul
    goto build_msvc
)

set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" (
    call "%VCVARS%" >nul
    goto build_msvc
)

where gcc.exe >nul 2>nul
if %ERRORLEVEL%==0 (
    gcc -Wall -Wextra -O2 -municode wdu.c -lshell32 -o wdu.exe
    exit /b %ERRORLEVEL%
)

echo No supported C compiler found.
echo Install Microsoft Build Tools or MinGW-w64, then run build.bat again.
exit /b 1

:build_msvc
cl /W4 /O2 wdu.c shell32.lib /Fe:wdu.exe
exit /b %ERRORLEVEL%
