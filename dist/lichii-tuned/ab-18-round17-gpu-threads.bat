@echo off
rem Round 18's GPU thread work from upstream #805 off, as in round 17, with the perf-trace logging:
rem semaphore acquires and macro reads decided by the front's old rules (drains, shadow and mixed
rem reads), macros run on the front, command lists wait for all queued compute and copy work, compute
rem list words read back, one command per upload. If a glitch, a hang or a frame-rate change goes away
rem here, #805 caused it.
set LICHII_TUNED4_ACQUIRES=1&& set LICHII_TUNED4_DMA_READS=1&& set EDEN_NX2_BACK_MACROS=0&& set LICHII_LIST_WRITER_WAIT=all&& set EDEN_NX2_LIST_INDIRECT=0&& set LICHII_UPLOAD_BATCH=0&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_MME_DMA_STATS=1
start "" "%~dp0lichii.exe" %*
