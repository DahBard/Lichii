@echo off
rem Logs GPU thread zones every 10 s and frame timing every second (EDEN_NX2_GPU_PROFILE=1,
rem EDEN_DKB_FRAME_TRACE=1); the log is %APPDATA%\eden\log\eden_log.txt.
set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
