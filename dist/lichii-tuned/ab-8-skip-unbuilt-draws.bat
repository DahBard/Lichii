@echo off
rem Skips draws whose new pipeline is still compiling instead of waiting for it
rem (fewer hitches on new content; an object or effect can be missing for a few frames).
rem Also logs GPU timing like perf-trace.bat, for comparison.
set EDEN_NX2_ASYNC_DISCARD=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
