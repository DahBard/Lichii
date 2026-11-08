@echo off
rem AMD FSR 3.1 in place of Ocarina of Time's own anti-aliasing pass, logged like perf-trace.bat
rem (EDEN_NX2_DLSS_REPLACEMENT=fsr, the same as the "Use AMD FSR 3.1 upscaling in Ocarina of Time"
rem setting). While FSR runs, "No sharpening" is off. The log says whether FSR replaced the pass.
set EDEN_NX2_DLSS_REPLACEMENT=fsr&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
