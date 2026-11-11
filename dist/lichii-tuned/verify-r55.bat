@echo off
rem Checks r55 against the slow ways while you play, with the perf-trace logging. Slower: not
rem for frame rates. Every 10 s or second, these must stay 0: 'Native bank check (mode 2)'
rem missed and bitsets missed; 'Read download check' bytes that differ (1 in 16 read
rem downloads); the verify mismatches in 'Delta descriptors' and 'Uniform keeps'; and no
rem 'Nx2Perf' errors. Unlike verify-r52 it leaves EDEN_NX2_FAST_DRAW_VERIFY out, which resolves
rem every delta draw in full: here the delta draws run, r55's new ones too (shaders that pick
rem their textures from memory), so the descriptor and uniform keep checks have them to check.
set LICHII_BANK_CHECK=2&& set EDEN_NX2_READ_CHECK=16&& set EDEN_NX2_DELTA_DESCRIPTORS_VERIFY=1&& set EDEN_NX2_UBO_KEEP_VERIFY=1&& set EDEN_NX2_PERF_VERIFY=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
