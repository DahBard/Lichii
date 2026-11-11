@echo off
rem The texture collector deletes at most 40 images a frame past the critical memory, as before
rem upstream #1306 (EDEN_NX2_TEST_1306=0). Expect the Hyrule Castle pot crash to come back: run
rem it only to compare, not to play there.
set EDEN_NX2_TEST_1306=0&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
