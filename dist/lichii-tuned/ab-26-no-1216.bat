@echo off
rem Fast draws end as in r50 (EDEN_NX2_TEST_1216=0): a vertex stream's stride word, the render
rem enable addresses and the back macros' calls end the delta draw chain again (upstream #1216
rem keeps it). Unprofiled, with the frame trace: compare gpu_back_cpu_ms_per_frame and the frame
rem times with perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_TEST_1216=0&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
