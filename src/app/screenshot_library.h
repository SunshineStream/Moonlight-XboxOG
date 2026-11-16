/**
 * @file src/app/screenshot_library.h
 * @brief Associates local screenshots with their host and app, independently of the current capture folder.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/app/host_records.h"

namespace app {
  /**
   * @brief Identity and display labels of the stream that produced a capture.
   */
  struct ScreenshotSource {
    std::string hostUuid;  ///< Stable server UUID, preferred over its network endpoint.
    std::string hostAddress;  ///< Saved endpoint used when a UUID is unavailable.
    uint16_t hostPort = DEFAULT_HOST_PORT;  ///< Effective HTTP port of the saved endpoint.
    std::string hostName;  ///< Server label at capture time.
    int appId = 0;  ///< Host-defined app identifier.
    std::string appName;  ///< App label at capture time.
  };

  /**
   * @brief One saved bitmap and its stream provenance.
   */
  struct ScreenshotRecord {
    ScreenshotSource source;  ///< Host and app responsible for this capture.
    std::string path;  ///< Original bitmap location, retained when the capture folder changes.
    std::int64_t capturedAt = 0;  ///< Capture time in UTC seconds since the Unix epoch.
  };

  /**
   * @brief Captures returned from the local journal, newest first.
   */
  struct ScreenshotLibraryResult {
    std::vector<ScreenshotRecord> entries;  ///< Matching records, including unavailable images for explicit viewer errors.
    std::string error;  ///< Read failures or a warning about skipped damaged records.
  };

  /**
   * @brief Snapshot a host and app's persistent identity for a capture or lookup.
   * @param host Server record, including its saved endpoint and UUID.
   * @param application Selected host app.
   * @return Identity and names independent of subsequent shell selection changes.
   */
  ScreenshotSource screenshot_source(const HostRecord &host, const HostAppRecord &application);
  /**
   * @brief Match app IDs and server UUIDs, falling back to endpoints only when a UUID is missing.
   * @param captured Source stored with the capture.
   * @param requested Source currently being browsed.
   * @return True for the same app on the same server.
   */
  bool screenshot_source_matches(const ScreenshotSource &captured, const ScreenshotSource &requested);
  /**
   * @brief Locate the title-scoped screenshot journal, separate from the configurable bitmap folder.
   * @return Persistent journal path.
   */
  std::string screenshot_library_path();
  /**
   * @brief Encode one versioned journal record with escaped tabs, newlines and backslashes.
   * @param record Complete capture metadata.
   * @return Single journal line without its terminating newline; empty for invalid metadata.
   */
  std::string serialize_screenshot(const ScreenshotRecord &record);
  /**
   * @brief Validate and decode a single journal record.
   * @param line Serialized metadata without a terminating newline.
   * @return Record, or no value for damaged or unsupported data.
   */
  std::optional<ScreenshotRecord> parse_screenshot(std::string_view line);
  /**
   * @brief Append a completed capture without rewriting existing journal records.
   * @param record Metadata for a bitmap that has already been saved.
   * @param journalPath Journal destination.
   * @param error Receives any write or validation failure.
   * @return True when the record was finalized successfully.
   */
  bool append_screenshot(const ScreenshotRecord &record, const std::string &journalPath, std::string &error);
  /**
   * @brief Read locally indexed captures for an app without contacting the host.
   * @param source Server and app to match.
   * @param journalPath Journal to read; a missing journal is an empty library.
   * @return Matching captures and any recoverable journal warning.
   */
  ScreenshotLibraryResult load_screenshots(const ScreenshotSource &source, const std::string &journalPath);
  /**
   * @brief Choose a FATX-compatible capture name that never replaces an existing bitmap.
   * @param directory Configured capture folder, or empty for the default.
   * @param capturedAt UTC timestamp used in the filename.
   * @param error Receives directory or name-allocation failures.
   * @return Unused bitmap path, or empty on failure. The event thread owns capture creation.
   */
  std::string next_screenshot_path(std::string_view directory, std::int64_t capturedAt, std::string &error);
  /**
   * @brief Format capture time for the viewer without relying on a local timezone configuration.
   * @param capturedAt UTC timestamp stored in the journal.
   * @return Readable UTC time, or an unavailable-time label.
   */
  std::string screenshot_time_label(std::int64_t capturedAt);
}  // namespace app
