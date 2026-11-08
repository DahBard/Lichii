@echo off
rem Round 17's in-place texture reads off, as in round 16, with the perf-trace logging: each run of
rem guest texture memory is copied out before it is untiled. If a texture glitch goes away here,
rem the in-place reads caused it.
set EDEN_NX2_UNSWIZZLE_COPY=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
