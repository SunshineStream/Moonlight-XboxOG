/**
 * @file src/platform/filesystem_utils.h
 * @brief Declares filesystem utility helpers.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace platform {

  /**
   * @brief Immediate subdirectories, or mounted drive roots, available in the folder picker.
   */
  struct DirectoryListing {
    std::vector<std::string> paths;  ///< Sorted full paths; files and dot entries are excluded.
    std::string error;  ///< Empty on success, including an empty directory.
  };

  /**
   * @brief List child directories, or mounted drives when the path is empty.
   * @param path Directory to inspect; empty selects the drive list.
   * @return Child paths and any enumeration failure.
   */
  DirectoryListing list_directories(const std::string &path);

  /**
   * @brief Find the parent folder without turning a drive root into a drive-relative path.
   * @param path Folder path, optionally ending in separators.
   * @return Parent folder, or empty to return to the drive list.
   */
  std::string directory_parent(std::string path);

  /**
   * @brief Validate a single printable folder name using the Xbox's 42-byte component limit.
   * @param name Proposed name, without its parent directory.
   * @return True for a usable component, excluding traversal and reserved characters.
   */
  bool valid_directory_name(std::string_view name);
  /**
   * @brief Create one new child folder, refusing existing names and multi-component paths.
   * @param parent Existing parent folder.
   * @param name Valid single-component child name.
   * @param error Receives creation or validation failures; cleared on success.
   * @return True only when a new directory was created.
   */
  bool create_child_directory(const std::string &parent, const std::string &name, std::string &error);

  /**
   * @brief Verify a destination by creating and removing a unique empty file.
   * @param path Existing destination directory.
   * @param error Receives an actionable failure; cleared on success.
   * @return True when the directory permits creating and deleting files.
   */
  bool directory_is_writable(const std::string &path, std::string &error);

  /**
   * @brief Return the preferred path separator for the active platform.
   *
   * @return Preferred path separator character.
   */
  char preferred_path_separator();

  /**
   * @brief Return whether a character is recognized as a path separator.
   *
   * @param character Character to inspect.
   * @return True when the character is a supported path separator.
   */
  bool is_path_separator(char character);

  /**
   * @brief Join two path components using the preferred platform separator.
   *
   * @param left Left path component.
   * @param right Right path component.
   * @return Joined path.
   */
  std::string join_path(const std::string &left, const std::string &right);

  /**
   * @brief Return the parent directory portion of a file path.
   *
   * @param filePath File path to inspect.
   * @return Parent directory path, or an empty string when none exists.
   */
  std::string parent_directory(std::string_view filePath);

  /**
   * @brief Return the last path component from a file path.
   *
   * @param path Path to inspect.
   * @return Final path component, or the original string when no separator exists.
   */
  std::string file_name_from_path(std::string_view path);

  /**
   * @brief Ensure a directory exists, mounting its standard Xbox drive and creating missing segments as needed.
   *
   * @param directoryPath Directory path to create.
   * @param errorMessage Optional output for creation failures.
   * @return True when the directory exists or was created successfully.
   */
  bool ensure_directory_exists(const std::string &directoryPath, std::string *errorMessage = nullptr);

  /**
   * @brief Try to read the size of a regular file.
   *
   * @param path File path to inspect.
   * @param sizeBytes Optional output for the file size in bytes.
   * @return True when the path exists and is a regular file.
   */
  bool try_get_file_size(std::string_view path, std::uint64_t *sizeBytes = nullptr);

  /**
   * @brief Return whether a path begins with the requested prefix.
   *
   * @param path Full path to inspect.
   * @param prefix Prefix to compare against.
   * @return True when the path starts with the prefix.
   */
  bool path_has_prefix(const std::string &path, const std::string &prefix);

}  // namespace platform
