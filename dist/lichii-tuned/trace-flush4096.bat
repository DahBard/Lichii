@echo off
rem Submits GPU work every 4096 draws instead of every 128 (EDEN_NX2_FLUSH_DRAWS=4096),
rem with the logging of perf-trace.bat: fewer, larger submissions to the GPU.
set EDEN_NX2_FLUSH_DRAWS=4096&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
