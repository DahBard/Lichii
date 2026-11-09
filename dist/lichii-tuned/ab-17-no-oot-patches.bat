@echo off
rem Ocarina's round 18 patches from upstream #717 off (host zstd, faster string compares, the
rem resource-poll backoff, wider thread core masks), with the perf-trace logging. The patches can
rem also be unchecked one by one in the game's Add-Ons.
set EDEN_NX2_OOT_WIDE_AFFINITY=0&& set EDEN_NX2_OOT_NATIVE_ZSTD=0&& set EDEN_NX2_OOT_FAST_LOOKUP=0&& set EDEN_NX2_OOT_POLL_BACKOFF=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
