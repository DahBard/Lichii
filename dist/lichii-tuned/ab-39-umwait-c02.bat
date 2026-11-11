@echo off
rem Waiting cores idle in UMWAIT's deeper state, C0.2, instead of C0.1 (LICHII_UMWAIT_C02=1): the
rem emulated CPU cores' idle polls (about 745 ms a second together) and the GPU threads' waits for
rem each other. C0.2 saves more power and wakes a little slower; on the power-limited laptop the
rem saved power may raise the busy cores' clock. If Windows does not allow C0.2, the cores wait in
rem C0.1 as before and nothing changes. Unprofiled, with the frame, heap and clock traces: compare
rem gpu_back_cpu_ms_per_frame, the frame times and the clocks with perf-trace-noprofile.bat over
rem the same spots.
set LICHII_UMWAIT_C02=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
