@echo off
rem Dynamic FPS keeps real speed only down to 20 fps, as in official builds
rem (LICHII_DYNFPS_MIN_FPS=20; this build: 10 fps), with the logging of perf-trace.bat.
rem Below 20 fps the game slows down.
set LICHII_DYNFPS_MIN_FPS=20&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
