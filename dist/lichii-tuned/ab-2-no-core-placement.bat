@echo off
rem Windows places every thread.
set LICHII_CPU_POLICY=off
start "" "%~dp0lichii.exe" %*
