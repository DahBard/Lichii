@echo off
rem The default settings with only the frame, heap and clock traces (EDEN_DKB_FRAME_TRACE=1,
rem EDEN_NX2_HEAP_TRACE=1, EDEN_NX2_CLOCK_TRACE=1), without the GPU thread zones:
rem gpu_back_cpu_ms_per_frame and the frame times as unprofiled play pays them, and every 10 s what
rem the game's heap maps and unmaps cost and how fast the threads' cores ran. The baseline for the
rem unprofiled A/B launchers (ab-26 to ab-41), which log the same; for option 3's arms, run it in
rem every block as README-tuned.txt says.
set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
