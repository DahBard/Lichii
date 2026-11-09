@echo off
rem Checks round 20's Fritchii shortcuts against the slow way while you play, with the perf-trace
rem logging: worker translations are compared with the GPU thread's ('Async translate verify: N
rem compared, M differ', M should stay 0), relocated render targets leave tombstones whose hit
rem count should stay 0, and the bank check compares every frame ('missed' should stay 0). Slower.
set EDEN_NX2_ASYNC_TRANSLATE=2&& set EDEN_NX2_EXP43=4&& set LICHII_BANK_CHECK=2&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
