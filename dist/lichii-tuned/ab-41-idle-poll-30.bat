@echo off
rem Only if ab-40 helps: an idle emulated CPU core polls 30 microseconds instead of 100 before it
rem sleeps (EDEN_NX2_IDLE_SPIN_US=30), ab-40's shorter poll alone.
rem Unprofiled, with the frame, heap and clock traces: compare with perf-trace-noprofile.bat as
rem README-tuned.txt's option 3 says.
set EDEN_NX2_IDLE_SPIN_US=30&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
