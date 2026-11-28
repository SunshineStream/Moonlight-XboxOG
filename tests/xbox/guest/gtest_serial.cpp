/**
 * @file tests/xbox/guest/gtest_serial.cpp
 * @brief Route GoogleTest's native result printer through the Xbox debug console.
 */
// standard includes
#include <cstdarg>
#include <cstdio>
#include <string>

// lib includes
#include <xboxkrnl/xboxkrnl.h>

namespace testing {
  /**
   * @brief Send formatted test output through DbgPrint in bounded chunks.
   * @param format Printf format string.
   * @param arguments Format arguments.
   * @return Number of characters formatted, or a negative formatting error.
   */
  static int vprintf(const char *format, std::va_list arguments) {
    std::va_list copy;
    va_copy(copy, arguments);
    const int count = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (count <= 0) {
      return count;
    }

    std::string output(static_cast<std::size_t>(count) + 1, '\0');
    std::vsnprintf(output.data(), output.size(), format, arguments);
    constexpr int chunkSize = 200;
    for (int offset = 0; offset < count; offset += chunkSize) {
      DbgPrint("%.*s", chunkSize, output.c_str() + offset);
    }
    return count;
  }

  /**
   * @brief Route GoogleTest's standard output to the debug serial console.
   * @param format Printf format string.
   * @param ... Format arguments.
   * @return Number of characters formatted, or a negative formatting error.
   */
  static int printf(const char *format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    const int count = testing::vprintf(format, arguments);
    va_end(arguments);
    return count;
  }
}  // namespace testing

// Include GoogleTest after the adapters so its unqualified printf calls use the
// debug console. nxdk's standard output stream has no backing console handle.
#include "src/gtest-all.cc"
