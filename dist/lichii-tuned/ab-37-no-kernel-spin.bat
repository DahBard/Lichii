@echo off
rem ab-20 without the profile: the emulated kernel's locks and address waits sleep at once
rem instead of spinning first (EDEN_NX2_KSPIN=0, EDEN_NX2_ARB_SPIN=0). On the power-limited
rem laptop the spinning may cost more clock speed than it saves. Unprofiled, with the frame and
rem heap traces: compare gpu_back_cpu_ms_per_frame and the frame times with
rem perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_KSPIN=0&& set EDEN_NX2_ARB_SPIN=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1
start "" "%~dp0lichii.exe" %*
