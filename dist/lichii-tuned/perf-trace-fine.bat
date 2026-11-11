@echo off
rem perf-trace.bat with the fine zones and counters as well (EDEN_NX2_GPU_PROFILE=2): the parts
rem of method and draw (constant buffer binds and uploads, reports, draw setup and queries),
rem stream copies, the uploads and write marks in compute synchronize, and how uniform buffers
rem are bound. More time stamp reads make it slower than perf-trace.bat: it shows where the
rem time goes, not the frame rate.
set EDEN_NX2_GPU_PROFILE=2&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1&& set EDEN_NX2_HEAP_TRACE=1
start "" "%~dp0lichii.exe" %*
