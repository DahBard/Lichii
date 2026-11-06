@echo off
rem Emulated CPU cores on P-cores too.
set LICHII_CPU_POLICY=strict
start "" "%~dp0lichii.exe" %*
