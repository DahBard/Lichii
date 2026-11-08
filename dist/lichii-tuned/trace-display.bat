@echo off
rem The host frame pacer measuring only, with the perf-trace logging: frames are presented as
rem usual, and every 10 s a "Host pacing off:" line says how many display refreshes they stayed
rem on screen, and the log names the display's refresh rate. The baseline for trace-host-pace.bat
rem (LICHII_HOST_PACE=2).
set LICHII_HOST_PACE=2&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
