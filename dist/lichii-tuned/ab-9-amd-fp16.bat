@echo off
rem Keeps float16 shader math on the AMD driver (twice the float32 rate on RDNA). Official
rem builds turn it off because of a driver bug in another game: look for wrong colors.
set EDEN_AMD_FP16=1
start "" "%~dp0lichii.exe" %*
