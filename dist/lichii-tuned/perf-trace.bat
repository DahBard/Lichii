@echo off
rem The default settings plus logging; the log is %APPDATA%\lichii\log\eden_log.txt. Every second:
rem frame timing and each frame's time (for the 1% lows). Every 10 s: GPU thread zones, what each
rem emulated CPU core spent its time on, and why macro reads and semaphore waits waited
rem (EDEN_NX2_GPU_PROFILE=1, EDEN_DKB_FRAME_TRACE=1, EDEN_NX2_MME_DMA_STATS=1).
set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
