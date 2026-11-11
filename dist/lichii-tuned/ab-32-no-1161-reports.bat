@echo off
rem Reports and fences as in r51 where they can be switched (EDEN_NX2_TEST_1161_REPORTS=0): a new
rem fence operation list per fence, the buffer cache lock for every pending report and the query
rem pool reset on the GPU thread. Unprofiled, with the frame, heap and clock traces: compare
rem gpu_back_cpu_ms_per_frame and the frame times with perf-trace-noprofile.bat over the same
rem spots.
set EDEN_NX2_TEST_1161_REPORTS=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
