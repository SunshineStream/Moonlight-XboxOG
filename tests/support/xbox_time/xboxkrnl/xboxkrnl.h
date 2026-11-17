/**
 * @file tests/support/xbox_time/xboxkrnl/xboxkrnl.h
 * @brief Minimal kernel time types for native tests of the Xbox UTC adapter.
 */
#pragma once

// standard includes
#include <cstdint>

/**
 * @brief Signed NT timestamp received by the kernel conversion.
 */
struct LARGE_INTEGER {
  std::int64_t QuadPart = 0;  ///< Ticks since the NT epoch.
};

/**
 * @brief Calendar fields returned by the kernel conversion.
 */
struct TIME_FIELDS {
  short Year = 1970;  ///< Calendar year.
  short Month = 1;  ///< One-based month.
  short Day = 1;  ///< One-based day of month.
  short Hour = 0;  ///< Hour within the day.
  short Minute = 0;  ///< Minute within the hour.
  short Second = 0;  ///< Second within the minute.
  short Milliseconds = 0;  ///< Fractional seconds unused by the adapter.
  short Weekday = 4;  ///< Day of week, with Sunday as zero.
};

/**
 * @brief Return deterministic calendar fields and record the supplied timestamp.
 * @param time Signed NT timestamp.
 * @param fields Receives the scripted calendar fields.
 */
void RtlTimeToTimeFields(const LARGE_INTEGER *time, TIME_FIELDS *fields);
