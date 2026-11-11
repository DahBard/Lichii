@echo off
rem The native bank check walks every page with a snapshot lookup each, as in r51
rem (EDEN_NX2_TEST_BANK_BITS=0).
rem Unprofiled, with the frame trace: compare gpu_back_cpu_ms_per_frame and the frame
rem times with perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_TEST_BANK_BITS=0&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
