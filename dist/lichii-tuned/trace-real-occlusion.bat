@echo off
rem Runs the GPU occlusion queries Ocarina of Time reports, as before round 10
rem (LICHII_FAKE_OCCLUSION=0), with the logging of perf-trace.bat. Compare with perf-trace.bat.
set LICHII_FAKE_OCCLUSION=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
