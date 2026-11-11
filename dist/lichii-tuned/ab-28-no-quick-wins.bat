@echo off
rem r51's switchable quick wins off (EDEN_NX2_TEST_QUICK=0): a flush again after each queued fence,
rem the device memory usage asked of the driver on every call, and InvalidateGPUCache gathering
rem even with nothing collected. Unprofiled, with the frame, heap and clock traces: compare
rem gpu_back_cpu_ms_per_frame and the frame times with perf-trace-noprofile.bat.
set EDEN_NX2_TEST_QUICK=0&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
