@echo off
rem Ocarina's own upscale pass instead of AMD FSR (EDEN_NX2_DLSS_REPLACEMENT=native): no 4K
rem output, logged like perf-trace.bat, to see what FSR costs.
set EDEN_NX2_DLSS_REPLACEMENT=native&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
