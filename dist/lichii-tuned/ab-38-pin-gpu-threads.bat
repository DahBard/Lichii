@echo off
rem The split GPU thread's front and back each on a P-core of their own, and every other thread
rem the emulator places kept off those two: the emulated CPU cores and presentation keep the other
rem P- and E-cores, the Vulkan worker the other P-cores (EDEN_NX2_PIN_GPU_THREADS=2, TEST). The
rem "Pin GPU threads to their own cores" setting is not this arm: it puts the six emulated CPU
rem cores on the four P-cores left. Keep the setting off (the default) for every run, or the
rem baseline pins too: its "TEST switches in effect" line would say "pin_gpu_threads on
rem (setting, applied)". This arm's says "pin_gpu_threads GPU threads only
rem (EDEN_NX2_PIN_GPU_THREADS, applied)", after a "GPU thread pinning" line; "skipped" means
rem the CPU has too few P-cores and nothing was pinned.
rem Unprofiled, with the frame, heap and clock traces: compare with perf-trace-noprofile.bat as
rem README-tuned.txt's option 3 says.
set EDEN_NX2_PIN_GPU_THREADS=2&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
