@echo off
rem Emulated CPU cores on P-cores too; pipeline builds on E-cores only.
set LICHII_CPU_POLICY=strict
start "" "%~dp0lichii.exe" %*
