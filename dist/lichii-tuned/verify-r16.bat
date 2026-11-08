@echo off
rem Checks round 16's two new shortcuts against the old way while you play, with logging. Native
rem bank pages are compared on every bind and the log counts what the cheaper check would have
rem missed (LICHII_BANK_CHECK=2, "Native bank check" lines, "missed" should stay 0), and macro
rem reads made from the shadow and memory are compared with a drained read
rem (LICHII_DMA_MIXED_READS=2, "Mixed MME DMA reads (verify)" lines, "differed" should stay 0).
rem Slower than the default.
set LICHII_BANK_CHECK=2&& set LICHII_DMA_MIXED_READS=2&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
