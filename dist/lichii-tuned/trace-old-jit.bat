@echo off
rem The emulated CPU as before round 7: memory barriers as fences and the counter read
rem through a host call (LICHII_JIT_TSO=0, LICHII_JIT_INLINE_CNTPCT=0), with the logging of
rem perf-trace.bat. Compare with perf-trace.bat.
set LICHII_JIT_TSO=0&& set LICHII_JIT_INLINE_CNTPCT=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
