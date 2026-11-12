@echo off
rem verify-r55 with three ports off: #1161's delta draws for shaders that pick their textures from
rem memory (EDEN_NX2_TEST_1161_HANDLES=0), #1216 (EDEN_NX2_TEST_1216=0) and #1161's other repeat
rem draw changes (EDEN_NX2_TEST_1161_DRAWS=0), with the same checks and logging. Slower: not for
rem frame rates.
rem It shows whether the uniform keep mismatches depend on those three ports: compare the
rem 'Uniform keeps' lines' mismatches per verified keep, by kind, with verify-r55 over the same
rem route (about 2-3 per 100,000 there). At that rate they don't; if they vanish, they do.
set EDEN_NX2_TEST_1161_HANDLES=0&& set EDEN_NX2_TEST_1216=0&& set EDEN_NX2_TEST_1161_DRAWS=0&& set LICHII_BANK_CHECK=2&& set EDEN_NX2_READ_CHECK=16&& set EDEN_NX2_DELTA_DESCRIPTORS_VERIFY=1&& set EDEN_NX2_UBO_KEEP_VERIFY=1&& set EDEN_NX2_PERF_VERIFY=1&& set EDEN_NX2_GPU_PROFILE=1&& set EDEN_DKB_FRAME_TRACE=1
start "" "%~dp0lichii.exe" %*
