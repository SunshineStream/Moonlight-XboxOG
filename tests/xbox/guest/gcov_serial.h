/**
 * @file tests/xbox/guest/gcov_serial.h
 * @brief Export guest coverage and test results through the second serial port.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

  /**
   * @brief Send a formatted machine-readable record to xemu over COM2.
   * @param format Printf format string.
   * @param ... Format arguments.
   */
  void sunshine_write_guest_record(const char *format, ...);

  /** @brief Configure the second LPC47M157 serial port for coverage records. */
  void sunshine_prepare_guest_records(void);

  /** @brief Send registered translation units' gcov data to the xemu runner. */
  void sunshine_write_guest_coverage(void);

#ifdef __cplusplus
}
#endif
