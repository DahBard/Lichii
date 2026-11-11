@echo off
rem Fast draws end as in r50 (EDEN_NX2_TEST_1216=0): a vertex stream's stride word, the render
rem enable addresses and the back macros' calls end the delta draw chain again (upstream #1216
rem keeps it). Unprofiled, with the frame, heap and clock traces: compare gpu_back_cpu_ms_per_frame
rem and the frame times with perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_TEST_1216=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
