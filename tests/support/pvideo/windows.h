/**
 * @file tests/support/pvideo/windows.h
 * @brief Supplies Win32 types and timing declarations for presenter lifecycle tests.
 */
#pragma once

#include "synchapi.h"
#include "sysinfoapi.h"

#include <cstdint>

using DWORD = std::uint32_t;  ///< Xbox millisecond tick type.
inline constexpr int TRUE = 1;  ///< Enabled output.
