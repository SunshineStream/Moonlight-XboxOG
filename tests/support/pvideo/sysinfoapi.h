/**
 * @file tests/support/pvideo/sysinfoapi.h
 * @brief Declares the fake monotonic clock for presenter lifecycle tests.
 */
#pragma once

#include <cstdint>

/**
 * @brief Get simulated milliseconds.
 * @return Fixture clock.
 */
std::uint32_t GetTickCount();
