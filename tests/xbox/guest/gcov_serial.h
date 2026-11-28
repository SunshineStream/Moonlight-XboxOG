/**
 * @file tests/xbox/guest/gcov_serial.h
 * @brief Guest coverage export through the Xbox debug serial port.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

  /** @brief Send registered translation units' gcov data to the xemu runner. */
  void sunshine_write_guest_coverage(void);

#ifdef __cplusplus
}
#endif
