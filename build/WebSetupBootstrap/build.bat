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
if not exist ..\..\dist mkdir ..\..\dist
rc /nologo /fo obj\TricBarWebSetup.res TricBarWebSetup.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE /Fo.\obj\ TricBarWebSetup.cpp /Fe:bin\TricBar-v2-WebSetup.exe /link obj\TricBarWebSetup.res urlmon.lib shell32.lib ole32.lib user32.lib
if errorlevel 1 exit /b 1
copy /Y bin\TricBar-v2-WebSetup.exe ..\..\dist\TricBar-v2-WebSetup.exe >nul
if errorlevel 1 exit /b 1

echo OK: build\WebSetupBootstrap\bin\TricBar-v2-WebSetup.exe
exit /b 0

:novs
echo MSVC nao encontrado. Instale o Build Tools com "Desenvolvimento para desktop com C++".
exit /b 1
