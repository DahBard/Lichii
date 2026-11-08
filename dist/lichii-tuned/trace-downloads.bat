@echo off
rem perf-trace.bat plus a trace of the GPU readbacks (EDEN_NX2_DOWNLOAD_TRACE=1): every 10 s the
rem log lists the GPU writes that are copied back the most and the first CPU reads of
rem GPU-written memory, to see what the game reads.
set EDEN_NX2_DOWNLOAD_TRACE=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
