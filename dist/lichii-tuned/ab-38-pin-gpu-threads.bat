@echo off
rem The GPU thread's front and back each on a P-core of their own (EDEN_NX2_PIN_GPU_THREADS=2,
rem TEST): the emulated CPU cores, presentation, fencing and VSync keep the other P- and E-cores,
rem the Vulkan worker the other P-cores. Lower-priority work, such as pipeline builds, texture
rem decoding, shader translation and audio, and the driver's threads can still run on those two
rem cores while the GPU threads sleep. The "Pin GPU threads to their own cores" setting is not
rem this arm: it puts the six emulated CPU cores on the four P-cores left. Keep the setting off
rem (the default) for every run, or the baseline pins too: its "TEST switches in effect" line
rem would say "pin_gpu_threads on (setting, applied)". This arm's says "pin_gpu_threads GPU
rem threads only (EDEN_NX2_PIN_GPU_THREADS, applied)", after a "GPU thread pinning" line;
rem "skipped" means the CPU has too few P-cores and nothing was pinned. The "Core placement"
rem lines give each thread's cores before this arm narrows them.
rem Unprofiled, with the frame, heap and clock traces: compare with perf-trace-noprofile.bat as
rem README-tuned.txt's option 3 says.
set EDEN_NX2_PIN_GPU_THREADS=2&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
