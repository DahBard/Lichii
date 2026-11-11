@echo off
rem The default settings with only the frame and heap traces (EDEN_DKB_FRAME_TRACE=1,
rem EDEN_NX2_HEAP_TRACE=1), without the GPU thread zones: gpu_back_cpu_ms_per_frame and the frame
rem times as unprofiled play pays them, and every 10 s what the game's heap maps and unmaps cost.
rem The baseline for the unprofiled A/B launchers (ab-26 to ab-37), which log the same.
set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1
start "" "%~dp0lichii.exe" %*
