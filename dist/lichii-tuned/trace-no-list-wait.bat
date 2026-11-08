@echo off
rem Experiment: the GPU thread reads the next commands without waiting for queued compute work
rem that could write them (LICHII_LIST_WRITER_WAIT=0), logged like perf-trace.bat. Look for
rem flicker, missing or broken objects and crashes; the log says if the game ever writes
rem commands on the GPU (then the waits come back).
set LICHII_LIST_WRITER_WAIT=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
