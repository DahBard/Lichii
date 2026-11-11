@echo off
rem No zero-page passes (EDEN_NX2_ZERO_TRIM=0), with the perf-trace logging: guest pages that hold
rem only zeros stay in RAM (about 7 GB more in Ocarina). The default logs every slow pass as a
rem Zero-page pass line: compare the frame times around those passes and the 1 percent lows over
rem the same route with perf-trace.bat, which keeps the passes.
set EDEN_NX2_ZERO_TRIM=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
