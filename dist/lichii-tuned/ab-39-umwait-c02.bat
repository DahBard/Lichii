@echo off
rem Waiting cores idle in UMWAIT's deeper state, C0.2, instead of C0.1 (LICHII_UMWAIT_C02=1,
rem TEST): the emulated CPU cores' idle polls (together about 745 ms a second) and the GPU
rem threads' waits for each other. An idle emulated core's poll then waits in one UMWAIT instead
rem of 4-microsecond slices, which may be too short to reach C0.2; the GPU threads' waits keep
rem their 4-microsecond slices. C0.2 saves more power and wakes a little slower; on the
rem power-limited laptop the saved power may raise the busy cores' clock. If this arm matches the
rem baseline, C0.2 had no measurable effect as built, which does not make it useless: Windows may
rem keep C0.2 off, and the log cannot show that. Signs that it acted: lower package power, or
rem higher P-core ghz, at the same frame rate.
rem Unprofiled, with the frame, heap and clock traces: compare with perf-trace-noprofile.bat as
rem README-tuned.txt's option 3 says.
set LICHII_UMWAIT_C02=1&& set EDEN_DKB_FRAME_TRACE=1&& set EDEN_NX2_HEAP_TRACE=1&& set EDEN_NX2_CLOCK_TRACE=1
start "" "%~dp0lichii.exe" %*
