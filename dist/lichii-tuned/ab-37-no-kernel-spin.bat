@echo off
rem ab-20 without the profile: the emulated kernel's locks and address waits sleep at once instead
rem of spinning first (EDEN_NX2_KSPIN=0, EDEN_NX2_ARB_SPIN=0). On the power-limited laptop the
rem spinning may cost more clock speed than it saves.
rem Unprofiled, with the frame, heap and clock traces: compare with perf-trace-noprofile.bat as
rem README-tuned.txt's option 3 says.
set EDEN_NX2_KSPIN=0&& set EDEN_NX2_ARB_SPIN=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
