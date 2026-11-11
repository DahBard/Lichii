@echo off
rem Storage bindings in one device range go through the virtual range cache again, as in r50
rem (EDEN_NX2_TEST_CONTIG=0; #717's check off). Unprofiled, with the frame trace: compare
rem gpu_back_cpu_ms_per_frame and the frame times with perf-trace-noprofile.bat over the same
rem spots.
set EDEN_NX2_TEST_CONTIG=0&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
