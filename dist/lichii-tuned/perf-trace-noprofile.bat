@echo off
rem The default settings with only the frame trace (EDEN_DKB_FRAME_TRACE=1), without the GPU
rem thread zones: gpu_back_cpu_ms_per_frame and the frame times as unprofiled play pays them.
rem The baseline for the r51 A/B launchers (ab-26, ab-27, ab-28). Every 10 s it also logs what
rem the game's heap maps and unmaps cost (EDEN_NX2_HEAP_TRACE=1), as r55's ab-26, ab-36 and
rem ab-37 do.
set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1
start "" "%~dp0lichii.exe" %*
