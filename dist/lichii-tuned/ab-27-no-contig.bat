@echo off
rem Storage bindings in one device range go through the virtual range cache again, as in r50
rem (EDEN_NX2_TEST_CONTIG=0; #717's check off). Unprofiled, with the frame and heap traces:
rem compare gpu_back_cpu_ms_per_frame and the frame times with perf-trace-noprofile.bat over the
rem same spots.
set EDEN_NX2_TEST_CONTIG=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1
start "" "%~dp0lichii.exe" %*
