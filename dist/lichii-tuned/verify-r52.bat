@echo off
rem Checks r52 against the slow ways while you play, with the perf-trace logging. Slower: not
rem for frame rates. Every 10 s or second, these must stay 0: 'Native bank check (mode 2)'
rem missed and bitsets missed; 'Read download check' bytes that differ (1 in 16 read
rem downloads); 'Fast draw verify per second' mismatches; the delta descriptor and uniform
rem keep checks' mismatches; and no 'Nx2Perf' errors. The 'Instance fold per 2 s' and 'reports
rem and fences' lines show the new paths at work.
set LICHII_BANK_CHECK=2&& set EDEN_NX2_READ_CHECK=16&& set EDEN_NX2_FAST_DRAW_VERIFY=1&& set EDEN_NX2_DELTA_DESCRIPTORS_VERIFY=1&& set EDEN_NX2_UBO_KEEP_VERIFY=1&& set EDEN_NX2_PERF_VERIFY=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
