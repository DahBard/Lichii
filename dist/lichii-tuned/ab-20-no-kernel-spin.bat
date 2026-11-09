@echo off
rem The emulated kernel's locks and address waits sleep at once instead of spinning first (round 18,
rem upstream #717: up to 20 us for locks, 8 us for waits), with the perf-trace logging. On a
rem power-limited laptop the spinning may cost more clock speed than it saves: compare the
rem frame rate and the guest cores' times with the default.
set EDEN_NX2_KSPIN=0&& set EDEN_NX2_ARB_SPIN=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
