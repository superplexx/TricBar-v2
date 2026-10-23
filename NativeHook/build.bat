@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if %errorlevel%==0 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

:build
if not exist bin mkdir bin
if not exist obj mkdir obj
cl /nologo /std:c++17 /permissive- /EHsc /O2 /MT /LD /W3 /DUNICODE /D_UNICODE /D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS /Foobj\ TricBar.cpp /link /DEF:TricBar.def /OUT:bin\TricBar.dll windowsapp.lib ole32.lib user32.lib
if errorlevel 1 exit /b 1
echo.
echo OK: NativeHook\bin\TricBar.dll
exit /b 0

:novs
echo MSVC nao encontrado. Instale o Build Tools com "Desenvolvimento para desktop com C++".
exit /b 1
