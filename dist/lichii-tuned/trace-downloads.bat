@echo off
rem trace-preflush.bat plus a trace of the GPU readbacks (EDEN_NX2_DOWNLOAD_TRACE=1): every
rem 10 s the log lists the GPU writes that are copied back the most and the first CPU reads
rem of GPU-written memory, to see what the game reads.
set EDEN_NX2_PREFLUSH=1&& set EDEN_NX2_DOWNLOAD_TRACE=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
