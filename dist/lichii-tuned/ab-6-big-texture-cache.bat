@echo off
rem Texture cache keeps up to 9 GiB before evicting (default 5 GiB).
set EDEN_NX2_TC_CRITICAL_MIB=9216&& set EDEN_NX2_TC_EXPECTED_MIB=8448
start "" "%~dp0lichii.exe" %*
