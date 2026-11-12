@echo off
rem Official shader handling: boot compiles only the pipelines the last sessions used, as with
rem "Build all cached shaders before game start" off (EDEN_NX2_PRELOAD_ALL_PIPELINES=0 over the
rem line in nx2_switches.txt). Unused pipelines are kept up to a quarter of the RAM either way.
set EDEN_NX2_PRELOAD_ALL_PIPELINES=0
start "" "%~dp0lichii.exe" %*
