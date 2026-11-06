@echo off
rem PAUSE spinning instead of UMWAIT waits.
set LICHII_WAITPKG_SPIN=0
start "" "%~dp0lichii.exe" %*
