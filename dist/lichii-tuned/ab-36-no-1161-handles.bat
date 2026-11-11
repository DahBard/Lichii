@echo off
rem Pipelines whose texture handles a handle program computes at draw time take no delta draws, as
rem in r54 (EDEN_NX2_TEST_1161_HANDLES=0, upstream #1161's e05a72847 off). Unprofiled, with the
rem frame, heap and clock traces: compare gpu_back_cpu_ms_per_frame and the frame times with
rem perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_TEST_1161_HANDLES=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
