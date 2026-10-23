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
rc /nologo /fo obj\TricBar.res TricBar.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O1 /Os /MT /GL /GS /Gy /Gw /EHsc /W3 /DUNICODE /D_UNICODE /Foobj\ TricBar.cpp /Fe:bin\TricBar.exe /link obj\TricBar.res /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS ole32.lib oleacc.lib oleaut32.lib shell32.lib user32.lib
if errorlevel 1 exit /b 1
echo.
echo OK: Engine\bin\TricBar.exe
exit /b 0

:novs
echo MSVC nao encontrado. Instale o Build Tools com "Desenvolvimento para desktop com C++".
exit /b 1
