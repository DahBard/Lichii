@echo off
rem ab-9-amd-fp16.bat with the logging of perf-trace.bat: float16 shader math stays on the
rem AMD driver (EDEN_AMD_FP16=1). Look for wrong colors.
set EDEN_AMD_FP16=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
