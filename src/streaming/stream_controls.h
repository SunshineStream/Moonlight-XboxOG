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
   * @brief Text alignment, background and color for a cached stream-control label.
   */
  struct StreamLabelStyle {
    bool leading = false;  ///< Align text to its top-left corner.
    bool background = true;  ///< Restore an opaque label background before drawing.
    bool fitSingleLine = false;  ///< Shrink the label to fit on one line.
    SDL_Color foreground {255, 255, 255, 255};  ///< Original text color.
  };

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
     * @brief Sample the state of all connected controllers.
     * @return True when any controller has the menu chord pressed.
     */
    bool poll();
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
     * @param style Text alignment, background and color.
     */
    void label(SDL_Renderer *renderer, TTF_Font *font, const std::string &text, SDL_Rect bounds, const StreamLabelStyle &style = {});

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
     * @brief Refresh live log entries while preserving a scrolled reading position.
     * @param now Monotonic millisecond tick.
     */
    void refresh_logs(Uint32 now);
    /**
     * @brief Publish local controls or capture feedback through the shared overlay.
     * @param backend Streaming backend.
     * @param font Overlay font.
     * @param width Output width.
     * @param height Output height.
     * @param wasVisible Controls were visible before this input batch.
     * @param screenshotChanged Capture feedback needs repainting.
     * @param stats Session statistics setting.
     */
    void refresh_overlay(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height, bool wasVisible, bool screenshotChanged, bool stats);
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
     * @brief Rasterization context shared by the sectors of one menu wheel.
     */
    struct WheelPaint {
      OverlayBitmap target;  ///< Writable panel pixels and geometry.
      MenuWheel geometry;  ///< Wheel center, radius and selection.
      int index;  ///< Left or right action wheel.
      int line;  ///< Text line height including spacing.
      bool focusOnly;  ///< Restore only changed sectors and labels.
      bool stats;  ///< Current session statistics setting.
    };

    /**
     * @brief Draw one keyboard cell, its shortcut glyph and focus outline.
     * @param renderer Panel renderer.
     * @param font Keyboard font.
     * @param i Cell index.
     * @param key Cell pixel bounds.
     */
    void render_keyboard_key(SDL_Renderer *renderer, TTF_Font *font, int i, SDL_Rect key);
    /**
     * @brief Draw or restore one wheel sector and its action label.
     * @param renderer Panel renderer.
     * @param font Wheel font.
     * @param paint Wheel rasterization context.
     * @param sector Clockwise sector from up.
     */
    void render_wheel_sector(SDL_Renderer *renderer, TTF_Font *font, const WheelPaint &paint, int sector);
    /**
     * @brief Draw both menu wheels and their footer controls.
     * @param renderer Panel renderer.
     * @param font Wheel font.
     * @param width Panel width.
     * @param height Panel height.
     * @param stats Current statistics setting.
     * @param focusOnly Repaint only changed sectors.
     */
    void render_wheels(SDL_Renderer *renderer, TTF_Font *font, int width, int height, bool stats, bool focusOnly);
    /**
     * @brief Draw the live log panel and scroll position.
     * @param renderer Panel renderer.
     * @param font Log font.
     * @param width Panel width.
     * @param height Panel height.
     */
    void render_logs(SDL_Renderer *renderer, TTF_Font *font, int width, int height);
    /**
     * @brief Draw direct cancel, disconnect and quit-app actions.
     * @param renderer Panel renderer.
     * @param font Dialog font.
     * @param width Panel width.
     * @param height Panel height.
     */
    void render_exit_confirmation(SDL_Renderer *renderer, TTF_Font *font, int width, int height);
    /**
     * @brief Allocate panel geometry when needed and enforce the raster cache budget.
     * @param width Panel width.
     * @param height Panel height.
     * @return True when a panel renderer is available.
     */
    bool prepare_panel(int width, int height);
    /**
     * @brief Publish changed panel regions through the shared GPU and software overlay.
     * @param backend Overlay backend.
     * @param width Panel width.
     * @param height Panel height.
     * @param keyboard Opaque docked keyboard placement.
     * @param focusOnly Publish only damaged regions.
     */
    void publish_panel(FfmpegStreamBackend &backend, int width, int height, bool keyboard, bool focusOnly) const;
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
     * @brief Process ordered input events using state owned by the enclosing stream session.
     */
    class EventHandler {
    public:
      /**
       * @brief Bind input processing to its owning session without copying SDL references.
       * @param controls Session whose lifetime contains this handler.
       */
      explicit EventHandler(StreamControls &controls) noexcept;
      /**
       * @brief Apply sampled or queued digital/stick navigation with shared repeat timing.
       * @param sample Controller state, with positive stick Y upward.
       * @param now Current SDL tick.
       */
      void navigate_controller(const input::ControllerState &sample, Uint32 now) const;
      /**
       * @brief Track deliberate gamepad activity and keyboard trigger shortcuts.
       * @param event Queued SDL event.
       */
      void note_gamepad_activity(const SDL_Event &event) const;
      /**
       * @brief Apply trigger edges while the keyboard owns this controller.
       * @param axis Queued controller axis transition.
       */
      void keyboard_trigger(const SDL_ControllerAxisEvent &axis) const;
      /**
       * @brief Recognize the original Xbox raw-button shortcut.
       * @param button Queued joystick button transition.
       */
      void joystick_button(const SDL_JoyButtonEvent &button) const;
      /**
       * @brief Route controller transitions before local menu activation.
       * @param button Queued controller button transition.
       * @param active Transport readiness.
       */
      void route_controller_button(const SDL_ControllerButtonEvent &button, bool active) const;
      /**
       * @brief Navigate from the current sample of the controller owning the local menu.
       */
      void navigate_owner_controller() const;
      /**
       * @brief Handle controller release gates and local button activation.
       * @param button Queued controller button transition.
       * @param backend Streaming backend.
       * @param active Transport readiness.
       * @param statisticsEnabled Session statistics setting.
       * @return True on a confirmed stream exit.
       */
      bool controller_button(const SDL_ControllerButtonEvent &button, FfmpegStreamBackend &backend, bool active, bool &statisticsEnabled) const;
      /**
       * @brief Apply one local controller button action.
       * @param button SDL controller button.
       * @param backend Streaming backend.
       * @param active Transport readiness.
       * @param statisticsEnabled Session statistics setting.
       * @return True on a confirmed stream exit.
       */
      bool activate_controller_button(Uint8 button, FfmpegStreamBackend &backend, bool active, bool &statisticsEnabled) const;
      /**
       * @brief Apply queued stick movement before the next sampled state.
       * @param axis Queued controller axis transition.
       */
      void controller_axis(const SDL_ControllerAxisEvent &axis) const;
      /**
       * @brief Route physical keyboard input to local controls or the host.
       * @param key Queued keyboard transition.
       * @param backend Streaming backend.
       * @param active Transport readiness.
       * @param statisticsEnabled Session statistics setting.
       * @return True on a confirmed stream exit.
       */
      bool keyboard_event(const SDL_KeyboardEvent &key, FfmpegStreamBackend &backend, bool active, bool &statisticsEnabled) const;
      /**
       * @brief Dispatch a single SDL event without delaying the bounded input batch.
       * @param event Queued SDL event.
       * @param backend Streaming backend.
       * @param active Transport readiness.
       * @param statisticsEnabled Session statistics setting.
       * @return True on a confirmed stream exit or window close.
       */
      bool handle_event(const SDL_Event &event, FfmpegStreamBackend &backend, bool active, bool &statisticsEnabled) const;
      /**
       * @brief Route and navigate one sampled controller.
       * @param device Owned controller slot.
       * @param active Transport readiness.
       * @param now Monotonic millisecond tick.
       */
      void update_device(Device &device, bool active, Uint32 now) const;
      /**
       * @brief Open the wheels from gameplay or any local page and release host inputs.
       * @param device Controller instance ID, or -1 for keyboard control.
       */
      void open_menu(int device) const;
      /**
       * @brief Apply direct cancel, disconnect or quit-app input on the exit confirmation page.
       * @param key SDL key code.
       * @return True for a confirmed stream exit.
       */
      bool confirm_exit_key(SDL_Keycode key) const;
      /**
       * @brief Apply a layout, modifier, docking or editing shortcut on the keyboard page.
       * @param key SDL key code.
       * @param active Transport readiness for host key taps.
       * @return True when the keyboard consumes this shortcut.
       */
      bool handle_keyboard_shortcut(SDL_Keycode key, bool active) const;
      /**
       * @brief Execute the selected wheel action or open its local page.
       * @param backend Overlay and screenshot backend.
       * @param active Transport readiness.
       * @param stats Mutable session statistics setting.
       */
      void activate_stream_action(FfmpegStreamBackend &backend, bool active, bool &stats) const;
      /**
       * @brief Process a local navigation or activation key.
       * @param key SDL key code.
       * @param backend Video backend for screenshots.
       * @param active Transport readiness.
       * @param stats Mutable statistics toggle.
       * @return True on confirmed quit.
       */
      bool menu_key(SDL_Keycode key, FfmpegStreamBackend &backend, bool active, bool &stats) const;
      /**
       * @brief Tap a host key with the on-screen keyboard's modifier toggles.
       * @param code Virtual key code.
       */
      void tap_key(int code) const;
      /**
       * @brief End a Guide/Share pulse while restoring the latest gameplay state.
       */
      void release_pulse() const;

    private:
      StreamControls &controls_;  ///< Session state updated by this handler's input operations.
    };

    EventHandler events_ {*this};  ///< Input processing bound to this session's stable lifetime.
    std::array<Device, 4> devices_ {};  ///< Stable local controller slots.
    input::StreamInput router_;  ///< Ordered event-thread host input.
    input::StreamMenu menu_;  ///< Local menu state.
    bool focused_ = true;  ///< Window focus state.
    int mouseDevice_ = -1;  ///< Virtual mouse controller, or disabled.
    Uint32 lastTick_ = 0;  ///< Pointer integration clock.
    int pulseSlot_ = -1;  ///< Controller with a pending guide/share release.
    Uint32 pulseUntil_ = 0;  ///< Guide/share pulse deadline.
    std::uint32_t pulseButtons_ = 0;  ///< Button held across normal packets after the wheel closes.
    std::array<input::ControllerState, 4> latestHostController_ {};  ///< Latest unmodified transport state for pulse release.
    bool keyboardTop_ = false;  ///< Dock the keyboard flush with the top edge.
    std::string screenshotDirectory_;  ///< Configured screenshot folder, empty for the default.
    ScreenshotNotification screenshotNotification_;  ///< Progress and timed completion feedback independent of menu capture.
    app::ScreenshotSource screenshotSource_;  ///< Stream identity retained with each capture.
    /**
     * @brief Content dependencies other than the focused key or wheel sector.
     */
    using PanelContent = std::tuple<TTF_Font *, input::UiInputDevice, input::StreamMenuPage, int, int, bool, bool, bool, int, bool, std::string>;

    /**
     * @brief Input ownership, release gates and local navigation timing.
     */
    struct NavigationState {
      input::KeyboardNavigation keyboard;  ///< Local keyboard repeats, independent of gameplay key forwarding.
      input::DirectionNavigation controller;  ///< Immediate direction changes with the shared repeat interval.
      bool applied = false;  ///< Queued events and the polling sample can advance focus only once per input pass.
      bool neutralRequired = false;  ///< Release wheel navigation before moving around a newly opened page.
      input::UiInputDevice inputDevice = input::UiInputDevice::gamepad;  ///< Device used by local helper text.
      bool armed = false;  ///< Opening buttons have been released.
      bool pageTriggerDown = false;  ///< Edge latch for the left-trigger keyboard layout shortcut.
      bool dockTriggerDown = false;  ///< Edge latch for right-trigger keyboard docking.
    };

    NavigationState navigation_;  ///< Input ownership, release gates and local navigation timing.

    /**
     * @brief Retained overlay surfaces, rasters and content invalidation.
     */
    struct PanelCache {
      bool dirty = false;  ///< Overlay requires rasterization.
      std::string message;  ///< Last action result.
      std::vector<std::string> logs;  ///< Snapshot of runtime log lines.
      int width = 0;  ///< Last video rectangle width used for local controls.
      int height = 0;  ///< Last video rectangle height used for local controls.
      Uint32 logRefreshTick = 0;  ///< Last live log refresh.
      std::map<std::pair<std::string, int>, std::shared_ptr<SDL_Surface>> icons;  ///< Cached SVG rasters survive panel renderer replacement.
      std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface {nullptr, &SDL_FreeSurface};  ///< Retained software composition target.
      std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer {nullptr, &SDL_DestroyRenderer};  ///< Renderer reused at a stable panel size.
      std::map<std::tuple<TTF_Font *, std::string, int, Uint32>, CachedText> text;  ///< Bounded cache of unchanged label rasters.
      std::size_t textBytes = 0;  ///< Estimated text texture bytes, bounded to protect the Xbox memory budget.
      std::map<std::pair<std::string, int>, std::shared_ptr<SDL_Texture>> iconTextures;  ///< Renderer-owned SVG textures.
      std::optional<PanelContent> content;  ///< Last fully drawn content; changes invalidate incremental focus painting.
      int selection = -1;  ///< Previously painted keyboard cell or wheel sector.
      std::vector<VideoRectangle> damage;  ///< Regions repainted during the current input pass.
    };

    PanelCache panel_;  ///< Retained overlay surfaces, rasters and content invalidation.
  };
}  // namespace streaming
