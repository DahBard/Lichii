@echo off
rem Without preflushing GPU readbacks (EDEN_NX2_PREFLUSH=0; on by default for Ocarina since
rem round 15), logged like perf-trace.bat: more readback stalls, for comparison.
set EDEN_NX2_PREFLUSH=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
