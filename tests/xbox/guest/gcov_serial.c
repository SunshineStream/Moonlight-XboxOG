/**
 * @file tests/xbox/guest/gcov_serial.c
 * @brief Write Clang's gcda records to xemu's serial console after the guest tests.
 */
// standard includes
#include <stddef.h>
#include <stdint.h>

// lib includes
#include <xboxkrnl/xboxkrnl.h>

// local includes
#include "gcov_serial.h"

/** @brief Clang-generated function that writes a translation unit's profile. */
typedef void (*GcovWriter)(void);

/** @brief Maximum number of translation units instrumented in one guest test XBE. */
enum {
  MAX_GCOV_WRITERS = 64
};

static GcovWriter writers[MAX_GCOV_WRITERS];  ///< Registered at startup by the instrumented objects.
static size_t writer_count;  ///< Number of successfully registered writers.
static char serial_hex[129];  ///< Holds up to 64 bytes encoded as hex plus a terminator.
static size_t serial_size;  ///< Number of encoded characters currently in the buffer.

/** @brief Send a complete line within the kernel's DbgPrint message size limit. */
static void flush_serial(void) {
  if (serial_size != 0) {
    serial_hex[serial_size] = '\0';
    DbgPrint("SUNSHINE_GCOV_DATA %s\r\n", serial_hex);
    serial_size = 0;
  }
}

/**
 * @brief Append one little-endian word to the hex stream for the current gcda file.
 * @param value Word to append.
 */
static void write_word(uint32_t value) {
  static const char hex[] = "0123456789abcdef";
  for (unsigned int byte = 0; byte < 4; ++byte) {
    serial_hex[serial_size++] = hex[(value >> 4) & 0xf];
    serial_hex[serial_size++] = hex[value & 0xf];
    value >>= 8;
  }
  if (serial_size == sizeof(serial_hex) - 1) {
    flush_serial();
  }
}

/**
 * @brief Register a write callback from Clang's gcov initializer.
 * @param writeout Coverage writer for one instrumented translation unit.
 * @param reset Counter reset callback, unused for a single test run.
 */
void llvm_gcov_init(GcovWriter writeout, GcovWriter reset) {
  (void) reset;
  if (writer_count == MAX_GCOV_WRITERS) {
    DbgPrint("SUNSHINE_GCOV_ERROR Too many instrumented translation units\r\n");
    return;
  }
  writers[writer_count++] = writeout;
}

void sunshine_write_guest_coverage(void) {
  for (size_t index = 0; index < writer_count; ++index) {
    writers[index]();
  }
}

/**
 * @brief Begin a gcda stream using the matching compiler's version and checksum.
 * @param filename Path to the gcda as emitted by Clang.
 * @param version Encoded gcov format.
 * @param checksum Checksum shared with the matching gcno file.
 */
void llvm_gcda_start_file(const char *filename, uint32_t version, uint32_t checksum) {
  const char *basename = filename;
  for (const char *current = filename; *current != '\0'; ++current) {
    if (*current == '/' || *current == '\\') {
      basename = current + 1;
    }
  }
  DbgPrint("SUNSHINE_GCOV_BEGIN %s\r\n", basename);
  write_word(0x67636461);  // GCDA magic.
  write_word(version);
  write_word(checksum);
}

/**
 * @brief Write a gcov function record with both source and CFG checksums.
 * @param ident Function ID in the notes file.
 * @param function_checksum Function checksum.
 * @param cfg_checksum Control-flow graph checksum.
 */
void llvm_gcda_emit_function(uint32_t ident, uint32_t function_checksum, uint32_t cfg_checksum) {
  write_word(0x01000000);  // Function tag.
  write_word(3);
  write_word(ident);
  write_word(function_checksum);
  write_word(cfg_checksum);
}

/**
 * @brief Write a gcov arc record containing the guest's execution counts.
 * @param count Number of counters for the current function.
 * @param counters Execution counters assigned to the function.
 */
void llvm_gcda_emit_arcs(uint32_t count, uint64_t *counters) {
  write_word(0x01a10000);  // Arc counter tag.
  write_word(count * 2);
  for (uint32_t index = 0; index < count; ++index) {
    write_word((uint32_t) counters[index]);
    write_word((uint32_t) (counters[index] >> 32));
  }
}

/** @brief Record one completed run in Clang's gcov object summary. */
void llvm_gcda_summary_info(void) {
  write_word(0xa1000000);  // Object summary tag.
  write_word(2);
  write_word(1);  // Runs.
  write_word(0);  // Maximum sum is unused by llvm-cov.
}

/** @brief Terminate the current gcda and send its final encoded bytes. */
void llvm_gcda_end_file(void) {
  write_word(0);
  write_word(0);
  flush_serial();
  DbgPrint("SUNSHINE_GCOV_END\r\n");
}
