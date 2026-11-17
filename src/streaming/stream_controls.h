/**
 * @file src/streaming/stream_controls.h
 * @brief SDL input devices and local controls for an active stream.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// lib includes
#include <SDL.h>
#include <SDL_ttf.h>

// local includes
#include "src/app/screenshot_library.h"
#include "src/input/stream_exit.h"
#include "src/input/stream_input.h"
#include "src/input/stream_menu.h"
#include "src/input/ui_input.h"
#include "src/streaming/screenshot_status.h"
#include "src/streaming/video_overlay.h"

namespace streaming {
  class FfmpegStreamBackend;

  /**
   * @brief Owns four gamepads, local stream controls and ordered host input.
   */
  class StreamControls {
  public:
    /**
     * @brief Construct an event-thread input router.
     */
    StreamControls();
    /**
     * @brief Close owned controllers.
     */
    ~StreamControls();
    StreamControls(const StreamControls &) = delete;
    StreamControls &operator=(const StreamControls &) = delete;
    /**
     * @brief Adopt a controller and discover other devices.
     * @param controller Owned SDL controller reference.
     */
    void reset(SDL_GameController *controller);
    /**
     * @brief Apply persisted local-control preferences for this session.
     * @param screenshotDirectory Screenshot folder, or empty for the title-scoped default.
     * @param keyboardTop True to dock the keyboard to the top edge.
     * @param source Stable host and app metadata for saved screenshots.
     */
    void configure(std::string screenshotDirectory, bool keyboardTop, app::ScreenshotSource source = {});
    /**
     * @brief Read the placement selected while streaming.
     * @return True for top docking.
     */
    bool keyboard_docked_top() const;
    /**
     * @brief Read whether exit confirmation requested terminating the host app.
     * @return True only for the explicit Quit app action.
     */
    bool quit_app_requested() const;
    /**
     * @brief Close all owned SDL references.
     */
    void close();
    /**
     * @brief Open a controller in the lowest free local slot.
     * @param deviceIndex SDL joystick index.
     */
    void open_if_needed(int deviceIndex);
    /**
     * @brief Remove a controller while preserving all other slot numbers.
     * @param instance SDL joystick instance ID.
     */
    void close_if_removed(int instance);
    /**
     * @brief Sample devices for summary dismissal.
     * @return True when any controller has the menu chord pressed.
     */
    bool poll();
    /**
     * @brief Check summary dismissal buttons.
     * @return True if any connected pad has a pressed button or trigger.
     */
    bool any_button_pressed() const;
    /**
     * @brief Transfer a connected controller to the shell.
     * @return Owned reference, or nullptr.
     */
    SDL_GameController *take_controller_for_shell();
    /**
     * @brief Describe controller connectivity for diagnostic builds.
     * @return One line for each occupied local slot.
     */
    std::vector<std::string> diagnostic_lines() const;
    /**
     * @brief Route a bounded batch of SDL events and update cached local controls without starving video presentation.
     * @param backend Active video backend receiving the shared GPU/software overlay.
     * @param font Font for menu labels and keyboard cells.
     * @param width Output width.
     * @param height Output height.
     * @param active True once the transport is ready for input.
     * @param statisticsEnabled Mutable session statistics toggle.
     * @return True when the user confirms quit or closes the application window.
     */
    bool pump(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height, bool active, bool &statisticsEnabled);
    /**
     * @brief Check whether a local screen currently owns the video overlay.
     * @return True while the radial menu, keyboard, log viewer or confirmation is visible.
     */
    bool visible() const;
    /**
     * @brief Check whether local controls or a screenshot popup currently own the shared overlay.
     * @return True while a menu or capture notification is visible.
     */
    bool has_overlay() const;
    /**
     * @brief Release all held host input before transport shutdown.
     */
    void release_host_input();

  private:
    /**
     * @brief One renderer-owned text texture reused between focus updates.
     */
    struct CachedText {
      int width = 0;  ///< Natural raster width.
      int height = 0;  ///< Natural raster height.
      std::shared_ptr<SDL_Texture> texture;  ///< Texture tied to the retained panel renderer.
    };

    /**
     * @brief Release panel caches before destroying their renderer and surface.
     */
    void clear_panel();
    /**
     * @brief Draw cached text without rerasterizing unchanged labels on each selection.
     * @param renderer Retained panel renderer.
     * @param font Label font.
     * @param text UTF-8 label.
     * @param bounds Label bounds.
     * @param leading Align console output to its top-left corner.
     * @param background Paint an opaque label background.
     * @param fitSingleLine Shrink a single-line key label to fit.
     * @param foreground Text color.
     */
    void label(SDL_Renderer *renderer, TTF_Font *font, const std::string &text, SDL_Rect bounds, bool leading = false, bool background = true, bool fitSingleLine = false, SDL_Color foreground = {255, 255, 255, 255});
    /**
     * @brief Apply sampled or queued digital/stick navigation with shared repeat timing.
     * @param sample Controller state, with positive stick Y upward.
     * @param now Current SDL tick.
     */
    void navigate_controller(const input::ControllerState &sample, Uint32 now);

    /**
     * @brief Independent controller state and shortcut latch.
     */
    struct Device {
      SDL_GameController *handle = nullptr;  ///< Owned SDL reference.
      input::StreamExitChord chord;  ///< Event-order chord detection.
      input::ControllerState sample {};  ///< Latest sampled state.
      bool chordDown = false;  ///< Chord edge latch.
    };

    /**
     * @brief Open the wheels from gameplay or any local page and release host inputs.
     * @param device Controller instance ID, or -1 for keyboard control.
     */
    void open_menu(int device);
    /**
     * @brief Draw cached colored controls with an opaque OSK and a translucent wheel backdrop.
     * @param backend Video backend.
     * @param font Menu font.
     * @param width Output width.
     * @param height Output height.
     * @param stats Current statistics toggle.
     */
    void render_menu(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height, bool stats);
    /**
     * @brief Draw a compact screenshot progress or completion popup without capturing host input.
     * @param backend Shared GPU/software overlay owner.
     * @param font Notification font.
     * @param width Available video width.
     * @param height Available video height.
     */
    void render_screenshot_notification(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height);
    /**
     * @brief Draw the keyboard, optionally repainting only the old and new focus cells.
     * @param renderer Software panel renderer.
     * @param font Keyboard font.
     * @param width Panel width.
     * @param height Panel height, including the helper footer.
     * @param rowHeight Height of one character row.
     * @param focusOnly Retain unchanged keys and helpers during directional navigation.
     */
    void render_keyboard(SDL_Renderer *renderer, TTF_Font *font, int width, int height, int rowHeight, bool focusOnly = false);
    /**
     * @brief Draw a cached SVG raster on the current software panel.
     * @param renderer Panel renderer.
     * @param asset Icon filename under assets/icons.
     * @param bounds Destination bounds.
     * @param clip Optional repaint rectangle, applied through source cropping rather than renderer clipping.
     * @return True when the icon was drawn.
     */
    bool render_icon(SDL_Renderer *renderer, const char *asset, SDL_Rect bounds, const SDL_Rect *clip = nullptr);
    /**
     * @brief Draw a controller glyph or keyboard keycap beside its action label.
     * @param renderer Panel renderer.
     * @param font Label font.
     * @param asset Controller SVG filename.
     * @param key Keyboard keycap label.
     * @param text Action label.
     * @param bounds Available hint rectangle.
     * @param compact Use smaller glyphs inside keyboard keys, preserving footer sizing.
     */
    void render_hint(SDL_Renderer *renderer, TTF_Font *font, const char *asset, const char *key, const char *text, SDL_Rect bounds, bool compact = false);
    /**
     * @brief Process a local navigation or activation key.
     * @param key SDL key code.
     * @param backend Video backend for screenshots.
     * @param active Transport readiness.
     * @param stats Mutable statistics toggle.
     * @return True on confirmed quit.
     */
    bool menu_key(SDL_Keycode key, FfmpegStreamBackend &backend, bool active, bool &stats);
    /**
     * @brief Tap a host key with the on-screen keyboard's modifier toggles.
     * @param code Virtual key code.
     */
    void tap_key(int code);
    /**
     * @brief End a Guide/Share pulse while restoring the latest gameplay state.
     */
    void release_pulse();
    std::array<Device, 4> devices_ {};  ///< Stable local controller slots.
    input::StreamInput router_;  ///< Ordered event-thread host input.
    input::StreamMenu menu_;  ///< Local menu state.
    input::KeyboardNavigation keyboardNavigation_;  ///< Local keyboard repeats, independent of gameplay key forwarding.
    input::DirectionNavigation controllerNavigation_;  ///< Immediate direction changes with the shared repeat interval.
    bool controllerNavigationApplied_ = false;  ///< Queued events and the polling sample can advance focus only once per input pass.
    bool navigationNeutralRequired_ = false;  ///< Release wheel navigation before moving around a newly opened page.
    input::UiInputDevice inputDevice_ = input::UiInputDevice::gamepad;  ///< Device used by local helper text.
    bool armed_ = false;  ///< Opening buttons have been released.
    bool dirty_ = false;  ///< Overlay requires rasterization.
    bool focused_ = true;  ///< Window focus state.
    int mouseDevice_ = -1;  ///< Virtual mouse controller, or disabled.
    Uint32 lastTick_ = 0;  ///< Pointer integration clock.
    int pulseSlot_ = -1;  ///< Controller with a pending guide/share release.
    Uint32 pulseUntil_ = 0;  ///< Guide/share pulse deadline.
    std::uint32_t pulseButtons_ = 0;  ///< Button held across normal packets after the wheel closes.
    std::array<input::ControllerState, 4> latestHostController_ {};  ///< Latest unmodified transport state for pulse release.
    std::string message_;  ///< Last action result.
    std::vector<std::string> logs_;  ///< Snapshot of runtime log lines.
    int menuWidth_ = 0;  ///< Last video rectangle width used for local controls.
    int menuHeight_ = 0;  ///< Last video rectangle height used for local controls.
    bool keyboardPageTriggerDown_ = false;  ///< Edge latch for the left-trigger keyboard layout shortcut.
    bool keyboardDockTriggerDown_ = false;  ///< Edge latch for right-trigger keyboard docking.
    bool keyboardTop_ = false;  ///< Dock the keyboard flush with the top edge.
    std::string screenshotDirectory_;  ///< Configured screenshot folder, empty for the default.
    ScreenshotNotification screenshotNotification_;  ///< Progress and timed completion feedback independent of menu capture.
    app::ScreenshotSource screenshotSource_;  ///< Stream identity retained with each capture.
    Uint32 logRefreshTick_ = 0;  ///< Last live log refresh.
    std::map<std::pair<std::string, int>, std::shared_ptr<SDL_Surface>> icons_;  ///< Cached SVG rasters survive panel renderer replacement.
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> panel_ {nullptr, SDL_FreeSurface};  ///< Retained software composition target.
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> panelRenderer_ {nullptr, SDL_DestroyRenderer};  ///< Renderer reused at a stable panel size.
    std::map<std::tuple<TTF_Font *, std::string, int, Uint32>, CachedText> text_;  ///< Bounded cache of unchanged label rasters.
    std::size_t textBytes_ = 0;  ///< Estimated text texture bytes, bounded to protect the Xbox memory budget.
    std::map<std::pair<std::string, int>, std::shared_ptr<SDL_Texture>> iconTextures_;  ///< Renderer-owned SVG textures.
    /**
     * @brief Content dependencies other than the focused key or wheel sector.
     */
    using PanelContent = std::tuple<TTF_Font *, input::UiInputDevice, input::StreamMenuPage, int, int, bool, bool, bool, int, bool, std::string>;
    std::optional<PanelContent> panelContent_;  ///< Last fully drawn content; changes invalidate incremental focus painting.
    int panelSelection_ = -1;  ///< Previously painted keyboard cell or wheel sector.
    std::vector<VideoRectangle> panelDamage_;  ///< Regions repainted during the current input pass.
  };
}  // namespace streaming
