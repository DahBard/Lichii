@echo off
rem ab-7-preflush.bat with the logging of perf-trace.bat: GPU readbacks are preflushed
rem (EDEN_NX2_PREFLUSH=1), fewer readback stalls. A page stays preflushed for 120 frames
rem (longer if the game reads it again; LICHII_PREFLUSH_FRAMES=0 keeps it for good, as in r18).
set EDEN_NX2_PREFLUSH=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
