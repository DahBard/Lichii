@echo off
rem Official shader handling: boot compiles only the pipelines the last sessions used, and a
rem pipeline unused for 9000 frames is destroyed (LICHII_KEEP_PIPELINES=0, and
rem EDEN_NX2_PRELOAD_ALL_PIPELINES=0 over the line in nx2_switches.txt).
set LICHII_KEEP_PIPELINES=0&& set EDEN_NX2_PRELOAD_ALL_PIPELINES=0
start "" "%~dp0lichii.exe" %*
