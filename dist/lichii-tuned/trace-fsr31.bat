@echo off
rem AMD FSR 3.1 instead of FSR 4 for Ocarina's upscaling to 4K (LICHII_FSR4=0), logged like
rem perf-trace.bat: to compare picture and speed with FSR 4.
set LICHII_FSR4=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
