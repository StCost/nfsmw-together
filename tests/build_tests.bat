@echo off
rem Builds and runs the test suite. No game needed.
rem
rem   tests\build_tests.bat            build + run build\tests.exe (/RTC1, debug CRT), then the same
rem                                    tests under AddressSanitizer (build\tests_asan.exe) if this
rem                                    MSVC has it for x86 - skipped with a note otherwise
rem   tests\build_tests.bat build      only build, don't run
rem   tests\build_tests.bat noasan     skip the AddressSanitizer variant
rem   tests\build_tests.bat analyze    MSVC static analysis (/analyze) of the plugin sources; report only
rem
rem Exit code 0 = everything built and every test passed.
setlocal
cd /d "%~dp0\.."

rem Use the compiler already on PATH (CI / Developer Command Prompt x86), else find vcvars32.
where cl >nul 2>nul && goto :have_cl
set VCVARS=G:\Programs\Visual Studio\Community\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat
)
call "%VCVARS%" >nul 2>nul || (echo vcvars32 not found & exit /b 1)
:have_cl

set COMMON=/nologo /EHsc /std:c++17 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DMWT_TESTS
set SRC=tests\test_main.cpp tests\test_protocol.cpp tests\test_util.cpp tests\test_net.cpp tests\test_packets.cpp tests\test_file.cpp tests\test_config.cpp tests\test_fuzz.cpp tests\test_loopback.cpp tests\test_pursuit.cpp tests\test_milestone.cpp tests\test_race.cpp tests\test_profile.cpp tests\test_ground.cpp src\net.cpp src\config.cpp src\log.cpp

if /i "%1"=="analyze" goto :analyze

rem ---- 1. run-time checks build ---------------------------------------------------------------
if not exist build\tests mkdir build\tests
cl %COMMON% /Od /MTd /RTC1 /Zi /Fo"build\tests\\" /Fd"build\tests\tests.pdb" %SRC% ^
   /Fe"build\tests.exe" /link /INCREMENTAL:NO ws2_32.lib || exit /b 1
echo Built build\tests.exe
if /i "%1"=="build" goto :asan
build\tests.exe || (echo TESTS FAILED & exit /b 1)

rem ---- 2. AddressSanitizer build (optional) ---------------------------------------------------
:asan
if /i "%1"=="noasan" exit /b 0
if /i "%2"=="noasan" exit /b 0
if not exist build\tests_asan mkdir build\tests_asan
cl %COMMON% /Od /MT /Zi /fsanitize=address /Fo"build\tests_asan\\" /Fd"build\tests_asan\tests_asan.pdb" %SRC% ^
   /Fe"build\tests_asan.exe" /link /INCREMENTAL:NO ws2_32.lib >build\tests_asan\build.log 2>&1
if errorlevel 1 (
  echo AddressSanitizer build not available with this toolset - skipped. Details: build\tests_asan\build.log
  exit /b 0
)
echo Built build\tests_asan.exe
if /i "%1"=="build" exit /b 0
rem (tests_asan.exe needs the ASan runtime DLL from the MSVC bin folder on PATH: run it through this
rem script or a Developer Command Prompt. Some Windows builds refuse a few of ASan's function
rem intercepts at start-up; memory checking still works, so carry on.)
set ASAN_WIN_CONTINUE_ON_INTERCEPTION_FAILURE=1
build\tests_asan.exe || (echo TESTS FAILED under AddressSanitizer & exit /b 1)
echo All tests passed.
exit /b 0

rem ---- static analysis (report only) ----------------------------------------------------------
:analyze
if not exist build\analyze mkdir build\analyze
cl /nologo /EHsc /std:c++17 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /analyze /analyze:only /analyze:stacksize 32768 /wd6031 /wd6101 /wd6320 /wd6322 /wd28159 /Fo"build\analyze\\" ^
   src\main.cpp src\log.cpp src\config.cpp src\net.cpp src\overlay.cpp src\mem.cpp src\game.cpp src\input.cpp src\pad.cpp
exit /b %errorlevel%
