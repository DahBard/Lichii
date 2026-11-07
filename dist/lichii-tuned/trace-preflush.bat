@echo off
rem ab-7-preflush.bat with the logging of perf-trace.bat: GPU readbacks are preflushed
rem (EDEN_NX2_PREFLUSH=1), fewer readback stalls, but some flicker was reported.
set EDEN_NX2_PREFLUSH=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
