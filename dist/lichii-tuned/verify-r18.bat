@echo off
rem Checks round 18's GPU thread shortcuts from upstream #805 against the slow way while you play, with
rem the perf-trace logging: acquires and command lists are also decided after a full drain, back-run
rem macros and macro reads are compared, and compute list words are read back and compared. In the
rem log, the "not met after the drain", "differing" and mismatch counts should stay 0. Slower.
set EDEN_NX2_DRAIN_CHECK=1&& set EDEN_NX2_DMA_READ_VERIFY=1&& set EDEN_NX2_BACK_MACRO_VERIFY=64&& set EDEN_NX2_LIST_VERIFY=1&& set EDEN_NX2_LIST_INDIRECT_CHECK=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
