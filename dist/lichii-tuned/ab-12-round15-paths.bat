@echo off
rem Round 16's new shortcuts off, as in round 15, with the perf-trace logging: native bank pages
rem compared on every bind, texture uploads as before, storage-pointer texture handles bound,
rem macro reads that overlap queued writes drained, and pipelines keyed by every vertex
rem attribute type. If a glitch or a frame-rate change goes away here, one of them caused it.
set LICHII_BANK_CHECK=0&& set LICHII_TEXTURE_UPLOADS=0&& set LICHII_NULL_POINTER_HANDLES=0&& set LICHII_DMA_MIXED_READS=0&& set LICHII_KEY_ALL_ATTRIBUTES=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
