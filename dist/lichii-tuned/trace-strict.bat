@echo off
rem ab-4 with the logging of perf-trace.bat: emulated CPU cores on P-cores too, pipeline
rem builds on E-cores only (LICHII_CPU_POLICY=strict). The CPU side limits the frame rate
rem since round 10, so P-core placement may help now.
set LICHII_CPU_POLICY=strict&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
