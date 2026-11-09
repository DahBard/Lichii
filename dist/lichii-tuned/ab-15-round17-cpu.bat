@echo off
rem Round 18's CPU work from upstream #717 all off, as in round 17, with the perf-trace logging: the
rem emulated CPU's code generation, the memory trap fast paths, the kernel locks and waits, and
rem Ocarina's zstd, string and resource-poll patches. If a crash, a glitch or a frame-rate change
rem goes away here, one of them caused it; ab-16 and ab-17 narrow it down.
set EDEN_NX2_JIT_FLAGS=0&& set EDEN_NX2_JIT_DISPATCH=0&& set EDEN_NX2_JIT_COMPACT_EXITS=0&& set EDEN_NX2_JIT_FAR_CODE=0&& set EDEN_NX2_JIT_FOLD_ADDR=0&& set EDEN_NX2_JIT_ZEXT=0&& set EDEN_NX2_JIT_SPILL_GPR=0&& set EDEN_NX2_JIT_SCRATCH_COPY=0&& set EDEN_NX2_JIT_NO_FRAME=0&& set EDEN_NX2_JIT_BACKEDGE_HALT=0&& set EDEN_NX2_JIT_RSB32=0&& set EDEN_NX2_HINT_PAUSE=0&& set EDEN_NX2_TRAP_FASTPATH=0&& set EDEN_NX2_SYS_SLOT=0&& set EDEN_NX2_KSPIN=0&& set EDEN_NX2_ARB_SPIN=0&& set EDEN_NX2_CORE_LOCK=0&& set EDEN_NX2_IDLE_PAUSE=0&& set EDEN_NX2_CONTEXT_GUARD=0&& set EDEN_NX2_FAST_TLS=0&& set EDEN_NX2_OFFSETOF_CACHE=0&& set EDEN_NX2_CACHE_LOCK_WAIT=0&& set EDEN_NX2_OOT_WIDE_AFFINITY=0&& set EDEN_NX2_OOT_NATIVE_ZSTD=0&& set EDEN_NX2_OOT_FAST_LOOKUP=0&& set EDEN_NX2_OOT_POLL_BACKOFF=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
