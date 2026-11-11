@echo off
rem The native bank check walks every page with a snapshot lookup each, as in r51
rem (EDEN_NX2_TEST_BANK_BITS=0). Unprofiled, with the frame, heap and clock traces: compare
rem gpu_back_cpu_ms_per_frame and the frame times with perf-trace-noprofile.bat over the same
rem spots.
set EDEN_NX2_TEST_BANK_BITS=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
