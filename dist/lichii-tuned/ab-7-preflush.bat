@echo off
rem Preflush GPU readbacks (fewer readback stalls); a page stays preflushed for 120 frames.
set EDEN_NX2_PREFLUSH=1
start "" "%~dp0lichii.exe" %*
