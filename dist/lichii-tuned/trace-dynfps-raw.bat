@echo off
rem Dynamic FPS takes each frame's own time instead of an 8-frame average
rem (LICHII_DYNFPS_SMOOTHING=1), with the logging of perf-trace.bat. The average keeps the
rem game running fast for a moment after slow frames; this should not.
set LICHII_DYNFPS_SMOOTHING=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
