@echo off
cd /d %~dp0
mkdir build
cd build
set PATH=%TFROOT%\gccForTMS\crosstool\bin;%TFROOT%\Cygwin_mini\bin;%TFROOT%\Cygwin_mini\bin\evtl;%PATH%
set LDFLAGS=-static
bash ../configure --host=mipsel-linux-uclibc
make
pause
