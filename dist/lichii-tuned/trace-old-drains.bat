@echo off
rem The split GPU thread waits as in r18, logged like perf-trace.bat: every poll of a waiting
rem semaphore drains the back thread (LICHII_ACQUIRE_POLL=0), and every macro read of a page
rem with occlusion reports drains it too (LICHII_REPORT_DRAIN=all). Compare with perf-trace.bat.
set LICHII_ACQUIRE_POLL=0&& set LICHII_REPORT_DRAIN=all&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
