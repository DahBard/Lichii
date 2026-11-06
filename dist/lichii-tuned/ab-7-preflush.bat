@echo off
rem Preflush GPU readbacks (fewer readback stalls, may flicker).
set EDEN_NX2_PREFLUSH=1
start "" "%~dp0lichii.exe" %*
