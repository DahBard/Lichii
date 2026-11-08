@echo off
rem Texture cache collects from 5 GiB as in official Lichii (this build: 5/8 of the VRAM budget).
set EDEN_NX2_TC_CRITICAL_MIB=5120&& set EDEN_NX2_TC_EXPECTED_MIB=4352
start "" "%~dp0lichii.exe" %*
