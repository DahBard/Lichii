@echo off
rem FSR's reactive mask at full strength on jittered frames, as in round 18 (LICHII_FSR_REACTIVE=1),
rem with the perf-trace logging. Round 19 halves it, which should calm the textures where water
rem meets the screen's edges: if they shimmer here and not in the default, that was the cause.
set LICHII_FSR_REACTIVE=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
