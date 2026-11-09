@echo off
rem Round 18's emulated CPU code generation from upstream #717 off (the JIT as in round 17), with
rem the perf-trace logging. The memory, kernel and Ocarina patch changes stay on.
set EDEN_NX2_JIT_FLAGS=0&& set EDEN_NX2_JIT_DISPATCH=0&& set EDEN_NX2_JIT_COMPACT_EXITS=0&& set EDEN_NX2_JIT_FAR_CODE=0&& set EDEN_NX2_JIT_FOLD_ADDR=0&& set EDEN_NX2_JIT_ZEXT=0&& set EDEN_NX2_JIT_SPILL_GPR=0&& set EDEN_NX2_JIT_SCRATCH_COPY=0&& set EDEN_NX2_JIT_NO_FRAME=0&& set EDEN_NX2_JIT_BACKEDGE_HALT=0&& set EDEN_NX2_JIT_RSB32=0&& set EDEN_NX2_HINT_PAUSE=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
