@echo off
rem Occlusion reports read 4096 samples at once and no GPU occlusion query runs
rem (LICHII_FAKE_OCCLUSION=4096): the game thinks everything is visible. Logged like
rem perf-trace.bat. Look for lens flares or glows showing through walls and trees.
set LICHII_FAKE_OCCLUSION=4096&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
