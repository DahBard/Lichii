@echo off
rem Per-draw work as in r51 (EDEN_NX2_TEST_DRAW_TRIMS=0): three copies of the whole fast draw
rem delta, the buffer cache's invalidations under their own lock and the FSR state read on every
rem tracker call.
rem Unprofiled, with the frame trace: compare gpu_back_cpu_ms_per_frame and the frame
rem times with perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_TEST_DRAW_TRIMS=0&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
