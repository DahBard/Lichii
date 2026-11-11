@echo off
rem Every later instance of an instance loop is sent to the GPU back thread as its own write again
rem (EDEN_NX2_FOLD_INSTANCES=0, upstream #1274's switch). Unprofiled, with the frame, heap and
rem clock traces: compare gpu_back_cpu_ms_per_frame and the frame times with
rem perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_FOLD_INSTANCES=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
