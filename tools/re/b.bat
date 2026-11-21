@echo off
cd /d "%~dp0"
where cl >nul 2>nul && goto :have
call "G:\Programs\Visual Studio\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
:have
cl /nologo /O1 huff.c /Fehuff.exe /link /BASE:0x10000000 /FIXED /DYNAMICBASE:NO || exit /b 1
if not exist out mkdir out
huff.exe "H:\Need for Speed Most Wanted — co-campaign\speed.exe" "H:\Need for Speed Most Wanted — co-campaign\GLOBAL\gameplay.bin" out
