/**
 * @file src/_nxdk_compat/include/sunshine_time.h
 * @brief Converts UTC timestamps with caller-owned storage through the Xbox kernel.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <ctime>
#include <limits>

// lib includes
#include <xboxkrnl/xboxkrnl.h>

/**
 * @brief Convert nonnegative Unix time into caller-owned UTC calendar fields.
 *
 * PDCLib declares gmtime_s without supplying its implementation. Its gmtime uses
 * shared static storage, so use the kernel conversion without changing libc feature macros.
 *
 * @param timestamp Unix timestamp in seconds.
 * @param result Receives the complete UTC calendar fields.
 * @return False if the timestamp cannot be represented as signed NT time.
 */
inline bool sunshine_gmtime(std::time_t timestamp, std::tm &result) {
  constexpr std::int64_t epochOffset = 11644473600LL;
  constexpr std::int64_t ticksPerSecond = 10000000;
  if (timestamp < 0 || timestamp > std::numeric_limits<std::int64_t>::max() / ticksPerSecond - epochOffset) {
    return false;
  }
  LARGE_INTEGER time {};
  time.QuadPart = (static_cast<std::int64_t>(timestamp) + epochOffset) * ticksPerSecond;
  TIME_FIELDS fields {};
  RtlTimeToTimeFields(&time, &fields);
  result = {};
  result.tm_year = fields.Year - 1900;
  result.tm_mon = fields.Month - 1;
  result.tm_mday = fields.Day;
  result.tm_hour = fields.Hour;
  result.tm_min = fields.Minute;
  result.tm_sec = fields.Second;
  result.tm_wday = fields.Weekday;
  constexpr std::array<int, 12> monthStarts {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  const bool leap = fields.Year % 4 == 0 && (fields.Year % 100 != 0 || fields.Year % 400 == 0);
  result.tm_yday = monthStarts.at(result.tm_mon) + fields.Day - 1 + (fields.Month > 2 && leap ? 1 : 0);
  return true;
}
