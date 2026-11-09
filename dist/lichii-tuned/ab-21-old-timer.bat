@echo off
rem Core timing waits as in round 19 (a timed event wait) instead of on Fritchii's high-resolution
rem waitable timer (round 20), with the perf-trace logging. Compare the frame rate and frame pacing
rem with the default.
set EDEN_HIRES_CLOCK=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
