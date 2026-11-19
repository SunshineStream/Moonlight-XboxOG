/**
 * @file tests/support/pvideo/synchapi.h
 * @brief Declares the fake sleep function for presenter lifecycle tests.
 */
#pragma once

#include <cstdint>

/**
 * @brief Advance time and simulated vblank.
 * @param milliseconds Simulated sleep duration.
 */
void Sleep(std::uint32_t milliseconds);
