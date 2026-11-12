@echo off
rem The emulated CPU's counter read through a host call, as before round 7
rem (LICHII_JIT_INLINE_CNTPCT=0), with the logging of perf-trace.bat. Compare with
rem perf-trace.bat. Round 7's barriers are upstream's since nx2 #1327 and always on.
set LICHII_JIT_INLINE_CNTPCT=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
