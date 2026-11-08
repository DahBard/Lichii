@echo off
rem The default settings with only each frame's time logged (LICHII_FRAME_TIMES=1): the 1% lows
rem without the other logging slowing the frames down. Every 10 s a "Frame times:" line.
set LICHII_FRAME_TIMES=1
start "" "%~dp0lichii.exe" %*
