@echo off
cd /d %~dp0
rem if exist build rmdir /s /q build
mkdir build
cd build
set PATH=%TFROOT%\gccForTMS\crosstool\bin;%TFROOT%\gccForTMS\crosstool\mipsel-linux-uclibc\bin;%TFROOT%\Cygwin_mini\bin;C:\Programme\Cygwin\bin
set CFLAGS=-pipe -Os -static
set LDFLAGS=-static
rem bash ../configure --host=mipsel-linux-uclibc
make
pause
