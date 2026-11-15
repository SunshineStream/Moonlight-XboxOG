/**
 * @file src/_nxdk_compat/assert_compat.h
 * @brief Declares structured reporting for nxdk assertion failures.
 */
#pragma once

namespace nxdk_compat {

  /**
   * @brief Report an assertion through the existing logger and configured diagnostic sinks.
   *
   * Before logger initialization, Xbox failures fall back to the debugger console.
   * This function reports the failure; the Xbox assertion entry point retains nxdk's fatal behavior.
   *
   * @param expression Failed expression, or nullptr when unavailable.
   * @param fileName Source path, or nullptr when unavailable.
   * @param functionName Function containing the assertion, or nullptr when unavailable.
   * @param line One-based source line supplied by nxdk's assertion macro.
   */
  void report_assertion_failure(const char *expression, const char *fileName, const char *functionName, unsigned long line);

}  // namespace nxdk_compat
