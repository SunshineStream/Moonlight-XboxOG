/**
 * @file src/_nxdk_compat/assert_compat.cpp
 * @brief Routes nxdk assertion failures through structured diagnostics instead of framebuffer text.
 */
// class header include
#include "src/_nxdk_compat/assert_compat.h"

// standard includes
#include <string>

#if defined(NXDK)
  // lib includes
  #include <xboxkrnl/xboxkrnl.h>
#endif

// local includes
#include "src/logging/logger.h"

namespace nxdk_compat {

  void report_assertion_failure(const char *expression, const char *fileName, const char *functionName, unsigned long line) {
    const std::string message = std::string("Assertion failed: '") + (expression != nullptr ? expression : "<unknown>") + "' in function '" + (functionName != nullptr ? functionName : "<unknown>") + "'";
    if (!logging::error("assert", message, {fileName, static_cast<int>(line)})) {
#if defined(NXDK)
      // An assertion can fire before main() has registered the application logger.
      DbgPrint("%s, file '%s', line %lu\r\n", message.c_str(), fileName != nullptr ? fileName : "<unknown>", line);
#endif
    }
  }

}  // namespace nxdk_compat

#if defined(NXDK)
/**
 * @brief Replace nxdk's framebuffer assertion reporter while preserving its debugger or halt behavior.
 *
 * This application object resolves the symbol before nxdk's static assertion object is extracted.
 * Fatal assertions cannot safely resume the shell to display a notification popup.
 *
 * @param expression Failed expression.
 * @param fileName Source path containing the assertion.
 * @param functionName Function containing the assertion.
 * @param line One-based source line.
 */
extern "C" void _xbox_assert(const char *expression, const char *fileName, const char *functionName, unsigned long line) {
  nxdk_compat::report_assertion_failure(expression, fileName, functionName, line);
  #if defined(DEBUG_CONSOLE)
  char buffer[512];
  RtlSnprintf(buffer, sizeof(buffer), "In function '%s': ", functionName);
  RtlAssert(const_cast<char *>(expression), const_cast<char *>(fileName), line, buffer);
  #else
  // Match nxdk's fatal stop: continuing could use invalid state or corrupt saved data.
  asm volatile(
    "cli\n"
    "1:\n"
    "hlt\n"
    "jmp 1b\n"
  );
  #endif
}
#endif
