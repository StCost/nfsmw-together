@echo off
rem Builds MWTogether.asi (32-bit). If the game folder is the parent (..\speed.exe), also installs it
rem into ..\scripts. Set MWT_VERSION to stamp a version (CI does this).
setlocal
cd /d "%~dp0"

rem Use the compiler already on PATH (CI / Developer Command Prompt x86), else find vcvars32.
where cl >nul 2>nul && goto :have_cl
set VCVARS=G:\Programs\Visual Studio\Community\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat
)
call "%VCVARS%" >nul || (echo vcvars32 not found & exit /b 1)
:have_cl

if not exist build mkdir build
set VERDEF=
if defined MWT_VERSION set VERDEF=/DMWT_VERSION=\"%MWT_VERSION%\"

cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS %VERDEF% ^
   /Fo"build\\" /Fd"build\\" ^
   src\main.cpp src\log.cpp src\config.cpp src\net.cpp src\overlay.cpp src\mem.cpp src\game.cpp src\input.cpp src\pad.cpp ^
   /LD /Fe"build\MWTogether.asi" ^
   /link /SAFESEH:NO /DYNAMICBASE:NO ws2_32.lib user32.lib gdi32.lib dxguid.lib shell32.lib || exit /b 1

if not exist "..\speed.exe" (echo Built build\MWTogether.asi & exit /b 0)
rem renamed from CoopMod: remove the old plugin (both would load) and keep its settings
if exist "..\scripts\CoopMod.asi" del "..\scripts\CoopMod.asi"
if not exist "..\scripts\MWTogether.ini" if exist "..\scripts\CoopMod.ini" copy /y "..\scripts\CoopMod.ini" "..\scripts\MWTogether.ini" >nul
copy /y "build\MWTogether.asi" "..\scripts\MWTogether.asi" >nul || (echo copy failed - is the game running? & exit /b 1)
if not exist "..\scripts\MWTogether.ini" copy /y "MWTogether.ini" "..\scripts\MWTogether.ini" >nul
echo Built and installed scripts\MWTogether.asi
