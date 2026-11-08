@echo off
rem Ocarina's scene drawn without the camera jitter round 18 adds for FSR 4 (LICHII_FSR_JITTER=0), with
rem the perf-trace logging: compare edges and fine detail with the default.
set LICHII_FSR_JITTER=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
