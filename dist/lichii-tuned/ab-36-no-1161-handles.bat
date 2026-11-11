@echo off
rem Pipelines whose texture handles a handle program computes at draw time take no delta draws,
rem as in r54 (EDEN_NX2_TEST_1161_HANDLES=0, upstream #1161's e05a72847 off).
rem Unprofiled, with the frame trace: compare gpu_back_cpu_ms_per_frame and the frame
rem times with perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_TEST_1161_HANDLES=0&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
