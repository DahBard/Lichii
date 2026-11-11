@echo off
rem Fast draws as in r51 (EDEN_NX2_TEST_1161_DRAWS=0): a full delta list ends the delta draw
rem chain again (delta overflow), handle checks cover the whole constant buffer and vertex
rem attribute format writes end it (upstream #1161's fast-draw half off).
rem Unprofiled, with the frame and heap traces: compare gpu_back_cpu_ms_per_frame and the frame
rem times with perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_TEST_1161_DRAWS=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1
start "" "%~dp0lichii.exe" %*
