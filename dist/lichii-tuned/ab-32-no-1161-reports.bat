@echo off
rem Reports and fences as in r51 where they can be switched (EDEN_NX2_TEST_1161_REPORTS=0): a new
rem fence operation list per fence, the buffer cache lock for every pending report and the query
rem pool reset on the GPU thread.
rem Unprofiled, with the frame trace: compare gpu_back_cpu_ms_per_frame and the frame
rem times with perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_TEST_1161_REPORTS=0&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
