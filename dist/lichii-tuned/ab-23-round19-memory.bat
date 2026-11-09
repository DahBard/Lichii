@echo off
rem Round 19's guest memory paths, with the perf-trace logging: guest reads and writes take the
rem caches' locks (Fritchii EXP35 off, EXP36 on), the uniform bank check compares once a frame again
rem (LICHII_BANK_CHECK=1), #717's EXP18/EXP29 trims are off, invalidations cover whole 32-byte
rem blocks (#700) and the guest memory section is committed at boot. Compare the guest cores' times.
set EDEN_NX2_EXP35=0&& set EDEN_NX2_EXP36=1&& set LICHII_BANK_CHECK=1&& set EDEN_NX2_EXP18=0&& set EDEN_NX2_EXP29=0&& set EDEN_NX2_INVALIDATE_BLOCKS=1&& set EDEN_NX2_SEC_RESERVE=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
