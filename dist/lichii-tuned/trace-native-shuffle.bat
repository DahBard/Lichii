@echo off
rem Fragment shader shuffles stay native on the AMD driver (EDEN_AMD_FRAGMENT_SHUFFLE=1)
rem instead of running as loops, with the logging of perf-trace.bat. An older AMD driver
rem crashed building one Ocarina shader this way: if the emulator closes while it builds
rem shaders, this driver still has that crash.
set EDEN_AMD_FRAGMENT_SHUFFLE=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
