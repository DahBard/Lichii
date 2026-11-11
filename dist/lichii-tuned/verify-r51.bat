@echo off
rem Checks r51's fast draws and mappings against the slow way while you play, with the
rem perf-trace logging: delta draws are resolved in full and compared ('Fast draw verify per
rem second' mismatches should stay 0), the first 64 calls of each back macro also run the old way
rem (in 'Split back macros', those 'with other writes' should stay 0), and the mapping runs and
rem caches are checked (no 'Nx2Perf mapping runs' errors). Slower: not for frame rates.
set EDEN_NX2_FAST_DRAW_VERIFY=1&& set EDEN_NX2_BACK_MACRO_VERIFY=64&& set EDEN_NX2_PERF_VERIFY=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
