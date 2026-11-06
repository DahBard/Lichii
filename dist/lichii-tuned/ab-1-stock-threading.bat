@echo off
rem Compiler changes only: Windows places threads, PAUSE spinning.
set LICHII_CPU_POLICY=off&& set LICHII_WAITPKG_SPIN=0
start "" "%~dp0lichii.exe" %*
