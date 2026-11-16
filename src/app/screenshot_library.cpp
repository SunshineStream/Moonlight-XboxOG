/**
 * @file src/app/screenshot_library.cpp
 * @brief Implements the append-only screenshot journal and source matching.
 */
// class header include
#include "src/app/screenshot_library.h"

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <utility>

// local includes
#include "src/platform/filesystem_utils.h"
#include "src/startup/storage_paths.h"

namespace {
  constexpr std::size_t MAX_RECORD_BYTES = 8192;  ///< Bound journal parsing and reject unbounded damaged lines.

  /**
   * @brief Escape arbitrary labels and paths into one tab-separated journal field.
   * @param field Unescaped field value.
   * @return Escaped field with no literal record or field separators.
   */
  std::string escape_field(std::string_view field) {
    std::string result;
    for (char c : field) {
      switch (c) {
        case '\\':
          result += "\\\\";
          break;
        case '\t':
          result += "\\t";
          break;
        case '\n':
          result += "\\n";
          break;
        case '\r':
          result += "\\r";
          break;
        default:
          result += c;
          break;
      }
    }
    return result;
  }

  /**
   * @brief Compare UUIDs case-insensitively without changing endpoint or display labels.
   * @param first First UUID.
   * @param second Second UUID.
   * @return True when the UUID bytes agree apart from ASCII case.
   */
  bool same_uuid(std::string_view first, std::string_view second) {
    return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin(), [](unsigned char a, unsigned char b) {
             return std::tolower(a) == std::tolower(b);
           });
  }
}  // namespace

namespace app {
  ScreenshotSource screenshot_source(const HostRecord &host, const HostAppRecord &application) {
    return {host.uuid, host.address, effective_host_port(host.port), host.displayName, application.id, application.name};
  }

  bool screenshot_source_matches(const ScreenshotSource &captured, const ScreenshotSource &requested) {
    if (captured.appId != requested.appId) {
      return false;
    }
    if (!captured.hostUuid.empty() && !requested.hostUuid.empty()) {
      return same_uuid(captured.hostUuid, requested.hostUuid);
    }
    return !captured.hostAddress.empty() && captured.hostAddress == requested.hostAddress && effective_host_port(captured.hostPort) == effective_host_port(requested.hostPort);
  }

  std::string screenshot_library_path() {
    return startup::default_storage_path("screenshots.tsv");
  }

  std::string serialize_screenshot(const ScreenshotRecord &record) {
    if (record.path.empty() || record.capturedAt < 0 || record.source.appId <= 0 || (record.source.hostUuid.empty() && record.source.hostAddress.empty())) {
      return {};
    }
    std::string result = "1\t" + std::to_string(record.capturedAt) + "\t" + std::to_string(record.source.appId) + "\t" + std::to_string(effective_host_port(record.source.hostPort));
    for (const auto *field : {&record.source.hostUuid, &record.source.hostAddress, &record.source.hostName, &record.source.appName, &record.path}) {
      if (field->find('\0') != std::string::npos) {
        return {};
      }
      result += '\t' + escape_field(*field);
    }
    result += "\tend";
    return result.size() < MAX_RECORD_BYTES ? result : std::string {};
  }

  std::optional<ScreenshotRecord> parse_screenshot(std::string_view line) {
    if (line.empty() || line.size() >= MAX_RECORD_BYTES || line.find('\0') != std::string_view::npos) {
      return std::nullopt;
    }
    std::vector<std::string> fields(1);
    bool escaped = false;
    for (char c : line) {
      if (escaped) {
        switch (c) {
          case '\\':
            fields.back() += '\\';
            break;
          case 't':
            fields.back() += '\t';
            break;
          case 'n':
            fields.back() += '\n';
            break;
          case 'r':
            fields.back() += '\r';
            break;
          default:
            return std::nullopt;
        }
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '\t') {
        fields.emplace_back();
      } else if (c == '\n' || c == '\r') {
        return std::nullopt;
      } else {
        fields.back() += c;
      }
    }
    if (escaped || fields.size() != 10 || fields[0] != "1" || fields[9] != "end") {
      return std::nullopt;
    }
    ScreenshotRecord record;
    unsigned int port = 0;
    std::istringstream numbers(fields[1] + " " + fields[2] + " " + fields[3]);
    if (!(numbers >> record.capturedAt >> record.source.appId >> port) || !(numbers >> std::ws).eof() || port == 0 || port > 65535) {
      return std::nullopt;
    }
    record.source.hostPort = static_cast<uint16_t>(port);
    record.source.hostUuid = fields[4];
    record.source.hostAddress = fields[5];
    record.source.hostName = fields[6];
    record.source.appName = fields[7];
    record.path = fields[8];
    if (serialize_screenshot(record).empty()) {
      return std::nullopt;
    }
    return record;
  }

  bool append_screenshot(const ScreenshotRecord &record, const std::string &journalPath, std::string &error) {
    error.clear();
    const std::string line = serialize_screenshot(record);
    if (line.empty()) {
      error = "Invalid screenshot metadata.";
      return false;
    }
    if (!platform::ensure_directory_exists(platform::parent_directory(journalPath), &error)) {
      return false;
    }
    FILE *file = std::fopen(journalPath.c_str(), "ab");
    if (file == nullptr) {
      error = "Cannot open screenshot index: " + std::string(std::strerror(errno));
      return false;
    }
    // A leading newline isolates a previously interrupted append. Only newline-terminated
    // records are read, so an incomplete capture never poisons the remaining index.
    const std::string append = "\n" + line + "\n";
    const bool written = std::fwrite(append.data(), 1, append.size(), file) == append.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
      error = "Cannot finalize screenshot index.";
      return false;
    }
    return true;
  }

  ScreenshotLibraryResult load_screenshots(const ScreenshotSource &source, const std::string &journalPath) {
    ScreenshotLibraryResult result;
    FILE *file = std::fopen(journalPath.c_str(), "rb");
    if (file == nullptr) {
      if (errno != ENOENT) {
        result.error = "Cannot read screenshot index: " + std::string(std::strerror(errno));
      }
      return result;
    }
    std::array<char, MAX_RECORD_BYTES + 2> buffer {};
    bool oversized = false;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), file) != nullptr) {
      std::string_view line(buffer.data());
      const bool complete = !line.empty() && line.back() == '\n';
      if (!complete || oversized) {
        oversized = !complete;
        result.error = "Some incomplete or damaged screenshot records were skipped.";
        continue;
      }
      line.remove_suffix(1);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      if (line.empty()) {
        continue;
      }
      if (auto record = parse_screenshot(line); record.has_value()) {
        if (screenshot_source_matches(record->source, source)) {
          result.entries.push_back(std::move(*record));
        }
      } else {
        result.error = "Some incomplete or damaged screenshot records were skipped.";
      }
    }
    if (std::ferror(file) != 0) {
      result.error = "Reading the screenshot index failed.";
    }
    std::fclose(file);
    // Reverse first to give the newest append priority when the clock has not advanced.
    std::reverse(result.entries.begin(), result.entries.end());
    std::stable_sort(result.entries.begin(), result.entries.end(), [](const ScreenshotRecord &a, const ScreenshotRecord &b) {
      return a.capturedAt > b.capturedAt;
    });
    return result;
  }

  std::string next_screenshot_path(std::string_view directory, std::int64_t capturedAt, std::string &error) {
    error.clear();
    const std::string folder = startup::screenshot_storage_path(directory, {});
    if (capturedAt < 0 || !platform::ensure_directory_exists(folder, &error)) {
      if (error.empty()) {
        error = "Invalid screenshot timestamp.";
      }
      return {};
    }
    for (unsigned int suffix = 0; suffix < 10000; ++suffix) {
      const std::string path = folder + "screenshot-" + std::to_string(capturedAt) + "-" + std::to_string(suffix) + ".bmp";
      if (!platform::try_get_file_size(path)) {
        return path;
      }
    }
    error = "No unused screenshot filename is available.";
    return {};
  }

  std::string screenshot_time_label(std::int64_t capturedAt) {
    const auto time = static_cast<std::time_t>(capturedAt);
    std::array<char, 32> label {};
    const std::tm *utc = capturedAt >= 0 && static_cast<std::int64_t>(time) == capturedAt ? std::gmtime(&time) : nullptr;
    if (utc == nullptr || std::strftime(label.data(), label.size(), "%Y-%m-%d %H:%M:%S UTC", utc) == 0) {
      return "Capture time unavailable";
    }
    return label.data();
  }
}  // namespace app
