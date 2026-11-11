@echo off
rem Less spinning all at once: the emulated kernel's locks and address waits sleep at once
rem (EDEN_NX2_KSPIN=0, EDEN_NX2_ARB_SPIN=0, as ab-37), waits idle in C0.2 (LICHII_UMWAIT_C02=1, as
rem ab-39), and an idle emulated core polls 30 microseconds instead of 100 before it sleeps
rem (EDEN_NX2_IDLE_SPIN_US=30; shortened, not removed: in another game frames took 42 ms without
rem the poll and 34-35 ms with 50-100). If this arm helps but neither ab-37 nor ab-39 does, the
rem shorter poll or the parts together did it: run ab-41, the shorter poll alone, next.
rem Unprofiled, with the frame, heap and clock traces: compare with perf-trace-noprofile.bat as
rem README-tuned.txt's option 3 says.
set EDEN_NX2_KSPIN=0&& set EDEN_NX2_ARB_SPIN=0&& set LICHII_UMWAIT_C02=1&& set EDEN_NX2_IDLE_SPIN_US=30&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
