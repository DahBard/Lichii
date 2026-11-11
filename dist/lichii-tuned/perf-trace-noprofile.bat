@echo off
rem The default settings with only the frame trace (EDEN_DKB_FRAME_TRACE=1), without the GPU
rem thread zones: gpu_back_cpu_ms_per_frame and the frame times as unprofiled play pays them.
rem The baseline for the r51 A/B launchers (ab-26, ab-27, ab-28).
set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
