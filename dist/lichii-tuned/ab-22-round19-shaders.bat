@echo off
rem Round 19's shader handling, with the perf-trace logging: new pipelines are translated on the
rem GPU thread instead of Fritchii's worker threads, every draw waits for its new pipeline instead
rem of skipping ones into targets redrawn each frame. Compare hitches when new effects appear and
rem missing objects with the default.
set EDEN_NX2_ASYNC_TRANSLATE=0&& set EDEN_NX2_ASYNC_DISCARD=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
