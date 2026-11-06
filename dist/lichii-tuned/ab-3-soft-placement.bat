@echo off
rem Only keep threads off the low-power E-cores; pipeline builds on E-cores.
set LICHII_CPU_POLICY=soft
start "" "%~dp0lichii.exe" %*
