@echo off
rem Round 19's render targets, with the perf-trace logging: no render target relocation (Fritchii
rem EXP43), no render-frame clock and no cold sweep of chain images. Compare the frame rate in
rem Castle Town and any garbled or flickering image with the default.
set EDEN_NX2_EXP43=0&& set EDEN_NX2_RENDER_CLOCK=0&& set EDEN_NX2_TC_COLD_FRAMES=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
