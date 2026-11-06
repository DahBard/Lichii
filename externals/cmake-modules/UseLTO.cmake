# SPDX-FileCopyrightText: Copyright 2025 crueter
# SPDX-License-Identifier: LGPL-3.0-or-later

## UseLTO ##

# Enable Interprocedural Optimization (IPO).
# Self-explanatory.

option(ENABLE_LTO "Enable Link-Time Optimization (LTO)" OFF)

if (ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT COMPILER_SUPPORTS_LTO OUTPUT LTO_CHECK_OUTPUT)
    if (COMPILER_SUPPORTS_LTO)
        set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${COMPILER_SUPPORTS_LTO})
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        # MSYS2's clang fails CMake's IPO check, while clang and lld do ThinLTO fine: pass the
        # flags directly (archives then need llvm-ar).
        message(WARNING "check_ipo_supported failed, using -flto=thin directly:\n${LTO_CHECK_OUTPUT}")
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-flto=thin>)
        add_link_options(-flto=thin)
    else()
        message(FATAL_ERROR
        "Your compiler does not support interprocedural optimization"
        " (IPO). Disable ENABLE_LTO and try again.\n${LTO_CHECK_OUTPUT}")
    endif()
endif()
