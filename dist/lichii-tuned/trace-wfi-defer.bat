@echo off
rem Waits for idle inside a render pass keep it (LICHII_WFI_DEFER=1): later draws break it only
rem when they read what it wrote, and the full barrier follows its end. Logged like
rem perf-trace.bat. Look for missing or flickering effects.
set LICHII_WFI_DEFER=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
