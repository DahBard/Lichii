@echo off
rem Fritchii's host frame pacer, with the perf-trace logging: each frame is held until just before
rem the display refresh it belongs to and the game's frame clock follows the display (genlock).
rem Needs asynchronous presentation (on by default). Every 10 s a "Host pacing on:" line says how
rem many refreshes frames stayed on screen; compare with trace-display.bat (LICHII_HOST_PACE=1).
set LICHII_HOST_PACE=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
