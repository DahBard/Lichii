@echo off
rem The game prepares the next frame while the GPU still renders this one, also when the GPU
rem is behind (EDEN_NX2_FRAMES_AHEAD=2), so a slow GPU never idles between frames. Also logs
rem GPU timing like perf-trace.bat, for comparison.
set EDEN_NX2_FRAMES_AHEAD=2&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
