@echo off
rem The split GPU thread's front and back each on a P-core of their own, and the six emulated CPU
rem cores on the other four P-cores instead of spread over P- and E-cores
rem (EDEN_NX2_PIN_GPU_THREADS=1: the "Pin GPU threads to their own cores" setting, for this run
rem only; the setting itself is not changed). The setting is off by default; if you turned it on,
rem this arm equals the baseline, and the log's "TEST switches in effect" line says
rem "pin_gpu_threads on (setting)" in both. Unprofiled, with the frame, heap and clock traces:
rem compare gpu_back_cpu_ms_per_frame, the frame times and the back thread's ghz in the "Clock
rem trace per second" lines with perf-trace-noprofile.bat over the same spots.
set EDEN_NX2_PIN_GPU_THREADS=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
