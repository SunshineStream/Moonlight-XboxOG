/**
 * @file src/streaming/stream_controls.h
 * @brief SDL input devices and local controls for an active stream.
 */
#pragma once

#include "src/input/stream_exit.h"
#include "src/input/stream_input.h"
#include "src/input/stream_menu.h"
#include "src/input/ui_input.h"

#include <array>
#include <map>
#include <memory>
#include <SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <vector>

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
     */
    void configure(std::string screenshotDirectory, bool keyboardTop);
    /**
     * @brief Read the placement selected while streaming.
     * @return True for top docking.
     */
    bool keyboard_docked_top() const;
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
     * @brief Route SDL events and rebuild local UI only when it changes.
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
     * @brief Release all held host input before transport shutdown.
     */
    void release_host_input();

  private:
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
     * @brief Draw colored controls with a solid OSK panel and a transparent wheel background.
     * @param backend Video backend.
     * @param font Menu font.
     * @param width Output width.
     * @param height Output height.
     * @param stats Current statistics toggle.
     */
    void render_menu(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height, bool stats);
    /**
     * @brief Draw the Xbox-style character grid, wide space bar and tall shortcut controls.
     * @param renderer Software panel renderer.
     * @param font Keyboard font.
     * @param width Panel width.
     * @param height Panel height, including the helper footer.
     * @param rowHeight Height of one character row.
     */
    void render_keyboard(SDL_Renderer *renderer, TTF_Font *font, int width, int height, int rowHeight);
    /**
     * @brief Draw a cached SVG raster on the current software panel.
     * @param renderer Panel renderer.
     * @param asset Icon filename under assets/icons.
     * @param bounds Destination bounds.
     * @return True when the icon was drawn.
     */
    bool render_icon(SDL_Renderer *renderer, const char *asset, SDL_Rect bounds);
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
    std::array<Device, 4> devices_ {};  ///< Stable local controller slots.
    input::StreamInput router_;  ///< Ordered event-thread host input.
    input::StreamMenu menu_;  ///< Local menu state.
    input::KeyboardNavigation keyboardNavigation_;  ///< Local keyboard repeats, independent of gameplay key forwarding.
    input::UiInputDevice inputDevice_ = input::UiInputDevice::gamepad;  ///< Device used by local helper text.
    bool armed_ = false;  ///< Opening buttons have been released.
    bool dirty_ = false;  ///< Overlay requires rasterization.
    bool focused_ = true;  ///< Window focus state.
    int mouseDevice_ = -1;  ///< Virtual mouse controller, or disabled.
    Uint32 lastTick_ = 0;  ///< Pointer integration clock.
    Uint32 navigationTick_ = 0;  ///< Stick navigation repeat clock.
    int pulseSlot_ = -1;  ///< Controller with a pending guide/share release.
    Uint32 pulseUntil_ = 0;  ///< Guide/share pulse deadline.
    std::string message_;  ///< Last action result.
    std::vector<std::string> logs_;  ///< Snapshot of runtime log lines.
    int menuWidth_ = 0;  ///< Last video rectangle width used for local controls.
    int menuHeight_ = 0;  ///< Last video rectangle height used for local controls.
    bool keyboardPageTriggerDown_ = false;  ///< Edge latch for the left-trigger keyboard layout shortcut.
    bool keyboardDockTriggerDown_ = false;  ///< Edge latch for right-trigger keyboard docking.
    bool keyboardTop_ = false;  ///< Dock the keyboard flush with the top edge.
    std::string screenshotDirectory_;  ///< Configured screenshot folder, empty for the default.
    Uint32 logRefreshTick_ = 0;  ///< Last live log refresh.
    std::map<std::pair<std::string, int>, std::shared_ptr<SDL_Surface>> icons_;  ///< Cached SVG rasters survive panel renderer replacement.
  };
}  // namespace streaming
