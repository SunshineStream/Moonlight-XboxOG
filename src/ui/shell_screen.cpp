/**
 * @file src/ui/shell_screen.cpp
 * @brief Implements the shell screen controller.
 */
// class header include
#include "src/ui/shell_screen.h"

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// lib includes
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

// local includes
#include "src/app/file_save_activity.h"
#include "src/app/settings_storage.h"
#include "src/input/navigation_input.h"
#include "src/input/shell_controller.h"
#include "src/input/stream_input.h"
#include "src/logging/log_file.h"
#include "src/logging/logger.h"
#include "src/network/host_discovery.h"
#include "src/network/host_pairing.h"
#include "src/network/runtime_network.h"
#include "src/os.h"
#include "src/platform/error_utils.h"
#include "src/platform/filesystem_utils.h"
#include "src/splash/splash_layout.h"
#include "src/startup/client_identity_storage.h"
#include "src/startup/cover_art_cache.h"
#include "src/startup/host_storage.h"
#include "src/startup/saved_files.h"
#include "src/streaming/session.h"
#include "src/streaming/video_overlay.h"
#include "src/ui/asset_path.h"
#include "src/ui/dialog_surface.h"
#include "src/ui/host_probe_result_queue.h"
#include "src/ui/save_indicator.h"
#include "src/ui/screenshot_image.h"
#include "src/ui/sdl_render.h"
#include "src/ui/shell_layout.h"
#include "src/ui/shell_view.h"
#include "src/ui/svg_assets.h"

#ifdef MOONLIGHT_DESKTOP
  #include "src/ui/desktop_window.h"
#endif

namespace {

#ifdef MOONLIGHT_DESKTOP
  constexpr bool DESKTOP_SHELL = true;  ///< Use resizable desktop layouts and device-independent coordinates.
#else
  constexpr bool DESKTOP_SHELL = false;  ///< Use the full-screen device's native shell layout.
#endif

  constexpr std::size_t PAIRING_THREAD_STACK_SIZE = 1024U * 1024U;
  constexpr Uint32 HOST_DISCOVERY_REFRESH_INTERVAL_MILLISECONDS = 10000U;
  constexpr uint32_t HOST_DISCOVERY_TIMEOUT_MILLISECONDS = 1500U;
  constexpr Uint32 HOST_PROBE_REFRESH_INTERVAL_MILLISECONDS = 10000U;
  constexpr Uint32 APP_LIST_REFRESH_INTERVAL_MILLISECONDS = 30000U;

  constexpr Uint8 BACKGROUND_RED = 0x0D;
  constexpr Uint8 BACKGROUND_GREEN = 0x12;
  constexpr Uint8 BACKGROUND_BLUE = 0x1D;
  constexpr Uint8 PANEL_RED = 0x1C;
  constexpr Uint8 PANEL_GREEN = 0x28;
  constexpr Uint8 PANEL_BLUE = 0x3D;
  constexpr Uint8 PANEL_ALT_RED = 0x15;
  constexpr Uint8 PANEL_ALT_GREEN = 0x1E;
  constexpr Uint8 PANEL_ALT_BLUE = 0x2E;
  constexpr Uint8 ACCENT_RED = 0x82;
  constexpr Uint8 ACCENT_GREEN = 0xB4;
  constexpr Uint8 ACCENT_BLUE = 0xFF;
  constexpr Uint8 TEXT_RED = 0xF2;
  constexpr Uint8 TEXT_GREEN = 0xF5;
  constexpr Uint8 TEXT_BLUE = 0xF8;
  constexpr Uint8 MUTED_RED = 0xA3;
  constexpr Uint8 MUTED_GREEN = 0xAB;
  constexpr Uint8 MUTED_BLUE = 0xB5;
  constexpr Sint16 TRIGGER_PAGE_SCROLL_THRESHOLD = 16000;
  constexpr Sint16 LEFT_STICK_NAVIGATION_THRESHOLD = input::STICK_NAVIGATION_THRESHOLD;  ///< Shared shell/OSK stick activation threshold.
  constexpr Sint16 LEFT_STICK_NAVIGATION_RELEASE_THRESHOLD = input::STICK_NAVIGATION_RELEASE_THRESHOLD;  ///< Shared hysteresis prevents near-center jitter.
  constexpr Uint32 EXIT_COMBO_HOLD_MILLISECONDS = 900U;
  constexpr Uint32 CONTROLLER_NAVIGATION_INITIAL_REPEAT_MILLISECONDS = input::NAVIGATION_INITIAL_REPEAT_MILLISECONDS;
  constexpr Uint32 CONTROLLER_NAVIGATION_REPEAT_MILLISECONDS = input::NAVIGATION_REPEAT_MILLISECONDS;
  constexpr Uint32 LOG_VIEWER_SCROLL_REPEAT_MILLISECONDS = 110U;
  constexpr Uint32 LOG_VIEWER_FAST_SCROLL_REPEAT_MILLISECONDS = 45U;
  constexpr int SHELL_EVENT_WAIT_TIMEOUT_MILLISECONDS = 2;
  constexpr std::size_t LOG_VIEWER_MAX_LOADED_LINES = 512U;
  constexpr std::size_t LOG_VIEWER_MAX_RENDER_CHARACTERS = 320U;

  using ui::build_asset_path;

  bool asset_path_uses_svg(const char *relativePath) {
    if (relativePath == nullptr) {
      return false;
    }

    const std::string path(relativePath);
    return path.size() >= 4U && path.substr(path.size() - 4U) == ".svg";
  }

  bool asset_path_is_svg_icon(const char *relativePath) {
    if (!asset_path_uses_svg(relativePath)) {
      return false;
    }

    const std::string path(relativePath);
    return path.rfind("icons\\", 0U) == 0U;
  }

  /**
   * @brief Start a worker with enough stack for the synchronous TLS handshake.
   * @tparam Task Type of the worker's userdata.
   * @param function SDL thread entry point.
   * @param name Diagnostic thread name.
   * @param stackSize Requested stack size in bytes.
   * @param context Entry point userdata.
   * @return Owned SDL thread, or null on failure.
   */
  template<typename Task>
  SDL_Thread *create_pairing_thread(SDL_ThreadFunction function, const char *name, Sint64 stackSize, Task *context) {
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
      return nullptr;
    }
    // SDL transports the callback address through a pointer property. On Xbox,
    // function and data pointers have the same representation.
    void *entry = nullptr;
    static_assert(sizeof(entry) == sizeof(function));
    std::memcpy(&entry, &function, sizeof(entry));
    SDL_Thread *thread = nullptr;
    if (SDL_SetPointerProperty(properties, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, entry) && SDL_SetStringProperty(properties, SDL_PROP_THREAD_CREATE_NAME_STRING, name) && SDL_SetNumberProperty(properties, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, stackSize) && SDL_SetPointerProperty(properties, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, context)) {
      thread = SDL_CreateThreadWithProperties(properties);
    }
    SDL_DestroyProperties(properties);
    return thread;
  }

  /**
   * @brief Build an asset texture cache key, including raster dimensions for size-aware SVG textures.
   *
   * @param relativePath Asset path relative to `DATA_PATH/assets`.
   * @param maxWidth Requested maximum raster width in pixels.
   * @param maxHeight Requested maximum raster height in pixels.
   * @return Cache key used to store or look up the texture.
   */
  std::string build_asset_texture_cache_key(const std::string &relativePath, int maxWidth = 0, int maxHeight = 0) {
    if (relativePath.empty() || maxWidth <= 0 || maxHeight <= 0 || !asset_path_uses_svg(relativePath.c_str())) {
      return relativePath;
    }

    return relativePath + "#" + std::to_string(maxWidth) + "x" + std::to_string(maxHeight);
  }

  /**
   * @brief Normalize a loaded asset to the shell's blended ARGB texture format.
   * @param surface Owned source surface, freed during normalization.
   * @return Owned normalized surface, or nullptr on failure.
   */
  SDL_Surface *normalize_asset_surface(SDL_Surface *surface) {
    if (surface == nullptr) {
      return nullptr;
    }

    SDL_Surface *normalizedSurface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ARGB8888);
    if (normalizedSurface == nullptr) {
      SDL_DestroySurface(surface);
      return nullptr;
    }

    SDL_DestroySurface(surface);
    if (!SDL_SetSurfaceBlendMode(normalizedSurface, SDL_BLENDMODE_BLEND)) {
      SDL_DestroySurface(normalizedSurface);
      return nullptr;
    }

    return normalizedSurface;
  }

  SDL_Surface *prepare_asset_surface(SDL_Surface *surface) {
    return normalize_asset_surface(surface);
  }

  SDL_Texture *load_texture_from_asset(SDL_Renderer *renderer, const char *relativePath, int maxWidth, int maxHeight);

  /**
   * @brief Upload a texture with the filter appropriate for its artwork.
   * @param renderer Active shell renderer.
   * @param surface Decoded artwork.
   * @param scaleMode Texture sampling mode.
   * @return Owned texture, or null on failure.
   */
  SDL_Texture *create_asset_texture(SDL_Renderer *renderer, SDL_Surface *surface, SDL_ScaleMode scaleMode) {
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture != nullptr && !SDL_SetTextureScaleMode(texture, scaleMode)) {
      SDL_DestroyTexture(texture);
      return nullptr;
    }
    return texture;
  }

  int report_shell_failure(const char *category, const std::string &message) {
    logging::error(category, message);
#ifdef NXDK
    logging::warn(category, "Holding the failure screen for 5 seconds before exit.");
    SDL_Delay(5000);
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Moonlight", message.c_str(), nullptr);
#endif
    return 1;
  }

  void destroy_texture(SDL_Texture *texture) {
    if (texture != nullptr) {
      SDL_DestroyTexture(texture);
    }
  }

  bool render_surface_line(SDL_Renderer *renderer, SDL_Surface *surface, int x, int y, int *drawnHeight) {
    if (renderer == nullptr || surface == nullptr) {
      return false;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == nullptr) {
      SDL_DestroySurface(surface);
      return false;
    }

    SDL_Rect destination {x, y, surface->w, surface->h};
    SDL_DestroySurface(surface);
    const bool rendered = ui::render_texture(renderer, texture, nullptr, &destination);
    destroy_texture(texture);

    if (drawnHeight != nullptr) {
      *drawnHeight = destination.h;
    }

    return rendered;
  }

  /**
   * @brief Caches cover-art textures keyed by the app art cache key.
   */
  struct CoverArtTextureCache {
    std::unordered_map<std::string, SDL_Texture *> textures;  ///< Loaded cover-art textures by cache key.
    std::unordered_map<std::string, bool> failedKeys;  ///< Cache keys that already failed to load.
  };

  /**
   * @brief Caches UI asset textures keyed by asset path.
   */
  struct AssetTextureCache {
    std::unordered_map<std::string, SDL_Texture *> textures;  ///< Loaded asset textures by path.
    std::unordered_map<std::string, bool> failedKeys;  ///< Asset paths that already failed to load.
  };

  /**
   * @brief Stores a reusable rendered text texture and the inputs that produced it.
   */
  struct CachedTextTexture {
    SDL_Texture *texture = nullptr;  ///< Cached SDL texture for the rendered text.
    TTF_Font *font = nullptr;  ///< Font used to build the cached texture.
    std::string sourceText;  ///< Original uncropped text source.
    std::string renderedText;  ///< Sanitized or cropped text actually rendered into the texture.
    int maxWidth = 0;  ///< Maximum width used when generating the cached texture.
    int width = 0;  ///< Cached texture width in pixels.
    int height = 0;  ///< Cached texture height in pixels.
    SDL_Color color {0x00, 0x00, 0x00, 0x00};  ///< Text color used when rendering the cached texture.
    bool wrapped = false;  ///< True when the texture was generated with wrapped rendering.
  };

  /**
   * @brief Retained folder-dialog pixels and text for only the visible list window.
   */
  struct FolderDialogCache {
    ui::ShellModalView content;  ///< Static dialog content, excluding the changing selection.
    std::map<std::size_t, CachedTextTexture> labels;  ///< Visible row labels; offscreen textures are released.
    SDL_Rect rows {};  ///< Logical list rectangle inside the dialog.
    int rowHeight = 0;  ///< Painted row height, excluding the six-pixel gap.
    std::size_t first = 0;  ///< First visible row, retained until focus leaves the window.
    std::size_t selected = 0;  ///< Previously painted selection.
    int logicalWidth = 0;  ///< Logical output width used to paint the retained frame.
    int framebufferWidth = 0;  ///< Actual output width before encoder correction.
    int framebufferHeight = 0;  ///< Actual output height.
    TTF_Font *titleFont = nullptr;  ///< Font identity used for the retained heading.
    TTF_Font *bodyFont = nullptr;  ///< Font identity used for row labels.
    TTF_Font *smallFont = nullptr;  ///< Font identity used for guidance and helpers.
    bool valid = false;  ///< A complete frame has been presented for these dependencies.
  };

  /**
   * @brief Retains one decoded screenshot and its load result for the current renderer.
   */
  struct ScreenshotTextureCache {
    SDL_Texture *texture = nullptr;  ///< Single open bitmap, released before a different image is decoded.
    std::string path;  ///< Last attempted image path, including failed loads.
    std::string error;  ///< User-facing unavailable or decoding error for this path.
  };

  /**
   * @brief Renderer-owned resources retained across local input and image dialogs.
   */
  struct KeypadModalLayoutCache {
    SDL_Window *window = nullptr;  ///< Shared window; needed only for validated partial Xbox framebuffer presentation.
    FolderDialogCache folder;  ///< Shared retained rows for host/app menus, other action dialogs and the folder browser.
    ScreenshotTextureCache screenshot;  ///< At most one decoded capture is retained.
    int modalInnerWidth = 0;  ///< Last modal content width used to build the cached line layout.
    int modalTextHeight = 0;  ///< Cached combined height of the modal body text.
    std::vector<std::string> lines;  ///< Last body lines used to populate the cache.
    CachedTextTexture titleTexture;  ///< Cached rendered title texture.
    std::vector<CachedTextTexture> lineTextures;  ///< Cached rendered body line textures.
    std::vector<CachedTextTexture> buttonLabelTextures;  ///< Cached rendered keypad button labels.
  };

  /**
   * @brief Geometry shared by measuring and drawing shell hints.
   */
  struct FooterHintStyle {
    int glyphSize;  ///< Stream control glyph size in output pixels.
    bool compact = false;  ///< Use tighter spacing for dense keypad actions.
  };

  /**
   * @brief Measure wrapped input helper rows before laying out a modal.
   * @param font Footer font.
   * @param actions Input helper actions.
   * @param width Footer width.
   * @param style Glyph size and horizontal spacing.
   * @return Required footer height.
   */
  int measure_footer_actions_height(TTF_Font *font, const std::vector<ui::ShellFooterAction> &actions, int width, FooterHintStyle style);

  bool render_footer_actions(
    SDL_Renderer *renderer,
    TTF_Font *font,
    AssetTextureCache *assetCache,
    const std::vector<ui::ShellFooterAction> &actions,
    const SDL_Rect &footerRect,
    FooterHintStyle style
  );

  void clear_cover_art_texture_cache(CoverArtTextureCache *cache) {
    if (cache == nullptr) {
      return;
    }

    for (const auto &[cacheKey, texture] : cache->textures) {
      (void) cacheKey;
      destroy_texture(texture);
    }
    cache->textures.clear();
    cache->failedKeys.clear();
  }

  void clear_cover_art_texture(CoverArtTextureCache *cache, const std::string &cacheKey) {
    if (cache == nullptr || cacheKey.empty()) {
      return;
    }

    if (const auto textureIterator = cache->textures.find(cacheKey); textureIterator != cache->textures.end()) {
      destroy_texture(textureIterator->second);
      cache->textures.erase(textureIterator);
    }
    cache->failedKeys.erase(cacheKey);
  }

  void clear_asset_texture_cache(AssetTextureCache *cache) {
    if (cache == nullptr) {
      return;
    }

    for (const auto &[assetPath, texture] : cache->textures) {
      (void) assetPath;
      destroy_texture(texture);
    }
    cache->textures.clear();
    cache->failedKeys.clear();
  }

  /**
   * @brief Returns whether two SDL colors are identical.
   *
   * @param left First color to compare.
   * @param right Second color to compare.
   * @return True when all RGBA components match.
   */
  bool colors_match(SDL_Color left, SDL_Color right) {
    return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
  }

  /**
   * @brief Releases a cached text texture and resets its metadata.
   *
   * @param cache Cached text entry to clear.
   */
  void clear_cached_text_texture(CachedTextTexture *cache) {
    if (cache == nullptr) {
      return;
    }

    destroy_texture(cache->texture);
    *cache = {};
  }

  /**
   * @brief Release the folder window's labels and invalidate its retained frame.
   * @param cache Folder cache owned by the current renderer.
   */
  void clear_folder_dialog_cache(FolderDialogCache &cache) {
    for (auto &[index, label] : cache.labels) {
      (void) index;
      clear_cached_text_texture(&label);
    }
    cache = {};
  }

  /**
   * @brief Releases every cached text texture and retained frame stored for local dialogs.
   *
   * @param cache Keypad modal cache to clear.
   */
  void clear_keypad_modal_layout_cache(KeypadModalLayoutCache *cache) {
    if (cache == nullptr) {
      return;
    }

    clear_folder_dialog_cache(cache->folder);
    destroy_texture(cache->screenshot.texture);
    cache->screenshot = {};
    clear_cached_text_texture(&cache->titleTexture);
    for (CachedTextTexture &lineTexture : cache->lineTextures) {
      clear_cached_text_texture(&lineTexture);
    }
    for (CachedTextTexture &buttonLabelTexture : cache->buttonLabelTextures) {
      clear_cached_text_texture(&buttonLabelTexture);
    }
    cache->lineTextures.clear();
    cache->buttonLabelTextures.clear();
    cache->modalInnerWidth = 0;
    cache->modalTextHeight = 0;
    cache->lines.clear();
  }

  Uint32 color_seed(std::string_view text) {
    Uint32 value = 2166136261U;
    for (char character : text) {
      value ^= static_cast<Uint8>(character);
      value *= 16777619U;
    }
    return value;
  }

  SDL_Color placeholder_color(std::string_view seedText) {
    const Uint32 seed = color_seed(seedText);
    return {
      static_cast<Uint8>(0x40 + (seed & 0x3F)),
      static_cast<Uint8>(0x50 + ((seed >> 8U) & 0x4F)),
      static_cast<Uint8>(0x70 + ((seed >> 16U) & 0x5F)),
      0xFF,
    };
  }

  std::string sanitize_text_for_render(std::string_view text) {
    std::string sanitized;
    sanitized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {  // NOSONAR(cpp:S886) UTF-8 validation needs explicit cursor control
      const auto character = static_cast<unsigned char>(text[index]);
      if (character == '\r' || character == '\n') {
        continue;
      }
      if (character == '\t') {
        sanitized.append("  ");
        continue;
      }
      if (character < 0x80) {
        if (character < 0x20) {
          sanitized.push_back('?');
          continue;
        }
        sanitized.push_back(static_cast<char>(character));
        continue;
      }

      std::size_t sequenceLength = 0U;
      if ((character & 0xE0U) == 0xC0U) {  // NOSONAR(cpp:S6022) UTF-8 parsing is byte-oriented by design
        sequenceLength = 2U;
      } else if ((character & 0xF0U) == 0xE0U) {  // NOSONAR(cpp:S6022) UTF-8 parsing is byte-oriented by design
        sequenceLength = 3U;
      } else if ((character & 0xF8U) == 0xF0U) {  // NOSONAR(cpp:S6022) UTF-8 parsing is byte-oriented by design
        sequenceLength = 4U;
      }

      const bool sequenceAvailable = sequenceLength > 0U && index + sequenceLength <= text.size();
      bool sequenceValid = sequenceAvailable;
      for (std::size_t continuationIndex = 1U; sequenceValid && continuationIndex < sequenceLength; ++continuationIndex) {  // NOSONAR(cpp:S886) UTF-8 validation needs explicit cursor control
        const auto continuation = static_cast<unsigned char>(text[index + continuationIndex]);
        sequenceValid = (continuation & 0xC0U) == 0x80U;  // NOSONAR(cpp:S6022) UTF-8 parsing is byte-oriented by design
      }

      if (sequenceValid) {
        sanitized.append(text.substr(index, sequenceLength));
        index += sequenceLength - 1U;
        continue;
      }

      sanitized.push_back('?');
    }
    return sanitized;
  }

  std::string truncate_text_for_render(std::string_view text, std::size_t maxCharacters) {
    if (maxCharacters == 0U || text.size() <= maxCharacters) {
      return std::string(text);
    }

    if (maxCharacters <= 3U) {
      return std::string(maxCharacters, '.');
    }

    return std::string(text.substr(0, maxCharacters - 3U)) + "...";
  }

  std::string sanitize_ascii_text_for_render(std::string_view text) {
    std::string sanitized;
    sanitized.reserve(text.size());
    for (const unsigned char character : text) {
      if (character == '\r' || character == '\n') {
        continue;
      }
      if (character == '\t') {
        sanitized.append("  ");
        continue;
      }
      if (character >= 0x20 && character <= 0x7E) {
        sanitized.push_back(static_cast<char>(character));
        continue;
      }
      sanitized.push_back('?');
    }
    return sanitized;
  }

  std::string app_monogram(const ui::ShellAppTile &tile) {
    for (char character : tile.name) {
      if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')) {
        return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
      }
    }

    return "?";
  }

  bool render_default_app_cover(
    SDL_Renderer *renderer,
    TTF_Font *labelFont,
    const ui::ShellAppTile &tile,
    const SDL_Rect &rect,
    AssetTextureCache *assetCache
  );

  SDL_Texture *load_cover_art_texture(SDL_Renderer *renderer, CoverArtTextureCache *cache, const std::string &cacheKey) {
    if (renderer == nullptr || cache == nullptr || cacheKey.empty()) {
      return nullptr;
    }

    if (const auto existingTexture = cache->textures.find(cacheKey); existingTexture != cache->textures.end()) {
      return existingTexture->second;
    }
    if (cache->failedKeys.find(cacheKey) != cache->failedKeys.end()) {
      return nullptr;
    }

    const startup::LoadCoverArtResult loadResult = startup::load_cover_art(cacheKey);
    if (!loadResult.fileFound || loadResult.bytes.empty()) {
      cache->failedKeys[cacheKey] = true;
      return nullptr;
    }

    SDL_IOStream *rw = SDL_IOFromConstMem(loadResult.bytes.data(), loadResult.bytes.size());
    if (rw == nullptr) {
      cache->failedKeys[cacheKey] = true;
      return nullptr;
    }

    SDL_Surface *surface = IMG_Load_IO(rw, true);
    if (surface == nullptr) {
      cache->failedKeys[cacheKey] = true;
      return nullptr;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture == nullptr) {
      cache->failedKeys[cacheKey] = true;
      return nullptr;
    }

    cache->textures.try_emplace(cacheKey, texture);
    return texture;
  }

  /**
   * @brief Load an asset texture, optionally rasterizing SVG assets to requested bounds.
   *
   * @param renderer SDL renderer that will own the texture.
   * @param relativePath Asset path relative to `DATA_PATH/assets`.
   * @param maxWidth Optional maximum raster width used for SVG assets.
   * @param maxHeight Optional maximum raster height used for SVG assets.
   * @return Loaded texture, or null when the asset could not be prepared.
   */
  SDL_Texture *load_texture_from_asset(SDL_Renderer *renderer, const char *relativePath, int maxWidth = 0, int maxHeight = 0) {
    if (renderer == nullptr || relativePath == nullptr) {
      return nullptr;
    }

    SDL_Surface *surface = (maxWidth > 0 && maxHeight > 0 && asset_path_uses_svg(relativePath)) ? ui::load_svg_surface(build_asset_path(relativePath), maxWidth, maxHeight) : nullptr;
    if (surface == nullptr) {
      const std::string assetPath = build_asset_path(relativePath);
      surface = IMG_Load(assetPath.c_str());
    }
    if (surface == nullptr) {
      return nullptr;
    }

    surface = prepare_asset_surface(surface);
    if (surface == nullptr) {
      return nullptr;
    }

    SDL_Texture *texture = create_asset_texture(renderer, surface, asset_path_is_svg_icon(relativePath) ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(surface);
    return texture;
  }

  SDL_Texture *load_cached_asset_texture(SDL_Renderer *renderer, AssetTextureCache *cache, const std::string &relativePath, int maxWidth = 0, int maxHeight = 0) {
    if (renderer == nullptr || cache == nullptr || relativePath.empty()) {
      return nullptr;
    }

    const std::string cacheKey = build_asset_texture_cache_key(relativePath, maxWidth, maxHeight);

    if (const auto existingTexture = cache->textures.find(cacheKey); existingTexture != cache->textures.end()) {
      return existingTexture->second;
    }
    if (cache->failedKeys.find(cacheKey) != cache->failedKeys.end()) {
      return nullptr;
    }

    SDL_Texture *texture = load_texture_from_asset(renderer, relativePath.c_str(), maxWidth, maxHeight);
    if (texture == nullptr) {
      cache->failedKeys[cacheKey] = true;
      return nullptr;
    }

    cache->textures.try_emplace(cacheKey, texture);
    return texture;
  }

  void fill_rect(SDL_Renderer *renderer, const SDL_Rect &rect, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha = 0xFF) {
    SDL_SetRenderDrawColor(renderer, red, green, blue, alpha);
    ui::fill_rect(renderer, rect);
  }

  void draw_rect(SDL_Renderer *renderer, const SDL_Rect &rect, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha = 0xFF) {
    SDL_SetRenderDrawColor(renderer, red, green, blue, alpha);
    ui::draw_rect(renderer, rect);
  }

  /**
   * @brief Convert tested shell geometry to an SDL drawing rectangle.
   *
   * @param rect Rectangle in logical display coordinates.
   * @return Equivalent SDL rectangle.
   */
  SDL_Rect sdl_rect(const ui::ShellRect &rect) {
    return {rect.x, rect.y, rect.w, rect.h};
  }

  /**
   * @brief Outline the same rounded contour used by filled checkbox surfaces.
   *
   * @param renderer Active shell renderer, including the software fallback.
   * @param rect Outer bounds of the outline.
   * @param color Border color.
   * @param radius Outer corner radius in logical pixels.
   */
  void draw_rounded_rect_outline(SDL_Renderer *renderer, const SDL_Rect &rect, SDL_Color color, int radius) {
    if (rect.w <= 0 || rect.h <= 0) {
      return;
    }
    radius = std::max(0, std::min(radius, std::min(rect.w, rect.h) / 2));
    const int border = std::min(2, std::min(rect.w, rect.h) / 2);
    const auto corner_inset = [](int y, int height, int cornerRadius) {
      const int edgeDistance = std::min(y, height - y - 1);
      if (edgeDistance >= cornerRadius) {
        return 0;
      }
      const int dy = cornerRadius - edgeDistance - 1;
      return cornerRadius - static_cast<int>(std::sqrt(cornerRadius * cornerRadius - dy * dy));
    };
    for (int y = 0; y < rect.h; ++y) {
      const int outerInset = corner_inset(y, rect.h, radius);
      if (border == 0 || rect.w <= border * 2 || y < border || y >= rect.h - border) {
        fill_rect(renderer, {rect.x + outerInset, rect.y + y, rect.w - outerInset * 2, 1}, color.r, color.g, color.b, color.a);
        continue;
      }
      const int innerInset = corner_inset(y - border, rect.h - border * 2, std::max(0, radius - border));
      const int edgeWidth = border + innerInset - outerInset;
      if (edgeWidth > 0) {
        fill_rect(renderer, {rect.x + outerInset, rect.y + y, edgeWidth, 1}, color.r, color.g, color.b, color.a);
        fill_rect(renderer, {rect.x + rect.w - outerInset - edgeWidth, rect.y + y, edgeWidth, 1}, color.r, color.g, color.b, color.a);
      }
    }
  }

  /**
   * @brief Fill a modestly rounded surface using primitives supported by both SDL paths.
   *
   * @param renderer Active shell renderer.
   * @param rect Surface bounds.
   * @param color Surface color.
   * @param radius Corner radius in logical pixels.
   */
  void fill_rounded_rect(SDL_Renderer *renderer, const SDL_Rect &rect, SDL_Color color, int radius = 8) {
    radius = std::max(0, std::min(radius, std::min(rect.w, rect.h) / 2));
    fill_rect(renderer, {rect.x, rect.y + radius, rect.w, rect.h - radius * 2}, color.r, color.g, color.b, color.a);
    for (int y = 0; y < radius; ++y) {
      const int dy = radius - y - 1;
      const int inset = radius - static_cast<int>(std::sqrt(radius * radius - dy * dy));
      fill_rect(renderer, {rect.x + inset, rect.y + y, rect.w - inset * 2, 1}, color.r, color.g, color.b, color.a);
      fill_rect(renderer, {rect.x + inset, rect.y + rect.h - y - 1, rect.w - inset * 2, 1}, color.r, color.g, color.b, color.a);
    }
  }

  /**
   * @brief Draw a card with a substantial focus ring visible from a couch.
   *
   * @param renderer Active shell renderer.
   * @param rect Card bounds.
   * @param selected Whether the card has controller focus.
   */
  void render_card_surface(SDL_Renderer *renderer, const SDL_Rect &rect, bool selected) {
    if (selected) {
      fill_rounded_rect(renderer, rect, {ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF});
      fill_rounded_rect(renderer, {rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6}, {PANEL_RED, PANEL_GREEN, PANEL_BLUE, 0xFF}, 5);
    } else {
      fill_rounded_rect(renderer, rect, {PANEL_ALT_RED, PANEL_ALT_GREEN, PANEL_ALT_BLUE, 0xFF});
    }
  }

  /**
   * @brief Draw the opaque rounded dialog surface above a dimmed page.
   *
   * @param renderer Active shell renderer.
   * @param rect Dialog bounds.
   */
  void render_dialog_surface(SDL_Renderer *renderer, const SDL_Rect &rect) {
    ui::draw_dialog_surface(renderer, rect);
  }

  bool render_text_line(  // NOSONAR(cpp:S107) helper keeps the SDL text rendering callsite contract explicit
    SDL_Renderer *renderer,
    TTF_Font *font,
    const std::string &text,
    SDL_Color color,
    int x,
    int y,
    int maxWidth,
    int *drawnHeight = nullptr
  ) {
    if (font == nullptr || maxWidth <= 0) {
      if (drawnHeight != nullptr) {
        *drawnHeight = 0;
      }
      return false;
    }

    const std::string renderText = sanitize_text_for_render(text);
    if (renderText.empty()) {
      if (drawnHeight != nullptr) {
        *drawnHeight = TTF_GetFontLineSkip(font);
      }
      return true;
    }

    SDL_Surface *surface = TTF_RenderText_Blended_Wrapped(font, renderText.c_str(), 0, color, static_cast<Uint32>(maxWidth));
    if (surface == nullptr) {
      return false;
    }

    return render_surface_line(renderer, surface, x, y, drawnHeight);
  }

  /**
   * @brief Draw wrapped text cropped to a panel using the texture source rectangle.
   *
   * @param renderer Active shell renderer.
   * @param font Text font.
   * @param text Text to wrap within the panel.
   * @param color Foreground color.
   * @param rect Maximum text bounds.
   * @param drawnHeight Receives the visible text height when requested.
   * @return True when the visible text rendered, or the bounds contain no visible text.
   */
  bool render_bounded_text(SDL_Renderer *renderer, TTF_Font *font, const std::string &text, SDL_Color color, const SDL_Rect &rect, int *drawnHeight = nullptr) {
    if (drawnHeight != nullptr) {
      *drawnHeight = 0;
    }
    if (rect.w <= 0 || rect.h <= 0 || text.empty()) {
      return true;
    }
    SDL_Surface *surface = TTF_RenderText_Blended_Wrapped(font, sanitize_text_for_render(text).c_str(), 0, color, static_cast<Uint32>(rect.w));
    if (surface == nullptr) {
      return false;
    }
    const SDL_Rect source {0, 0, surface->w, std::min(surface->h, rect.h)};
    const SDL_Rect destination {rect.x, rect.y, source.w, source.h};
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture == nullptr) {
      return false;
    }
    const bool success = ui::render_texture(renderer, texture, &source, &destination);
    destroy_texture(texture);
    if (drawnHeight != nullptr) {
      *drawnHeight = source.h;
    }
    return success;
  }

  std::string fit_single_line_text(TTF_Font *font, const std::string &text, int maxWidth) {
    if (font == nullptr || maxWidth <= 0) {
      return {};
    }

    const std::string sanitized = sanitize_ascii_text_for_render(text);
    if (sanitized.empty()) {
      return {};
    }

    int textWidth = 0;
    int textHeight = 0;
    if (TTF_GetStringSize(font, sanitized.c_str(), 0, &textWidth, &textHeight) && textWidth <= maxWidth) {
      return sanitized;
    }

    const std::string ellipsis = "...";
    for (std::size_t length = sanitized.size(); length > 0U; --length) {
      const std::string candidate = sanitize_ascii_text_for_render(std::string_view(sanitized).substr(0, length)) + ellipsis;
      if (TTF_GetStringSize(font, candidate.c_str(), 0, &textWidth, &textHeight) && textWidth <= maxWidth) {
        return candidate;
      }
    }

    return ellipsis;
  }

  bool render_text_line_simple(  // NOSONAR(cpp:S107) helper keeps the SDL text rendering callsite contract explicit
    SDL_Renderer *renderer,
    TTF_Font *font,
    const std::string &text,
    SDL_Color color,
    int x,
    int y,
    int maxWidth,
    int *drawnHeight = nullptr
  ) {
    if (font == nullptr || maxWidth <= 0) {
      if (drawnHeight != nullptr) {
        *drawnHeight = 0;
      }
      return false;
    }

    const std::string renderText = fit_single_line_text(font, text, maxWidth);
    if (renderText.empty()) {
      if (drawnHeight != nullptr) {
        *drawnHeight = TTF_GetFontLineSkip(font);
      }
      return true;
    }

    SDL_Surface *surface = TTF_RenderText_Blended(font, renderText.c_str(), 0, color);
    if (surface == nullptr) {
      return false;
    }

    return render_surface_line(renderer, surface, x, y, drawnHeight);
  }

  /**
   * @brief Builds or reuses a cached wrapped text texture for keypad modal content.
   *
   * @param renderer SDL renderer used to create textures.
   * @param font Font used to render the text.
   * @param text Source text to cache.
   * @param color Text color used for rendering.
   * @param maxWidth Maximum wrapped width in pixels.
   * @param cache Cache entry to populate or reuse.
   * @return True when the wrapped texture is ready for use.
   */
  bool ensure_wrapped_text_texture(
    SDL_Renderer *renderer,
    TTF_Font *font,
    const std::string &text,
    SDL_Color color,
    int maxWidth,
    CachedTextTexture *cache
  ) {
    if (font == nullptr || maxWidth <= 0 || cache == nullptr) {
      return false;
    }

    const std::string renderText = sanitize_text_for_render(text);
    if (
      cache->texture != nullptr && cache->wrapped && cache->font == font && cache->sourceText == text && cache->renderedText == renderText && cache->maxWidth == maxWidth &&
      colors_match(cache->color, color)
    ) {
      return true;
    }

    clear_cached_text_texture(cache);
    cache->font = font;
    cache->sourceText = text;
    cache->renderedText = renderText;
    cache->maxWidth = maxWidth;
    cache->color = color;
    cache->wrapped = true;
    cache->height = renderText.empty() ? TTF_GetFontLineSkip(font) : 0;

    if (renderText.empty()) {
      return true;
    }

    SDL_Surface *surface = TTF_RenderText_Blended_Wrapped(font, renderText.c_str(), 0, color, static_cast<Uint32>(maxWidth));
    if (surface == nullptr) {
      cache->height = TTF_GetFontLineSkip(font);
      return false;
    }

    cache->width = surface->w;
    cache->height = surface->h;
    cache->texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (cache->texture == nullptr) {
      cache->width = 0;
      cache->height = TTF_GetFontLineSkip(font);
      return false;
    }

    return true;
  }

  /**
   * @brief Builds or reuses a cached single-line text texture for keypad button labels.
   *
   * @param renderer SDL renderer used to create textures.
   * @param font Font used to render the text.
   * @param text Source text to cache.
   * @param color Text color used for rendering.
   * @param maxWidth Maximum label width in pixels.
   * @param cache Cache entry to populate or reuse.
   * @return True when the single-line texture is ready for use.
   */
  bool ensure_single_line_text_texture(
    SDL_Renderer *renderer,
    TTF_Font *font,
    const std::string &text,
    SDL_Color color,
    int maxWidth,
    CachedTextTexture *cache
  ) {
    if (font == nullptr || maxWidth <= 0 || cache == nullptr) {
      return false;
    }

    const std::string renderText = fit_single_line_text(font, text, maxWidth);
    if (
      cache->texture != nullptr && !cache->wrapped && cache->font == font && cache->sourceText == text && cache->renderedText == renderText && cache->maxWidth == maxWidth &&
      colors_match(cache->color, color)
    ) {
      return true;
    }

    clear_cached_text_texture(cache);
    cache->font = font;
    cache->sourceText = text;
    cache->renderedText = renderText;
    cache->maxWidth = maxWidth;
    cache->color = color;
    cache->wrapped = false;
    cache->height = renderText.empty() ? TTF_GetFontLineSkip(font) : 0;

    if (renderText.empty()) {
      return true;
    }

    SDL_Surface *surface = TTF_RenderText_Blended(font, renderText.c_str(), 0, color);
    if (surface == nullptr) {
      cache->height = TTF_GetFontLineSkip(font);
      return false;
    }

    cache->width = surface->w;
    cache->height = surface->h;
    cache->texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (cache->texture == nullptr) {
      cache->width = 0;
      cache->height = TTF_GetFontLineSkip(font);
      return false;
    }

    return true;
  }

  /**
   * @brief Draws a cached text texture at a fixed point, using stored height for blank lines.
   *
   * @param renderer SDL renderer used for drawing.
   * @param cache Cached text texture to render.
   * @param x Left destination coordinate.
   * @param y Top destination coordinate.
   * @param drawnHeight Receives the rendered or reserved text height.
   * @return True when drawing succeeded or no texture was required.
   */
  bool render_cached_text_texture(SDL_Renderer *renderer, const CachedTextTexture &cache, int x, int y, int *drawnHeight = nullptr) {
    if (cache.texture == nullptr) {
      if (drawnHeight != nullptr) {
        *drawnHeight = cache.height;
      }
      return true;
    }

    const SDL_Rect destination {x, y, cache.width, cache.height};
    const bool rendered = ui::render_texture(renderer, cache.texture, nullptr, &destination);
    if (drawnHeight != nullptr) {
      *drawnHeight = cache.height;
    }
    return rendered;
  }

  /**
   * @brief Draws a cached single-line text texture centered within a button rect.
   *
   * @param renderer SDL renderer used for drawing.
   * @param cache Cached text texture to render.
   * @param rect Button rectangle that should contain the rendered text.
   * @param drawnHeight Receives the rendered or reserved text height.
   * @return True when drawing succeeded or no texture was required.
   */
  bool render_cached_centered_text_texture(SDL_Renderer *renderer, const CachedTextTexture &cache, const SDL_Rect &rect, int *drawnHeight = nullptr) {
    if (cache.texture == nullptr) {
      if (drawnHeight != nullptr) {
        *drawnHeight = cache.height;
      }
      return true;
    }

    const SDL_Rect destination {
      rect.x + std::max(0, (rect.w - cache.width) / 2),
      rect.y + std::max(0, (rect.h - cache.height) / 2),
      cache.width,
      cache.height,
    };
    const bool rendered = ui::render_texture(renderer, cache.texture, nullptr, &destination);
    if (drawnHeight != nullptr) {
      *drawnHeight = cache.height;
    }
    return rendered;
  }

  bool render_text_centered_simple(
    SDL_Renderer *renderer,
    TTF_Font *font,
    const std::string &text,
    SDL_Color color,
    const SDL_Rect &rect,
    int *drawnHeight = nullptr
  ) {
    const std::string renderText = fit_single_line_text(font, text, rect.w);
    if (renderText.empty()) {
      if (drawnHeight != nullptr) {
        *drawnHeight = TTF_GetFontLineSkip(font);
      }
      return true;
    }

    int textWidth = 0;
    int textHeight = 0;
    if (!TTF_GetStringSize(font, renderText.c_str(), 0, &textWidth, &textHeight)) {
      return render_text_line_simple(renderer, font, renderText, color, rect.x + 8, rect.y + 8, std::max(1, rect.w - 16), drawnHeight);
    }

    const int x = rect.x + std::max(0, (rect.w - textWidth) / 2);
    const int y = rect.y + std::max(0, (rect.h - textHeight) / 2);
    return render_text_line_simple(renderer, font, renderText, color, x, y, rect.w, drawnHeight);
  }

  int measure_wrapped_text_height(TTF_Font *font, const std::string &text, int maxWidth) {
    if (font == nullptr || maxWidth <= 0) {
      return 0;
    }

    const std::string renderText = sanitize_text_for_render(text);
    if (renderText.empty()) {
      return TTF_GetFontLineSkip(font);
    }

    SDL_Surface *surface = TTF_RenderText_Blended_Wrapped(font, renderText.c_str(), 0, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, static_cast<Uint32>(maxWidth));
    if (surface == nullptr) {
      return TTF_GetFontLineSkip(font);
    }

    const int height = surface->h;
    SDL_DestroySurface(surface);
    return height;
  }

  /**
   * @brief Reuses keypad modal text measurements until the content or width changes.
   *
   * @param renderer SDL renderer used when cached text textures must be rebuilt.
   * @param font Font used to measure and render the keypad body text.
   * @param viewModel Shell view model containing the keypad modal content.
   * @param modalInnerWidth Available wrapped-text width inside the modal.
   * @param cache Cache entry that stores the last measured keypad modal layout.
   * @return Total pixel height required for the keypad modal body text.
   */
  int keypad_modal_text_height(
    SDL_Renderer *renderer,
    TTF_Font *font,
    const ui::ShellViewModel &viewModel,
    int modalInnerWidth,
    KeypadModalLayoutCache *cache
  ) {
    if (cache != nullptr && cache->modalInnerWidth == modalInnerWidth && cache->lines == viewModel.keypad.lines) {
      return cache->modalTextHeight;
    }

    if (cache != nullptr) {
      if (cache->lineTextures.size() > viewModel.keypad.lines.size()) {
        for (std::size_t index = viewModel.keypad.lines.size(); index < cache->lineTextures.size(); ++index) {
          clear_cached_text_texture(&cache->lineTextures[index]);
        }
      }
      cache->lineTextures.resize(viewModel.keypad.lines.size());
    }

    int modalTextHeight = 0;
    for (std::size_t index = 0; index < viewModel.keypad.lines.size(); ++index) {
      const std::string &line = viewModel.keypad.lines[index];
      if (cache != nullptr) {
        if (!ensure_wrapped_text_texture(renderer, font, line, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, modalInnerWidth, &cache->lineTextures[index])) {
          modalTextHeight += measure_wrapped_text_height(font, line, modalInnerWidth) + 6;
          continue;
        }
        modalTextHeight += cache->lineTextures[index].height + 6;
        continue;
      }

      modalTextHeight += measure_wrapped_text_height(font, line, modalInnerWidth) + 6;
    }

    if (cache != nullptr) {
      cache->modalInnerWidth = modalInnerWidth;
      cache->modalTextHeight = modalTextHeight;
      cache->lines = viewModel.keypad.lines;
    }

    return modalTextHeight;
  }

  bool render_text_centered(
    SDL_Renderer *renderer,
    TTF_Font *font,
    const std::string &text,
    SDL_Color color,
    const SDL_Rect &rect,
    int *drawnHeight = nullptr
  ) {
    int textWidth = 0;
    int textHeight = 0;
    if (!TTF_GetStringSize(font, text.c_str(), 0, &textWidth, &textHeight)) {
      return render_text_line(renderer, font, text, color, rect.x + 8, rect.y + 8, rect.w - 16, drawnHeight);
    }

    const int x = rect.x + std::max(0, (rect.w - textWidth) / 2);
    const int y = rect.y + std::max(0, (rect.h - textHeight) / 2);
    return render_text_line(renderer, font, text, color, x, y, rect.w, drawnHeight);
  }

  /**
   * @brief Fit cached artwork or a screenshot inside a square-pixel shell rectangle without cropping.
   *
   * @param renderer Active shell renderer.
   * @param texture Loaded image texture.
   * @param rect Available logical bounds before encoder correction.
   * @return True when the fitted image was copied successfully.
   */
  bool render_texture_fit(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect &rect) {
    if (renderer == nullptr || texture == nullptr || rect.w <= 0 || rect.h <= 0) {
      return false;
    }

    float width = 0;
    float height = 0;
    if (!SDL_GetTextureSize(texture, &width, &height) || width <= 0 || height <= 0) {
      return false;
    }

    const SDL_Rect destination = sdl_rect(ui::fit_screenshot({static_cast<int>(width), static_cast<int>(height)}, {rect.x, rect.y, rect.w, rect.h}));

    return ui::render_texture(renderer, texture, nullptr, &destination);
  }

  bool render_texture_fill(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect &rect) {
    if (renderer == nullptr || texture == nullptr || rect.w <= 0 || rect.h <= 0) {
      return false;
    }

    float width = 0;
    float height = 0;
    if (!SDL_GetTextureSize(texture, &width, &height) || width <= 0 || height <= 0) {
      return false;
    }

    const auto textureWidth = static_cast<int>(width);
    const auto textureHeight = static_cast<int>(height);
    SDL_Rect source {0, 0, textureWidth, textureHeight};
    if ((textureWidth * rect.h) > (textureHeight * rect.w)) {
      source.w = std::max(1, (textureHeight * rect.w) / rect.h);
      source.x = std::max(0, (textureWidth - source.w) / 2);
    } else {
      source.h = std::max(1, (textureWidth * rect.h) / rect.w);
      source.y = std::max(0, (textureHeight - source.h) / 2);
    }

    return ui::render_texture(renderer, texture, &source, &rect);
  }

  /**
   * @brief Draw a cached icon with optional clockwise rotation and a static fallback.
   *
   * @param renderer Active renderer, including the software fallback.
   * @param cache Cached SVG textures.
   * @param relativePath Bundled icon path.
   * @param rect Icon bounds.
   * @param angle Clockwise rotation in degrees; zero leaves the icon stationary.
   * @return True when the rotated or stationary icon rendered successfully.
   */
  bool render_asset_icon(SDL_Renderer *renderer, AssetTextureCache *cache, const std::string &relativePath, const SDL_Rect &rect, double angle = 0.0) {
    SDL_Texture *texture = load_cached_asset_texture(renderer, cache, relativePath, rect.w, rect.h);
    if (texture == nullptr) {
      return false;
    }

    const auto destination = ui::render_bounds(rect);
    if (angle != 0.0 && SDL_RenderTextureRotated(renderer, texture, nullptr, &destination, angle, nullptr, SDL_FLIP_NONE)) {
      return true;
    }

    return render_texture_fit(renderer, texture, rect);
  }

  /**
   * @brief Render a calm Lucide placeholder when host artwork is unavailable.
   *
   * @param renderer Active shell renderer.
   * @param labelFont Font used if the bundled placeholder icon cannot be loaded.
   * @param tile Application name used for placeholder selection and tint.
   * @param rect Portrait cover bounds.
   * @param assetCache Cache for bundled Lucide textures.
   * @return True when the placeholder icon or fallback label rendered.
   */
  bool render_default_app_cover(
    SDL_Renderer *renderer,
    TTF_Font *labelFont,
    const ui::ShellAppTile &tile,
    const SDL_Rect &rect,
    AssetTextureCache *assetCache
  ) {
    const SDL_Color seedColor = placeholder_color(tile.name);
    fill_rect(renderer, rect, PANEL_ALT_RED, PANEL_ALT_GREEN, PANEL_ALT_BLUE);
    // A subdued tint distinguishes missing covers without imitating game artwork.
    fill_rect(renderer, rect, seedColor.r, seedColor.g, seedColor.b, 0x24);
    fill_rect(renderer, {rect.x, rect.y + rect.h - 3, rect.w, 3}, seedColor.r, seedColor.g, seedColor.b);
    const int iconSize = std::max(24, rect.w / 2);
    const SDL_Rect icon {rect.x + (rect.w - iconSize) / 2, rect.y + (rect.h - iconSize) / 2, iconSize, iconSize};
    const std::string iconPath = tile.name == "Desktop" ? "icons\\monitor.svg" : "icons\\gamepad-2.svg";
    if (render_asset_icon(renderer, assetCache, iconPath, icon)) {
      return true;
    }
    return render_text_centered_simple(renderer, labelFont, app_monogram(tile), {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, icon);
  }

  void draw_line(SDL_Renderer *renderer, int x1, int y1, int x2, int y2, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha = 0xFF) {  // NOSONAR(cpp:S107) low-level draw helper intentionally mirrors SDL primitive arguments
    SDL_SetRenderDrawColor(renderer, red, green, blue, alpha);
    SDL_RenderLine(renderer, x1, y1, x2, y2);
  }

  /**
   * @brief Render unobscured cover art with a focus ring and optional status badge.
   *
   * @param renderer Active shell renderer.
   * @param labelFont Font for badges and artwork fallback.
   * @param tile Application metadata.
   * @param rect Portrait cover bounds, excluding its caption.
   * @param textureCache Cached host cover art.
   * @param assetCache Cached bundled artwork.
   * @return True when the cover and badge rendered successfully.
   */
  bool render_app_cover(
    SDL_Renderer *renderer,
    TTF_Font *labelFont,
    const ui::ShellAppTile &tile,
    const SDL_Rect &rect,
    CoverArtTextureCache *textureCache,
    AssetTextureCache *assetCache
  ) {
    fill_rect(renderer, rect, PANEL_ALT_RED, PANEL_ALT_GREEN, PANEL_ALT_BLUE, 0xFF);
    if (SDL_Texture *texture = tile.boxArtCached ? load_cover_art_texture(renderer, textureCache, tile.boxArtCacheKey) : nullptr; texture != nullptr) {
      if (!render_texture_fill(renderer, texture, rect)) {
        return false;
      }
    } else {
      if (!render_default_app_cover(renderer, labelFont, tile, rect, assetCache)) {
        return false;
      }
    }

    // Captions live below the artwork; never cover game titles baked into the image.
    if (tile.selected) {
      for (int inset = 1; inset <= 3; ++inset) {
        draw_rect(renderer, {rect.x - inset, rect.y - inset, rect.w + inset * 2, rect.h + inset * 2}, ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE);
      }
    }
    if (tile.running) {
      const int size = std::min(42, std::max(24, rect.w / 3));
      const SDL_Rect paused {rect.x + rect.w - size - 8, rect.y + rect.h - size - 8, size, size};
      fill_rounded_rect(renderer, paused, {BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xEF}, 6);
      render_asset_icon(renderer, assetCache, "icons\\pause.svg", {paused.x + 4, paused.y + 4, size - 8, size - 8});
    }
    const std::string badge = tile.badgeLabel;
    if (!badge.empty()) {
      const int badgeHeight = TTF_GetFontLineSkip(labelFont) + 8;
      const int badgeWidth = std::min(rect.w - 12, static_cast<int>(badge.size()) * std::max(7, TTF_GetFontHeight(labelFont) / 2) + 16);
      const SDL_Rect badgeRect {rect.x + 6, rect.y + 6, badgeWidth, badgeHeight};
      fill_rounded_rect(renderer, badgeRect, {BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xEF}, 4);
      if (!render_text_centered_simple(renderer, labelFont, badge, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, badgeRect)) {
        return false;
      }
    }
    return true;
  }

  SDL_Rect log_viewer_rect(const ui::ShellViewModel &viewModel, int screenWidth, int screenHeight, int outerMargin) {
    const int fullWidth = screenWidth - (outerMargin * 2);
    const int dockedWidth = std::min(fullWidth, std::max(420, (screenWidth - (outerMargin * 3)) / 2));
    const int height = screenHeight - (outerMargin * 2);

    switch (viewModel.logViewer.placement) {
      case app::LogViewerPlacement::left:
        return {outerMargin, outerMargin, dockedWidth, height};
      case app::LogViewerPlacement::right:
        return {screenWidth - outerMargin - dockedWidth, outerMargin, dockedWidth, height};
      case app::LogViewerPlacement::full:
        return {outerMargin, outerMargin, fullWidth, height};
    }

    return {outerMargin, outerMargin, fullWidth, height};
  }

  struct LogViewerLayout {
    std::vector<const std::string *> visibleLines;
    std::size_t firstVisibleIndex = 0U;
  };

  LogViewerLayout build_log_viewer_layout(const ui::ShellViewModel &viewModel, TTF_Font *font, int availableWidth, int availableHeight, std::size_t clampedOffset) {
    LogViewerLayout layout {};
    if (viewModel.logViewer.lines.empty()) {
      layout.visibleLines.push_back(nullptr);
      return layout;
    }

    int usedHeight = 0;
    std::size_t endIndex = viewModel.logViewer.lines.size() > clampedOffset ? viewModel.logViewer.lines.size() - clampedOffset : 0U;
    layout.firstVisibleIndex = endIndex;
    while (endIndex > 0U) {
      const std::string renderedLine = truncate_text_for_render(viewModel.logViewer.lines[endIndex - 1U], LOG_VIEWER_MAX_RENDER_CHARACTERS);
      const int lineHeight = measure_wrapped_text_height(font, renderedLine, std::max(1, availableWidth - 12)) + 4;
      if (!layout.visibleLines.empty() && usedHeight + lineHeight > availableHeight - 8) {
        break;
      }
      layout.visibleLines.push_back(&viewModel.logViewer.lines[endIndex - 1U]);
      usedHeight += lineHeight;
      --endIndex;
    }
    layout.firstVisibleIndex = endIndex;
    std::reverse(layout.visibleLines.begin(), layout.visibleLines.end());
    return layout;
  }

  bool render_log_viewer_lines(SDL_Renderer *renderer, TTF_Font *smallFont, const ui::ShellViewModel &viewModel, const SDL_Rect &textRect, const LogViewerLayout &layout) {
    int contentCursorY = textRect.y + 6;
    if (layout.firstVisibleIndex > 0U) {
      if (!render_text_line_simple(renderer, smallFont, "Earlier lines above", {ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF}, textRect.x + 6, contentCursorY, std::max(1, textRect.w - 12))) {
        return false;
      }
      contentCursorY += TTF_GetFontLineSkip(smallFont) + 4;
    }

    if (layout.visibleLines.size() == 1U && layout.visibleLines.front() == nullptr) {
      if (!render_text_line_simple(renderer, smallFont, "The log file is empty.", {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, textRect.x + 6, contentCursorY, std::max(1, textRect.w - 12))) {
        return false;
      }
    } else {
      for (const std::string *line : layout.visibleLines) {
        int drawnHeight = 0;
        if (!render_text_line_simple(renderer, smallFont, truncate_text_for_render(*line, LOG_VIEWER_MAX_RENDER_CHARACTERS), {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, textRect.x + 6, contentCursorY, std::max(1, textRect.w - 12), &drawnHeight)) {
          return false;
        }
        contentCursorY += drawnHeight + 4;
      }
    }

    if (viewModel.logViewer.scrollOffset > 0U) {
      return render_text_line_simple(renderer, smallFont, "Newer lines below", {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, textRect.x + 6, std::max(textRect.y + 6, textRect.y + textRect.h - TTF_GetFontLineSkip(smallFont) - 6), std::max(1, textRect.w - 12));
    }
    return true;
  }

  void render_vertical_scrollbar(SDL_Renderer *renderer, const SDL_Rect &trackRect, int totalItemCount, int visibleItemCount, int startItemIndex) {
    if (
      renderer == nullptr || trackRect.w <= 0 || trackRect.h <= 0 || totalItemCount <= visibleItemCount || visibleItemCount <= 0
    ) {
      return;
    }

    fill_rect(renderer, trackRect, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xA0);
    draw_rect(renderer, trackRect, TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0x30);

    const int thumbHeight = std::max(24, (trackRect.h * visibleItemCount) / totalItemCount);
    const int maxThumbTravel = std::max(0, trackRect.h - thumbHeight);
    const int maxStartItem = std::max(1, totalItemCount - visibleItemCount);
    const int clampedStartItem = std::clamp(startItemIndex, 0, std::max(0, totalItemCount - visibleItemCount));
    const int thumbY = trackRect.y + ((maxThumbTravel * clampedStartItem) / maxStartItem);
    const SDL_Rect thumbRect {trackRect.x + 1, thumbY, std::max(1, trackRect.w - 2), thumbHeight};
    fill_rect(renderer, thumbRect, ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xD0);
  }

  /**
   * @brief Display diagnostics in the shared opaque rounded dialog surface.
   *
   * @param renderer Active renderer.
   * @param bodyFont Dialog heading font.
   * @param smallFont Diagnostic text font.
   * @param assetCache Cached controller glyphs.
   * @param viewModel Log content and pane placement.
   * @param screenWidth Logical display width.
   * @param screenHeight Framebuffer height.
   * @param outerMargin Television-safe inset.
   * @param hints Shared input hint size and spacing.
   * @return True when the viewer rendered successfully.
   */
  bool render_log_viewer_modal(  // NOSONAR(cpp:S107) modal rendering keeps the layout inputs explicit
    SDL_Renderer *renderer,
    TTF_Font *bodyFont,
    TTF_Font *smallFont,
    AssetTextureCache *assetCache,
    const ui::ShellViewModel &viewModel,
    int screenWidth,
    int screenHeight,
    int outerMargin,
    FooterHintStyle hints
  ) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    fill_rect(renderer, {0, 0, screenWidth, screenHeight}, 0x00, 0x00, 0x00, 0xA6);

    const SDL_Rect modalRect = log_viewer_rect(viewModel, screenWidth, screenHeight, outerMargin);
    render_dialog_surface(renderer, modalRect);

    if (!render_text_line_simple(renderer, bodyFont, viewModel.modal.title, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, modalRect.x + 18, modalRect.y + 28, modalRect.w - 36)) {
      return false;
    }

    int pathHeight = 0;
    if (!render_text_line_simple(renderer, smallFont, "Path: " + viewModel.logViewer.path, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, modalRect.x + 18, modalRect.y + 64, modalRect.w - 36, &pathHeight)) {
      return false;
    }

    const int hintHeight = measure_footer_actions_height(smallFont, viewModel.modal.footerActions, modalRect.w - 36, hints);
    if (const SDL_Rect hintRect {modalRect.x + 18, modalRect.y + 64 + pathHeight + 8, modalRect.w - 36, hintHeight}; !render_footer_actions(renderer, smallFont, assetCache, viewModel.modal.footerActions, hintRect, hints)) {
      return false;
    }

    const int contentBottom = modalRect.y + modalRect.h - 18;
    const int requestedContentY = modalRect.y + 64 + pathHeight + hintHeight + 18;
    const int contentAreaY = std::min(requestedContentY, std::max(modalRect.y + 72, contentBottom - 1));
    const SDL_Rect contentRect {
      modalRect.x + 18,
      contentAreaY,
      std::max(1, modalRect.w - 36),
      std::max(1, contentBottom - contentAreaY),
    };
    fill_rect(renderer, contentRect, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0x70);

    const std::size_t maxOffset = viewModel.logViewer.lines.size() > 1U ? viewModel.logViewer.lines.size() - 1U : 0U;
    const std::size_t clampedOffset = std::min(viewModel.logViewer.scrollOffset, maxOffset);

    constexpr int logViewerScrollbarWidth = 10;
    constexpr int logViewerScrollbarGap = 12;
    LogViewerLayout logViewerLayout = build_log_viewer_layout(viewModel, smallFont, contentRect.w, contentRect.h, clampedOffset);
    const bool overflow = !viewModel.logViewer.lines.empty() && viewModel.logViewer.lines.size() > logViewerLayout.visibleLines.size();
    if (overflow) {
      logViewerLayout = build_log_viewer_layout(viewModel, smallFont, std::max(1, contentRect.w - logViewerScrollbarWidth - logViewerScrollbarGap), contentRect.h, clampedOffset);
    }

    if (const SDL_Rect textRect {
          contentRect.x,
          contentRect.y,
          std::max(1, contentRect.w - (overflow ? logViewerScrollbarWidth + logViewerScrollbarGap : 0)),
          contentRect.h,
        };
        !render_log_viewer_lines(renderer, smallFont, viewModel, textRect, logViewerLayout)) {
      return false;
    }

    if (overflow) {
      render_vertical_scrollbar(
        renderer,
        {contentRect.x + contentRect.w - logViewerScrollbarWidth, contentRect.y, logViewerScrollbarWidth, contentRect.h},
        static_cast<int>(viewModel.logViewer.lines.size()),
        static_cast<int>(std::max<std::size_t>(1U, logViewerLayout.visibleLines.size())),
        static_cast<int>(logViewerLayout.firstVisibleIndex)
      );
    }

    return true;
  }

  /**
   * @brief Optional icon, focus and retained-cache settings for action rows.
   */
  struct ActionRowsOptions {
    AssetTextureCache *assetCache = nullptr;  ///< Shared action icon cache.
    bool active = true;  ///< Whether the list owns controller focus.
    FolderDialogCache *folder = nullptr;  ///< Retained row-label and scroll state.
    bool repaint = false;  ///< Restore only rows affected by a focus change.
  };

  /**
   * @brief Geometry and visible indices for a retained action-list repaint.
   */
  struct ActionRowsLayout {
    SDL_Rect bounds;  ///< Complete list bounds.
    int rowHeight;  ///< Single row height.
    int rowStep;  ///< Distance between successive rows.
    int contentWidth;  ///< Width excluding the scrollbar.
    std::size_t startIndex;  ///< First visible row.
    std::size_t endIndex;  ///< One past the last visible row.
    std::size_t selectedIndex;  ///< Newly focused row.
    bool windowChanged;  ///< The visible row range moved.
    std::size_t visibleRowCount;  ///< Maximum number of rows in the viewport.
    bool overflow;  ///< Whether the list needs a scrollbar.
    int scrollBarWidth;  ///< Scrollbar width, or zero when all rows fit.
  };

  /**
   * @brief Erase stale focus backgrounds before repainting retained action rows.
   * @param renderer Dialog renderer.
   * @param folder Previous focus and scroll state.
   * @param layout Current visible row geometry.
   */
  void restore_action_row_backgrounds(SDL_Renderer *renderer, const FolderDialogCache &folder, const ActionRowsLayout &layout) {
    if (layout.windowChanged) {
      fill_rect(renderer, layout.bounds, 0x15, 0x1E, 0x2E);
    } else {
      for (const auto index : {folder.selected, layout.selectedIndex}) {
        if (index >= layout.startIndex && index < layout.endIndex) {
          fill_rect(renderer, {layout.bounds.x, layout.bounds.y + static_cast<int>(index - layout.startIndex) * layout.rowStep, layout.contentWidth, layout.rowHeight}, 0x15, 0x1E, 0x2E);
        }
      }
    }
  }

  /**
   * @brief Find the first focused action, using the first row when no focus is marked.
   * @param rows Dialog actions.
   * @return Focused row index, or zero.
   */
  std::size_t selected_action_index(const std::vector<ui::ShellActionRow> &rows) {
    const auto found = std::find_if(rows.begin(), rows.end(), [](const ui::ShellActionRow &row) {
      return row.selected;
    });
    return found == rows.end() ? 0 : static_cast<std::size_t>(found - rows.begin());
  }

  /**
   * @brief Calculate the visible action range while retaining folder-browser scroll position.
   * @param rows Nonempty action list.
   * @param rect List bounds.
   * @param rowHeight Single row height.
   * @param folder Previous folder viewport, or nullptr for centered ordinary menus.
   * @return Row geometry, selected index and scrollbar state.
   */
  ActionRowsLayout action_rows_layout(const std::vector<ui::ShellActionRow> &rows, const SDL_Rect &rect, int rowHeight, const FolderDialogCache *folder) {
    constexpr int rowSpacing = 6;
    const int rowStep = rowHeight + rowSpacing;
    const auto visible = static_cast<std::size_t>(std::max(1, (rect.h + rowSpacing) / std::max(1, rowStep)));
    const auto selected = selected_action_index(rows);
    std::size_t first = 0;
    if (rows.size() > visible) {
      first = selected > visible / 2 ? selected - visible / 2 : 0;
      first = std::min(first, rows.size() - visible);
    }
    if (folder != nullptr) {
      first = ui::stable_list_first_row(rows.size(), selected, visible, folder->first);
    }
    const bool changed = folder != nullptr && first != folder->first;
    const bool overflow = rows.size() > visible;
    const int barWidth = overflow ? 8 : 0;
    const int contentWidth = std::max(1, rect.w - (overflow ? barWidth + 10 : 0));
    return {rect, rowHeight, rowStep, contentWidth, first, std::min(rows.size(), first + visible), selected, changed, visible, overflow, barWidth};
  }

  /**
   * @brief Release cached row labels that have scrolled out of view.
   * @param folder Retained row textures.
   * @param layout Current visible range.
   */
  void prune_action_row_labels(FolderDialogCache &folder, const ActionRowsLayout &layout) {
    for (auto it = folder.labels.begin(); it != folder.labels.end();) {
      if (it->first < layout.startIndex || it->first >= layout.endIndex) {
        clear_cached_text_texture(&it->second);
        it = folder.labels.erase(it);
      } else {
        ++it;
      }
    }
  }

  /**
   * @brief Render one action's focus, icon, checkbox and retained or ordinary label.
   * @param renderer Dialog renderer.
   * @param font Action font.
   * @param row Action content.
   * @param index Row index in the complete list.
   * @param layout Visible row geometry.
   * @param options Icon, focus and cache settings.
   * @return True when the label rendered successfully.
   */
  bool render_action_row(SDL_Renderer *renderer, TTF_Font *font, const ui::ShellActionRow &row, std::size_t index, const ActionRowsLayout &layout, const ActionRowsOptions &options) {
    auto *const assetCache = options.assetCache;
    const bool active = options.active;
    auto *const folder = options.folder;
    const int y = layout.bounds.y + static_cast<int>(index - layout.startIndex) * layout.rowStep;
    const SDL_Rect rowRect {layout.bounds.x, y, layout.contentWidth, layout.rowHeight};
    if (row.selected) {
      fill_rounded_rect(renderer, rowRect, {ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, static_cast<Uint8>(active ? 0x28 : 0x12)}, 7);
      if (active) {
        fill_rect(renderer, {rowRect.x, rowRect.y + 8, 3, rowRect.h - 16}, ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE);
      }
    }

    int labelX = rowRect.x + 12;
    if (assetCache != nullptr && !row.iconAssetPath.empty()) {
      const int iconSize = std::min(24, rowRect.h - 16);
      render_asset_icon(renderer, assetCache, row.iconAssetPath, {labelX, rowRect.y + (rowRect.h - iconSize) / 2, iconSize, iconSize});
      labelX += iconSize + 10;
    }
    const int checkboxSize = row.checkable ? std::min(22, rowRect.h - 16) : 0;
    if (row.checkable) {
      const SDL_Rect checkbox {rowRect.x + rowRect.w - checkboxSize - 14, rowRect.y + (rowRect.h - checkboxSize) / 2, checkboxSize, checkboxSize};
      if (row.checked) {
        fill_rounded_rect(renderer, checkbox, {ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF}, 4);
        for (int stroke = 0; stroke < 2; ++stroke) {
          draw_line(renderer, checkbox.x + checkboxSize / 4, checkbox.y + checkboxSize / 2 + stroke, checkbox.x + checkboxSize * 5 / 12, checkbox.y + checkboxSize * 2 / 3 + stroke, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE);
          draw_line(renderer, checkbox.x + checkboxSize * 5 / 12, checkbox.y + checkboxSize * 2 / 3 + stroke, checkbox.x + checkboxSize * 3 / 4, checkbox.y + checkboxSize / 3 + stroke, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE);
        }
      } else {
        draw_rounded_rect_outline(renderer, checkbox, {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, 4);
      }
    }
    const SDL_Color color = row.enabled ? SDL_Color {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF} : SDL_Color {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF};
    const int labelWidth = rowRect.x + rowRect.w - labelX - 12 - (row.checkable ? checkboxSize + 16 : 0);
    const int labelY = rowRect.y + (rowRect.h - TTF_GetFontLineSkip(font)) / 2;
    if (folder != nullptr) {
      auto &label = folder->labels[index];
      if (!ensure_single_line_text_texture(renderer, font, row.label, color, labelWidth, &label) || !render_cached_text_texture(renderer, label, labelX, labelY)) {
        return false;
      }
    } else if (!render_text_line_simple(renderer, font, row.label, color, labelX, labelY, labelWidth)) {
      return false;
    }
    return true;
  }

  /**
   * @brief Render visible action rows using retained labels for folder navigation.
   * @param renderer Dialog renderer.
   * @param font Action font.
   * @param rows Complete action list.
   * @param rect List bounds.
   * @param rowHeight Single row height.
   * @param options Icon, focus and retained-cache settings.
   * @return True when visible labels rendered successfully.
   */
  bool render_action_rows(SDL_Renderer *renderer, TTF_Font *font, const std::vector<ui::ShellActionRow> &rows, const SDL_Rect &rect, int rowHeight, const ActionRowsOptions &options = {}) {
    if (rows.empty()) {
      return true;
    }
    const auto layout = action_rows_layout(rows, rect, rowHeight, options.folder);
    auto *const folder = options.folder;
    if (folder != nullptr) {
      if (options.repaint) {
        restore_action_row_backgrounds(renderer, *folder, layout);
      }
      prune_action_row_labels(*folder, layout);
    }
    for (std::size_t index = layout.startIndex; index < layout.endIndex; ++index) {
      if (folder != nullptr && options.repaint && !layout.windowChanged && index != folder->selected && index != layout.selectedIndex) {
        continue;
      }
      if (!render_action_row(renderer, font, rows[index], index, layout, options)) {
        return false;
      }
    }
    if (layout.overflow && (!options.repaint || layout.windowChanged)) {
      render_vertical_scrollbar(renderer, {rect.x + rect.w - layout.scrollBarWidth, rect.y, layout.scrollBarWidth, rect.h}, static_cast<int>(rows.size()), static_cast<int>(layout.visibleRowCount), static_cast<int>(layout.startIndex));
    }
    if (folder != nullptr) {
      folder->first = layout.startIndex;
      folder->selected = layout.selectedIndex;
    }
    return true;
  }

  /**
   * @brief Render a compact navigation card with a Lucide icon and visible focus.
   *
   * @param renderer Active shell renderer.
   * @param font Fallback glyph font.
   * @param smallFont Font for the action label.
   * @param assetCache Cached SVG textures.
   * @param button Navigation action and focus state.
   * @param buttonRect Available button bounds.
   * @return True when icon and label rendered successfully.
   */
  bool render_toolbar_button(
    SDL_Renderer *renderer,
    TTF_Font *font,
    TTF_Font *smallFont,
    AssetTextureCache *assetCache,
    const ui::ShellToolbarButton &button,
    const SDL_Rect &buttonRect
  ) {
    render_card_surface(renderer, buttonRect, button.selected);

    const int iconSize = std::max(18, buttonRect.h - 16);
    const SDL_Rect iconRect {buttonRect.x + 10, buttonRect.y + (buttonRect.h - iconSize) / 2, iconSize, iconSize};
    if (const bool renderedIcon = !button.iconAssetPath.empty() && render_asset_icon(renderer, assetCache, button.iconAssetPath, iconRect); !renderedIcon && !render_text_centered(renderer, font, button.glyph, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, iconRect)) {
      return false;
    }

    return render_text_line_simple(renderer, smallFont, button.label, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, iconRect.x + iconRect.w + 8, buttonRect.y + std::max(6, (buttonRect.h - TTF_GetFontLineSkip(smallFont)) / 2), buttonRect.w - (iconRect.w + 26));
  }

  /**
   * @brief Measured widths of an unboxed input hint.
   */
  struct FooterHintWidths {
    int primary;  ///< Primary glyph or keycap width.
    int secondary;  ///< Optional secondary glyph or keycap width.
    int total;  ///< Width of glyphs, spacing and the label.
  };

  constexpr int HINT_GLYPH_GAP = 4;  ///< Spacing between glyphs or between the last glyph and label.
  constexpr int HINT_ROW_GAP = 6;  ///< Spacing between wrapped rows.

  /**
   * @brief Measure text used in an input hint or keyboard keycap.
   * @param font Hint font.
   * @param label Null-terminated UTF-8 text to size.
   * @param length Label length in bytes, excluding the terminator.
   * @return Text width, with an approximate width if SDL_ttf cannot measure it.
   */
  int measure_footer_action_label_width(TTF_Font *font, const char *label, std::size_t length) {
    int width = 0;
    return TTF_GetStringSize(font, label, length, &width, nullptr) ? width : static_cast<int>(length) * 8;
  }

  /**
   * @brief Measure the button glyphs and label as one action.
   * @param font Hint font.
   * @param action Controller glyphs or keyboard labels and the action name.
   * @param glyphSize Height of the controller glyphs and keycaps.
   * @return Widths for rendering and wrapping.
   */
  FooterHintWidths measure_footer_hint(TTF_Font *font, const ui::ShellFooterAction &action, int glyphSize) {
    const auto glyph_width = [font, glyphSize](const std::string &key, bool hasAsset) {
      if (key.empty()) {
        return hasAsset ? glyphSize : 0;
      }
      return std::max(glyphSize, measure_footer_action_label_width(font, key.c_str(), key.size()) + 12);
    };
    const int primary = glyph_width(action.keyLabel, !action.iconAssetPath.empty());
    const int secondary = glyph_width(action.secondaryKeyLabel, !action.secondaryIconAssetPath.empty());
    const int label = measure_footer_action_label_width(font, action.label.c_str(), action.label.size());
    return {primary, secondary, primary + secondary + label + (primary > 0 ? HINT_GLYPH_GAP : 0) + (secondary > 0 ? HINT_GLYPH_GAP : 0)};
  }

  int measure_footer_actions_height(TTF_Font *font, const std::vector<ui::ShellFooterAction> &actions, int width, FooterHintStyle style) {
    if (width <= 0) {
      return 0;
    }
    const int rowHeight = std::max(style.glyphSize, TTF_GetFontLineSkip(font));
    const int gap = style.compact ? 10 : 18;
    int rows = 0;
    int used = 0;
    for (const auto &action : actions) {
      const int actionWidth = std::min(width, measure_footer_hint(font, action, style.glyphSize).total);
      if (rows == 0 || (used > 0 && used + gap + actionWidth > width)) {
        ++rows;
        used = 0;
      }
      used += (used == 0 ? 0 : gap) + actionWidth;
    }
    return rows == 0 ? 0 : rows * rowHeight + (rows - 1) * HINT_ROW_GAP;
  }

  /**
   * @brief Draw one unboxed input hint with centered glyphs and keycap outlines.
   * @param renderer Active renderer.
   * @param font Hint font.
   * @param assets Cached controller glyphs.
   * @param action Input and action label.
   * @param widths Measured glyph and label widths.
   * @param bounds Bounded region for the whole hint.
   * @param glyphSize Controller and keycap height.
   * @return True when the hint rendered successfully.
   */
  bool render_footer_hint(SDL_Renderer *renderer, TTF_Font *font, AssetTextureCache *assets, const ui::ShellFooterAction &action, const FooterHintWidths &widths, const SDL_Rect &bounds, int glyphSize) {
    int x = bounds.x;
    const int right = bounds.x + bounds.w;
    const auto draw_glyph = [&](const std::string &asset, const std::string &key, int requestedWidth) {
      const int width = std::min(requestedWidth, right - x);
      if (width <= 0) {
        return;
      }
      const SDL_Rect rect {x, bounds.y + (bounds.h - glyphSize) / 2, width, glyphSize};
      if (!key.empty()) {
        draw_rounded_rect_outline(renderer, rect, {0xB8, 0xC2, 0xD0, 0xFF}, 3);
        render_text_centered_simple(renderer, font, key, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, rect);
      } else {
        render_asset_icon(renderer, assets, asset, rect);
      }
      x += width + HINT_GLYPH_GAP;
    };
    draw_glyph(action.iconAssetPath, action.keyLabel, widths.primary);
    draw_glyph(action.secondaryIconAssetPath, action.secondaryKeyLabel, widths.secondary);
    return x >= right || render_text_line_simple(renderer, font, action.label, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, x, bounds.y + (bounds.h - TTF_GetFontLineSkip(font)) / 2, right - x);
  }

  /**
   * @brief Draw wrapped actions using the same sizes as the footer measurement.
   * @param renderer Active renderer.
   * @param font Hint font.
   * @param assetCache Cached controller glyphs.
   * @param actions Ordered input hints.
   * @param footerRect Available row bounds.
   * @param style Glyph size and spacing.
   * @return True when the visible hints rendered successfully.
   */
  bool render_footer_actions(
    SDL_Renderer *renderer,
    TTF_Font *font,
    AssetTextureCache *assetCache,
    const std::vector<ui::ShellFooterAction> &actions,
    const SDL_Rect &footerRect,
    FooterHintStyle style
  ) {
    if (footerRect.w <= 0) {
      return true;
    }
    const int right = footerRect.x + footerRect.w;
    const int rowHeight = std::max(style.glyphSize, TTF_GetFontLineSkip(font));
    const int gap = style.compact ? 10 : 18;
    int x = footerRect.x;
    int y = footerRect.y;
    for (const auto &action : actions) {
      const auto widths = measure_footer_hint(font, action, style.glyphSize);
      const int width = std::min(footerRect.w, widths.total);
      if (x + width > right && x > footerRect.x) {
        x = footerRect.x;
        y += rowHeight + HINT_ROW_GAP;
      }
      if (y + rowHeight > footerRect.y + footerRect.h) {
        break;
      }
      if (!render_footer_hint(renderer, font, assetCache, action, widths, {x, y, width, rowHeight}, style.glyphSize)) {
        return false;
      }
      x += width + gap;
    }
    return true;
  }

  /**
   * @brief Draw rounded notification cards with contextual icons above the page footer.
   *
   * @param renderer Active renderer, including the software fallback.
   * @param titleFont Card-title font.
   * @param bodyFont Supporting text and controller-hint font.
   * @param assetCache Cached Lucide and controller glyphs.
   * @param notifications Outstanding notifications in arrival order.
   * @param area Safe notification column above the footer.
   * @param hints Shared input hint size and spacing.
   * @return True when all visible notification cards rendered successfully.
   */
  bool render_notifications(
    SDL_Renderer *renderer,
    TTF_Font *titleFont,
    TTF_Font *bodyFont,
    AssetTextureCache *assetCache,
    const std::vector<ui::ShellNotification> &notifications,
    const SDL_Rect &area,
    FooterHintStyle hints
  ) {
    if (notifications.empty()) {
      return true;
    }
    const int padding = 16;
    const int innerWidth = area.w - padding * 2;
    const int titleHeight = TTF_GetFontLineSkip(titleFont);
    const int iconSize = std::max(20, titleHeight - 2);
    std::array<int, ui::MAX_VISIBLE_NOTIFICATIONS> actionHeights {};
    std::vector<int> heights;
    for (std::size_t offset = 0; offset < std::min(notifications.size(), ui::MAX_VISIBLE_NOTIFICATIONS); ++offset) {
      const auto &notification = notifications[notifications.size() - offset - 1U];
      const int messageHeight = std::min(measure_wrapped_text_height(bodyFont, notification.message, innerWidth), TTF_GetFontLineSkip(bodyFont) * 3);
      actionHeights[offset] = measure_footer_actions_height(bodyFont, notification.actions, innerWidth, hints);
      heights.push_back(28 + titleHeight + 8 + messageHeight + padding + (actionHeights[offset] == 0 ? 0 : actionHeights[offset] + 12));
    }
    const auto cards = ui::calculate_notification_stack({area.x, area.y, area.w, area.h}, heights);
    for (std::size_t offset = 0; offset < cards.size(); ++offset) {
      const auto &notification = notifications[notifications.size() - offset - 1U];
      const SDL_Rect card = sdl_rect(cards[offset]);
      render_dialog_surface(renderer, card);
      int titleX = card.x + padding;
      if (!notification.iconAssetPath.empty()) {
        render_asset_icon(renderer, assetCache, notification.iconAssetPath, {titleX, card.y + 28, iconSize, iconSize});
        titleX += iconSize + 10;
      }
      if (!render_text_line_simple(renderer, titleFont, notification.title, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, titleX, card.y + 28, card.x + card.w - padding - titleX)) {
        return false;
      }
      const int footerY = card.y + card.h - padding - actionHeights[offset];
      const int bodyY = card.y + 28 + titleHeight + 8;
      const int bodyBottom = footerY - (notification.actions.empty() ? 0 : 12);
      if (!render_bounded_text(renderer, bodyFont, notification.message, {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, {card.x + padding, bodyY, innerWidth, bodyBottom - bodyY})) {
        return false;
      }
      if (!notification.actions.empty()) {
        draw_line(renderer, card.x + padding, footerY - 6, card.x + card.w - padding, footerY - 6, MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0x25);
        if (!render_footer_actions(renderer, bodyFont, assetCache, notification.actions, {card.x + padding, footerY, innerWidth, actionHeights[offset]}, hints)) {
          return false;
        }
      }
    }
    return true;
  }

  std::size_t selected_host_tile_index(const std::vector<ui::ShellHostTile> &tiles) {
    for (std::size_t index = 0; index < tiles.size(); ++index) {
      if (tiles[index].selected) {
        return index;
      }
    }

    return 0U;
  }

  std::size_t selected_app_tile_index(const std::vector<ui::ShellAppTile> &tiles) {
    for (std::size_t index = 0; index < tiles.size(); ++index) {
      if (tiles[index].selected) {
        return index;
      }
    }

    return 0U;
  }

  input::UiCommand translate_controller_button(Uint8 button) {
    switch (button) {
      case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::dpad_up);
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::dpad_down);
      case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::dpad_left);
      case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::dpad_right);
      case SDL_GAMEPAD_BUTTON_SOUTH:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::a);
      case SDL_GAMEPAD_BUTTON_EAST:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::b);
      case SDL_GAMEPAD_BUTTON_WEST:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::x);
      case SDL_GAMEPAD_BUTTON_NORTH:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::y);
      case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::left_shoulder);
      case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::right_shoulder);
      case SDL_GAMEPAD_BUTTON_START:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::start);
      case SDL_GAMEPAD_BUTTON_BACK:
        return input::map_gamepad_button_to_ui_command(input::GamepadButton::back);
      default:
        return input::UiCommand::none;
    }
  }

  input::UiCommand translate_keyboard_key(SDL_Keycode key, Uint16 modifiers) {
    const bool shiftPressed = (modifiers & SDL_KMOD_SHIFT) != 0;

    switch (key) {
      case SDLK_UP:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::up, shiftPressed);
      case SDLK_DOWN:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::down, shiftPressed);
      case SDLK_LEFT:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::left, shiftPressed);
      case SDLK_RIGHT:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::right, shiftPressed);
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::enter, shiftPressed);
      case SDLK_ESCAPE:
#ifdef MOONLIGHT_PORTABLE
      case SDLK_AC_BACK:
#endif
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::escape, shiftPressed);
      case SDLK_BACKSPACE:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::backspace, shiftPressed);
      case SDLK_DELETE:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::delete_key, shiftPressed);
      case SDLK_SPACE:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::space, shiftPressed);
      case SDLK_TAB:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::tab, shiftPressed);
      case SDLK_PAGEUP:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::page_up, shiftPressed);
      case SDLK_PAGEDOWN:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::page_down, shiftPressed);
      case SDLK_I:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::i, shiftPressed);
      case SDLK_M:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::m, shiftPressed);
      case SDLK_F3:
        return input::map_keyboard_key_to_ui_command(input::KeyboardKey::f3, shiftPressed);
      default:
        return input::UiCommand::none;
    }
  }

  input::UiCommand translate_trigger_axis(const SDL_GamepadAxisEvent &event, bool *leftTriggerPressed, bool *rightTriggerPressed) {
    if (leftTriggerPressed == nullptr || rightTriggerPressed == nullptr) {
      return input::UiCommand::none;
    }

    const bool thresholdCrossed = event.value >= TRIGGER_PAGE_SCROLL_THRESHOLD;

    switch (event.axis) {
      case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        if (thresholdCrossed && !*leftTriggerPressed) {
          *leftTriggerPressed = true;
          return input::UiCommand::fast_previous_page;
        }
        if (!thresholdCrossed) {
          *leftTriggerPressed = false;
        }
        break;
      case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        if (thresholdCrossed && !*rightTriggerPressed) {
          *rightTriggerPressed = true;
          return input::UiCommand::fast_next_page;
        }
        if (!thresholdCrossed) {
          *rightTriggerPressed = false;
        }
        break;
      default:
        break;
    }

    return input::UiCommand::none;
  }

  /**
   * @brief Tracks the repeat timing for one controller navigation direction.
   */
  using input::ControllerNavigationHoldState;

  /**
   * @brief Clears the held-repeat bookkeeping for every controller navigation direction.
   *
   * @param upState Repeat state for up navigation.
   * @param downState Repeat state for down navigation.
   * @param leftState Repeat state for left navigation.
   * @param rightState Repeat state for right navigation.
   */
  void reset_controller_navigation_hold_states(
    ControllerNavigationHoldState *upState,
    ControllerNavigationHoldState *downState,
    ControllerNavigationHoldState *leftState,
    ControllerNavigationHoldState *rightState
  ) {
    if (upState != nullptr) {
      *upState = {};
    }
    if (downState != nullptr) {
      *downState = {};
    }
    if (leftState != nullptr) {
      *leftState = {};
    }
    if (rightState != nullptr) {
      *rightState = {};
    }
  }

  /**
   * @brief Returns whether a UI command is part of directional navigation and should keep repeat state alive.
   *
   * @param command UI command to classify.
   * @return True when the command represents directional navigation.
   */
  bool is_navigation_command(input::UiCommand command) {
    switch (command) {
      case input::UiCommand::move_up:
      case input::UiCommand::move_down:
      case input::UiCommand::move_left:
      case input::UiCommand::move_right:
        return true;
      case input::UiCommand::activate:
      case input::UiCommand::confirm:
      case input::UiCommand::back:
      case input::UiCommand::delete_character:
      case input::UiCommand::open_context_menu:
      case input::UiCommand::previous_page:
      case input::UiCommand::next_page:
      case input::UiCommand::fast_previous_page:
      case input::UiCommand::fast_next_page:
      case input::UiCommand::toggle_overlay:
      case input::UiCommand::none:
        return false;
    }

    return false;
  }

  /**
   * @brief Returns whether a negative stick direction remains active, using hysteresis to avoid jitter near center.
   *
   * @param value Current stick axis value.
   * @param state Existing hold state for the direction.
   * @return True when the negative navigation direction should be treated as active.
   */
  bool axis_value_is_negative_navigation_active(Sint16 value, const ControllerNavigationHoldState *state) {
    const Sint16 threshold = state != nullptr && state->active ? LEFT_STICK_NAVIGATION_RELEASE_THRESHOLD : LEFT_STICK_NAVIGATION_THRESHOLD;
    return value <= -threshold;
  }

  /**
   * @brief Returns whether a positive stick direction remains active, using hysteresis to avoid jitter near center.
   *
   * @param value Current stick axis value.
   * @param state Existing hold state for the direction.
   * @return True when the positive navigation direction should be treated as active.
   */
  bool axis_value_is_positive_navigation_active(Sint16 value, const ControllerNavigationHoldState *state) {
    const Sint16 threshold = state != nullptr && state->active ? LEFT_STICK_NAVIGATION_RELEASE_THRESHOLD : LEFT_STICK_NAVIGATION_THRESHOLD;
    return value >= threshold;
  }

  /**
   * @brief Returns the held-repeat state associated with a directional UI command.
   *
   * @param command UI command whose hold state should be returned.
   * @param upState Repeat state for up navigation.
   * @param downState Repeat state for down navigation.
   * @param leftState Repeat state for left navigation.
   * @param rightState Repeat state for right navigation.
   * @return Pointer to the matching hold state, or null when the command is not directional.
   */
  ControllerNavigationHoldState *controller_navigation_hold_state_for_command(
    input::UiCommand command,
    ControllerNavigationHoldState *upState,
    ControllerNavigationHoldState *downState,
    ControllerNavigationHoldState *leftState,
    ControllerNavigationHoldState *rightState
  ) {
    switch (command) {
      case input::UiCommand::move_up:
        return upState;
      case input::UiCommand::move_down:
        return downState;
      case input::UiCommand::move_left:
        return leftState;
      case input::UiCommand::move_right:
        return rightState;
      case input::UiCommand::activate:
      case input::UiCommand::confirm:
      case input::UiCommand::back:
      case input::UiCommand::delete_character:
      case input::UiCommand::open_context_menu:
      case input::UiCommand::previous_page:
      case input::UiCommand::next_page:
      case input::UiCommand::fast_previous_page:
      case input::UiCommand::fast_next_page:
      case input::UiCommand::toggle_overlay:
      case input::UiCommand::none:
        return nullptr;
    }

    return nullptr;
  }

  /**
   * @brief Arms held-repeat timing for a freshly pressed navigation direction.
   *
   * @param now Current SDL tick count.
   * @param command Directional command that was just pressed.
   * @param upState Repeat state for up navigation.
   * @param downState Repeat state for down navigation.
   * @param leftState Repeat state for left navigation.
   * @param rightState Repeat state for right navigation.
   */
  void seed_controller_navigation_hold_state(
    Uint32 now,
    input::UiCommand command,
    ControllerNavigationHoldState *upState,
    ControllerNavigationHoldState *downState,
    ControllerNavigationHoldState *leftState,
    ControllerNavigationHoldState *rightState
  ) {
    if (command == input::UiCommand::move_up && downState != nullptr) {
      *downState = {};
    } else if (command == input::UiCommand::move_down && upState != nullptr) {
      *upState = {};
    } else if (command == input::UiCommand::move_left && rightState != nullptr) {
      *rightState = {};
    } else if (command == input::UiCommand::move_right && leftState != nullptr) {
      *leftState = {};
    }

    if (ControllerNavigationHoldState *state = controller_navigation_hold_state_for_command(command, upState, downState, leftState, rightState); state != nullptr) {
      state->active = true;
      state->activatedTick = now;
      state->lastRepeatTick = now;
    }
  }

  /**
   * @brief Clears held-repeat timing for a released navigation direction.
   *
   * @param command Directional command that was just released.
   * @param upState Repeat state for up navigation.
   * @param downState Repeat state for down navigation.
   * @param leftState Repeat state for left navigation.
   * @param rightState Repeat state for right navigation.
   */
  void release_controller_navigation_hold_state(
    input::UiCommand command,
    ControllerNavigationHoldState *upState,
    ControllerNavigationHoldState *downState,
    ControllerNavigationHoldState *leftState,
    ControllerNavigationHoldState *rightState
  ) {
    if (ControllerNavigationHoldState *state = controller_navigation_hold_state_for_command(command, upState, downState, leftState, rightState); state != nullptr) {
      *state = {};
    }
  }

  /**
   * @brief Returns whether the controller is still holding any D-pad or left-stick navigation input.
   *
   * @param controller SDL game controller to inspect.
   * @return True when any navigation direction is still held.
   */
  bool is_controller_navigation_active(SDL_Gamepad *controller) {
    if (controller == nullptr) {
      return false;
    }

    const Sint16 leftStickX = SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTX);
    const Sint16 leftStickY = SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTY);
    return SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_UP) || SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ||
           SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ||
           leftStickY <= -LEFT_STICK_NAVIGATION_RELEASE_THRESHOLD || leftStickY >= LEFT_STICK_NAVIGATION_RELEASE_THRESHOLD || leftStickX <= -LEFT_STICK_NAVIGATION_RELEASE_THRESHOLD ||
           leftStickX >= LEFT_STICK_NAVIGATION_RELEASE_THRESHOLD;
  }

  input::UiCommand update_controller_navigation_hold_state(bool active, Uint32 now, input::UiCommand command, ControllerNavigationHoldState *state) {
    if (state == nullptr || command == input::UiCommand::none) {
      return input::UiCommand::none;
    }

    if (!active) {
      *state = {};
      return input::UiCommand::none;
    }

    if (!state->active) {
      state->active = true;
      state->activatedTick = now;
      state->lastRepeatTick = now;
      return command;
    }

    if (now - state->activatedTick < CONTROLLER_NAVIGATION_INITIAL_REPEAT_MILLISECONDS || now - state->lastRepeatTick < CONTROLLER_NAVIGATION_REPEAT_MILLISECONDS) {
      return input::UiCommand::none;
    }

    state->lastRepeatTick = now;
    return command;
  }

  input::UiCommand poll_controller_navigation(
    SDL_Gamepad *controller,
    Uint32 now,
    ControllerNavigationHoldState *upState,
    ControllerNavigationHoldState *downState,
    ControllerNavigationHoldState *leftState,
    ControllerNavigationHoldState *rightState
  ) {
    if (controller == nullptr) {
      reset_controller_navigation_hold_states(upState, downState, leftState, rightState);
      return input::UiCommand::none;
    }

    const Sint16 leftStickX = SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTX);
    const Sint16 leftStickY = SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTY);
    const bool moveUpActive = SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_UP) || axis_value_is_negative_navigation_active(leftStickY, upState);
    const bool moveDownActive = SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || axis_value_is_positive_navigation_active(leftStickY, downState);
    const bool moveLeftActive = SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || axis_value_is_negative_navigation_active(leftStickX, leftState);
    const bool moveRightActive = SDL_GetGamepadButton(controller, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || axis_value_is_positive_navigation_active(leftStickX, rightState);

    if (const input::UiCommand command = update_controller_navigation_hold_state(moveUpActive, now, input::map_gamepad_axis_direction_to_ui_command(input::GamepadAxisDirection::left_stick_up), upState); command != input::UiCommand::none) {
      return command;
    }
    if (const input::UiCommand command = update_controller_navigation_hold_state(moveDownActive, now, input::map_gamepad_axis_direction_to_ui_command(input::GamepadAxisDirection::left_stick_down), downState); command != input::UiCommand::none) {
      return command;
    }
    if (const input::UiCommand command = update_controller_navigation_hold_state(moveLeftActive, now, input::map_gamepad_axis_direction_to_ui_command(input::GamepadAxisDirection::left_stick_left), leftState); command != input::UiCommand::none) {
      return command;
    }
    return update_controller_navigation_hold_state(moveRightActive, now, input::map_gamepad_axis_direction_to_ui_command(input::GamepadAxisDirection::left_stick_right), rightState);
  }

  void log_app_update(const app::ClientState &state, const app::AppUpdate &update) {
    if (!update.navigation.activatedItemId.empty()) {
      logging::info("ui", "Activated menu item: " + update.navigation.activatedItemId);
    }
    if (update.navigation.screenChanged) {
      logging::info("ui", std::string("Switched screen to ") + app::to_string(state.shell.activeScreen));
    }
    if (update.navigation.overlayVisibilityChanged) {
      logging::info("overlay", state.shell.overlayVisible ? "Overlay enabled" : "Overlay disabled");
    }
    if (update.navigation.exitRequested) {
      logging::info("app", "Exit requested from shell");
    }
  }

  app::HostRecord *find_persisted_host_record(std::vector<app::HostRecord> &hosts, const std::string &address, uint16_t port) {
    const auto iterator = std::find_if(hosts.begin(), hosts.end(), [&address, port](const app::HostRecord &host) {
      return app::host_matches_endpoint(host, address, port);
    });
    return iterator == hosts.end() ? nullptr : &(*iterator);
  }

  void merge_host_for_persistence(app::HostRecord *targetHost, const app::HostRecord &sourceHost) {
    if (targetHost == nullptr) {
      return;
    }

    targetHost->displayName = sourceHost.displayName;
    targetHost->address = sourceHost.address;
    targetHost->port = sourceHost.port;
    targetHost->pairingState = sourceHost.pairingState;
    targetHost->reachability = sourceHost.reachability;
    targetHost->activeAddress = sourceHost.activeAddress;
    targetHost->uuid = sourceHost.uuid;
    targetHost->localAddress = sourceHost.localAddress;
    targetHost->remoteAddress = sourceHost.remoteAddress;
    targetHost->ipv6Address = sourceHost.ipv6Address;
    targetHost->manualAddress = sourceHost.manualAddress;
    targetHost->macAddress = sourceHost.macAddress;
    targetHost->httpsPort = sourceHost.httpsPort;
    targetHost->runningGameId = sourceHost.runningGameId;
    targetHost->apps = sourceHost.apps;
    targetHost->appListState = sourceHost.appListState;
    targetHost->appListStatusMessage = sourceHost.appListStatusMessage;
    targetHost->resolvedHttpPort = sourceHost.resolvedHttpPort;
    targetHost->appListContentHash = sourceHost.appListContentHash;
    targetHost->lastAppListRefreshTick = sourceHost.lastAppListRefreshTick;
  }

  bool ensure_hosts_loaded_for_active_screen(app::ClientState &state) {
    if (state.shell.activeScreen != app::ScreenId::hosts || state.hosts.loaded) {
      return true;
    }

    const startup::LoadSavedHostsResult loadedHosts = startup::load_saved_hosts();
    for (const std::string &warning : loadedHosts.warnings) {
      logging::warn("hosts", warning);
    }
    app::replace_hosts(state, loadedHosts.hosts, state.shell.statusMessage);
    return true;
  }

  /**
   * @brief Keep repainting on the render thread while a synchronous shell action awaits its save.
   */
  struct ShellSaveFeedback {
    std::function<void()> redraw;  ///< Repaint the current shell without processing new commands.

    /**
     * @brief Save on a worker while preserving the caller's synchronous result and ordering.
     * @tparam Save Callable returning a save result.
     * @param operation File operation using the caller's retained data.
     * @return Result of the save operation.
     */
    template<typename Save>
    auto run(Save operation) const {
      const std::unique_ptr<SDL_Semaphore, decltype(&SDL_DestroySemaphore)> completed(SDL_CreateSemaphore(0), SDL_DestroySemaphore);
      decltype(operation()) result {};
      auto write = [&] {
        result = operation();
        SDL_SignalSemaphore(completed.get());
        return 0;
      };
      const app::FileSaveActivity saving;
      redraw();
      if (completed) {
        if (auto *thread = SDL_CreateThread([](auto *context) {
              return (*static_cast<decltype(write) *>(context))();
            },
                                            "save-file",
                                            &write);
            thread != nullptr) {
          while (!SDL_WaitSemaphoreTimeout(completed.get(), 60)) {
            redraw();
          }
          SDL_WaitThread(thread, nullptr);
          return result;
        }
      }
      return operation();
    }
  };

  /**
   * @brief Save the current host records and clear their dirty flag on success.
   * @param state Client state supplying the host records.
   * @param feedback Repaints the shell during the save.
   * @return True when the records were persisted.
   */
  bool persist_hosts(app::ClientState &state, const ShellSaveFeedback &feedback) {
    std::vector<app::HostRecord> hostsToSave;
    if (state.hosts.loaded) {
      hostsToSave = state.hosts.items;
    } else if (state.hosts.activeLoaded) {
      const startup::LoadSavedHostsResult loadedHosts = startup::load_saved_hosts();
      for (const std::string &warning : loadedHosts.warnings) {
        logging::warn("hosts", warning);
      }
      hostsToSave = loadedHosts.hosts;
      if (app::HostRecord *host = find_persisted_host_record(hostsToSave, state.hosts.active.address, state.hosts.active.port); host != nullptr) {
        merge_host_for_persistence(host, state.hosts.active);
      } else {
        hostsToSave.push_back(state.hosts.active);
      }
    } else {
      hostsToSave = state.hosts.items;
    }

    const auto saveResult = feedback.run([&hostsToSave] {
      return startup::save_saved_hosts(hostsToSave);
    });
    if (saveResult.success) {
      state.hosts.dirty = false;
      logging::info("hosts", "Saved host records");
      return true;
    }

    logging::error("hosts", saveResult.errorMessage);
    return false;
  }

  /**
   * @brief Persist user-requested PC changes and report the completed operation or storage failure.
   *
   * @param state Mutable client state.
   * @param update Requested persistence operation.
   * @param feedback Repaints the shell during the save.
   */
  void persist_hosts_if_needed(app::ClientState &state, const app::AppUpdate &update, const ShellSaveFeedback &feedback) {
    if (!update.persistence.hostsChanged) {
      return;
    }

    if (!persist_hosts(state, feedback)) {
      app::show_notification(state, app::NotificationKind::error, "PC changes not saved", "Your changes could not be saved. Check the available storage and try again.");
    } else if (update.persistence.hostDeleteCleanupRequested) {
      app::show_notification(state, app::NotificationKind::success, "PC removed", "Removed the saved PC at " + update.persistence.deletedHostAddress + ":" + std::to_string(app::effective_host_port(update.persistence.deletedHostPort)) + ".", 5000U);
    } else if (update.navigation.activatedItemId == "save-host") {
      const app::HostRecord *host = app::selected_host(state);
      app::show_notification(state, app::NotificationKind::success, "PC saved", (host == nullptr ? std::string("Your PC") : host->displayName) + " has been saved. Select it to pair and stream.", 5000U);
    }
  }

  /**
   * @brief Execute explicit folder-picker requests and accept only writable screenshot destinations.
   * @param state Reducer state, including the staged picker directory and name.
   * @param update Requests to execute and persistence flags to set on acceptance.
   */
  void process_directory_picker_requests(app::ClientState &state, app::AppUpdate &update) {
    auto &picker = state.directoryPicker;
    if (update.requests.directorySelectRequested && platform::directory_is_writable(picker.path, picker.error)) {
      app::accept_screenshot_directory(state, update, picker.path);
    }
    if (update.requests.directoryCreateRequested && platform::create_child_directory(picker.path, picker.name, picker.error)) {
      picker.path = platform::join_path(picker.path, picker.name);
      picker.creating = false;
      state.modal.selectedActionIndex = 0;
      update.requests.directoryListRequested = true;
    }
    if (update.requests.directoryListRequested) {
      auto listing = platform::list_directories(picker.path);
      // A configured/default folder can have been removed, or may not have
      // received its first screenshot yet. Start at its nearest existing parent.
      while (update.navigation.modalOpened && !listing.error.empty() && !picker.path.empty()) {
        picker.path = platform::directory_parent(picker.path);
        listing = platform::list_directories(picker.path);
      }
      picker.folders = std::move(listing.paths);
      picker.error = std::move(listing.error);
      picker.available = picker.error.empty();
      if (picker.path.empty()) {
        state.modal.selectedActionIndex = picker.folders.empty() ? 3 : 4;
      }
    }
  }

  /**
   * @brief Snapshot persisted shell and stream preferences, including codec and GPU buffering.
   * @param state Current client state.
   * @return Settings snapshot ready for TOML serialization.
   */
  app::AppSettings persistent_settings_from_state(const app::ClientState &state) {
    return {
      state.settings.loggingLevel,
      state.settings.xemuConsoleLoggingLevel,
      state.settings.logViewerPlacement,
      state.settings.preferredVideoMode,
      state.settings.preferredVideoModeSet,
      state.settings.streamFramerate,
      state.settings.streamBitrateKbps,
      state.settings.video.codec,
      state.settings.audio.playOnPc,
      state.settings.video.showPerformanceStats,
      state.settings.audio.playOnXbox,
      state.settings.controls.screenshotDirectory,
      state.settings.controls.keyboardDockTop,
      state.settings.video.gpuPresentation,
      state.settings.audio.mode,
      state.settings.video.gpuBufferCount,
    };
  }

  /**
   * @brief Save changed settings quietly, surfacing only an actionable storage failure.
   *
   * @param state Mutable client state.
   * @param update Requested settings operation.
   * @param feedback Repaints the shell during the save.
   */
  void persist_settings_if_needed(app::ClientState &state, const app::AppUpdate &update, const ShellSaveFeedback &feedback) {
    if (!update.persistence.settingsChanged || !state.settings.dirty) {
      return;
    }

    const auto settings = persistent_settings_from_state(state);
    const auto saveResult = feedback.run([&settings] {
      return app::save_app_settings(settings);
    });
    if (saveResult.success) {
      state.settings.dirty = false;
      logging::info("settings", "Saved Moonlight settings");
      return;
    }

    logging::error("settings", saveResult.errorMessage);
    app::show_notification(state, app::NotificationKind::error, "Settings not saved", "Your changes work for this session. Check the available storage and try again.");
  }

  bool update_host_metadata_from_server_info(app::HostRecord *host, const std::string &address, const network::HostPairingServerInfo &serverInfo) {
    if (host == nullptr) {
      return false;
    }

    bool persistedMetadataChanged = false;
    if (!serverInfo.hostName.empty()) {
      persistedMetadataChanged = persistedMetadataChanged || host->displayName != serverInfo.hostName;
      host->displayName = serverInfo.hostName;
    }
    host->reachability = app::HostReachability::online;
    host->activeAddress = network::resolve_reachable_address(address, serverInfo);
    host->uuid = serverInfo.uuid;
    host->localAddress = serverInfo.localAddress;
    host->remoteAddress = serverInfo.remoteAddress;
    host->ipv6Address = serverInfo.ipv6Address;
    host->manualAddress = address;
    host->macAddress = serverInfo.macAddress;
    host->resolvedHttpPort = serverInfo.httpPort;
    host->httpsPort = serverInfo.httpsPort;
    host->runningGameId = serverInfo.runningGameId;
    return persistedMetadataChanged;
  }

  bool update_host_pairing_from_server_info(
    app::ClientState &state,
    app::HostRecord *host,
    const std::string &address,
    uint16_t port,
    const network::HostPairingServerInfo &serverInfo
  ) {
    if (host == nullptr || !serverInfo.pairingStatusCurrentClientKnown) {
      return false;
    }

    const bool hostRequiresManualPairing = app::host_requires_manual_pairing(state, address, port);
    const bool clientIsEffectivelyPaired = serverInfo.pairingStatusCurrentClient && !hostRequiresManualPairing;
    const app::PairingState resolvedPairingState = clientIsEffectivelyPaired ? app::PairingState::paired : app::PairingState::not_paired;
    const bool pairingChanged = host->pairingState != resolvedPairingState;
    host->pairingState = resolvedPairingState;
    if (clientIsEffectivelyPaired) {
      return pairingChanged;
    }

    host->apps.clear();
    host->appListState = hostRequiresManualPairing ? app::HostAppListState::idle : app::HostAppListState::failed;
    host->appListStatusMessage = hostRequiresManualPairing ? "This host was removed locally. Pair it again to restore apps and authorization." : "The host reports that this client is no longer paired. Pair the host again.";
    host->appListContentHash = 0;
    host->lastAppListRefreshTick = 0;
    state.apps.selectedAppIndex = 0U;
    if (state.shell.activeScreen == app::ScreenId::apps && state.hosts.activeLoaded && host == &state.hosts.active) {
      state.shell.statusMessage = host->appListStatusMessage;
    }
    return true;
  }

  void apply_server_info_to_host(app::ClientState &state, const std::string &address, uint16_t port, const network::HostPairingServerInfo &serverInfo) {
    auto apply_to_host = [&](app::HostRecord &host) {
      bool persistedMetadataChanged = update_host_metadata_from_server_info(&host, address, serverInfo);
      persistedMetadataChanged = update_host_pairing_from_server_info(state, &host, address, port, serverInfo) || persistedMetadataChanged;
      state.hosts.dirty = state.hosts.dirty || persistedMetadataChanged;
    };

    for (app::HostRecord &host : state.hosts.items) {
      if (!app::host_matches_endpoint(host, address, port)) {
        continue;
      }
      apply_to_host(host);
      return;
    }

    if (state.hosts.activeLoaded && app::host_matches_endpoint(state.hosts.active, address, port)) {
      apply_to_host(state.hosts.active);
    }
  }

  std::string display_name_for_saved_file(const app::ClientState &state, const std::string &path) {
    for (const startup::SavedFileEntry &savedFile : state.settings.savedFiles) {
      if (savedFile.path == path) {
        return savedFile.displayName;
      }
    }
    return path;
  }

  std::string cover_art_cache_key_from_path(const std::string &path) {
    if (const std::string coverArtRoot = startup::default_cover_art_cache_root(); coverArtRoot.empty() || path.size() <= coverArtRoot.size() || path.rfind(coverArtRoot, 0U) != 0U) {
      return {};
    }

    const std::string fileName = platform::file_name_from_path(path);
    if (fileName.empty()) {
      return {};
    }

    if (fileName.size() <= 4U || fileName.substr(fileName.size() - 4U) != ".bin") {
      return {};
    }
    return fileName.substr(0, fileName.size() - 4U);
  }

  void clear_deleted_cover_art_flag(app::ClientState &state, std::string_view cacheKey) {
    if (cacheKey.empty()) {
      return;
    }

    for (app::HostRecord &host : state.hosts.items) {
      for (app::HostAppRecord &appRecord : host.apps) {
        if (appRecord.boxArtCacheKey == cacheKey) {
          appRecord.boxArtCached = false;
        }
      }
    }
  }

  void refresh_saved_files_if_needed(app::ClientState &state) {
    if (state.shell.activeScreen != app::ScreenId::settings || !state.settings.savedFilesDirty) {
      return;
    }

    const startup::ListSavedFilesResult savedFiles = startup::list_saved_files();
    for (const std::string &warning : savedFiles.warnings) {
      logging::warn("storage", warning);
    }
    app::replace_saved_files(state, savedFiles.files);
  }

  void release_page_resources_for_screen(app::ScreenId previousScreen, app::ScreenId nextScreen, CoverArtTextureCache *coverArtTextureCache, KeypadModalLayoutCache *keypadModalLayoutCache) {
    if (previousScreen != nextScreen && previousScreen == app::ScreenId::apps) {
      clear_cover_art_texture_cache(coverArtTextureCache);
    }
    if (previousScreen != nextScreen && previousScreen == app::ScreenId::add_host) {
      clear_keypad_modal_layout_cache(keypadModalLayoutCache);
    }
  }

  /**
   * @brief Delete a requested file and report its confirmed outcome.
   *
   * @param state Mutable client state.
   * @param update Requested file deletion.
   * @param coverArtTextureCache Cached artwork affected by the deletion.
   */
  void delete_saved_file_if_requested(app::ClientState &state, const app::AppUpdate &update, CoverArtTextureCache *coverArtTextureCache) {
    if (!update.persistence.savedFileDeleteRequested) {
      return;
    }

    if (std::string errorMessage; !startup::delete_saved_file(update.persistence.savedFileDeletePath, &errorMessage)) {
      state.shell.statusMessage = errorMessage;
      app::show_notification(state, app::NotificationKind::error, "File not deleted", "The file could not be removed. Restart Moonlight and try again.");
      logging::warn("storage", errorMessage);
      return;
    }

    const std::string deletedDisplayName = display_name_for_saved_file(state, update.persistence.savedFileDeletePath);
    const std::string deletedCoverArtCacheKey = cover_art_cache_key_from_path(update.persistence.savedFileDeletePath);
    clear_deleted_cover_art_flag(state, deletedCoverArtCacheKey);
    clear_cover_art_texture(coverArtTextureCache, deletedCoverArtCacheKey);
    state.settings.savedFilesDirty = true;
    state.shell.statusMessage = "Deleted saved file " + deletedDisplayName;
    app::show_notification(state, app::NotificationKind::success, "File deleted", deletedDisplayName + " has been removed.", 5000U);
    logging::info("storage", state.shell.statusMessage);
  }

  void delete_host_data_if_requested(app::ClientState &state, const app::AppUpdate &update, CoverArtTextureCache *coverArtTextureCache) {
    if (!update.persistence.hostDeleteCleanupRequested) {
      return;
    }

    std::size_t deletedCoverArtCount = 0U;
    for (const std::string &cacheKey : update.persistence.deletedHostCoverArtCacheKeys) {
      if (std::string errorMessage; !startup::delete_cover_art(cacheKey, &errorMessage)) {
        logging::warn("storage", errorMessage);
      } else {
        ++deletedCoverArtCount;
      }
      clear_cover_art_texture(coverArtTextureCache, cacheKey);
    }

    bool deletedClientIdentity = false;
    if (update.persistence.deletedHostWasPaired) {
      const bool pairedHostsRemain = std::any_of(state.hosts.begin(), state.hosts.end(), [](const app::HostRecord &host) {
        return host.pairingState == app::PairingState::paired;
      });
      if (!pairedHostsRemain) {
        std::string errorMessage;
        if (!startup::delete_client_identity(&errorMessage)) {
          logging::warn("storage", errorMessage);
        } else {
          deletedClientIdentity = true;
        }
      } else {
        logging::info("storage", "Retained the shared pairing identity because other paired hosts still exist");
      }
    }

    state.shell.statusMessage = "Deleted saved host";
    if (deletedCoverArtCount > 0U) {
      state.shell.statusMessage += " and cleared " + std::to_string(deletedCoverArtCount) + " cached asset" + (deletedCoverArtCount == 1U ? std::string {} : "s");
    }
    if (deletedClientIdentity) {
      state.shell.statusMessage += " and reset local pairing identity";
    }
    logging::info("storage", state.shell.statusMessage);
  }

  /**
   * @brief Apply a confirmed reset and report completion or an actionable failure.
   *
   * @param state Mutable client state.
   * @param update Confirmed reset request.
   * @param coverArtTextureCache Cached artwork to release after the reset.
   */
  void factory_reset_if_requested(app::ClientState &state, const app::AppUpdate &update, CoverArtTextureCache *coverArtTextureCache) {
    if (!update.persistence.factoryResetRequested) {
      return;
    }

    if (std::string errorMessage; !startup::delete_all_saved_files(&errorMessage)) {
      state.shell.statusMessage = errorMessage;
      app::show_notification(state, app::NotificationKind::error, "Reset incomplete", "Some saved data could not be removed. Restart Moonlight and try again.");
      logging::warn("storage", errorMessage);
      return;
    }

    state.hosts.clear();
    state = app::create_initial_state();
    state.settings.savedFiles.clear();
    state.settings.savedFilesDirty = true;
    state.shell.statusMessage = "Factory reset completed";
    app::show_notification(state, app::NotificationKind::success, "Reset complete", "Moonlight's saved PCs, settings and cached artwork have been removed.", 5000U);
    clear_cover_art_texture_cache(coverArtTextureCache);
    app::set_log_file_path(state, logging::default_log_file_path());
    logging::info("storage", state.shell.statusMessage);
  }

  bool try_load_saved_pairing_identity(network::PairingIdentity *identity) {
    const startup::LoadClientIdentityResult loadedIdentity = startup::load_client_identity();
    if (!loadedIdentity.fileFound || !network::is_valid_pairing_identity(loadedIdentity.identity)) {
      return false;
    }

    if (identity != nullptr) {
      *identity = loadedIdentity.identity;
    }
    return true;
  }

  bool load_saved_pairing_identity_for_streaming(network::PairingIdentity *identity, std::string *errorMessage) {
    const startup::LoadClientIdentityResult loadedIdentity = startup::load_client_identity();
    if (!loadedIdentity.fileFound || !network::is_valid_pairing_identity(loadedIdentity.identity)) {
      return platform::append_error(errorMessage, "No valid paired client identity is available. Pair the host again before browsing apps.");
    }

    if (identity != nullptr) {
      *identity = loadedIdentity.identity;
    }
    return true;
  }

  bool test_tcp_host_connection(
    const std::string &address,
    uint16_t port,
    const network::PairingIdentity *clientIdentity,
    std::string *message,
    network::HostPairingServerInfo *serverInfoResult = nullptr
  ) {
    if (!network::runtime_network_ready()) {
      if (message != nullptr) {
        *message = network::runtime_network_status().summary;
      }
      return false;
    }

    network::HostPairingServerInfo serverInfo {};
    if (std::string errorMessage; !network::query_server_info(address, port, clientIdentity, &serverInfo, &errorMessage)) {
      if (message != nullptr) {
        *message = std::move(errorMessage);
      }
      return false;
    }

    if (serverInfoResult != nullptr) {
      *serverInfoResult = serverInfo;
    }

    if (message != nullptr) {
      *message = "Received /serverinfo from " + address + ":" + std::to_string(serverInfo.httpPort) + " and discovered HTTPS pairing on port " + std::to_string(serverInfo.httpsPort);
      if (serverInfo.pairingStatusCurrentClientKnown) {
        *message += serverInfo.pairingStatusCurrentClient ? "; the current client is paired and authorized" : "; the current client is no longer paired or authorized";
      }
    }
    return true;
  }

  struct PairingAttemptState {
    SDL_Thread *thread;
    std::atomic<bool> completed;
    std::atomic<bool> discardResult;
    std::atomic<bool> cancelRequested;
    network::HostPairingRequest request;
    network::HostPairingResult result;

    struct DeferredLogEntry {
      logging::LogLevel level;
      std::string message;
    };

    std::vector<DeferredLogEntry> deferredLogs;
  };

  struct PairingTaskState {
    std::unique_ptr<PairingAttemptState> activeAttempt;
    std::vector<std::unique_ptr<PairingAttemptState>> retiredAttempts;
  };

  /**
   * @brief Own a manual connection probe without sharing mutable app state with its worker.
   */
  struct ConnectionTestTaskState {
    SDL_Thread *thread = nullptr;  ///< Joinable connection-test worker.
    std::atomic<bool> completed = false;  ///< Publishes the worker's completed result.
    std::string address;  ///< Immutable endpoint copied before starting the worker.
    uint16_t port = 0;  ///< Effective endpoint port.
    bool success = false;  ///< Result published by the worker.
    std::string message;  ///< Diagnostic text published by the worker.
    network::HostPairingServerInfo serverInfo {};  ///< Metadata returned by the host.
  };

  /**
   * @brief One confirmed host quit operation, isolated from UI selection changes.
   */
  struct AppQuitTaskState {
    SDL_Thread *thread = nullptr;  ///< Owned background HTTP worker.
    std::atomic<bool> completed = false;  ///< Publishes success and error detail to the shell.
    app::AppQuitState target;  ///< Server and app confirmed by the user.
    bool success = false;  ///< True after the host verified termination.
    std::string message;  ///< Host error or completion detail.
  };

  struct AppListTaskState {
    SDL_Thread *thread = nullptr;
    std::atomic<bool> completed = false;
    std::string address;
    uint16_t port = 0;
    bool serverInfoAvailable = false;
    bool success = false;
    uint64_t appListContentHash = 0;
    std::string message;
    network::HostPairingServerInfo serverInfo;
    std::vector<app::HostAppRecord> apps;
  };

  /**
   * @brief Owns an artwork download batch that can yield to stream startup.
   */
  struct AppArtTaskState {
    SDL_Thread *thread = nullptr;  ///< Worker retained until its results have been joined.
    std::atomic<bool> completed = false;  ///< Published once the worker has finished writing results.
    std::atomic<bool> cancelRequested = false;  ///< Set by the shell before handing off to a stream.
    std::string address;  ///< Host endpoint for the downloads.
    uint16_t port = 0;  ///< Host HTTPS port.
    std::vector<app::HostAppRecord> apps;  ///< Snapshot of the app library.
    std::vector<int> cachedAppIds;  ///< IDs whose artwork was stored successfully.
    std::size_t failureCount = 0;  ///< Downloads that failed before cancellation.
  };

  struct HostDiscoveryTaskState {
    SDL_Thread *thread = nullptr;
    std::atomic<bool> completed = false;
    std::vector<network::DiscoveredHost> hosts;
    std::string errorMessage;
  };

  struct HostProbeTaskState {
    struct ProbeWorkerState {
      SDL_Thread *thread = nullptr;
      std::atomic<bool> completed = false;
      std::string address;
      uint16_t port = 0;
      bool success = false;
      network::HostPairingServerInfo serverInfo;
      const network::PairingIdentity *clientIdentity = nullptr;
      ui::HostProbeResultQueue *resultQueue = nullptr;
    };

    std::vector<std::unique_ptr<ProbeWorkerState>> workers;
    network::PairingIdentity clientIdentity;
    bool clientIdentityAvailable = false;
    ui::HostProbeResultQueue resultQueue;
    std::size_t onlineCount = 0U;
    std::size_t offlineCount = 0U;
    bool metadataChanged = false;
  };

  void reset_pairing_attempt(PairingAttemptState *attempt) {
    if (attempt == nullptr) {
      return;
    }

    attempt->thread = nullptr;
    attempt->completed.store(false);
    attempt->discardResult.store(false);
    attempt->cancelRequested.store(false);
    attempt->request = {};
    attempt->result = {false, false, {}};
    attempt->deferredLogs.clear();
  }

  void reset_pairing_task(PairingTaskState *task) {
    if (task == nullptr) {
      return;
    }

    task->activeAttempt.reset();
    task->retiredAttempts.clear();
  }

  bool pairing_task_is_active(const PairingTaskState &task) {
    if (task.activeAttempt == nullptr) {
      return false;
    }
    return task.activeAttempt->thread != nullptr && !task.activeAttempt->completed.load();
  }

  bool pairing_attempt_is_ready(const PairingAttemptState *attempt) {
    return attempt != nullptr && attempt->thread != nullptr && attempt->completed.load();
  }

  /**
   * @brief Join a pairing attempt and report storage failures after successful authorization.
   *
   * @param state Client state receiving the result, or null when discarding it.
   * @param attempt Completed pairing worker to join and release.
   * @param feedback Repaints the shell if the accepted result must be saved.
   */
  void finalize_pairing_attempt(app::ClientState *state, std::unique_ptr<PairingAttemptState> attempt, const ShellSaveFeedback &feedback = {}) {
    if (attempt == nullptr || attempt->thread == nullptr) {
      return;
    }

    int threadResult = 0;
    SDL_WaitThread(attempt->thread, &threadResult);
    (void) threadResult;

    const network::HostPairingRequest request = attempt->request;
    const network::HostPairingResult result = attempt->result;
    const bool discardResult = attempt->discardResult.load();
    const std::vector<PairingAttemptState::DeferredLogEntry> deferredLogs = attempt->deferredLogs;
    reset_pairing_attempt(attempt.get());

    for (const PairingAttemptState::DeferredLogEntry &entry : deferredLogs) {
      logging::log(entry.level, "pairing", entry.message);
    }

    if (discardResult || state == nullptr) {
      logging::info("pairing", "Ignored a completed pairing result after leaving the pairing screen or starting a new attempt");
      return;
    }

    const bool hostsChanged = app::apply_pairing_result(
      *state,
      request.address,
      request.httpPort,
      result.success || result.alreadyPaired,
      result.message
    );

    logging::log(result.success || result.alreadyPaired ? logging::LogLevel::info : logging::LogLevel::warning, "pairing", result.message);
    if (hostsChanged && !persist_hosts(*state, feedback)) {
      app::show_notification(*state, app::NotificationKind::error, "Pairing not saved", "Pairing succeeded, but the PC could not be saved. Check the available storage.");
    }
  }

  void retire_active_pairing_attempt(PairingTaskState *task, bool discardResult) {
    if (task == nullptr || task->activeAttempt == nullptr) {
      return;
    }

    if (discardResult) {
      task->activeAttempt->discardResult.store(true);
    }
    task->activeAttempt->cancelRequested.store(true);
    task->retiredAttempts.push_back(std::move(task->activeAttempt));
  }

  void reap_retired_pairing_attempts(PairingTaskState *task) {
    if (task == nullptr) {
      return;
    }

    auto iterator = task->retiredAttempts.begin();
    while (iterator != task->retiredAttempts.end()) {
      if (!pairing_attempt_is_ready(iterator->get())) {
        ++iterator;
        continue;
      }

      finalize_pairing_attempt(nullptr, std::move(*iterator));
      iterator = task->retiredAttempts.erase(iterator);
    }
  }

  void reset_app_list_task(AppListTaskState *task) {
    if (task == nullptr) {
      return;
    }

    task->thread = nullptr;
    task->completed.store(false);
    task->address.clear();
    task->port = 0;
    task->serverInfoAvailable = false;
    task->success = false;
    task->appListContentHash = 0;
    task->message.clear();
    task->serverInfo = {};
    task->apps.clear();
  }

  bool app_list_task_is_active(const AppListTaskState &task) {
    return task.thread != nullptr && !task.completed.load();
  }

  /**
   * @brief Clear a joined artwork worker and its cancellation request.
   * @param task Artwork batch to reset.
   */
  void reset_app_art_task(AppArtTaskState *task) {
    if (task == nullptr) {
      return;
    }

    task->thread = nullptr;
    task->completed.store(false);
    task->cancelRequested.store(false);
    task->address.clear();
    task->port = 0;
    task->apps.clear();
    task->cachedAppIds.clear();
    task->failureCount = 0;
  }

  bool app_art_task_is_active(const AppArtTaskState &task) {
    return task.thread != nullptr && !task.completed.load();
  }

  void reset_host_discovery_task(HostDiscoveryTaskState *task) {
    if (task == nullptr) {
      return;
    }

    task->thread = nullptr;
    task->completed.store(false);
    task->hosts.clear();
    task->errorMessage.clear();
  }

  bool host_discovery_task_is_active(const HostDiscoveryTaskState &task) {
    return task.thread != nullptr && !task.completed.load();
  }

  void reset_host_probe_task(HostProbeTaskState *task) {
    if (task == nullptr) {
      return;
    }

    task->workers.clear();
    task->clientIdentity = {};
    task->clientIdentityAvailable = false;
    ui::reset_host_probe_result_queue(&task->resultQueue);
    task->onlineCount = 0U;
    task->offlineCount = 0U;
    task->metadataChanged = false;
  }

  bool host_probe_task_is_active(const HostProbeTaskState &task) {
    return !task.workers.empty();
  }

  bool mark_host_offline(app::ClientState &state, const std::string &address, uint16_t port) {
    for (app::HostRecord &host : state.hosts) {
      if (!app::host_matches_endpoint(host, address, port)) {
        continue;
      }

      host.reachability = app::HostReachability::offline;
      host.manualAddress = address;
      return true;
    }

    return false;
  }

  int run_pairing_task(void *context) {  // NOSONAR(cpp:S5008) SDL_CreateThread requires void* callback signature
    auto *task = static_cast<PairingAttemptState *>(context);
    if (task == nullptr) {
      return -1;
    }

    const startup::LoadClientIdentityResult loadedIdentity = startup::load_client_identity();
    for (const std::string &warning : loadedIdentity.warnings) {
      task->deferredLogs.push_back({logging::LogLevel::warning, warning});
    }

    network::PairingIdentity identity = loadedIdentity.identity;
    if (!loadedIdentity.fileFound || !network::is_valid_pairing_identity(identity)) {
      if (loadedIdentity.fileFound) {
        task->deferredLogs.push_back({logging::LogLevel::warning, "Stored pairing identity was invalid. Generating a new one."});
      }

      std::string identityError;
      identity = network::create_pairing_identity(&identityError);
      if (!network::is_valid_pairing_identity(identity)) {
        task->result = {
          false,
          false,
          identityError.empty() ? "Failed to generate a valid client pairing identity" : "Failed to generate a valid client pairing identity: " + identityError,
        };
        task->completed.store(true);
        return 0;
      }

      if (const startup::SaveClientIdentityResult saveResult = startup::save_client_identity(identity); !saveResult.success) {
        task->result = {false, false, saveResult.errorMessage};
        task->completed.store(true);
        return 0;
      }

      task->deferredLogs.push_back({logging::LogLevel::info, "Saved pairing identity"});
    }

    task->request.identity = std::move(identity);
    task->result = network::pair_host(task->request, &task->cancelRequested);
    task->completed.store(true);
    return 0;
  }

  /**
   * @brief Apply a ready pairing result and persist the changed host record.
   * @param state Client state receiving the pairing result.
   * @param task Pairing worker state.
   * @param feedback Repaints the shell during the save.
   */
  void finish_pairing_task_if_ready(app::ClientState &state, PairingTaskState *task, const ShellSaveFeedback &feedback) {
    if (task == nullptr) {
      return;
    }

    reap_retired_pairing_attempts(task);
    if (!pairing_attempt_is_ready(task->activeAttempt.get())) {
      return;
    }

    finalize_pairing_attempt(&state, std::move(task->activeAttempt), feedback);
  }

  void cancel_pairing_if_requested(app::ClientState &state, const app::AppUpdate &update, PairingTaskState *task) {
    if (task == nullptr || !update.requests.pairingCancelledRequested || task->activeAttempt == nullptr || task->activeAttempt->thread == nullptr) {
      return;
    }

    task->activeAttempt->discardResult.store(true);
    task->activeAttempt->cancelRequested.store(true);
    retire_active_pairing_attempt(task, true);
    state.shell.statusMessage.clear();
    logging::info("pairing", "Cancelled the in-flight pairing attempt after leaving the pairing screen");
  }

  /**
   * @brief Probe an endpoint off the render thread and publish its complete result.
   *
   * @param context Connection task whose lifetime extends through the thread join.
   * @return Zero after publishing the probe result.
   */
  int run_connection_test_task(void *context) {  // NOSONAR(cpp:S5008) SDL_CreateThread requires void* callback signature
    auto *task = static_cast<ConnectionTestTaskState *>(context);
    network::PairingIdentity clientIdentity {};
    const network::PairingIdentity *clientIdentityPointer = try_load_saved_pairing_identity(&clientIdentity) ? &clientIdentity : nullptr;
    task->success = test_tcp_host_connection(task->address, task->port, clientIdentityPointer, &task->message, &task->serverInfo);
    task->completed.store(true);
    return 0;
  }

  /**
   * @brief Start a manual probe after the reducer has opened its progress dialog.
   *
   * @param state Client state receiving thread-start failures.
   * @param update Requested endpoint to test.
   * @param task Worker storage owned by the shell runtime.
   * @param feedback Repaints the shell if the host changes are saved.
   */
  void test_host_connection_if_requested(app::ClientState &state, const app::AppUpdate &update, ConnectionTestTaskState *task) {
    if (!update.requests.connectionTestRequested || task == nullptr || task->thread != nullptr) {
      return;
    }
    task->address = update.requests.connectionTestAddress;
    task->port = app::effective_host_port(update.requests.connectionTestPort);
    task->success = false;
    task->message.clear();
    task->serverInfo = {};
    task->completed.store(false);
    task->thread = create_pairing_thread(run_connection_test_task, "test-host", PAIRING_THREAD_STACK_SIZE, task);
    if (task->thread == nullptr) {
      app::apply_connection_test_result(state, false, "Unable to start the connection test. Please try again.");
      logging::warn("hosts", SDL_GetError());
    }
  }

  /**
   * @brief Join a completed manual probe and update the dialog and matching host.
   *
   * @param state Client state receiving the completed result.
   * @param task Worker storage owned by the shell runtime.
   */
  void finish_connection_test_if_ready(app::ClientState &state, ConnectionTestTaskState *task, const ShellSaveFeedback &feedback) {
    if (task == nullptr || task->thread == nullptr || !task->completed.load()) {
      return;
    }
    SDL_WaitThread(task->thread, nullptr);
    task->thread = nullptr;
    if (task->success) {
      apply_server_info_to_host(state, task->address, task->port, task->serverInfo);
    }
    app::apply_connection_test_result(state, task->success, task->message);
    logging::log(task->success ? logging::LogLevel::info : logging::LogLevel::warning, "hosts", task->message);
    if (state.hosts.dirty) {
      persist_hosts(state, feedback);
    }
  }

  /**
   * @brief Open a PC's library, reporting failures that otherwise leave the browser unchanged.
   *
   * @param state Mutable client state.
   * @param update Requested library navigation.
   * @param feedback Repaints the shell while saving refreshed host details.
   */
  void browse_host_apps_if_requested(app::ClientState &state, const app::AppUpdate &update, const ShellSaveFeedback &feedback) {
    if (!update.requests.appsBrowseRequested) {
      return;
    }

    const app::HostRecord *host = app::selected_host(state);
    if (host == nullptr) {
      return;
    }

    if (host->reachability == app::HostReachability::offline && host->pairingState == app::PairingState::paired && !host->apps.empty()) {
      app::begin_selected_host_app_browse(state, update.requests.appsBrowseShowHidden);
      return;
    }

    const std::string address = host->address;
    const uint16_t port = host->resolvedHttpPort == 0 ? app::effective_host_port(host->port) : host->resolvedHttpPort;
    network::PairingIdentity clientIdentity {};
    const network::PairingIdentity *clientIdentityPointer = try_load_saved_pairing_identity(&clientIdentity) ? &clientIdentity : nullptr;

    std::string resultMessage;
    network::HostPairingServerInfo serverInfo {};
    if (!test_tcp_host_connection(address, port, clientIdentityPointer, &resultMessage, &serverInfo)) {
      for (app::HostRecord &mutableHost : state.hosts.items) {
        if (mutableHost.address == address && app::effective_host_port(mutableHost.port) == app::effective_host_port(port)) {
          mutableHost.reachability = app::HostReachability::offline;
          mutableHost.manualAddress = address;
          break;
        }
      }
      state.shell.statusMessage = resultMessage;
      app::show_notification(state, app::NotificationKind::error, "PC unavailable", "Check that the PC is online and Sunshine is running, then try again.");
      logging::warn("apps", resultMessage);
      return;
    }

    apply_server_info_to_host(state, address, port, serverInfo);
    if (state.hosts.dirty) {
      persist_hosts(state, feedback);
    }

    host = app::selected_host(state);
    if (host == nullptr || host->pairingState != app::PairingState::paired) {
      state.shell.statusMessage = host != nullptr && !host->appListStatusMessage.empty() ? host->appListStatusMessage : "This host is no longer paired. Pair it again before opening apps.";
      app::show_notification(state, app::NotificationKind::error, "Pairing required", "Pair this PC again before opening its apps.");
      logging::warn("apps", state.shell.statusMessage);
      return;
    }

    if (app::begin_selected_host_app_browse(state, update.requests.appsBrowseShowHidden)) {
      logging::info("apps", "Authorized host browse for " + host->displayName);
      return;
    }

    logging::warn("apps", state.shell.statusMessage.empty() ? "Failed to enter the apps screen" : state.shell.statusMessage);
  }

  int run_host_probe_task(void *context) {  // NOSONAR(cpp:S5008) SDL_CreateThread requires void* callback signature
    auto *worker = static_cast<HostProbeTaskState::ProbeWorkerState *>(context);
    if (worker == nullptr) {
      return -1;
    }

    worker->success = test_tcp_host_connection(worker->address, worker->port, worker->clientIdentity, nullptr, &worker->serverInfo);
    ui::publish_host_probe_result(
      worker->resultQueue,
      {
        worker->address,
        worker->port,
        worker->success,
        worker->serverInfo,
      }
    );
    worker->completed.store(true);
    return 0;
  }

  int run_host_discovery_task(void *context) {  // NOSONAR(cpp:S5008) SDL_CreateThread requires void* callback signature
    auto *task = static_cast<HostDiscoveryTaskState *>(context);
    if (task == nullptr) {
      return -1;
    }

    const network::DiscoverHostsResult result = network::discover_hosts(HOST_DISCOVERY_TIMEOUT_MILLISECONDS);
    task->hosts = result.hosts;
    task->errorMessage = result.errorMessage;
    task->completed.store(true);
    return 0;
  }

  /**
   * @brief Apply discovered hosts and save any changes to persistent host records.
   * @param state Client state receiving discovered hosts.
   * @param task Discovery worker state.
   * @param nextHostProbeTick Receives the next scheduled probe time.
   * @param feedback Repaints the shell during the save.
   */
  void finish_host_discovery_task_if_ready(app::ClientState &state, HostDiscoveryTaskState *task, Uint32 *nextHostProbeTick, const ShellSaveFeedback &feedback) {
    if (task == nullptr || task->thread == nullptr || !task->completed.load()) {
      return;
    }

    SDL_Thread *thread = task->thread;
    task->thread = nullptr;
    int threadResult = 0;
    SDL_WaitThread(thread, &threadResult);
    (void) threadResult;

    std::vector<network::DiscoveredHost> discoveredHosts = std::move(task->hosts);
    const std::string errorMessage = task->errorMessage;
    reset_host_discovery_task(task);

    if (state.shell.activeScreen != app::ScreenId::hosts || !state.hosts.loaded) {
      return;
    }
    if (!errorMessage.empty()) {
      logging::warn("hosts", errorMessage);
      return;
    }

    std::size_t persistedHostCount = 0U;
    for (const network::DiscoveredHost &host : discoveredHosts) {
      if (app::merge_discovered_host(state, host.displayName, host.address, host.port)) {
        ++persistedHostCount;
      }
    }

    if (!discoveredHosts.empty()) {
      logging::debug("hosts", "Auto discovery found " + std::to_string(discoveredHosts.size()) + " Moonlight-compatible host(s)");
      if (nextHostProbeTick != nullptr) {
        *nextHostProbeTick = 0U;
      }
    } else {
      logging::info("hosts", "Auto discovery completed without finding any Moonlight-compatible hosts. If you are using xemu's default user-mode network, use TAP networking or add the host manually.");
    }
    if (persistedHostCount > 0U && state.hosts.dirty) {
      persist_hosts(state, feedback);
    }
  }

  void apply_published_host_probe_results(app::ClientState &state, HostProbeTaskState *task) {
    if (task == nullptr) {
      return;
    }

    const std::vector<ui::HostProbeResult> results = ui::drain_host_probe_results(&task->resultQueue);
    for (const ui::HostProbeResult &result : results) {
      if (result.success) {
        if (state.shell.activeScreen == app::ScreenId::hosts && state.hosts.loaded) {
          apply_server_info_to_host(state, result.address, result.port, result.serverInfo);
          task->metadataChanged = task->metadataChanged || state.hosts.dirty;
        }
        ++task->onlineCount;
        continue;
      }

      if (state.shell.activeScreen == app::ScreenId::hosts && state.hosts.loaded) {
        mark_host_offline(state, result.address, result.port);
      }
      ++task->offlineCount;
    }
  }

  void reap_completed_host_probe_workers(HostProbeTaskState *task) {
    if (task == nullptr) {
      return;
    }

    auto iterator = task->workers.begin();
    while (iterator != task->workers.end()) {
      if ((*iterator)->thread == nullptr || !(*iterator)->completed.load()) {
        ++iterator;
        continue;
      }

      int threadResult = 0;
      SDL_WaitThread((*iterator)->thread, &threadResult);
      (void) threadResult;
      iterator = task->workers.erase(iterator);
    }
  }

  /**
   * @brief Merge completed probes and persist changed host details.
   * @param state Client state receiving the probe results.
   * @param task Worker handles and published results.
   * @param feedback Repaints the shell during the save.
   */
  void finish_host_probe_task_if_ready(app::ClientState &state, HostProbeTaskState *task, const ShellSaveFeedback &feedback) {
    if (task == nullptr) {
      return;
    }

    apply_published_host_probe_results(state, task);
    reap_completed_host_probe_workers(task);
    apply_published_host_probe_results(state, task);
    if (host_probe_task_is_active(*task) || !ui::host_probe_result_round_complete(task->resultQueue)) {
      return;
    }

    logging::debug(
      "hosts",
      "Refreshed " + std::to_string(task->onlineCount + task->offlineCount) + " saved host(s): " + std::to_string(task->onlineCount) + " online, " + std::to_string(task->offlineCount) + " offline"
    );
    if (task->metadataChanged) {
      persist_hosts(state, feedback);
    }
    reset_host_probe_task(task);
  }

  void start_host_probe_task_if_needed(const app::ClientState &state, HostProbeTaskState *task, Uint32 now, Uint32 *nextHostProbeTick) {
    if (task == nullptr || host_probe_task_is_active(*task) || state.shell.activeScreen != app::ScreenId::hosts || !state.hosts.loaded || !network::runtime_network_ready()) {
      return;
    }
    if (nextHostProbeTick != nullptr && *nextHostProbeTick != 0U && now < *nextHostProbeTick) {
      return;
    }

    reset_host_probe_task(task);
    task->clientIdentityAvailable = try_load_saved_pairing_identity(&task->clientIdentity);
    ui::begin_host_probe_result_round(&task->resultQueue, state.hosts.items.size());
    for (const app::HostRecord &host : state.hosts.items) {
      auto worker = std::make_unique<HostProbeTaskState::ProbeWorkerState>();
      worker->address = host.address;
      worker->port = app::effective_host_port(host.port);
      worker->clientIdentity = task->clientIdentityAvailable ? &task->clientIdentity : nullptr;
      worker->resultQueue = &task->resultQueue;
      worker->thread = SDL_CreateThread(run_host_probe_task, "probe-saved-host", worker.get());
      if (worker->thread == nullptr) {
        logging::error("hosts", "Failed to start the saved-host refresh worker for " + host.address + ": " + SDL_GetError());
        ui::skip_host_probe_result_target(&task->resultQueue);
        continue;
      }

      task->workers.push_back(std::move(worker));
    }
    if (task->workers.empty()) {
      reset_host_probe_task(task);
      return;
    }

    if (nextHostProbeTick != nullptr) {
      *nextHostProbeTick = now + HOST_PROBE_REFRESH_INTERVAL_MILLISECONDS;
    }
  }

  void start_host_discovery_task_if_needed(const app::ClientState &state, HostDiscoveryTaskState *task, Uint32 now, Uint32 *nextHostDiscoveryTick) {
    if (task == nullptr || host_discovery_task_is_active(*task) || state.shell.activeScreen != app::ScreenId::hosts || !state.hosts.loaded || !network::runtime_network_ready()) {
      return;
    }
    if (nextHostDiscoveryTick != nullptr && *nextHostDiscoveryTick != 0U && now < *nextHostDiscoveryTick) {
      return;
    }

    reset_host_discovery_task(task);
    logging::debug("hosts", "Starting host auto discovery");
    task->thread = SDL_CreateThread(run_host_discovery_task, "discover-hosts", task);
    if (task->thread == nullptr) {
      logging::error("hosts", std::string("Failed to start the host auto-discovery task: ") + SDL_GetError());
      reset_host_discovery_task(task);
      if (nextHostDiscoveryTick != nullptr) {
        *nextHostDiscoveryTick = now + HOST_DISCOVERY_REFRESH_INTERVAL_MILLISECONDS;
      }
      return;
    }

    if (nextHostDiscoveryTick != nullptr) {
      *nextHostDiscoveryTick = now + HOST_DISCOVERY_REFRESH_INTERVAL_MILLISECONDS;
    }
  }

  /**
   * @brief Start a requested pairing worker after handling any completed attempt.
   * @param state Client state to update.
   * @param update Pairing request from the shell.
   * @param task Pairing worker state.
   * @param feedback Repaints the shell if processing the previous attempt saves a host.
   */
  void pair_host_if_requested(app::ClientState &state, const app::AppUpdate &update, PairingTaskState *task, const ShellSaveFeedback &feedback) {
    if (!update.requests.pairingRequested || task == nullptr) {
      return;
    }

    finish_pairing_task_if_ready(state, task, feedback);

    if (pairing_task_is_active(*task)) {
      retire_active_pairing_attempt(task, true);
      logging::info("pairing", "Discarded the previous background pairing attempt and started a fresh one");
    }

    auto attempt = std::make_unique<PairingAttemptState>();
    reset_pairing_attempt(attempt.get());
    attempt->request = {
      update.requests.pairingAddress,
      app::effective_host_port(update.requests.pairingPort),
      update.requests.pairingPin,
      "MoonlightXboxOG",
      {},
    };

    attempt->thread = create_pairing_thread(run_pairing_task, "pair-host", PAIRING_THREAD_STACK_SIZE, attempt.get());
    if (attempt->thread == nullptr) {
      reset_pairing_attempt(attempt.get());
      const std::string createThreadError = std::string("Failed to start the background pairing task: ") + SDL_GetError();
      app::apply_pairing_result(state, update.requests.pairingAddress, update.requests.pairingPort, false, createThreadError);
      state.pairingDraft.generatedPin.clear();
      logging::error("pairing", createThreadError);
      return;
    }

    task->activeAttempt = std::move(attempt);

    state.pairingDraft.stage = app::PairingStage::in_progress;
    state.pairingDraft.statusMessage = "Trying to reach the host now. Enter the PIN on the host when it appears and keep this screen open for the result.";
    state.shell.statusMessage.clear();
    logging::info("pairing", "Started background pairing with " + update.requests.pairingAddress + ":" + std::to_string(update.requests.pairingPort));
  }

  int run_app_list_task(void *context) {  // NOSONAR(cpp:S5008) SDL_CreateThread requires void* callback signature
    auto *task = static_cast<AppListTaskState *>(context);
    if (task == nullptr) {
      return -1;
    }

    network::PairingIdentity clientIdentity {};
    std::string errorMessage;
    if (!load_saved_pairing_identity_for_streaming(&clientIdentity, &errorMessage)) {
      task->success = false;
      task->message = errorMessage;
      task->completed.store(true);
      return 0;
    }

    std::vector<network::HostAppEntry> fetchedApps;
    errorMessage.clear();
    task->success = network::query_app_list(task->address, task->port, &clientIdentity, &fetchedApps, &task->serverInfo, &errorMessage);
    task->serverInfoAvailable = task->serverInfo.httpPort != 0 || task->serverInfo.httpsPort != 0 || !task->serverInfo.hostName.empty() || !task->serverInfo.uuid.empty();
    if (!task->success) {
      task->message = errorMessage.empty() ? "Failed to fetch the host app list" : errorMessage;
      task->completed.store(true);
      return 0;
    }

    task->appListContentHash = network::hash_app_list_entries(fetchedApps);

    task->apps.clear();
    task->apps.reserve(fetchedApps.size());
    const std::string hostIdentity = !task->serverInfo.uuid.empty() ? task->serverInfo.uuid : task->address;
    for (const network::HostAppEntry &entry : fetchedApps) {
      const std::string cacheKey = startup::build_cover_art_cache_key(hostIdentity, task->address, entry.id);
      task->apps.push_back({
        entry.name,
        entry.id,
        entry.hdrSupported,
        entry.hidden,
        false,
        cacheKey,
        startup::cover_art_exists(cacheKey),
        false,
      });
    }

    task->message = task->apps.empty() ? "Host returned no launchable apps for this host" : "Loaded " + std::to_string(task->apps.size()) + " Host app(s)";
    task->completed.store(true);
    return 0;
  }

  /**
   * @brief Publish a completed app list and persist any changed host details.
   * @param state Client state receiving the list.
   * @param task App-list worker state.
   * @param feedback Repaints the shell during the save.
   */
  void finish_app_list_task_if_ready(app::ClientState &state, AppListTaskState *task, const ShellSaveFeedback &feedback) {
    if (task == nullptr || task->thread == nullptr || !task->completed.load()) {
      return;
    }

    SDL_Thread *thread = task->thread;
    task->thread = nullptr;
    int threadResult = 0;
    SDL_WaitThread(thread, &threadResult);
    (void) threadResult;

    const std::string address = task->address;
    const uint16_t port = task->port;
    const bool success = task->success;
    const uint64_t appListContentHash = task->appListContentHash;
    const std::string message = task->message;
    const bool serverInfoAvailable = task->serverInfoAvailable;
    const network::HostPairingServerInfo serverInfo = task->serverInfo;
    std::vector<app::HostAppRecord> apps = std::move(task->apps);
    reset_app_list_task(task);

    if (serverInfoAvailable) {
      apply_server_info_to_host(state, address, port, serverInfo);
    }

    if (success) {
      app::apply_app_list_result(state, address, port, std::move(apps), appListContentHash, true, message);
      logging::info("apps", "Fetched app list from " + address + ":" + std::to_string(serverInfo.httpPort));
      if (state.hosts.dirty) {
        persist_hosts(state, feedback);
      }
      return;
    }

    app::apply_app_list_result(state, address, port, {}, 0, false, message);
    logging::warn("apps", message);
  }

  void start_app_list_task_if_needed(app::ClientState &state, AppListTaskState *task, Uint32 now) {
    if (task == nullptr || app_list_task_is_active(*task) || state.shell.activeScreen != app::ScreenId::apps) {
      return;
    }

    const app::HostRecord *host = app::apps_host(state);
    if (host == nullptr || host->pairingState != app::PairingState::paired || host->reachability == app::HostReachability::offline) {
      return;
    }

    if (host->appListState != app::HostAppListState::loading) {
      if (host->lastAppListRefreshTick != 0U && now - host->lastAppListRefreshTick < APP_LIST_REFRESH_INTERVAL_MILLISECONDS) {
        return;
      }

      if (state.hosts.activeLoaded) {
        app::HostRecord &mutableHost = state.hosts.active;
        mutableHost.appListState = app::HostAppListState::loading;
        mutableHost.appListStatusMessage = (mutableHost.apps.empty() ? "Loading apps for " : "Refreshing apps for ") + mutableHost.displayName + "...";
        state.shell.statusMessage.clear();
      }
    }

    reset_app_list_task(task);
    task->address = host->address;
    task->port = host->resolvedHttpPort == 0 ? app::effective_host_port(host->port) : host->resolvedHttpPort;
    task->thread = SDL_CreateThread(run_app_list_task, "fetch-app-list", task);
    if (task->thread == nullptr) {
      const std::string errorMessage = std::string("Failed to start the app-list fetch task: ") + SDL_GetError();
      logging::error("apps", errorMessage);
      if (state.hosts.activeLoaded) {
        state.hosts.active.appListState = app::HostAppListState::failed;
        state.hosts.active.appListStatusMessage = errorMessage;
        state.shell.statusMessage = errorMessage;
      }
      reset_app_list_task(task);
      return;
    }

    if (state.hosts.activeLoaded) {
      state.hosts.active.lastAppListRefreshTick = now;
    }
  }

  /**
   * @brief Download and store missing cover art until the batch completes or the shell requests a stop.
   * @param context Artwork task retained by the shell until the worker has joined.
   * @return Zero after publishing completion, or a negative value for an invalid task.
   */
  int run_app_art_task(void *context) {  // NOSONAR(cpp:S5008) SDL_CreateThread requires void* callback signature
    auto *task = static_cast<AppArtTaskState *>(context);
    if (task == nullptr) {
      return -1;
    }

    network::PairingIdentity clientIdentity {};
    if (std::string identityError; !load_saved_pairing_identity_for_streaming(&clientIdentity, &identityError)) {
      task->failureCount = task->apps.size();
      task->completed.store(true);
      return 0;
    }

    for (const app::HostAppRecord &appRecord : task->apps) {
      if (task->cancelRequested.load()) {
        break;
      }
      if (appRecord.boxArtCached || appRecord.boxArtCacheKey.empty()) {
        continue;
      }

      std::vector<unsigned char> assetBytes;
      if (!network::query_app_asset(task->address, task->port, &clientIdentity, appRecord.id, &assetBytes, nullptr, &task->cancelRequested)) {
        if (!task->cancelRequested.load()) {
          ++task->failureCount;
        }
        continue;
      }

      if (const startup::SaveCoverArtResult saveResult = startup::save_cover_art(appRecord.boxArtCacheKey, assetBytes); !saveResult.success) {
        ++task->failureCount;
        continue;
      }

      task->cachedAppIds.push_back(appRecord.id);
    }

    task->completed.store(true);
    return 0;
  }

  /**
   * @brief Collect cached artwork from a finished batch or join a batch asked to stop for streaming.
   * @param state Host library receiving the successfully cached app IDs.
   * @param task Artwork batch owned by the shell.
   * @param textureCache Cover-art textures owned by the active renderer.
   */
  void collect_app_art_task(app::ClientState &state, AppArtTaskState *task, CoverArtTextureCache *textureCache) {
    if (task == nullptr || task->thread == nullptr || (!task->completed.load() && !task->cancelRequested.load())) {
      return;
    }

    SDL_Thread *thread = task->thread;
    task->thread = nullptr;
    int threadResult = 0;
    SDL_WaitThread(thread, &threadResult);
    (void) threadResult;

    const std::string address = task->address;
    const uint16_t port = task->port;
    const std::vector<int> cachedAppIds = task->cachedAppIds;
    const std::size_t failureCount = task->failureCount;
    reset_app_art_task(task);

    for (int appId : cachedAppIds) {
      app::mark_cover_art_cached(state, address, port, appId);
    }

    if (textureCache != nullptr) {
      textureCache->failedKeys.clear();
    }

    if (!cachedAppIds.empty()) {
      logging::info("apps", "Cached cover art for " + std::to_string(cachedAppIds.size()) + " app(s)");
    }
    if (failureCount > 0U) {
      logging::warn("apps", std::to_string(failureCount) + " app artwork request(s) fell back to placeholders");
    }
  }

  void start_app_art_task_if_needed(const app::ClientState &state, AppArtTaskState *task) {
    if (task == nullptr || app_art_task_is_active(*task) || state.shell.activeScreen != app::ScreenId::apps) {
      return;
    }

    const app::HostRecord *host = app::apps_host(state);
    if (host == nullptr || host->appListState != app::HostAppListState::ready || host->apps.empty()) {
      return;
    }

    if (const bool missingArt = std::any_of(host->apps.begin(), host->apps.end(), [](const app::HostAppRecord &appRecord) {
          return !appRecord.boxArtCached && !appRecord.boxArtCacheKey.empty();
        });
        !missingArt) {
      return;
    }

    reset_app_art_task(task);
    task->address = host->address;
    task->port = host->httpsPort == 0 ? app::effective_host_port(host->port) : host->httpsPort;
    task->apps = host->apps;
    task->thread = SDL_CreateThread(run_app_art_task, "fetch-app-art", task);
    if (task->thread == nullptr) {
      logging::error("apps", std::string("Failed to start the cover-art fetch task: ") + SDL_GetError());
      reset_app_art_task(task);
    }
  }

  void show_log_file_if_requested(app::ClientState &state, const app::AppUpdate &update) {
    if (!update.requests.logViewRequested) {
      return;
    }

    const std::string filePath = state.settings.logFilePath.empty() ? logging::default_log_file_path() : state.settings.logFilePath;
    const logging::LoadLogFileResult loadedLog = logging::load_log_file(filePath, LOG_VIEWER_MAX_LOADED_LINES);
    app::set_log_file_path(state, loadedLog.filePath);
    if (!loadedLog.errorMessage.empty()) {
      app::apply_log_viewer_contents(state, {loadedLog.errorMessage}, loadedLog.errorMessage);
      logging::warn("logging", loadedLog.errorMessage);
      return;
    }

    std::vector<std::string> lines = loadedLog.lines;
    if (!loadedLog.fileFound) {
      lines = {"The log file does not exist yet."};
    } else if (lines.empty()) {
      lines = {"The log file is empty."};
    }

    const std::string statusMessage = loadedLog.fileFound ? "Loaded recent log file lines" : "No log file has been written yet";
    app::apply_log_viewer_contents(state, std::move(lines), statusMessage);
    logging::info("logging", statusMessage + ": " + loadedLog.filePath);
  }

  int measure_body_lines_height(TTF_Font *font, const std::vector<std::string> &lines, int maxWidth, int lineGap) {
    int textHeight = 0;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      textHeight += measure_wrapped_text_height(font, lines[index], maxWidth);
      if (index + 1U < lines.size()) {
        textHeight += lineGap;
      }
    }
    return textHeight;
  }

  struct BodyLinesRenderLayout {
    SDL_Color color {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF};
    int x = 0;
    int y = 0;
    int maxWidth = 0;
    int lineGap = 0;
  };

  bool render_body_lines(
    SDL_Renderer *renderer,
    TTF_Font *font,
    const std::vector<std::string> &lines,
    const BodyLinesRenderLayout &layout
  ) {
    int cursorY = layout.y;
    for (const std::string &line : lines) {
      int drawnHeight = 0;
      if (!render_text_line(renderer, font, line, layout.color, layout.x, cursorY, layout.maxWidth, &drawnHeight)) {
        return false;
      }
      cursorY += drawnHeight + layout.lineGap;
    }
    return true;
  }

  /**
   * @brief Render settings in stable font-based regions so changing values cannot shift the options.
   *
   * @param renderer Active shell renderer.
   * @param bodyFont Font for options.
   * @param smallFont Font for contextual help.
   * @param viewModel Selected settings and explanation.
   * @param bodyPanel Available settings surface.
   * @param panelPadding Padding inside the surface.
   * @return True when options and help rendered successfully.
   */
  bool render_settings_detail_panel(
    SDL_Renderer *renderer,
    TTF_Font *bodyFont,
    TTF_Font *smallFont,
    const ui::ShellViewModel &viewModel,
    const SDL_Rect &bodyPanel,
    int panelPadding
  ) {
    const int innerWidth = bodyPanel.w - panelPadding * 2;
    const int rowHeight = std::max(34, TTF_GetFontLineSkip(bodyFont) + 12);
    const int innerHeight = bodyPanel.h - panelPadding * 2;
    const auto layout = ui::calculate_settings_detail_layout({bodyPanel.x + panelPadding, bodyPanel.y + panelPadding, innerWidth, innerHeight}, TTF_GetFontLineSkip(smallFont));
    // Crop text to the layout bounds and keep option-row positions independent of glyph metrics.
    const bool success = render_text_line_simple(renderer, smallFont, viewModel.content.selectedMenuRowLabel, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, layout.title.x, layout.title.y, layout.title.w) &&
                         render_bounded_text(renderer, smallFont, viewModel.content.selectedMenuRowDescription, {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, sdl_rect(layout.description));
    draw_line(renderer, layout.optionsHeader.x, layout.optionsHeader.y - 10, layout.optionsHeader.x + layout.optionsHeader.w, layout.optionsHeader.y - 10, MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0x30);
    return success && render_text_line_simple(renderer, smallFont, "Options", {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, layout.optionsHeader.x, layout.optionsHeader.y, layout.optionsHeader.w) && render_action_rows(renderer, bodyFont, viewModel.content.detailMenuRows, sdl_rect(layout.options), rowHeight, {nullptr, viewModel.content.rightPanelActive});
  }

  /**
   * @brief Render portrait covers and separate captions in a scrolling library.
   *
   * @param renderer Active shell renderer.
   * @param smallFont Font for captions and badges.
   * @param viewModel Current library content.
   * @param gridRect Available grid bounds.
   * @param textureCache Cached host artwork.
   * @param assetCache Cached bundled artwork.
   * @return True when every visible tile rendered successfully.
   */
  bool render_app_tiles_grid(
    SDL_Renderer *renderer,
    TTF_Font *smallFont,
    const ui::ShellViewModel &viewModel,
    const SDL_Rect &gridRect,
    CoverArtTextureCache *textureCache,
    AssetTextureCache *assetCache
  ) {
    const int gap = std::max(14, gridRect.w / 50);
    const int columns = std::max(1, static_cast<int>(viewModel.content.appColumnCount));
    const int captionHeight = TTF_GetFontLineSkip(smallFont) + 14;
    const int preferredHeight = ((gridRect.w - gap * (columns - 1)) / columns) * 3 / 2 + captionHeight;
    const ui::ShellRect bounds {gridRect.x + 4, gridRect.y + 4, gridRect.w - 8, gridRect.h - 8};
    const auto grid = ui::calculate_shell_grid(bounds, viewModel.content.appTiles.size(), viewModel.content.appColumnCount, selected_app_tile_index(viewModel.content.appTiles), preferredHeight, gap);
    const auto first = static_cast<std::size_t>(grid.startRow * grid.columns);
    const std::size_t end = std::min(viewModel.content.appTiles.size(), static_cast<std::size_t>((grid.startRow + grid.visibleRows) * grid.columns));
    for (std::size_t index = first; index < end; ++index) {
      const SDL_Rect cell = sdl_rect(ui::shell_grid_cell(bounds, grid, index));
      const int coverHeight = std::max(1, cell.h - captionHeight);
      const int coverWidth = std::min(cell.w, coverHeight * 2 / 3);
      const SDL_Rect cover {cell.x + (cell.w - coverWidth) / 2, cell.y, coverWidth, coverHeight};
      const auto &tile = viewModel.content.appTiles[index];
      if (!render_app_cover(renderer, smallFont, tile, cover, textureCache, assetCache) || !render_text_centered_simple(renderer, smallFont, tile.name, tile.selected ? SDL_Color {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF} : SDL_Color {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, {cell.x, cell.y + coverHeight + 8, cell.w, captionHeight - 8})) {
        return false;
      }
    }
    if (grid.scrollbarWidth > 0) {
      render_vertical_scrollbar(renderer, {gridRect.x + gridRect.w - grid.scrollbarWidth, gridRect.y, grid.scrollbarWidth, gridRect.h}, grid.totalRows, grid.visibleRows, grid.startRow);
    }
    return true;
  }

  /**
   * @brief Render grayscale moon avatars with an independent focus orbit and status labels.
   *
   * @param renderer Active shell renderer.
   * @param bodyFont Font for host names.
   * @param smallFont Font for connection labels.
   * @param assetCache Cached bundled icons.
   * @param viewModel Current host collection.
   * @param gridRect Available content bounds.
   * @return True when all visible cards rendered successfully.
   */
  bool render_host_tiles_grid(SDL_Renderer *renderer, TTF_Font *bodyFont, TTF_Font *smallFont, AssetTextureCache *assetCache, const ui::ShellViewModel &viewModel, const SDL_Rect &gridRect) {
    const int gap = std::max(14, gridRect.w / 45);
    const int columns = DESKTOP_SHELL ? static_cast<int>(viewModel.content.hostColumnCount) : 3;
    const int preferredHeight = std::min(gridRect.h, gridRect.w / columns);
    const ui::ShellRect bounds {gridRect.x, gridRect.y, gridRect.w, gridRect.h};
    const auto grid = ui::calculate_shell_grid(bounds, viewModel.content.hostTiles.size(), viewModel.content.hostColumnCount, selected_host_tile_index(viewModel.content.hostTiles), preferredHeight, gap);
    const auto first = static_cast<std::size_t>(grid.startRow * grid.columns);
    const std::size_t end = std::min(viewModel.content.hostTiles.size(), static_cast<std::size_t>((grid.startRow + grid.visibleRows) * grid.columns));
    for (std::size_t index = first; index < end; ++index) {
      const SDL_Rect card = sdl_rect(ui::shell_grid_cell(bounds, grid, index));
      const auto &tile = viewModel.content.hostTiles[index];
      const int nameHeight = TTF_GetFontLineSkip(bodyFont);
      const int statusHeight = TTF_GetFontLineSkip(smallFont);
      const int nameY = card.y + card.h - nameHeight - statusHeight - 24;
      const int iconSize = std::max(24, std::min(card.w - 24, nameY - card.y - 8));
      const SDL_Rect icon {card.x + (card.w - iconSize) / 2, card.y + std::max(12, (nameY - card.y - iconSize) / 2), iconSize, iconSize};
      render_asset_icon(renderer, assetCache, tile.iconAssetPath, icon);
      if (tile.selected) {
        render_asset_icon(renderer, assetCache, "hosts\\orbit.svg", icon);
      }
      if (!render_text_centered_simple(renderer, bodyFont, tile.displayName, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, {card.x + 12, nameY, card.w - 24, nameHeight})) {
        return false;
      }
      const SDL_Color statusColor {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF};
      if (!render_text_centered_simple(renderer, smallFont, tile.statusLabel, statusColor, {card.x + 12, nameY + nameHeight + 6, card.w - 24, statusHeight})) {
        return false;
      }
    }
    if (grid.scrollbarWidth > 0) {
      render_vertical_scrollbar(renderer, {gridRect.x + gridRect.w - grid.scrollbarWidth, gridRect.y, grid.scrollbarWidth, gridRect.h}, grid.totalRows, grid.visibleRows, grid.startRow);
    }
    return true;
  }

  /**
   * @brief Display empty-library guidance near the page content in desktop windows.
   * @param renderer Active shell renderer.
   * @param smallFont Font used for guidance.
   * @param viewModel Empty-library content.
   * @param gridRect Available content bounds.
   * @return True when the guidance rendered successfully.
   */
  bool render_apps_empty_state(SDL_Renderer *renderer, TTF_Font *smallFont, const ui::ShellViewModel &viewModel, const SDL_Rect &gridRect) {
    const int lineGap = 8;
    const int textHeight = measure_body_lines_height(smallFont, viewModel.content.bodyLines, gridRect.w - 48, lineGap);
    const int top = DESKTOP_SHELL ? 16 : std::max(16, (gridRect.h - textHeight) / 2);
    return render_body_lines(
      renderer,
      smallFont,
      viewModel.content.bodyLines,
      {{TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, gridRect.x + 24, gridRect.y + top, gridRect.w - 48, lineGap}
    );
  }

  /**
   * @brief Fonts and input hint styling shared by the shell and its dialogs.
   */
  struct DialogFonts {
    TTF_Font *titleFont;  ///< Dialog heading font.
    TTF_Font *bodyFont;  ///< Dialog body font.
    TTF_Font *smallFont;  ///< Helper and action font.
    FooterHintStyle hints;  ///< Sizes for controller glyphs and keyboard hints.
  };

  /**
   * @brief Render a shell modal and optionally retain its folder-list viewport.
   * @param renderer Dialog renderer.
   * @param fonts Heading, body and helper fonts and input hint styling.
   * @param assetCache Cached action icons.
   * @param modal Dialog content.
   * @param width Logical display width.
   * @param height Framebuffer height.
   * @param folder Optional retained folder state.
   * @return True when the dialog rendered successfully.
   */
  bool render_shell_dialog(SDL_Renderer *renderer, const DialogFonts &fonts, AssetTextureCache *assetCache, const ui::ShellModalView &modal, int width, int height, FolderDialogCache *folder = nullptr) {
    const auto &[titleFont, bodyFont, smallFont, hints] = fonts;
    const int padding = std::max(18, height / 40);
    const int dialogWidth = std::min(760, width * 3 / 4);
    const int innerWidth = dialogWidth - padding * 2;
    const int statusIconSize = modal.iconAssetPath.empty() ? 0 : std::max(28, TTF_GetFontLineSkip(titleFont));
    const int titleHeight = std::max(TTF_GetFontLineSkip(titleFont), statusIconSize) + 18;
    const std::vector<ui::ShellFooterAction> defaultActions = modal.actions.empty() ?
                                                                std::vector<ui::ShellFooterAction> {{"close", "Close", "icons\\button-b.svg", {}, false}} :
                                                                std::vector<ui::ShellFooterAction> {{"select", "Select", "icons\\button-a.svg", {}, true}, {"back", "Back", "icons\\button-b.svg", {}, false}};
    const auto &footerActions = modal.footerActions.empty() ? defaultActions : modal.footerActions;
    const int hintHeight = measure_footer_actions_height(smallFont, footerActions, innerWidth, hints);
    const int footerHeight = std::max({40, TTF_GetFontLineSkip(smallFont) + 18, hintHeight});
    const int rowHeight = std::max(36, TTF_GetFontLineSkip(bodyFont) + 16);
    const int lineGap = modal.compactBody ? 3 : 8;
    const int textHeight = measure_body_lines_height(smallFont, modal.lines, innerWidth, lineGap);
    const int actionsHeight = static_cast<int>(modal.actions.size()) * (rowHeight + 6);
    const SDL_Rect bounds = sdl_rect(ui::calculate_shell_dialog(width, height, dialogWidth, padding * 2 + titleHeight + textHeight + actionsHeight + footerHeight + (modal.compactBody ? 22 : 20)));
    fill_rect(renderer, {0, 0, width, height}, 0, 0, 0, 0xA0);
    render_dialog_surface(renderer, bounds);
    const int titleInset = statusIconSize == 0 ? 0 : statusIconSize + 12;
    if (statusIconSize > 0) {
      render_asset_icon(renderer, assetCache, modal.iconAssetPath, {bounds.x + padding, bounds.y + 28, statusIconSize, statusIconSize}, modal.busy ? ui::shell_spinner_angle(SDL_GetTicks()) : 0.0);
    }
    if (!render_text_line_simple(renderer, titleFont, modal.title, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, bounds.x + padding + titleInset, bounds.y + 28, innerWidth - titleInset)) {
      return false;
    }
    const int footerY = bounds.y + bounds.h - padding - footerHeight;
    int cursorY = bounds.y + padding + titleHeight + 10;
    // Reserve the directory path / dialog guidance before sizing a scrolling
    // action list, so a large folder cannot push its own breadcrumb offscreen.
    const int availableHeight = std::max(0, footerY - cursorY - 12);
    const int actionSpace = std::min(actionsHeight, std::max(0, availableHeight - std::min(textHeight, availableHeight / 3)));
    const int textBottom = footerY - actionSpace - 12;
    for (const auto &line : modal.lines) {
      int drawnHeight = 0;
      if (!render_bounded_text(renderer, smallFont, line, {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, {bounds.x + padding, cursorY, innerWidth, textBottom - cursorY}, &drawnHeight)) {
        return false;
      }
      cursorY += drawnHeight + lineGap;
    }
    const SDL_Rect rows {bounds.x + padding, cursorY + 4, innerWidth, footerY - cursorY - 12};
    if (folder != nullptr) {
      folder->rows = rows;
      folder->rowHeight = rowHeight;
    }
    if (!modal.actions.empty() && !render_action_rows(renderer, bodyFont, modal.actions, rows, rowHeight, {assetCache, true, folder})) {
      return false;
    }
    if (modal.streamStartupStep.has_value()) {
      constexpr int progressHeight = 8;
      ui::draw_dialog_progress(renderer, {bounds.x + padding, footerY + (footerHeight - progressHeight) / 2, innerWidth, progressHeight}, static_cast<int>(*modal.streamStartupStep), ui::STREAM_STARTUP_STEP_COUNT);
      return true;
    }
    draw_line(renderer, bounds.x + padding, footerY - 6, bounds.x + bounds.w - padding, footerY - 6, MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0x25);
    if (modal.busy) {
      return render_text_line_simple(renderer, smallFont, "Please wait...", {ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF}, bounds.x + padding, footerY + 8, innerWidth);
    }
    return render_footer_actions(renderer, smallFont, assetCache, footerActions, {bounds.x + padding, footerY + (footerHeight - hintHeight) / 2, innerWidth, hintHeight}, hints);
  }

  /**
   * @brief Render local numeric or text entry with cached labels and optional shared keyboard geometry.
   *
   * @param renderer Active renderer.
   * @param fonts Keypad text fonts and shared controller glyph size.
   * @param assetCache Cached button glyphs.
   * @param viewModel Current keypad state.
   * @param displayBounds Logical display rectangle.
   * @param cache Reusable keypad text and measurements.
   * @return True when the complete keypad rendered.
   */
  bool render_keypad_dialog(SDL_Renderer *renderer, const DialogFonts &fonts, AssetTextureCache *assetCache, const ui::ShellViewModel &viewModel, const SDL_Rect &displayBounds, KeypadModalLayoutCache *cache) {
    auto *bodyFont = fonts.bodyFont;
    auto *smallFont = fonts.smallFont;
    const FooterHintStyle hints {fonts.hints.glyphSize, true};
    const int width = displayBounds.w;
    const int height = displayBounds.h;
    const int padding = std::max(18, height / 40);
    const int dialogWidth = std::min(720, width * 4 / 5);
    const int innerWidth = dialogWidth - padding * 2;
    const int columns = std::max(1, static_cast<int>(viewModel.keypad.columnCount));
    const bool keyboardGeometry = viewModel.keypad.rowCount != 0;
    const int rows = keyboardGeometry ? static_cast<int>(viewModel.keypad.rowCount) : std::max(1, static_cast<int>((viewModel.keypad.buttons.size() + columns - 1) / columns));
    const int gap = 8;
    const int titleHeight = TTF_GetFontLineSkip(bodyFont) + 14;
    const int hintHeight = keypad_modal_text_height(renderer, smallFont, viewModel, innerWidth, cache);
    const int valueHeight = TTF_GetFontLineSkip(bodyFont) + 20;
    const int footerHeight = measure_footer_actions_height(smallFont, viewModel.keypad.footerActions, innerWidth, hints);
    const int preferredKeyHeight = std::max(36, TTF_GetFontLineSkip(bodyFont) + 18);
    const int desiredHeight = padding * 2 + 12 + titleHeight + hintHeight + valueHeight + 20 + rows * (preferredKeyHeight + gap) + footerHeight;
    const SDL_Rect bounds = sdl_rect(ui::calculate_shell_dialog(width, height, dialogWidth, desiredHeight));
    fill_rect(renderer, displayBounds, 0, 0, 0, 0xA0);
    render_dialog_surface(renderer, bounds);
    if (!ensure_single_line_text_texture(renderer, bodyFont, viewModel.keypad.title, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, innerWidth, &cache->titleTexture) || !render_cached_text_texture(renderer, cache->titleTexture, bounds.x + padding, bounds.y + 28)) {
      return false;
    }
    int cursorY = bounds.y + padding + titleHeight + 12;
    for (const auto &line : cache->lineTextures) {
      int drawnHeight = 0;
      if (!render_cached_text_texture(renderer, line, bounds.x + padding, cursorY, &drawnHeight)) {
        return false;
      }
      cursorY += drawnHeight + 6;
    }
    const SDL_Rect valueRect {bounds.x + padding, cursorY + 2, innerWidth, valueHeight};
    if (viewModel.keypad.segmentedAddress) {
      const int separatorWidth = 18;
      const auto octets = ui::calculate_keypad_columns({valueRect.x, valueRect.y, valueRect.w, valueRect.h}, viewModel.keypad.addressOctets.size(), TTF_GetFontLineSkip(bodyFont) * 4, separatorWidth);
      for (std::size_t index = 0; index < viewModel.keypad.addressOctets.size(); ++index) {
        const SDL_Rect octetRect = sdl_rect(octets[index]);
        render_card_surface(renderer, octetRect, index == viewModel.keypad.activeOctet);
        const auto &digits = viewModel.keypad.addressOctets[index];
        const SDL_Color color = digits.empty() ? SDL_Color {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF} : SDL_Color {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF};
        if (!render_text_centered_simple(renderer, bodyFont, digits.empty() ? "___" : digits, color, octetRect)) {
          return false;
        }
        if (index < 3U && !render_text_centered_simple(renderer, bodyFont, ".", {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, {octetRect.x + octetRect.w, valueRect.y, separatorWidth, valueRect.h})) {
          return false;
        }
      }
    } else {
      fill_rounded_rect(renderer, valueRect, {BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xFF}, 6);
      if (!render_text_line_simple(renderer, bodyFont, viewModel.keypad.value, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, valueRect.x + 12, valueRect.y + 8, valueRect.w - 24)) {
        return false;
      }
    }
    const int keysY = valueRect.y + valueRect.h + 12;
    const int footerY = bounds.y + bounds.h - padding - footerHeight;
    const int keyHeight = std::max(1, (footerY - 12 - keysY - gap * (rows - 1)) / rows);
    const auto keyColumns = ui::calculate_keypad_columns({bounds.x + padding, keysY, innerWidth, keyHeight}, static_cast<std::size_t>(columns), TTF_GetFontLineSkip(bodyFont) * 5, gap);
    if (cache->buttonLabelTextures.size() > viewModel.keypad.buttons.size()) {
      for (std::size_t index = viewModel.keypad.buttons.size(); index < cache->buttonLabelTextures.size(); ++index) {
        clear_cached_text_texture(&cache->buttonLabelTextures[index]);
      }
    }
    cache->buttonLabelTextures.resize(viewModel.keypad.buttons.size());
    for (std::size_t index = 0; index < viewModel.keypad.buttons.size(); ++index) {
      const auto &column = keyColumns[index % columns];
      SDL_Rect key {column.x, keysY + static_cast<int>(index / columns) * (keyHeight + gap), column.w, keyHeight};
      const auto &button = viewModel.keypad.buttons[index];
      if (keyboardGeometry) {
        const int left = bounds.x + padding + button.column * innerWidth / columns;
        const int right = bounds.x + padding + (button.column + button.width) * innerWidth / columns;
        key = {left + 1, keysY + button.row * (keyHeight + gap), right - left - 3, button.height * (keyHeight + gap) - gap};
      }
      render_card_surface(renderer, key, button.selected);
      if (!ensure_single_line_text_texture(renderer, bodyFont, button.label, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, key.w, &cache->buttonLabelTextures[index]) || !render_cached_centered_text_texture(renderer, cache->buttonLabelTextures[index], key)) {
        return false;
      }
    }
    draw_line(renderer, bounds.x + padding, footerY - 6, bounds.x + bounds.w - padding, footerY - 6, MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0x25);
    const auto &actions = viewModel.keypad.footerActions;
    return render_footer_actions(renderer, smallFont, assetCache, actions, {bounds.x + padding, footerY, innerWidth, footerHeight}, hints);
  }

#ifdef NXDK
  /**
   * @brief Describe the Xbox framebuffer format and bytes per pixel.
   * @param bpp Video depth.
   * @return SDL format and byte count; zero bytes for an unsupported mode.
   */
  std::pair<Uint32, int> folder_framebuffer_format(int bpp) {
    if (bpp == 32) {
      return {SDL_PIXELFORMAT_XRGB8888, 4};
    }
    if (bpp == 16) {
      return {SDL_PIXELFORMAT_RGB565, 2};
    }
    return {SDL_PIXELFORMAT_XRGB1555, bpp == 15 ? 2 : 0};
  }

  /**
   * @brief Copy damaged rows into Xbox scanout after validating the source and destination formats.
   * @param surface Source software window surface.
   * @param framebuffer Write-combined scanout storage.
   * @param mode Current Xbox video mode.
   * @param area Validated repaint bounds.
   * @param bytes Bytes per pixel.
   * @return False if locking failed and ordinary SDL presentation is required.
   */
  bool copy_folder_framebuffer(SDL_Surface &surface, std::uint8_t *framebuffer, const VIDEO_MODE &mode, const ui::ShellRect &area, int bytes) {
    const bool locked = SDL_MUSTLOCK(&surface) != 0;
    if (locked && !SDL_LockSurface(&surface)) {
      return false;
    }
    for (int y = area.y; y < area.y + area.h; ++y) {
      const auto *source = static_cast<const std::uint8_t *>(surface.pixels) + static_cast<std::size_t>(y) * surface.pitch + area.x * bytes;
      auto *destination = framebuffer + (static_cast<std::size_t>(y) * mode.width + area.x) * bytes;
      std::memcpy(destination, source, static_cast<std::size_t>(area.w) * bytes);
    }
    if (locked) {
      SDL_UnlockSurface(&surface);
    }
    XVideoFlushFB();
    return true;
  }
#endif

  /**
   * @brief Present changed folder rows without the Xbox SDL driver's full-screen copy, retaining SDL fallback.
   * @param renderer Software shell renderer to flush.
   * @param window Shared window backing the renderer, or null for ordinary SDL presentation.
   * @param logicalDamage Repainted rows in logical display coordinates.
   * @param logicalWidth Logical width before encoder correction.
   */
  void present_folder_damage(SDL_Renderer *renderer, SDL_Window *window, const SDL_Rect &logicalDamage, int logicalWidth) {
#ifdef NXDK
    if (const char *name = SDL_GetRendererName(renderer); window != nullptr && name != nullptr && std::strcmp(name, SDL_SOFTWARE_RENDERER) == 0) {
      SDL_FlushRenderer(renderer);
      SDL_Surface *surface = SDL_GetWindowSurface(window);
      const VIDEO_MODE mode = XVideoGetMode();
      void *framebuffer = XVideoGetFB();
      const auto [format, bytes] = folder_framebuffer_format(mode.bpp);
      // The Xbox software window surface uses the video mode's RGB format.
      // Validate that relationship before copying into write-combined scanout.
      if (surface != nullptr && framebuffer != nullptr && bytes > 0 && surface->w == mode.width && surface->h == mode.height && surface->format == format && surface->pitch >= mode.width * bytes) {
        const auto area = ui::framebuffer_repaint_rect({logicalDamage.x, logicalDamage.y, logicalDamage.w, logicalDamage.h}, logicalWidth, mode.width, mode.height);
        if (copy_folder_framebuffer(*surface, static_cast<std::uint8_t *>(framebuffer), mode, area, bytes)) {
          return;
        }
      }
    }
#else
    (void) window;
    (void) logicalDamage;
    (void) logicalWidth;
#endif
    SDL_RenderPresent(renderer);
  }

  /**
   * @brief Load a different capture once, bounding allocations before decoding its BMP pixels.
   * @param renderer Renderer that owns the single image texture.
   * @param path Current capture path, or empty for an empty library.
   * @param cache Image and any load error retained until selection changes.
   */
  void load_screenshot_texture(SDL_Renderer *renderer, const std::string &path, ScreenshotTextureCache &cache) {
    if (cache.path == path) {
      return;
    }
    destroy_texture(cache.texture);
    cache = {};
    cache.path = path;
    if (path.empty()) {
      return;
    }
    std::uint64_t size = 0;
    if (!platform::try_get_file_size(path, &size)) {
      cache.error = "Screenshot unavailable. The file may have been moved or deleted.";
      return;
    }
    FILE *file = std::fopen(path.c_str(), "rb");
    std::array<char, 54> header {};
    const bool read = file != nullptr && std::fread(header.data(), 1, header.size(), file) == header.size();
    if (file != nullptr) {
      std::fclose(file);
    }
    if (const auto image = read ? ui::screenshot_bitmap_size({header.data(), header.size()}, size) : ui::ScreenshotImageSize {}; image.width == 0) {
      cache.error = "Cannot open this screenshot. The bitmap is damaged or unsupported.";
      return;
    }
    if (SDL_Surface *surface = SDL_LoadBMP(path.c_str()); surface != nullptr) {
      cache.texture = SDL_CreateTextureFromSurface(renderer, surface);
      SDL_DestroySurface(surface);
    }
    if (cache.texture == nullptr) {
      cache.error = "Cannot load the screenshot. Close the viewer and try again.";
      logging::warn("screenshots", "Failed to load " + path + ": " + SDL_GetError());
    }
  }

  /**
   * @brief Present an app capture with the shell's existing image fitting and input glyphs.
   * @param renderer Active software renderer, also used by the Xbox framebuffer path.
   * @param bodyFont Heading and error font.
   * @param smallFont Caption and helper font.
   * @param view Current capture, metadata and input hints.
   * @param bounds Television-safe logical content area.
   * @param cache Single retained capture texture.
   * @return True when the viewer frame was drawn successfully.
   */
  bool render_screenshot_viewer(SDL_Renderer *renderer, TTF_Font *bodyFont, TTF_Font *smallFont, const ui::ShellViewModel &view, const SDL_Rect &bounds, ScreenshotTextureCache &cache) {
    load_screenshot_texture(renderer, view.screenshot.path, cache);
    const SDL_Color text {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF};
    const SDL_Color muted {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF};
    const int heading = TTF_GetFontLineSkip(bodyFont) + 8;
    const int caption = TTF_GetFontLineSkip(smallFont) + 6;
    const SDL_Rect picture {bounds.x, bounds.y + heading + caption, bounds.w, std::max(1, bounds.h - heading - caption * 2 - 12)};
    if (!render_text_line_simple(renderer, bodyFont, view.modal.title, text, bounds.x, bounds.y, bounds.w) || !render_text_line_simple(renderer, smallFont, view.screenshot.caption, muted, bounds.x, bounds.y + heading, bounds.w)) {
      return false;
    }
    if (cache.texture != nullptr) {
      if (!render_texture_fit(renderer, cache.texture, picture)) {
        return false;
      }
    } else {
      const std::string message = !cache.error.empty() ? cache.error : view.screenshot.status;
      if (!render_bounded_text(renderer, bodyFont, message, text, picture)) {
        return false;
      }
    }
    if (const std::string details = !view.screenshot.status.empty() && !view.screenshot.path.empty() ? view.screenshot.status : view.screenshot.path; !render_text_line_simple(renderer, smallFont, details, muted, bounds.x, picture.y + picture.h + 6, bounds.w)) {
      return false;
    }
    return true;
  }

  /**
   * @brief Draw the save indicator in its reserved place below the footer divider.
   * @param renderer Active shell renderer.
   * @param indicator Cached save artwork.
   * @param frame Sampled save animation time, or no active save.
   * @param bounds Save indicator rectangle from the shared page layout.
   */
  void draw_shell_save_indicator(SDL_Renderer *renderer, ui::SaveIndicator &indicator, const std::optional<Uint64> &frame, ui::ShellRect bounds) {
    if (frame.has_value()) {
      indicator.draw(renderer, sdl_rect(bounds), *frame);
    }
  }

  /**
   * @brief Current shell output and the coordinate space used to position controls.
   */
  struct ShellRenderSize {
    int framebufferWidth = 0;  ///< Physical width of the renderer's window.
    int framebufferHeight = 0;  ///< Physical height of the renderer's window.
    int width = 0;  ///< Width available to the shell layout.
    int height = 0;  ///< Height available to the shell layout.
  };

  /**
   * @brief Measure the layout area from the current window or television mode.
   * @param renderer Renderer drawing the shell.
   * @param videoMode Output resolution and pixel aspect information.
   * @param encoderSettings Encoder flags used for widescreen correction.
   * @return Current physical and logical dimensions, or empty when unavailable.
   */
  ShellRenderSize measure_shell_render_size(SDL_Renderer *renderer, const VIDEO_MODE &videoMode, unsigned long encoderSettings) {
    ShellRenderSize size;
    if (!SDL_GetCurrentRenderOutputSize(renderer, &size.framebufferWidth, &size.framebufferHeight) || size.framebufferWidth <= 0 || size.framebufferHeight <= 0) {
      return {};
    }
    if constexpr (DESKTOP_SHELL) {
      const auto viewport = ui::calculate_desktop_shell_viewport(size.framebufferWidth, size.framebufferHeight, SDL_GetWindowDisplayScale(SDL_GetRenderWindow(renderer)));
      size.width = viewport.w;
      size.height = viewport.h;
    } else {
      size.width = splash::calculate_display_width(size.framebufferHeight, videoMode, encoderSettings);
      size.height = size.framebufferHeight;
    }
    return size;
  }

  /**
   * @brief Keep desktop focus movement aligned with the columns shown at this window width.
   * @param state Client state used by navigation and the shell view model.
   * @param width Current shell viewport width in layout units.
   */
  void update_shell_grid_columns(app::ClientState &state, int width) {
    if constexpr (DESKTOP_SHELL) {
      const auto columns = ui::calculate_desktop_grid_columns(width);
      state.hosts.columnCount = columns.hosts;
      state.apps.columnCount = columns.apps;
    }
  }

  /**
   * @brief Compose the shell, show a capture, or update retained folder rows without rebuilding unchanged page pixels.
   *
   * @param renderer Active SDL renderer, including the software fallback.
   * @param size Physical output and corresponding shell layout dimensions.
   * @param fonts Shell fonts and control hint styling.
   * @param viewModel Render-ready page and modal state.
   * @param textureCache Cached host artwork.
   * @param assetCache Cached SVG icons.
   * @param keypadModalLayoutCache Reusable keypad layout and text textures.
   * @param saveIndicator Cached save artwork.
   * @param saveFrame Sampled save animation time, or no active save.
   * @return True when the complete frame was presented successfully.
   */
  bool draw_shell(  // NOSONAR(cpp:S107) one-frame shell rendering keeps layout dependencies explicit
    SDL_Renderer *renderer,
    const ShellRenderSize &size,
    const DialogFonts &fonts,
    const ui::ShellViewModel &viewModel,
    CoverArtTextureCache *textureCache,
    AssetTextureCache *assetCache,
    KeypadModalLayoutCache *keypadModalLayoutCache,
    ui::SaveIndicator &saveIndicator,
    const std::optional<Uint64> &saveFrame
  ) {
    const auto &[titleFont, bodyFont, smallFont, hints] = fonts;
    const auto &[framebufferWidth, framebufferHeight, screenWidth, screenHeight] = size;
    if (screenWidth <= 0 || screenHeight <= 0) {
      return false;
    }
    const float horizontalScale = static_cast<float>(framebufferWidth) / static_cast<float>(screenWidth);
    const float verticalScale = static_cast<float>(framebufferHeight) / static_cast<float>(screenHeight);

    // Partial redraws depend on a retained window surface. The GPU renderer flips buffers.
    auto *folder = keypadModalLayoutCache != nullptr && keypadModalLayoutCache->window != nullptr ? &keypadModalLayoutCache->folder : nullptr;
    const bool folderBrowser = viewModel.modal.visible && !viewModel.modal.busy && !viewModel.modal.actions.empty() && !viewModel.logViewer.visible && !viewModel.screenshot.visible && !viewModel.overlay.visible && !viewModel.keypad.visible;
    if (folder != nullptr) {
      if (const bool reusable = !saveFrame.has_value() && folderBrowser && folder->valid && folder->logicalWidth == screenWidth && folder->framebufferWidth == framebufferWidth && folder->framebufferHeight == framebufferHeight && folder->titleFont == titleFont && folder->bodyFont == bodyFont && folder->smallFont == smallFont && ui::same_action_dialog_content(folder->content, viewModel.modal); reusable) {
        const auto selected = selected_action_index(viewModel.modal.actions);
        if (selected == folder->selected) {
          return true;
        }
        SDL_SetRenderScale(renderer, horizontalScale, 1.0f);
        const auto oldFirst = folder->first;
        const auto oldSelected = folder->selected;
        const bool painted = render_action_rows(renderer, bodyFont, viewModel.modal.actions, folder->rows, folder->rowHeight, {assetCache, true, folder, true});
        SDL_Rect damage = folder->rows;
        if (oldFirst == folder->first) {
          const auto top = std::min(oldSelected, selected) - folder->first;
          const auto bottom = std::max(oldSelected, selected) - folder->first;
          damage.y += static_cast<int>(top) * (folder->rowHeight + 6);
          damage.h = static_cast<int>(bottom - top) * (folder->rowHeight + 6) + folder->rowHeight;
        }
        present_folder_damage(renderer, keypadModalLayoutCache->window, damage, screenWidth);
        SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        return painted;
      }
      clear_folder_dialog_cache(*folder);
    }
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);

    const bool screenshotViewer = viewModel.screenshot.visible && !viewModel.overlay.visible && keypadModalLayoutCache != nullptr;
    const auto &footerActions = screenshotViewer ? viewModel.modal.footerActions : viewModel.frame.footerActions;
    const ui::ShellLayoutOptions layoutOptions {DESKTOP_SHELL, viewModel.frame.screen == app::ScreenId::hosts};
    auto layout = ui::calculate_shell_layout(screenWidth, screenHeight, hints.glyphSize, 0, layoutOptions);
    if (const int hintsHeight = measure_footer_actions_height(smallFont, footerActions, layout.footerHints.w, hints); hintsHeight > layout.footerHints.h) {
      layout = ui::calculate_shell_layout(screenWidth, screenHeight, hints.glyphSize, hintsHeight, layoutOptions);
    }
    const int outerMargin = layout.navigation.x;
    const int panelGap = layout.gap;
    const SDL_Rect headerRect = sdl_rect(layout.navigation);
    const SDL_Rect contentRect = sdl_rect(layout.content);
    const SDL_Rect footerRect = sdl_rect(layout.footer);

    SDL_SetRenderDrawColor(renderer, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xFF);
    if (!SDL_RenderClear(renderer) || !SDL_SetRenderScale(renderer, horizontalScale, verticalScale)) {
      return false;
    }

    const int footerDividerY = footerRect.y - 1;
    const int contentBottom = contentRect.y + contentRect.h;
    draw_line(renderer, footerRect.x, footerDividerY, footerRect.x + footerRect.w, footerDividerY, MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0x30);
    if (screenshotViewer) {
      const SDL_Rect viewerBounds {headerRect.x, headerRect.y, headerRect.w, contentBottom - headerRect.y};
      if (!render_screenshot_viewer(renderer, bodyFont, smallFont, viewModel, viewerBounds, keypadModalLayoutCache->screenshot) || !render_footer_actions(renderer, smallFont, assetCache, footerActions, sdl_rect(layout.footerHints), hints)) {
        return false;
      }
      draw_shell_save_indicator(renderer, saveIndicator, saveFrame, layout.saveIndicator);
      SDL_RenderPresent(renderer);
      SDL_SetRenderScale(renderer, 1.0f, 1.0f);
      return true;
    }

    const int logoSize = std::max(26, headerRect.h - 12);
    const SDL_Rect logoRect {headerRect.x, headerRect.y + (headerRect.h - logoSize) / 2, logoSize, logoSize};
    render_asset_icon(renderer, assetCache, "moonlight-logo.svg", logoRect);
    const SDL_Rect toolbarRect = sdl_rect(layout.toolbar);
    const bool stackedToolbar = toolbarRect.y != headerRect.y;
    const int brandWidth = stackedToolbar ? headerRect.w : headerRect.w / 3;
    if (!render_text_line_simple(renderer, bodyFont, viewModel.frame.title, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, logoRect.x + logoSize + 10, headerRect.y + (headerRect.h - TTF_GetFontLineSkip(bodyFont)) / 2, brandWidth - logoSize - 10)) {
      return false;
    }
    if (viewModel.frame.screen == app::ScreenId::hosts) {
      const int buttonGap = std::max(6, layout.gap / 2);
      const int availableWidth = (toolbarRect.w - buttonGap * 2 - (stackedToolbar ? 0 : brandWidth)) / 3;
      const int buttonWidth = stackedToolbar ? availableWidth : std::min(DESKTOP_SHELL ? 144 : screenHeight / 5, availableWidth);
      int buttonX = stackedToolbar ? toolbarRect.x : toolbarRect.x + toolbarRect.w - buttonWidth * 3 - buttonGap * 2;
      for (const auto &button : viewModel.content.toolbarButtons) {
        if (!render_toolbar_button(renderer, bodyFont, smallFont, assetCache, button, {buttonX, toolbarRect.y, buttonWidth, toolbarRect.h})) {
          return false;
        }
        buttonX += buttonWidth + buttonGap;
      }
    }
    if (!render_text_line_simple(renderer, titleFont, viewModel.frame.pageTitle, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, layout.heading.x, layout.heading.y, layout.heading.w) || !render_text_line_simple(renderer, smallFont, viewModel.frame.subtitle, {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, layout.heading.x, layout.heading.y + TTF_GetFontLineSkip(titleFont) + 5, layout.heading.w)) {
      return false;
    }

    if (viewModel.frame.screen == app::ScreenId::hosts) {
      if (!viewModel.content.hostTiles.empty()) {
        if (!render_host_tiles_grid(renderer, bodyFont, smallFont, assetCache, viewModel, contentRect)) {
          return false;
        }
      } else {
        const int iconSize = std::max(40, (DESKTOP_SHELL ? std::min(screenHeight, 720) : screenHeight) / 8);
        const SDL_Rect icon {contentRect.x + (contentRect.w - iconSize) / 2, contentRect.y + 8, iconSize, iconSize};
        render_asset_icon(renderer, assetCache, "icons\\circle-plus.svg", icon);
        const SDL_Rect messageRect {contentRect.x, icon.y + icon.h + 12, contentRect.w, contentRect.h - icon.h - 20};
        if (!render_apps_empty_state(renderer, smallFont, viewModel, messageRect)) {
          return false;
        }
      }
    } else if (viewModel.frame.screen == app::ScreenId::apps) {
      if (!viewModel.content.appTiles.empty()) {
        if (!render_app_tiles_grid(renderer, smallFont, viewModel, contentRect, textureCache, assetCache)) {
          return false;
        }
      } else if (!render_apps_empty_state(renderer, smallFont, viewModel, contentRect)) {
        return false;
      }
    } else {
      const bool settingsScreen = viewModel.frame.screen == app::ScreenId::settings;
      const bool hasDetailMenu = settingsScreen && !viewModel.content.detailMenuRows.empty();
      const int panelPadding = DESKTOP_SHELL ? 14 : std::max(14, screenWidth / 96);
      const int menuPanelWidth = contentRect.w * 30 / 100;
      const auto panels = DESKTOP_SHELL ? ui::calculate_desktop_menu_panels(layout, settingsScreen) : ui::ShellMenuPanels {
                                                                                                        {contentRect.x, contentRect.y, menuPanelWidth, contentRect.h},
                                                                                                        settingsScreen ? ui::calculate_settings_panel(layout) : ui::ShellRect {contentRect.x + menuPanelWidth + panelGap, contentRect.y, contentRect.w - menuPanelWidth - panelGap, contentRect.h},
                                                                                                      };
      const SDL_Rect menuPanel = sdl_rect(panels.menu);
      const SDL_Rect bodyPanel = sdl_rect(panels.detail);
      fill_rounded_rect(renderer, bodyPanel, {PANEL_ALT_RED, PANEL_ALT_GREEN, PANEL_ALT_BLUE, 0xFF});
      // Focus belongs to the selected row, rather than an entire outlined pane.

      const SDL_Rect menuHeaderRect {menuPanel.x + panelPadding, menuPanel.y + panelPadding, menuPanel.w - (panelPadding * 2), std::max(34, TTF_GetFontLineSkip(smallFont) + 10)};
      if (!render_text_line_simple(renderer, smallFont, settingsScreen ? "Categories" : "Actions", {ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF}, menuHeaderRect.x + 10, menuHeaderRect.y + std::max(6, (menuHeaderRect.h - TTF_GetFontLineSkip(smallFont)) / 2), menuHeaderRect.w - 20)) {
        return false;
      }

      const int menuRowHeight = std::max(36, DESKTOP_SHELL ? TTF_GetFontLineSkip(bodyFont) + 16 : screenHeight / 13);
      if (!render_action_rows(renderer, bodyFont, viewModel.content.menuRows, {menuPanel.x + panelPadding, menuHeaderRect.y + menuHeaderRect.h + 12, menuPanel.w - (panelPadding * 2), menuPanel.h - (menuHeaderRect.h + (panelPadding * 2) + 12)}, menuRowHeight, {assetCache, viewModel.content.leftPanelActive})) {
        return false;
      }

      if (hasDetailMenu) {
        if (!render_settings_detail_panel(renderer, bodyFont, smallFont, viewModel, bodyPanel, panelPadding)) {
          return false;
        }
      } else {
        if (!render_body_lines(
              renderer,
              bodyFont,
              viewModel.content.bodyLines,
              {{TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, bodyPanel.x + panelPadding, bodyPanel.y + panelPadding, bodyPanel.w - (panelPadding * 2), 8}
            )) {
          return false;
        }
      }
    }

    if (!viewModel.modal.visible && !viewModel.keypad.visible && !render_footer_actions(renderer, smallFont, assetCache, footerActions, sdl_rect(layout.footerHints), hints)) {
      return false;
    }

    if (viewModel.notification.visible) {
      const int notificationWidth = std::min(420, std::max(300, screenWidth / 3));
      const SDL_Rect notificationArea {screenWidth - outerMargin - notificationWidth, outerMargin, notificationWidth, contentBottom + panelGap - outerMargin - 14};
      if (!render_notifications(renderer, bodyFont, smallFont, assetCache, viewModel.notification.items, notificationArea, hints)) {
        return false;
      }
    }

    if (viewModel.overlay.visible) {
      const int overlayX = (screenWidth / 2) + (panelGap / 2);
      const SDL_Rect overlayRect {
        overlayX,
        outerMargin + 28,
        screenWidth - overlayX - outerMargin - 8,
        screenHeight - ((outerMargin + 28) * 2)
      };
      SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
      fill_rect(renderer, overlayRect, 0x00, 0x00, 0x00, 0xD8);
      draw_rect(renderer, overlayRect, ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF);

      if (!render_text_line(renderer, bodyFont, viewModel.overlay.title, {ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF}, overlayRect.x + 16, overlayRect.y + 16, overlayRect.w - 32)) {
        return false;
      }

      int overlayY = overlayRect.y + 54;
      for (const std::string &line : viewModel.overlay.lines) {
        int drawnHeight = 0;
        if (!render_text_line(renderer, smallFont, line, {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, overlayRect.x + 16, overlayY, overlayRect.w - 32, &drawnHeight)) {
          return false;
        }
        overlayY += drawnHeight + 6;
      }
    }

    if (viewModel.modal.visible && viewModel.logViewer.visible) {
      if (!render_log_viewer_modal(renderer, bodyFont, smallFont, assetCache, viewModel, screenWidth, screenHeight, outerMargin, hints)) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        fill_rect(renderer, {0, 0, screenWidth, screenHeight}, 0x00, 0x00, 0x00, 0xA6);
        const SDL_Rect modalRect {
          screenWidth / 6,
          screenHeight / 6,
          (screenWidth * 2) / 3,
          (screenHeight * 2) / 3,
        };
        fill_rect(renderer, modalRect, PANEL_RED, PANEL_GREEN, PANEL_BLUE, 0xF2);
        draw_rect(renderer, modalRect, ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE);
        render_text_line_simple(renderer, bodyFont, "Log File", {ACCENT_RED, ACCENT_GREEN, ACCENT_BLUE, 0xFF}, modalRect.x + 16, modalRect.y + 16, modalRect.w - 32);
        render_text_line_simple(renderer, smallFont, "The full log viewer could not be rendered safely.", {TEXT_RED, TEXT_GREEN, TEXT_BLUE, 0xFF}, modalRect.x + 16, modalRect.y + 54, modalRect.w - 32);
        render_text_line_simple(renderer, smallFont, (viewModel.modal.footerActions.empty() || viewModel.modal.footerActions.back().keyLabel.empty() ? "Press B to close." : "Press Esc to close."), {MUTED_RED, MUTED_GREEN, MUTED_BLUE, 0xFF}, modalRect.x + 16, modalRect.y + 54 + TTF_GetFontLineSkip(smallFont) + 8, modalRect.w - 32);
      }
    } else if (viewModel.modal.visible && !render_shell_dialog(renderer, fonts, assetCache, viewModel.modal, screenWidth, screenHeight, folderBrowser ? folder : nullptr)) {
      return false;
    }
    if (viewModel.keypad.visible && !render_keypad_dialog(renderer, fonts, assetCache, viewModel, {0, 0, screenWidth, screenHeight}, keypadModalLayoutCache)) {
      return false;
    }

    draw_shell_save_indicator(renderer, saveIndicator, saveFrame, layout.saveIndicator);
    SDL_RenderPresent(renderer);
    if (folder != nullptr && folderBrowser && !saveFrame.has_value()) {
      folder->content = viewModel.modal;
      folder->logicalWidth = screenWidth;
      folder->framebufferWidth = framebufferWidth;
      folder->framebufferHeight = framebufferHeight;
      folder->titleFont = titleFont;
      folder->bodyFont = bodyFont;
      folder->smallFont = smallFont;
      folder->valid = true;
    }
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    return true;
  }

  /**
   * @brief Release an owned SDL gamepad reference and clear the handle.
   * @param controller Owned handle, or null if no controller is open.
   */
  void close_controller(SDL_Gamepad *&controller) {
    if (controller != nullptr) {
      SDL_CloseGamepad(controller);
      controller = nullptr;
    }
  }

  bool should_open_added_controller(const SDL_Gamepad *controller, const SDL_GamepadDeviceEvent &event) {
    return controller == nullptr && SDL_IsGamepad(event.which);
  }

  bool should_close_removed_controller(SDL_Gamepad *controller, const SDL_GamepadDeviceEvent &event) {
    return controller != nullptr && SDL_GetGamepadID(controller) == event.which;
  }

  bool hosts_screen_exit_combo_allowed(const app::ClientState &state) {
    return state.shell.activeScreen == app::ScreenId::home || state.shell.activeScreen == app::ScreenId::hosts;
  }

  void update_trigger_repeat_tick(input::UiCommand command, Uint32 now, Uint32 *leftTriggerRepeatTick, Uint32 *rightTriggerRepeatTick) {
    if (command == input::UiCommand::fast_previous_page) {
      if (leftTriggerRepeatTick != nullptr) {
        *leftTriggerRepeatTick = now;
      }
      return;
    }
    if (command == input::UiCommand::fast_next_page && rightTriggerRepeatTick != nullptr) {
      *rightTriggerRepeatTick = now;
    }
  }

  /**
   * @brief Retains controller and keyboard hold state between shell frames.
   */
  using input::ShellInputState;

  void reset_shell_input_state(ShellInputState *inputState) {
    if (inputState == nullptr) {
      return;
    }

    *inputState = {};
  }

  /**
   * @brief Switch contextual helpers and stop held navigation from the previous device.
   * @param state Shell input device metadata.
   * @param inputState Held input state to clear on a device switch.
   * @param device Most recently used input device.
   */
  void set_shell_input_device(app::ClientState &state, ShellInputState *inputState, input::UiInputDevice device) {
    if (state.shell.inputDevice != device) {
      reset_shell_input_state(inputState);
      state.shell.inputDevice = device;
    }
  }

  void disarm_controller_exit_combo(ShellInputState *inputState) {
    if (inputState == nullptr) {
      return;
    }

    inputState->controllerExitComboArmed = false;
    inputState->controllerExitComboTriggered = false;
  }

  void update_exit_combo_hold(app::ClientState &state, ShellInputState *inputState) {
    if (inputState == nullptr || inputState->controllerExitComboTriggered) {
      return;
    }

    if (!inputState->controllerStartPressed || !inputState->controllerBackPressed || !hosts_screen_exit_combo_allowed(state)) {
      return;
    }

    inputState->controllerExitComboArmed = true;
    if (const Uint32 comboStartTick = inputState->controllerStartDownTick > inputState->controllerBackDownTick ? inputState->controllerStartDownTick : inputState->controllerBackDownTick; static_cast<Uint32>(SDL_GetTicks()) - comboStartTick < EXIT_COMBO_HOLD_MILLISECONDS) {
      return;
    }

    inputState->controllerExitComboTriggered = true;
    state.shell.shouldExit = true;
    logging::info("app", "Exit requested from held Start+Back on the hosts screen");
  }

  template<typename ProcessCommand>
  void process_log_viewer_repeat_commands(
    const app::ClientState &state,
    Uint32 now,
    ShellInputState *inputState,
    const ProcessCommand &processCommand
  ) {
    if (inputState == nullptr || state.modal.id != app::ModalId::log_viewer || state.shell.inputDevice != input::UiInputDevice::gamepad) {
      return;
    }

    if (inputState->leftShoulderPressed && now - inputState->leftShoulderRepeatTick >= LOG_VIEWER_SCROLL_REPEAT_MILLISECONDS) {
      inputState->leftShoulderRepeatTick = now;
      processCommand(input::UiCommand::previous_page);
    }
    if (inputState->rightShoulderPressed && now - inputState->rightShoulderRepeatTick >= LOG_VIEWER_SCROLL_REPEAT_MILLISECONDS) {
      inputState->rightShoulderRepeatTick = now;
      processCommand(input::UiCommand::next_page);
    }
    if (inputState->leftTriggerPressed && now - inputState->leftTriggerRepeatTick >= LOG_VIEWER_FAST_SCROLL_REPEAT_MILLISECONDS) {
      inputState->leftTriggerRepeatTick = now;
      processCommand(input::UiCommand::fast_previous_page);
    }
    if (inputState->rightTriggerPressed && now - inputState->rightTriggerRepeatTick >= LOG_VIEWER_FAST_SCROLL_REPEAT_MILLISECONDS) {
      inputState->rightTriggerRepeatTick = now;
      processCommand(input::UiCommand::fast_next_page);
    }
  }

  /**
   * @brief Acquire an added gamepad without interrupting keyboard holds or replacing a working controller.
   * @param controller Owned menu controller.
   * @param event SDL device-added notification.
   * @param inputState Holds to reset after controller ownership changes.
   */
  void handle_controller_device_added(SDL_Gamepad **controller, const SDL_GamepadDeviceEvent &event, ShellInputState *inputState) {
    if (controller == nullptr || !should_open_added_controller(*controller, event)) {
      return;
    }

    if (input::recover_shell_controller(controller) && *controller != nullptr) {
      if (inputState != nullptr) {
        inputState->reset_after_controller_change();
      }
      logging::info("input", "Controller connected");
    }
  }

  /**
   * @brief Release the shell's controller and clear held inputs when its device is removed.
   * @param controller Slot holding the shell's controller reference.
   * @param event SDL gamepad removal event.
   * @param inputState Shell input state to reset, if available.
   */
  void handle_controller_device_removed(SDL_Gamepad **controller, const SDL_GamepadDeviceEvent &event, ShellInputState *inputState) {
    if (controller == nullptr || !should_close_removed_controller(*controller, event)) {
      return;
    }

    close_controller(*controller);
    if (inputState != nullptr) {
      inputState->reset_after_controller_change();
    }
    logging::warn("input", "Controller disconnected");
  }

  input::UiCommand handle_controller_button_down_event(const SDL_GamepadButtonEvent &event, const app::ClientState &state, ShellInputState *inputState) {
    if (inputState == nullptr) {
      return input::UiCommand::none;
    }

    const Uint32 controllerButtonDownTick = SDL_GetTicks();
    if (event.button == SDL_GAMEPAD_BUTTON_START) {
      if (!inputState->controllerStartPressed) {
        inputState->controllerStartPressed = true;
        inputState->controllerStartDownTick = controllerButtonDownTick;
      }
    } else if (event.button == SDL_GAMEPAD_BUTTON_BACK) {
      if (!inputState->controllerBackPressed) {
        inputState->controllerBackPressed = true;
        inputState->controllerBackDownTick = controllerButtonDownTick;
      }
    } else {
      if (event.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
        inputState->leftShoulderPressed = true;
        inputState->leftShoulderRepeatTick = controllerButtonDownTick;
      } else if (event.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
        inputState->rightShoulderPressed = true;
        inputState->rightShoulderRepeatTick = controllerButtonDownTick;
      }

      const input::UiCommand command = translate_controller_button(event.button);
      if (is_navigation_command(command)) {
        seed_controller_navigation_hold_state(
          controllerButtonDownTick,
          command,
          &inputState->controllerNavigation.up,
          &inputState->controllerNavigation.down,
          &inputState->controllerNavigation.left,
          &inputState->controllerNavigation.right
        );
      }
      if (inputState->controllerStartPressed && inputState->controllerBackPressed && hosts_screen_exit_combo_allowed(state)) {
        inputState->controllerExitComboArmed = true;
      }
      return command;
    }

    if (inputState->controllerStartPressed && inputState->controllerBackPressed && hosts_screen_exit_combo_allowed(state)) {
      inputState->controllerExitComboArmed = true;
    }
    return input::UiCommand::none;
  }

  input::UiCommand handle_controller_button_up_event(const SDL_GamepadButtonEvent &event, ShellInputState *inputState) {
    if (inputState == nullptr) {
      return input::UiCommand::none;
    }

    input::UiCommand command = input::UiCommand::none;
    if (event.button == SDL_GAMEPAD_BUTTON_START && inputState->controllerStartPressed) {
      inputState->controllerStartPressed = false;
      if (!inputState->controllerExitComboArmed && !inputState->controllerExitComboTriggered) {
        command = input::map_gamepad_button_to_ui_command(input::GamepadButton::start);
      }
    } else if (event.button == SDL_GAMEPAD_BUTTON_BACK && inputState->controllerBackPressed) {
      inputState->controllerBackPressed = false;
      if (!inputState->controllerExitComboArmed && !inputState->controllerExitComboTriggered) {
        command = input::map_gamepad_button_to_ui_command(input::GamepadButton::back);
      }
    } else if (event.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
      inputState->leftShoulderPressed = false;
    } else if (event.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
      inputState->rightShoulderPressed = false;
    }

    if (!inputState->controllerStartPressed && !inputState->controllerBackPressed) {
      disarm_controller_exit_combo(inputState);
    }
    release_controller_navigation_hold_state(
      translate_controller_button(event.button),
      &inputState->controllerNavigation.up,
      &inputState->controllerNavigation.down,
      &inputState->controllerNavigation.left,
      &inputState->controllerNavigation.right
    );
    return command;
  }

  /**
   * @brief Type printable physical keyboard input into a folder-name draft.
   * @param state Active folder workflow.
   * @param event Physical keyboard event.
   * @return True when the folder-name keyboard consumes this key.
   */
  bool handle_directory_key(app::ClientState &state, const SDL_KeyboardEvent &event) {
    if (state.modal.id != app::ModalId::directory_picker || !state.directoryPicker.creating || event.key < 32 || event.key >= 127 || (event.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0) {
      return false;
    }
    const int code = input::keyboard_virtual_key(event.scancode);
    if (code == 0x20) {
      app::append_directory_name(state, " ");
      return true;
    }
    for (std::size_t i = 0; i < 60; ++i) {
      const auto &key = input::StreamMenu::KEYS[i];
      if (key.code != code) {
        continue;
      }
      const bool shifted = ((event.mod & SDL_KMOD_SHIFT) != 0) != (code >= 'A' && code <= 'Z' && (event.mod & SDL_KMOD_CAPS) != 0);
      if (const std::string text = shifted ? key.shifted : key.label; text.size() == 1) {
        app::append_directory_name(state, text);
      }
      break;
    }
    return true;
  }

  /**
   * @brief Translate a shell key press after handling exit and text input.
   * @param state Shell state and current input device.
   * @param event SDL key press.
   * @param inputState Keyboard repeat state, if available.
   * @return A command to apply or none if the key was consumed.
   */
  input::UiCommand handle_shell_key_down(app::ClientState &state, const SDL_KeyboardEvent &event, ShellInputState *inputState) {
    if (event.repeat != 0 || inputState == nullptr) {
      return input::UiCommand::none;
    }
#ifdef MOONLIGHT_DESKTOP
    if (event.key == SDLK_F11) {
      ui::toggle_desktop_fullscreen(event.windowID);
      return input::UiCommand::none;
    }
#endif
    set_shell_input_device(state, inputState, input::UiInputDevice::keyboard);
    if (event.key == SDLK_F4 && (event.mod & SDL_KMOD_ALT) != 0 && hosts_screen_exit_combo_allowed(state) && !state.modal.active()) {
      state.shell.shouldExit = true;
      return input::UiCommand::none;
    }
    if (handle_directory_key(state, event)) {
      return input::UiCommand::none;
    }
    return inputState->keyboardNavigation.press(event.scancode, translate_keyboard_key(event.key, event.mod), SDL_GetTicks());
  }

#ifdef MOONLIGHT_PORTABLE
  /**
   * @brief Translate a mouse release while ignoring SDL's synthetic touch mouse events.
   * @param state Shell state and current input device.
   * @param event SDL mouse button release.
   * @param inputState Shell input state, if available.
   * @return The mapped command, or none for unbound buttons.
   */
  input::UiCommand handle_shell_mouse_release(app::ClientState &state, const SDL_MouseButtonEvent &event, ShellInputState *inputState) {
    if (event.which == SDL_TOUCH_MOUSEID) {
      return input::UiCommand::none;
    }
    set_shell_input_device(state, inputState, input::UiInputDevice::keyboard);
    switch (event.button) {
      case SDL_BUTTON_LEFT:
        return input::UiCommand::activate;
      case SDL_BUTTON_RIGHT:
        return input::UiCommand::open_context_menu;
      case SDL_BUTTON_X1:
        return input::UiCommand::back;
      default:
        return input::UiCommand::none;
    }
  }

  /**
   * @brief Interpret the active finger's release as a swipe, hold or tap.
   * @param gesture Saved origin and timing, cleared when the finger matches.
   * @param event SDL finger release.
   * @return A shell command, or none for a different finger.
   */
  input::UiCommand handle_shell_touch_release(input::ShellTouchGesture &gesture, const SDL_TouchFingerEvent &event) {
    if (gesture.finger != event.fingerID) {
      return input::UiCommand::none;
    }
    const float dx = event.x - gesture.x;
    const float dy = event.y - gesture.y;
    const Uint64 held = SDL_GetTicks() - gesture.startedAt;
    gesture.finger = 0;
    if (std::max(std::fabs(dx), std::fabs(dy)) >= 0.05f) {
      if (std::fabs(dx) > std::fabs(dy)) {
        return input::direction_command(dx > 0.0f ? 1 : -1, 0);
      }
      return input::direction_command(0, dy > 0.0f ? 1 : -1);
    }
    if (held > 1000) {
      return input::UiCommand::back;
    }
    return held > 450 ? input::UiCommand::open_context_menu : input::UiCommand::activate;
  }
#endif

  /**
   * @brief Route device transitions and maintain the input source used by contextual helpers.
   * @param state Reducer state and current input device.
   * @param event SDL event to process.
   * @param controller Owned primary controller reference.
   * @param inputState Keyboard and controller hold timers.
   * @return Initial command from the event, or none for lifecycle and release events.
   */
  input::UiCommand handle_shell_event(
    app::ClientState &state,
    const SDL_Event &event,
    SDL_Gamepad **controller,
    ShellInputState *inputState
  ) {
    switch (event.type) {
      case SDL_EVENT_QUIT:
        state.shell.shouldExit = true;
        return input::UiCommand::none;
      case SDL_EVENT_GAMEPAD_ADDED:
        handle_controller_device_added(controller, event.gdevice, inputState);
        return input::UiCommand::none;
      case SDL_EVENT_GAMEPAD_REMOVED:
        handle_controller_device_removed(controller, event.gdevice, inputState);
        return input::UiCommand::none;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        set_shell_input_device(state, inputState, input::UiInputDevice::gamepad);
        return handle_controller_button_down_event(event.gbutton, state, inputState);
      case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return handle_controller_button_up_event(event.gbutton, inputState);
      case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (inputState == nullptr) {
          return input::UiCommand::none;
        }

        if (input::is_gamepad_ui_activity(event.gaxis.value, event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
          set_shell_input_device(state, inputState, input::UiInputDevice::gamepad);
        }
        return translate_trigger_axis(event.gaxis, &inputState->leftTriggerPressed, &inputState->rightTriggerPressed);
#ifdef MOONLIGHT_PORTABLE
      case SDL_EVENT_MOUSE_BUTTON_UP:
        return handle_shell_mouse_release(state, event.button, inputState);
      case SDL_EVENT_MOUSE_WHEEL:
        return input::direction_command(0, event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? event.wheel.integer_y : -event.wheel.integer_y);
      case SDL_EVENT_FINGER_DOWN:
        if (inputState != nullptr && inputState->touch.finger == 0) {
          set_shell_input_device(state, inputState, input::UiInputDevice::keyboard);
          inputState->touch = {event.tfinger.fingerID, SDL_GetTicks(), event.tfinger.x, event.tfinger.y};
        }
        return input::UiCommand::none;
      case SDL_EVENT_FINGER_UP:
        return inputState != nullptr ? handle_shell_touch_release(inputState->touch, event.tfinger) : input::UiCommand::none;
#endif
      case SDL_EVENT_KEY_DOWN:
        return handle_shell_key_down(state, event.key, inputState);
      case SDL_EVENT_KEY_UP:
        if (inputState != nullptr) {
          inputState->keyboardNavigation.release(event.key.scancode);
        }
        return input::UiCommand::none;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        {
          reset_shell_input_state(inputState);
        }
        return input::UiCommand::none;
      default:
        return input::UiCommand::none;
    }
  }

  template<typename ProcessCommand>
  void process_polled_shell_events(
    app::ClientState &state,
    SDL_Gamepad **controller,
    ShellInputState *inputState,
    const ProcessCommand &processCommand,
    bool *skipPolledControllerNavigation,
    bool *redrawRequested = nullptr
  ) {
    SDL_Event event;
    if (skipPolledControllerNavigation != nullptr) {
      *skipPolledControllerNavigation = false;
    }
    if (SDL_WaitEventTimeout(&event, SHELL_EVENT_WAIT_TIMEOUT_MILLISECONDS) == 0) {
      return;
    }

    do {
      if (redrawRequested != nullptr && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT || (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED)) {
        *redrawRequested = true;
      }
      const input::UiCommand command = handle_shell_event(state, event, controller, inputState);
      if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && inputState != nullptr) {
        update_trigger_repeat_tick(command, SDL_GetTicks(), &inputState->leftTriggerRepeatTick, &inputState->rightTriggerRepeatTick);
      }

      processCommand(command);
      if (command == input::UiCommand::none || is_navigation_command(command) || inputState == nullptr) {
        continue;
      }

      inputState->controllerNavigationNeutralRequired = true;
      if (skipPolledControllerNavigation != nullptr) {
        *skipPolledControllerNavigation = true;
      }
      reset_controller_navigation_hold_states(
        &inputState->controllerNavigation.up,
        &inputState->controllerNavigation.down,
        &inputState->controllerNavigation.left,
        &inputState->controllerNavigation.right
      );
    } while (SDL_PollEvent(&event));
  }

  template<typename ProcessCommand>
  void process_controller_navigation(
    SDL_Gamepad *controller,
    ShellInputState *inputState,
    bool skipPolledControllerNavigation,
    const ProcessCommand &processCommand
  ) {
    if (inputState == nullptr) {
      return;
    }

    if (inputState->controllerNavigationNeutralRequired) {
      if (is_controller_navigation_active(controller)) {
        reset_controller_navigation_hold_states(
          &inputState->controllerNavigation.up,
          &inputState->controllerNavigation.down,
          &inputState->controllerNavigation.left,
          &inputState->controllerNavigation.right
        );
      } else {
        inputState->controllerNavigationNeutralRequired = false;
      }
    }

    if (skipPolledControllerNavigation || inputState->controllerNavigationNeutralRequired) {
      return;
    }

    processCommand(poll_controller_navigation(
      controller,
      SDL_GetTicks(),
      &inputState->controllerNavigation.up,
      &inputState->controllerNavigation.down,
      &inputState->controllerNavigation.left,
      &inputState->controllerNavigation.right
    ));
  }

  /**
   * @brief Perform the authenticated quit request on a worker using only its copied target.
   * @param context AppQuitTaskState retained until this callback completes.
   * @return Zero after publishing the result to the shell.
   */
  int run_app_quit_task(void *context) {  // NOSONAR(cpp:S5008) SDL thread callback ABI
    auto &task = *static_cast<AppQuitTaskState *>(context);
    if (network::PairingIdentity identity {}; load_saved_pairing_identity_for_streaming(&identity, &task.message)) {
      task.success = network::quit_host_app(task.target.address, task.target.port, identity, task.target.appId, &task.message);
    }
    task.completed.store(true);
    return 0;
  }

  /**
   * @brief Start a confirmed quit operation without blocking navigation or rendering on HTTP.
   * @param state Retained target and progress dialog.
   * @param task Worker slot owned by the shell runtime.
   */
  void start_app_quit_task(app::ClientState &state, AppQuitTaskState &task) {
    if (task.thread != nullptr || !state.appQuit.inProgress) {
      return;
    }
    task.target = state.appQuit;
    task.success = false;
    task.message.clear();
    task.completed.store(false);
    task.thread = SDL_CreateThread(run_app_quit_task, "quit-host-app", &task);
    if (task.thread == nullptr) {
      app::apply_app_quit_result(state, false, std::string("Could not start quit request: ") + SDL_GetError());
    }
  }

  /**
   * @brief Apply and persist a completed host quit without waiting on unfinished workers.
   * @param state Local host library and progress dialog.
   * @param task Runtime-owned quit worker.
   * @param feedback Repaints the shell if the result must be saved.
   */
  void finish_app_quit_task(app::ClientState &state, AppQuitTaskState &task, const ShellSaveFeedback &feedback) {
    if (task.thread == nullptr || !task.completed.load()) {
      return;
    }
    SDL_WaitThread(task.thread, nullptr);
    task.thread = nullptr;
    app::apply_app_quit_result(state, task.success, task.message);
    app::AppUpdate update;
    update.persistence.hostsChanged = task.success;
    persist_hosts_if_needed(state, update, feedback);
  }

  /**
   * @brief Stores renderer resources and retained menu and screenshot caches.
   */
  struct ShellResources {
    SDL_Renderer *renderer = nullptr;  ///< Renderer used for all shell drawing.
    TTF_Font *titleFont = nullptr;  ///< Large font used for screen titles.
    TTF_Font *bodyFont = nullptr;  ///< Standard body font.
    TTF_Font *smallFont = nullptr;  ///< Small font for secondary labels.
    int hintGlyphSize = 0;  ///< Stream controller glyph height measured for the current output.
    SDL_Gamepad *controller = nullptr;  ///< Primary open game controller, when available.
    CoverArtTextureCache coverArtTextureCache;  ///< Cached cover-art textures.
    AssetTextureCache assetTextureCache;  ///< Cached UI asset textures.
    ui::SaveIndicator saveIndicator;  ///< Small reusable indicator raster independent of the current renderer.
    KeypadModalLayoutCache keypadModalLayoutCache;  ///< Cached keypad modal layout artifacts.
    unsigned long encoderSettings = 0;  ///< Active video encoder settings for layout calculations.
    bool ttfInitialized = false;  ///< Whether SDL_ttf was initialized in this shell session.
  };

  /**
   * @brief Stores mutable shell loop state that changes across frames.
   */
  struct ShellRuntimeState {
    bool running = true;  ///< False when the shell should stop processing frames.
    bool keypadRedrawRequested = true;  ///< True when visible shell state changed since its last retained frame.
    ShellInputState inputState {};  ///< Current controller and trigger input state.
    Uint32 nextHostDiscoveryTick = 0;  ///< Next scheduled host auto-discovery time.
    Uint32 nextHostProbeTick = 0;  ///< Next scheduled host probe time.
    Uint32 notificationTick = 0;  ///< Last clock sample used to advance visible notification lifetimes.
    std::optional<Uint64> lastSaveFrame;  ///< Indicator actually drawn by the last shell frame.
    ShellSaveFeedback saveFeedback;  ///< Repaints the shell while a file save is pending.
    PairingTaskState pairingTask {};  ///< Background pairing workflow state.
    ConnectionTestTaskState connectionTestTask {};  ///< Background manual connection test.
    AppListTaskState appListTask {};  ///< Background app-list fetch state.
    AppQuitTaskState appQuitTask {};  ///< Background termination of a confirmed running host app.
    AppArtTaskState appArtTask {};  ///< Background box-art download state.
    HostDiscoveryTaskState hostDiscoveryTask {};  ///< Background host auto-discovery state.
    HostProbeTaskState hostProbeTask {};  ///< Background host probe state.
  };

  /**
   * @brief Describes an initialization failure while preparing the interactive shell.
   */
  struct ShellInitializationFailure {
    const char *category = "sdl";  ///< Logging category associated with the failure.
    std::string message;  ///< Human-readable failure detail.
  };

  /**
   * @brief Wait for an SDL thread and discard its integer return code.
   *
   * @param thread Thread handle to join.
   */
  void wait_for_thread(SDL_Thread *thread) {
    if (thread == nullptr) {
      return;
    }

    int threadResult = 0;
    SDL_WaitThread(thread, &threadResult);
    (void) threadResult;
  }

  void join_shell_session_tasks(ShellRuntimeState &runtime);
  void finalize_shell_tasks(ShellRuntimeState &runtime);

  /**
   * @brief Open the first detected SDL game controller for shell navigation.
   *
   * @return The opened controller, or nullptr when none could be opened.
   */
  SDL_Gamepad *open_primary_controller() {
    SDL_Gamepad *controller = nullptr;
    if (input::recover_shell_controller(&controller)) {
      logging::info("input", "Opened primary controller");
    }
    return controller;
  }

  /**
   * @brief Release the SDL resources and render caches owned by the shell loop.
   *
   * @param resources Shell resources to release.
   */
  void close_shell_resources(ShellResources *resources) {
    if (resources == nullptr) {
      return;
    }

    close_controller(resources->controller);
    clear_cover_art_texture_cache(&resources->coverArtTextureCache);
    clear_asset_texture_cache(&resources->assetTextureCache);
    clear_keypad_modal_layout_cache(&resources->keypadModalLayoutCache);

    if (resources->smallFont != nullptr) {
      TTF_CloseFont(resources->smallFont);
      resources->smallFont = nullptr;
    }
    if (resources->bodyFont != nullptr) {
      TTF_CloseFont(resources->bodyFont);
      resources->bodyFont = nullptr;
    }
    if (resources->titleFont != nullptr) {
      TTF_CloseFont(resources->titleFont);
      resources->titleFont = nullptr;
    }
    if (resources->renderer != nullptr) {
      SDL_DestroyRenderer(resources->renderer);
      resources->renderer = nullptr;
    }
    if (resources->ttfInitialized) {
      TTF_Quit();
      resources->ttfInitialized = false;
    }
  }

  /**
   * @brief Initialize the SDL renderer, fonts, caches, and controller used by the shell.
   *
   * Preserve an existing connected controller reference across render-resource initialization.
   *
   * @param window SDL window hosting the interactive shell.
   * @param videoMode Active output mode used to size fonts.
   * @param resources Shell resources to populate.
   * @param failure Receives a failure category and message when initialization fails.
   * @return True when all required shell resources were prepared.
   */
  bool initialize_shell_resources(SDL_Window *window, const VIDEO_MODE &videoMode, ShellResources *resources, ShellInitializationFailure *failure) {
    if (window == nullptr || resources == nullptr || failure == nullptr) {
      return false;
    }

    if (!TTF_Init()) {
      failure->category = "ttf";
      failure->message = std::string("TTF_Init failed: ") + SDL_GetError();
      close_shell_resources(resources);
      return false;
    }
    resources->ttfInitialized = true;

#ifdef NXDK
    // The Xbox renderer excludes 720-pixel SD modes and needs several scanout buffers at 1080i.
    if (videoMode.width != 720 && videoMode.height < 1080) {
      resources->renderer = SDL_CreateRenderer(window, "nxdk_xgu");
      if (resources->renderer == nullptr) {
        logging::warn("sdl", std::string("Xbox renderer unavailable; using software: ") + SDL_GetError());
      }
    }
#else
    resources->renderer = SDL_CreateRenderer(window, nullptr);
#endif
    if (resources->renderer == nullptr) {
      resources->renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
#ifndef MOONLIGHT_DESKTOP
      resources->keypadModalLayoutCache.window = window;
#endif
    } else {
      resources->keypadModalLayoutCache.window = nullptr;
    }
    if (resources->renderer == nullptr) {
      failure->category = "sdl";
      failure->message = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
      close_shell_resources(resources);
      return false;
    }

    SDL_SetRenderDrawBlendMode(resources->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(resources->renderer, BACKGROUND_RED, BACKGROUND_GREEN, BACKGROUND_BLUE, 0xFF);
    SDL_RenderClear(resources->renderer);
    SDL_RenderPresent(resources->renderer);

    const std::string fontPath = build_asset_path("fonts\\vegur-regular.ttf");
    resources->titleFont = TTF_OpenFont(fontPath.c_str(), std::max(26, videoMode.height / 22));
    resources->bodyFont = TTF_OpenFont(fontPath.c_str(), std::max(18, videoMode.height / 32));
    resources->smallFont = TTF_OpenFont(fontPath.c_str(), std::max(14, videoMode.height / 42));
    if (resources->titleFont == nullptr || resources->bodyFont == nullptr || resources->smallFont == nullptr) {
      failure->category = "ttf";
      failure->message = std::string("Failed to load shell font from ") + fontPath + ": " + SDL_GetError();
      close_shell_resources(resources);
      return false;
    }

    const int controlsScale = streaming::controls_raster_scale(videoMode.height);
    const int rasterHeight = videoMode.height / controlsScale;
    const std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)> controlsFont(TTF_OpenFont(fontPath.c_str(), ui::control_hint_font_size(rasterHeight)), &TTF_CloseFont);
    const int lineHeight = TTF_GetFontLineSkip(controlsFont ? controlsFont.get() : resources->smallFont);
    resources->hintGlyphSize = ui::control_hint_glyph_size(lineHeight, rasterHeight) * controlsScale;

    if (resources->controller == nullptr || !SDL_GamepadConnected(resources->controller)) {
      close_controller(resources->controller);
      resources->controller = open_primary_controller();
    }
#ifdef NXDK
    resources->encoderSettings = XVideoGetEncoderSettings();
#else
    resources->encoderSettings = VIDEO_WIDESCREEN;
#endif
    return true;
  }

  /**
   * @brief Reset shell input, app tasks and the notification clock without resetting retained host workers.
   *
   * @param state Client state whose logging configuration should be applied.
   * @param runtime Runtime state to prepare.
   */
  void initialize_shell_runtime(const app::ClientState &state, ShellRuntimeState *runtime) {
    if (runtime == nullptr) {
      return;
    }

    runtime->inputState.reset_after_stream();
    reset_pairing_task(&runtime->pairingTask);
    reset_app_list_task(&runtime->appListTask);
    reset_app_art_task(&runtime->appArtTask);
    runtime->notificationTick = SDL_GetTicks();
    logging::set_minimum_level(logging::LogLevel::trace);
    logging::set_file_minimum_level(state.settings.loggingLevel);
    logging::set_debugger_console_minimum_level(state.settings.xemuConsoleLoggingLevel);
    logging::info("app", "Entered interactive shell");
  }

  /**
   * @brief Render the current shell frame and update redraw bookkeeping.
   *
   * @param videoMode Active output mode used by the shell renderer.
   * @param state Client state to visualize.
   * @param resources Shell resources supplying the renderer and caches.
   * @param runtime Runtime state receiving redraw and shutdown updates.
   * @return True when the frame rendered successfully.
   */
  bool draw_current_shell_frame(const VIDEO_MODE &videoMode, app::ClientState &state, ShellResources *resources, ShellRuntimeState *runtime) {
    if (resources == nullptr || runtime == nullptr) {
      return false;
    }

#ifdef MOONLIGHT_DESKTOP
    if ((SDL_GetWindowFlags(SDL_GetRenderWindow(resources->renderer)) & SDL_WINDOW_MINIMIZED) != 0) {
      return true;
    }
#endif
    const auto size = measure_shell_render_size(resources->renderer, videoMode, resources->encoderSettings);
    update_shell_grid_columns(state, size.width);
    const std::vector<logging::LogEntry> retainedEntries = logging::snapshot(logging::LogLevel::info);
    const auto viewModel = ui::build_shell_view_model(state, retainedEntries);
    const auto saveFrame = ui::save_indicator_frame(SDL_GetTicks());
    const DialogFonts fonts {resources->titleFont, resources->bodyFont, resources->smallFont, {resources->hintGlyphSize}};
    if (draw_shell(resources->renderer, size, fonts, viewModel, &resources->coverArtTextureCache, &resources->assetTextureCache, &resources->keypadModalLayoutCache, resources->saveIndicator, saveFrame)) {
      runtime->lastSaveFrame = saveFrame;
      runtime->keypadRedrawRequested = false;
      return true;
    }

    report_shell_failure("render", std::string("Shell render failed: ") + SDL_GetError());
    runtime->running = false;
    state.shell.shouldExit = true;
    return false;
  }

  /**
   * @brief Complete any background shell tasks whose results are ready.
   *
   * @param state Client state to update.
   * @param resources Shell resources that own render caches.
   * @param runtime Runtime state that owns the background tasks.
   */
  void finish_shell_background_tasks(app::ClientState &state, ShellResources *resources, ShellRuntimeState *runtime) {
    if (resources == nullptr || runtime == nullptr) {
      return;
    }

    const auto ready = [](const auto &task) {
      return task.thread != nullptr && task.completed.load();
    };
    bool changed = ready(runtime->connectionTestTask) || ready(runtime->appListTask) || ready(runtime->appArtTask) || ready(runtime->hostDiscoveryTask) || ready(runtime->appQuitTask);
    if (runtime->pairingTask.activeAttempt != nullptr) {
      changed = changed || ready(*runtime->pairingTask.activeAttempt);
    }
    for (const auto &worker : runtime->hostProbeTask.workers) {
      changed = changed || (worker != nullptr && ready(*worker));
    }
    runtime->keypadRedrawRequested = runtime->keypadRedrawRequested || changed;

    finish_pairing_task_if_ready(state, &runtime->pairingTask, runtime->saveFeedback);
    finish_connection_test_if_ready(state, &runtime->connectionTestTask, runtime->saveFeedback);
    finish_app_list_task_if_ready(state, &runtime->appListTask, runtime->saveFeedback);
    finish_app_quit_task(state, runtime->appQuitTask, runtime->saveFeedback);
    collect_app_art_task(state, &runtime->appArtTask, &resources->coverArtTextureCache);
    finish_host_discovery_task_if_ready(state, &runtime->hostDiscoveryTask, &runtime->nextHostProbeTick, runtime->saveFeedback);
    finish_host_probe_task_if_ready(state, &runtime->hostProbeTask, runtime->saveFeedback);
  }

  /**
   * @brief Start any background shell tasks that are due to run on this frame.
   *
   * @param state Client state used to decide which tasks are eligible.
   * @param runtime Runtime state that owns the background tasks.
   * @param now Current SDL tick count.
   */
  void start_shell_background_tasks_if_needed(app::ClientState &state, ShellRuntimeState *runtime, Uint32 now) {
    if (runtime == nullptr) {
      return;
    }

    start_host_discovery_task_if_needed(state, &runtime->hostDiscoveryTask, now, &runtime->nextHostDiscoveryTick);
    start_host_probe_task_if_needed(state, &runtime->hostProbeTask, now, &runtime->nextHostProbeTick);
    start_app_list_task_if_needed(state, &runtime->appListTask, now);
    start_app_art_task_if_needed(state, &runtime->appArtTask);
  }

  /**
   * @brief Launch the selected stream and show its report after restoring the app picker.
   *
   * @param window Shared SDL window reused when entering a streaming session.
   * @param activeVideoMode Active output mode used by the shell renderer.
   * @param state Client state to mutate.
   * @param update App command result containing requested side effects.
   * @param resources Shell resources supplying render caches.
   * @param runtime Runtime state that owns background tasks and redraw flags.
   * @return True when command processing can continue.
   */
  bool launch_stream_if_requested(
    SDL_Window *window,
    const VIDEO_MODE &activeVideoMode,
    app::ClientState &state,
    const app::AppUpdate &update,
    ShellResources *resources,
    ShellRuntimeState *runtime
  ) {
    if (!update.requests.streamLaunchRequested) {
      return true;
    }

    const app::HostRecord *streamHost = app::apps_host(state);
    const app::HostAppRecord *streamApp = app::selected_app(state);
    if (streamHost == nullptr || streamApp == nullptr) {
      state.shell.statusMessage = "Select a valid app before starting a stream.";
      app::show_notification(state, app::NotificationKind::error, "No app selected", "Select an app from the library before starting a stream.");
      logging::warn("stream", state.shell.statusMessage);
      return true;
    }

    if (window == nullptr) {
      state.shell.statusMessage = "Streaming requires a valid SDL window.";
      app::show_notification(state, app::NotificationKind::error, "Couldn't start stream", "Restart Moonlight and try again.");
      logging::error("stream", state.shell.statusMessage);
      return true;
    }

    const Uint64 preparationStarted = SDL_GetTicks();
    runtime->keypadRedrawRequested = true;

    network::PairingIdentity clientIdentity {};
    if (std::string identityError; !load_saved_pairing_identity_for_streaming(&clientIdentity, &identityError)) {
      state.shell.statusMessage = identityError;
      app::show_notification(state, app::NotificationKind::error, "Pairing required", "Pair this PC again before starting a stream.");
      logging::warn("stream", identityError);
      return true;
    }

    // A library can have many missing covers, each with several candidate URLs.
    // Stop the batch and keep any artwork it already saved before changing renderers.
    runtime->appArtTask.cancelRequested.store(true);
    collect_app_art_task(state, &runtime->appArtTask, &resources->coverArtTextureCache);
    // Host refresh workers only use their own snapshots. Keep their state alive
    // across the session and let them finish without holding up stream startup.
    join_shell_session_tasks(*runtime);
    // Keep the menu's working controller open while render resources change. The Xbox
    // SDL backend owns a pending USB read that must not be torn down at this boundary.
    std::unique_ptr<SDL_Gamepad, decltype(&SDL_CloseGamepad)> retainedController(std::exchange(resources->controller, nullptr), &SDL_CloseGamepad);
    streaming::StreamSessionReport report;
    logging::info("stream", "Stream startup: menu preparation took " + std::to_string(SDL_GetTicks() - preparationStarted) + " ms");
    const bool streamStarted = streaming::run_stream_session(
      window,
      activeVideoMode,
      state.settings,
      {*streamHost, *streamApp, clientIdentity},
      retainedController.get(),
      [resources] {
        close_shell_resources(resources);
      },
      report
    );
    state.shell.statusMessage = std::move(report.message);
    if (!streamStarted) {
      app::show_notification(state, app::NotificationKind::error, "Streaming failed", state.shell.statusMessage.empty() ? "Try starting the stream again." : state.shell.statusMessage);
    }
    if (report.controller != nullptr) {
      retainedController.reset(report.controller);
    }
    resources->controller = retainedController.release();

    if (ShellInitializationFailure initializationFailure {}; !initialize_shell_resources(window, activeVideoMode, resources, &initializationFailure)) {
      report_shell_failure(initializationFailure.category, initializationFailure.message);
      runtime->running = false;
      state.shell.shouldExit = true;
      return false;
    }

    initialize_shell_runtime(state, runtime);
    finish_host_discovery_task_if_ready(state, &runtime->hostDiscoveryTask, &runtime->nextHostProbeTick, runtime->saveFeedback);
    finish_host_probe_task_if_ready(state, &runtime->hostProbeTask, runtime->saveFeedback);
    app::AppUpdate settingsUpdate {};
    settingsUpdate.persistence.settingsChanged = state.settings.dirty;
    persist_settings_if_needed(state, settingsUpdate, runtime->saveFeedback);
    if (streamStarted && state.hosts.activeLoaded) {
      state.hosts.active.appListState = app::HostAppListState::loading;
      state.hosts.active.appListStatusMessage = "Refreshing host apps after the stream session...";
      state.hosts.active.lastAppListRefreshTick = 0U;
    }
    if (streamStarted) {
      // Require a new press once the summary opens, even if input queued during teardown.
      SDL_PumpEvents();
      SDL_FlushEvent(SDL_EVENT_KEY_DOWN);
      SDL_FlushEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN);
      app::show_stream_summary(state, std::move(report.summaryLines));
      return draw_current_shell_frame(activeVideoMode, state, resources, runtime);
    }

    return true;
  }

  /**
   * @brief Apply a single translated UI command inside the shell loop.
   *
   * @param window Shared SDL window reused when entering a streaming session.
   * @param videoMode Active output mode used by the shell renderer.
   * @param state Client state to mutate.
   * @param command UI command to process.
   * @param resources Shell resources supplying render caches.
   * @param runtime Runtime state that owns background tasks and redraw flags.
   */
  void process_shell_command(
    SDL_Window *window,
    const VIDEO_MODE *videoMode,
    app::ClientState &state,
    input::UiCommand command,
    ShellResources *resources,
    ShellRuntimeState *runtime
  ) {
    if (command == input::UiCommand::none || resources == nullptr || runtime == nullptr) {
      return;
    }

    runtime->keypadRedrawRequested = true;

    const app::ScreenId previousScreen = state.shell.activeScreen;
    app::AppUpdate update = app::handle_command(state, command);
    process_directory_picker_requests(state, update);
    if (update.requests.quitAppRequested) {
      start_app_quit_task(state, runtime->appQuitTask);
    }
    if (update.requests.screenshotDeleteRequested && !state.screenshotViewer.entries.empty()) {
      auto &viewer = state.screenshotViewer;
      const auto &entry = viewer.entries[std::min(viewer.selectedIndex, viewer.entries.size() - 1)];
      std::string error;
      const bool deleted = runtime->saveFeedback.run([&entry, &error] {
        return app::delete_screenshot(entry, app::screenshot_library_path(), error);
      });
      if (deleted) {
        destroy_texture(resources->keypadModalLayoutCache.screenshot.texture);
        resources->keypadModalLayoutCache.screenshot = {};
      }
      app::apply_screenshot_delete_result(state, deleted, std::move(error));
    }
    if (update.requests.screenshotsRequested) {
      auto result = app::load_screenshots(state.screenshotViewer.source, app::screenshot_library_path());
      state.screenshotViewer.entries = std::move(result.entries);
      state.screenshotViewer.error = std::move(result.error);
      // The image viewer draws no app tiles, so their textures can be released
      // before allocating a full stream-sized capture on a 64 MB Xbox.
      clear_cover_art_texture_cache(&resources->coverArtTextureCache);
    }
    logging::set_file_minimum_level(state.settings.loggingLevel);
    logging::set_debugger_console_minimum_level(state.settings.xemuConsoleLoggingLevel);
    log_app_update(state, update);
    show_log_file_if_requested(state, update);
    cancel_pairing_if_requested(state, update, &runtime->pairingTask);
    test_host_connection_if_requested(state, update, &runtime->connectionTestTask);
    browse_host_apps_if_requested(state, update, runtime->saveFeedback);
    pair_host_if_requested(state, update, &runtime->pairingTask, runtime->saveFeedback);
    delete_host_data_if_requested(state, update, &resources->coverArtTextureCache);
    delete_saved_file_if_requested(state, update, &resources->coverArtTextureCache);
    factory_reset_if_requested(state, update, &resources->coverArtTextureCache);
    refresh_saved_files_if_needed(state);
    persist_settings_if_needed(state, update, runtime->saveFeedback);
    persist_hosts_if_needed(state, update, runtime->saveFeedback);

    const VIDEO_MODE activeVideoMode = videoMode != nullptr ? *videoMode : VIDEO_MODE {};
    if (!launch_stream_if_requested(window, activeVideoMode, state, update, resources, runtime)) {
      return;
    }

    if (previousScreen != state.shell.activeScreen) {
      release_page_resources_for_screen(previousScreen, state.shell.activeScreen, &resources->coverArtTextureCache, &resources->keypadModalLayoutCache);
      ensure_hosts_loaded_for_active_screen(state);
    }
    if ((previousScreen != state.shell.activeScreen || update.navigation.screenChanged) && !draw_current_shell_frame(activeVideoMode, state, resources, runtime)) {
      return;
    }
    if (!(state.shell.activeScreen == app::ScreenId::add_host && state.addHostDraft.keypad.visible) && state.modal.id == app::ModalId::none) {
      clear_keypad_modal_layout_cache(&resources->keypadModalLayoutCache);
    }
  }

  /**
   * @brief Process shell input and background tasks, retaining every static screen until visible state changes.
   *
   * @param window Shared SDL window reused for shell and streaming rendering.
   * @param videoMode Active output mode used by the shell renderer.
   * @param state Client state to update.
   * @param resources Shell resources used by the frame.
   * @param runtime Runtime state that carries input and task progress.
   * @return True when the shell should continue processing future frames.
   */
  bool run_shell_frame(SDL_Window *window, const VIDEO_MODE *videoMode, app::ClientState &state, ShellResources *resources, ShellRuntimeState *runtime) {
    if (window == nullptr || resources == nullptr || runtime == nullptr) {
      return false;
    }

    const Uint32 notificationNow = SDL_GetTicks();
    const auto notificationCount = state.shell.notifications.size();
    app::advance_notifications(state, notificationNow - runtime->notificationTick);
    runtime->keypadRedrawRequested = runtime->keypadRedrawRequested || state.shell.notifications.size() != notificationCount;
    runtime->notificationTick = notificationNow;

    const auto processCommand = [window, &state, videoMode, resources, runtime](input::UiCommand command) {
      if constexpr (DESKTOP_SHELL) {
        if (is_navigation_command(command)) {
          const auto size = measure_shell_render_size(resources->renderer, videoMode != nullptr ? *videoMode : VIDEO_MODE {}, resources->encoderSettings);
          update_shell_grid_columns(state, size.width);
        }
      }
      process_shell_command(window, videoMode, state, command, resources, runtime);
    };

    ensure_hosts_loaded_for_active_screen(state);
    finish_shell_background_tasks(state, resources, runtime);
    refresh_saved_files_if_needed(state);
    start_shell_background_tasks_if_needed(state, runtime, SDL_GetTicks());

    update_exit_combo_hold(state, &runtime->inputState);
    const auto previousInputDevice = state.shell.inputDevice;
    bool skipPolledControllerNavigation = false;
    process_polled_shell_events(state, &resources->controller, &runtime->inputState, processCommand, &skipPolledControllerNavigation, &runtime->keypadRedrawRequested);
    // Sunshine can remove a virtual pad after a quit completes. Other Xbox
    // ports may already be attached, so no new ADDED event will arrive for them.
    // Recover during input polling, even when an idle frame is not being drawn.
    if (input::recover_shell_controller(&resources->controller)) {
      runtime->inputState.reset_after_controller_change();
      runtime->keypadRedrawRequested = true;
      if (resources->controller != nullptr) {
        logging::info("input", "Recovered menu controller instance " + std::to_string(SDL_GetGamepadID(resources->controller)));
      } else {
        logging::warn("input", "Menu controller unavailable; waiting for an attached gamepad");
      }
    }
    if (state.shell.inputDevice == input::UiInputDevice::keyboard) {
      processCommand(runtime->inputState.keyboardNavigation.poll(SDL_GetTicks()));
    } else {
      process_log_viewer_repeat_commands(state, SDL_GetTicks(), &runtime->inputState, processCommand);
      process_controller_navigation(resources->controller, &runtime->inputState, skipPolledControllerNavigation, processCommand);
    }
    if (previousInputDevice != state.shell.inputDevice) {
      runtime->keypadRedrawRequested = true;
    }

    finish_shell_background_tasks(state, resources, runtime);
    start_shell_background_tasks_if_needed(state, runtime, SDL_GetTicks());

    const bool saveFrameChanged = runtime->lastSaveFrame != ui::save_indicator_frame(SDL_GetTicks());
    if ((runtime->keypadRedrawRequested || saveFrameChanged || ui::shell_requires_animation(state)) && !draw_current_shell_frame(videoMode != nullptr ? *videoMode : VIDEO_MODE {}, state, resources, runtime)) {
      return false;
    }

    return runtime->running && !state.shell.shouldExit;
  }

  /**
   * @brief Join interactive shell requests before starting a stream or exiting the shell.
   * @param runtime Runtime state that owns the background tasks.
   */
  void join_shell_session_tasks(ShellRuntimeState &runtime) {
    if (std::unique_ptr<PairingAttemptState> activeAttempt = std::move(runtime.pairingTask.activeAttempt); activeAttempt != nullptr) {
      activeAttempt->discardResult.store(true);
      finalize_pairing_attempt(nullptr, std::move(activeAttempt));
    }
    while (!runtime.pairingTask.retiredAttempts.empty()) {
      std::unique_ptr<PairingAttemptState> attempt = std::move(runtime.pairingTask.retiredAttempts.back());
      runtime.pairingTask.retiredAttempts.pop_back();
      if (attempt != nullptr) {
        attempt->discardResult.store(true);
      }
      finalize_pairing_attempt(nullptr, std::move(attempt));
    }

    wait_for_thread(std::exchange(runtime.appListTask.thread, nullptr));
    wait_for_thread(std::exchange(runtime.appQuitTask.thread, nullptr));
    wait_for_thread(std::exchange(runtime.connectionTestTask.thread, nullptr));
    wait_for_thread(std::exchange(runtime.appArtTask.thread, nullptr));
  }

  /**
   * @brief Join remaining background workers before their shared shell state is destroyed.
   * @param runtime Runtime state that owns the background tasks and result queues.
   */
  void finalize_shell_tasks(ShellRuntimeState &runtime) {
    join_shell_session_tasks(runtime);
    wait_for_thread(runtime.hostDiscoveryTask.thread);
    for (const std::unique_ptr<HostProbeTaskState::ProbeWorkerState> &worker : runtime.hostProbeTask.workers) {
      if (worker == nullptr) {
        continue;
      }

      wait_for_thread(worker->thread);
    }
  }

}  // namespace

namespace ui {

  int run_shell(
    SDL_Window *window,
    const VIDEO_MODE &videoMode,
    app::ClientState &state
  ) {
    if (window == nullptr) {
      return report_shell_failure("sdl", "Shell requires a valid SDL window");
    }

    VIDEO_MODE activeVideoMode = videoMode;

    ShellResources resources {};
    if (ShellInitializationFailure initializationFailure {}; !initialize_shell_resources(window, activeVideoMode, &resources, &initializationFailure)) {
      return report_shell_failure(initializationFailure.category, initializationFailure.message);
    }

    ShellRuntimeState runtime {};
    initialize_shell_runtime(state, &runtime);
    runtime.saveFeedback.redraw = [&] {
      if (runtime.running && resources.renderer != nullptr) {
        draw_current_shell_frame(activeVideoMode, state, &resources, &runtime);
        runtime.keypadRedrawRequested = true;
      }
    };

    while (runtime.running && !state.shell.shouldExit) {
      if (!run_shell_frame(window, &activeVideoMode, state, &resources, &runtime)) {
        break;
      }
    }

    finalize_shell_tasks(runtime);
    close_shell_resources(&resources);
    return 0;
  }

}  // namespace ui
