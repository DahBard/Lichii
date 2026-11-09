@echo off
rem Round 19's render targets, with the perf-trace logging: no render target relocation (Fritchii
rem EXP43) and no cold sweep of chain images. The render-frame clock stays on: the default
rem asynchronous shaders count their clears by it. Compare the frame rate in Castle Town and any
rem garbled or flickering image with the default.
set EDEN_NX2_EXP43=0&& set EDEN_NX2_TC_COLD_FRAMES=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
