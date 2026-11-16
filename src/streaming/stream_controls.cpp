/**
 * @file src/streaming/stream_controls.cpp
 * @brief Implements dual-stick stream controls and SDL-to-Moonlight input routing.
 */
#include "src/streaming/stream_controls.h"

#include "src/logging/log_file.h"
#include "src/logging/logger.h"
#include "src/startup/storage_paths.h"
#include "src/streaming/ffmpeg_stream_backend.h"
#include "src/ui/asset_path.h"
#include "src/ui/dialog_surface.h"
#include "src/ui/svg_assets.h"
#include "third-party/moonlight-common-c/src/Limelight.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {
  /**
   * @brief Map local menu navigation keys to the shared repeat commands.
   * @param key SDL key symbol.
   * @return Repeatable local command, or none for activation keys.
   */
  input::UiCommand menu_navigation_command(SDL_Keycode key) {
    switch (key) {
      case SDLK_UP:
        return input::UiCommand::move_up;
      case SDLK_DOWN:
        return input::UiCommand::move_down;
      case SDLK_LEFT:
        return input::UiCommand::move_left;
      case SDLK_RIGHT:
        return input::UiCommand::move_right;
      default:
        return input::UiCommand::none;
    }
  }

  /**
   * @brief Convert a shared repeat command back to its local menu key.
   * @param command Repeated navigation command.
   * @return SDL key symbol, or unknown when no repeat is due.
   */
  SDL_Keycode menu_navigation_key(input::UiCommand command) {
    switch (command) {
      case input::UiCommand::move_up:
        return SDLK_UP;
      case input::UiCommand::move_down:
        return SDLK_DOWN;
      case input::UiCommand::move_left:
        return SDLK_LEFT;
      case input::UiCommand::move_right:
        return SDLK_RIGHT;
      default:
        return SDLK_UNKNOWN;
    }
  }

  /**
   * @brief Translate an SDL controller button to Moonlight's bit mask.
   * @param button SDL controller button.
   * @return Host flag, or zero for unsupported buttons.
   */
  int controller_flag(int button) {
    switch (button) {
      case SDL_CONTROLLER_BUTTON_A:
        return A_FLAG;
      case SDL_CONTROLLER_BUTTON_B:
        return B_FLAG;
      case SDL_CONTROLLER_BUTTON_X:
        return X_FLAG;
      case SDL_CONTROLLER_BUTTON_Y:
        return Y_FLAG;
      case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return UP_FLAG;
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return DOWN_FLAG;
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return LEFT_FLAG;
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return RIGHT_FLAG;
      case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return LB_FLAG;
      case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return RB_FLAG;
      case SDL_CONTROLLER_BUTTON_START:
        return PLAY_FLAG;
      case SDL_CONTROLLER_BUTTON_BACK:
        return BACK_FLAG;
      case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return LS_CLK_FLAG;
      case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        return RS_CLK_FLAG;
      case SDL_CONTROLLER_BUTTON_GUIDE:
        return SPECIAL_FLAG;
      default:
        return 0;
    }
  }

  /**
   * @brief Resolve a raw joystick button bound to one of the three menu shortcut buttons.
   * @param controller Connected game controller.
   * @param button Raw joystick button index.
   * @return Shortcut flag, or zero for another control.
   */
  int joystick_chord_flag(SDL_GameController *controller, int button) {
    int flags = 0;
    for (const auto mapped : {SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}) {
      const auto binding = SDL_GameControllerGetBindForButton(controller, mapped);
      if (binding.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON && binding.value.button == button) {
        flags |= controller_flag(mapped);
      }
    }
    return flags;
  }

  /**
   * @brief Read a connected SDL controller with saturated Y inversion.
   * @param handle SDL controller reference.
   * @return Complete host-oriented controller sample.
   */
  input::ControllerState sample_controller(SDL_GameController *handle) {
    input::ControllerState state;
    for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
      if (SDL_GameControllerGetButton(handle, static_cast<SDL_GameControllerButton>(button)) != 0) {
        state.buttonFlags |= controller_flag(button);
      }
    }
    // Read the shortcut's physical bindings too, independently of translated
    // controller events. This also handles a button held across a UI handoff.
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(handle);
    for (int button = 0; button < SDL_JoystickNumButtons(joystick); ++button) {
      if (SDL_JoystickGetButton(joystick, button) != 0) {
        state.buttonFlags |= joystick_chord_flag(handle, button);
      }
    }
    state.leftTrigger = std::clamp(static_cast<int>(SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_TRIGGERLEFT)), 0, 32767) * 255 / 32767;
    state.rightTrigger = std::clamp(static_cast<int>(SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)), 0, 32767) * 255 / 32767;
    state.leftStickX = SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_LEFTX);
    state.leftStickY = input::invert_stick_axis(SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_LEFTY));
    state.rightStickX = SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_RIGHTX);
    state.rightStickY = input::invert_stick_axis(SDL_GameControllerGetAxis(handle, SDL_CONTROLLER_AXIS_RIGHTY));
    return state;
  }

  /**
   * @brief Enqueue one ordered host input packet; transport calls do not wait for network I/O.
   * @param packet Transport-neutral input operation.
   */
  void send_packet(const input::StreamPacket &packet) {
    const auto &state = packet.controller;
    switch (packet.type) {
      case input::StreamPacketType::arrival:
        {
          constexpr std::uint32_t buttons = A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG | MISC_FLAG;
          // Rumble is not advertised until a host-to-device feedback callback exists.
          LiSendControllerArrivalEvent(packet.slot, packet.mask, LI_CTYPE_XBOX, buttons, LI_CCAP_ANALOG_TRIGGERS);
          break;
        }
      case input::StreamPacketType::controller:
        LiSendMultiControllerEvent(packet.slot, packet.mask, state.buttonFlags, state.leftTrigger, state.rightTrigger, state.leftStickX, state.leftStickY, state.rightStickX, state.rightStickY);
        break;
      case input::StreamPacketType::key:
        LiSendKeyboardEvent(packet.code & 0xFF, packet.pressed ? KEY_ACTION_DOWN : KEY_ACTION_UP, packet.modifiers | ((packet.code & 0x100) != 0 ? MODIFIER_EXTENDED : 0));
        break;
      case input::StreamPacketType::mouse_button:
        LiSendMouseButtonEvent(packet.pressed ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, packet.code);
        break;
      case input::StreamPacketType::mouse_move:
        LiSendMouseMoveEvent(packet.x, packet.y);
        break;
      case input::StreamPacketType::scroll:
        if (packet.x != 0) {
          LiSendHScrollEvent(packet.x);
        }
        if (packet.y != 0) {
          LiSendScrollEvent(packet.y);
        }
        break;
    }
  }

}  // namespace

namespace streaming {
  void StreamControls::clear_panel() {
    text_.clear();
    textBytes_ = 0;
    iconTextures_.clear();
    panelRenderer_.reset();
    panel_.reset();
  }

  void StreamControls::label(SDL_Renderer *renderer, TTF_Font *font, const std::string &text, SDL_Rect bounds, bool leading, bool background, bool fitSingleLine, SDL_Color foreground) {
    if (background) {
      SDL_SetRenderDrawColor(renderer, 16, 16, 16, 255);
      SDL_RenderFillRect(renderer, &bounds);
    }
    if (text.empty()) {
      return;
    }
    const Uint32 color = foreground.r | (static_cast<Uint32>(foreground.g) << 8) | (static_cast<Uint32>(foreground.b) << 16) | (static_cast<Uint32>(foreground.a) << 24);
    const auto key = std::make_tuple(font, text, fitSingleLine ? 0 : std::max(1, bounds.w - 4), color);
    auto found = text_.find(key);
    if (found == text_.end()) {
      std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(fitSingleLine ? TTF_RenderUTF8_Blended(font, text.c_str(), foreground) : TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), foreground, std::max(1, bounds.w - 4)), SDL_FreeSurface);
      if (!surface) {
        return;
      }
      const auto bytes = static_cast<std::size_t>(surface->w) * surface->h * 4;
      if (textBytes_ + bytes > 1024U * 1024U) {
        SDL_RenderFlush(renderer);
        text_.clear();
        textBytes_ = 0;
      }
      std::shared_ptr<SDL_Texture> texture(SDL_CreateTextureFromSurface(renderer, surface.get()), SDL_DestroyTexture);
      if (!texture) {
        return;
      }
      found = text_.emplace(key, CachedText {surface->w, surface->h, std::move(texture)}).first;
      textBytes_ += bytes;
    }
    const auto &raster = found->second;
    // nxdk SDL 2.0.9 requires source cropping rather than standalone clip commands.
    auto layout = layout_overlay_label(raster.width, raster.height, {bounds.x, bounds.y, bounds.w, bounds.h}, fitSingleLine);
    if (leading) {
      layout.source.x = layout.source.y = 0;
      layout.destination.x = bounds.x;
      layout.destination.y = bounds.y;
    }
    SDL_Rect source {layout.source.x, layout.source.y, layout.source.width, layout.source.height};
    SDL_Rect destination {layout.destination.x, layout.destination.y, layout.destination.width, layout.destination.height};
    SDL_RenderCopy(renderer, raster.texture.get(), &source, &destination);
  }

  void StreamControls::navigate_controller(const input::ControllerState &sample, Uint32 now) {
    if (menu_.page != input::StreamMenuPage::keyboard && menu_.page != input::StreamMenuPage::logs) {
      controllerNavigation_.reset();
      return;
    }
    int x = (sample.buttonFlags & RIGHT_FLAG) != 0 ? 1 : (sample.buttonFlags & LEFT_FLAG) != 0 ? -1 :
                                                                                                 0;
    int y = (sample.buttonFlags & DOWN_FLAG) != 0 ? 1 : (sample.buttonFlags & UP_FLAG) != 0 ? -1 :
                                                                                              0;
    if (x == 0 && y == 0) {
      if (std::max(std::abs(static_cast<int>(sample.leftStickX)), std::abs(static_cast<int>(sample.leftStickY))) >= input::StreamMenu::WHEEL_DEADZONE) {
        if (std::abs(static_cast<int>(sample.leftStickX)) > std::abs(static_cast<int>(sample.leftStickY))) {
          x = sample.leftStickX > 0 ? 1 : -1;
        } else {
          y = sample.leftStickY > 0 ? -1 : 1;
        }
      }
    }
    if (navigationNeutralRequired_) {
      if (x == 0 && y == 0) {
        navigationNeutralRequired_ = false;
      }
      return;
    }
    const auto command = x > 0 ? input::UiCommand::move_right : x < 0 ? input::UiCommand::move_left :
                                                              y > 0   ? input::UiCommand::move_down :
                                                              y < 0   ? input::UiCommand::move_up :
                                                                        input::UiCommand::none;
    const auto key = menu_navigation_key(controllerNavigation_.update(command, now));
    if (key != SDLK_UNKNOWN) {
      menu_.navigate(key == SDLK_RIGHT ? 1 : key == SDLK_LEFT ? -1 :
                                                                0,
                     key == SDLK_DOWN ? 1 : key == SDLK_UP ? -1 :
                                                             0);
      dirty_ = true;
    }
  }

  StreamControls::StreamControls():
      router_(send_packet) {}

  StreamControls::~StreamControls() {
    close();
  }

  void StreamControls::reset(SDL_GameController *controller) {
    close();
    devices_[0].handle = controller;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
      open_if_needed(i);
    }
    poll();
  }

  void StreamControls::configure(std::string screenshotDirectory, bool keyboardTop) {
    screenshotDirectory_ = std::move(screenshotDirectory);
    keyboardTop_ = keyboardTop;
  }

  bool StreamControls::keyboard_docked_top() const {
    return keyboardTop_;
  }

  void StreamControls::close() {
    clear_panel();
    icons_.clear();
    for (auto &device : devices_) {
      if (device.handle != nullptr) {
        SDL_GameControllerClose(device.handle);
      }
      device = {};
    }
  }

  void StreamControls::open_if_needed(int deviceIndex) {
    if (!SDL_IsGameController(deviceIndex)) {
      return;
    }
    const auto instance = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    for (const auto &device : devices_) {
      if (device.handle != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(device.handle)) == instance) {
        return;
      }
    }
    for (auto &device : devices_) {
      if (device.handle == nullptr) {
        device = {};
        device.handle = SDL_GameControllerOpen(deviceIndex);
        if (device.handle != nullptr) {
          logging::info("input", "Opened stream gamepad " + std::to_string(instance));
        }
        return;
      }
    }
  }

  void StreamControls::close_if_removed(int instance) {
    if (pulseSlot_ >= 0) {
      send_packet({input::StreamPacketType::controller, pulseSlot_, router_.active_mask()});
      pulseSlot_ = -1;
    }
    router_.disconnect(instance);
    if (mouseDevice_ == instance) {
      mouseDevice_ = -1;
    }
    if (menu_.owner == instance) {
      menu_.owner = -1;
      armed_ = true;
    }
    for (auto &device : devices_) {
      if (device.handle != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(device.handle)) == instance) {
        SDL_GameControllerClose(device.handle);
        device = {};
      }
    }
    // Recover a fifth device that was waiting when the four slots were occupied.
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
      open_if_needed(i);
    }
  }

  bool StreamControls::poll() {
    bool chord = false;
    for (auto &device : devices_) {
      const bool present = device.handle != nullptr && SDL_GameControllerGetAttached(device.handle) != SDL_FALSE;
      device.sample = present ? sample_controller(device.handle) : input::ControllerState {};
      chord = input::is_stream_exit_requested(device.sample.buttonFlags, present) || chord;
    }
    return chord;
  }

  bool StreamControls::any_button_pressed() const {
    return std::any_of(devices_.begin(), devices_.end(), [](const Device &d) {
      return d.sample.buttonFlags != 0 || d.sample.leftTrigger != 0 || d.sample.rightTrigger != 0;
    });
  }

  SDL_GameController *StreamControls::take_controller_for_shell() {
    poll();
    for (auto &device : devices_) {
      if (device.handle != nullptr && (device.sample.buttonFlags != 0 || device.sample.leftTrigger != 0 || device.sample.rightTrigger != 0)) {
        return std::exchange(device.handle, nullptr);
      }
    }
    for (auto &device : devices_) {
      if (device.handle != nullptr && SDL_GameControllerGetAttached(device.handle) != SDL_FALSE) {
        return std::exchange(device.handle, nullptr);
      }
    }
    return nullptr;
  }

  std::vector<std::string> StreamControls::diagnostic_lines() const {
    std::vector<std::string> lines {"STREAM MENU: Back + Black + White"};
    for (const auto &device : devices_) {
      if (device.handle != nullptr) {
        lines.push_back("Pad " + std::to_string(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(device.handle))) + " buttons=" + std::to_string(device.sample.buttonFlags));
      }
    }
    return lines;
  }

  bool StreamControls::visible() const {
    return menu_.page != input::StreamMenuPage::closed;
  }

  void StreamControls::open_menu(int device) {
    if (menu_.page == input::StreamMenuPage::radial) {
      return;
    }
    menu_.open(device);
    keyboardNavigation_.reset();
    controllerNavigation_.reset();
    navigationNeutralRequired_ = false;
    keyboardPageTriggerDown_ = false;
    keyboardDockTriggerDown_ = false;
    inputDevice_ = device < 0 ? input::UiInputDevice::keyboard : input::UiInputDevice::gamepad;
    router_.set_captured(true);
    armed_ = false;
    dirty_ = true;
    message_.clear();
    logging::info("input", "Opened stream menu from " + (device < 0 ? std::string("keyboard") : "gamepad " + std::to_string(device)));
  }

  void StreamControls::tap_key(int code) {
    if (code == 0) {
      return;
    }
    const int keyModifiers = menu_.key_modifiers(code);
    // No gameplay sample is forwarded during this event-thread-only local key tap.
    router_.set_captured(false);
    constexpr std::array<int, 4> modifiers {MODIFIER_SHIFT, MODIFIER_CTRL, MODIFIER_ALT, MODIFIER_META};
    constexpr std::array<int, 4> keys {0xA0, 0xA2, 0xA4, 0x15B};
    for (std::size_t i = 0; i < modifiers.size(); ++i) {
      if ((keyModifiers & modifiers[i]) != 0) {
        router_.key(-3, keys[i], true);
      }
    }
    router_.key(-3, code, true, keyModifiers);
    router_.key(-3, code, false, keyModifiers);
    router_.release_source(-3);
    router_.set_captured(true);
    menu_.key_sent();
    dirty_ = true;
  }

  bool StreamControls::menu_key(SDL_Keycode key, FfmpegStreamBackend &backend, bool active, bool &stats) {
    if (menu_.page == input::StreamMenuPage::quit_confirmation) {
      if (key == SDLK_RETURN) {
        return menu_.confirm_disconnect(true);
      }
      if (key == SDLK_ESCAPE) {
        dirty_ = true;
        return menu_.confirm_disconnect(false);
      }
      return false;
    }
    dirty_ = true;
    if (key == SDLK_ESCAPE) {
      if (menu_.page == input::StreamMenuPage::radial) {
        menu_.close();
      } else {
        menu_.page = input::StreamMenuPage::radial;
        menu_.modifiers = 0;
        menu_.shiftOnce = false;
      }
      return false;
    }
    if (menu_.page == input::StreamMenuPage::keyboard) {
      if (key == SDLK_TAB) {
        menu_.toggle_keyboard_page();
        return false;
      }
      if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) {
        menu_.toggle_shift();
        return false;
      }
      if (key == SDLK_CAPSLOCK) {
        menu_.caps = !menu_.caps;
        return false;
      }
      if (key == SDLK_F2) {
        keyboardTop_ = !keyboardTop_;
        return false;
      }
      if (key == SDLK_BACKSPACE || key == SDLK_SPACE) {
        if (active) {
          tap_key(key == SDLK_SPACE ? 0x20 : 0x08);
        }
        return false;
      }
    }
    if (key != SDLK_RETURN && key != SDLK_SPACE) {
      if (key == SDLK_TAB && menu_.page == input::StreamMenuPage::radial) {
        menu_.selected = menu_.selected == input::StreamAction::none ? input::StreamAction::keyboard : static_cast<input::StreamAction>((static_cast<int>(menu_.selected) + 1) % 8);
      } else {
        menu_.navigate(key == SDLK_RIGHT ? 1 : key == SDLK_LEFT ? -1 :
                                                                  0,
                       key == SDLK_DOWN ? 1 : key == SDLK_UP ? -1 :
                                                               0);
      }
      return false;
    }
    if (menu_.page == input::StreamMenuPage::keyboard) {
      if (active) {
        tap_key(menu_.keyboard_key());
      }
      return false;
    }
    if (menu_.page != input::StreamMenuPage::radial) {
      return false;
    }
    switch (menu_.selected) {
      case input::StreamAction::quit:
        menu_.page = input::StreamMenuPage::quit_confirmation;
        break;
      case input::StreamAction::keyboard:
        menu_.page = input::StreamMenuPage::keyboard;
        controllerNavigation_.reset();
        navigationNeutralRequired_ = true;
        break;
      case input::StreamAction::logs:
        menu_.page = input::StreamMenuPage::logs;
        controllerNavigation_.reset();
        navigationNeutralRequired_ = true;
        logs_.clear();
        for (const auto &entry : logging::snapshot()) {
          logs_.push_back(logging::format_entry(entry));
        }
        if (logs_.empty()) {
          logs_ = logging::load_log_file().lines;
        }
        break;
      case input::StreamAction::mouse:
        if (mouseDevice_ < 0 && menu_.owner < 0) {
          message_ = "Open the menu with a controller to enable mouse mode.";
          break;
        }
        mouseDevice_ = mouseDevice_ < 0 ? menu_.owner : -1;
        router_.set_mouse_device(mouseDevice_);
        menu_.close();
        break;
      case input::StreamAction::statistics:
        stats = !stats;
        menu_.close();
        break;
      case input::StreamAction::screenshot:
        {
          const std::string path = startup::screenshot_storage_path(screenshotDirectory_, "screenshot-" + std::to_string(SDL_GetTicks()) + ".bmp");
          message_ = backend.save_screenshot(path) ? "Saved " + path : "Screenshot failed; check the logs.";
          logging::info("stream", message_);
          break;
        }
      case input::StreamAction::guide:
      case input::StreamAction::share:
        {
          const int slot = active ? router_.connect(menu_.owner) : -1;
          if (slot < 0) {
            message_ = "A connected controller and active stream are required.";
            break;
          }
          if (pulseSlot_ >= 0) {
            send_packet({input::StreamPacketType::controller, pulseSlot_, router_.active_mask()});
          }
          pulseSlot_ = slot;
          pulseUntil_ = SDL_GetTicks() + 120;
          input::StreamPacket packet {input::StreamPacketType::controller, slot, router_.active_mask()};
          packet.controller.buttonFlags = menu_.selected == input::StreamAction::guide ? SPECIAL_FLAG : MISC_FLAG;
          send_packet(packet);
          message_ = menu_.selected == input::StreamAction::guide ? "Guide sent" : "Share sent (requires host support)";
          break;
        }
      case input::StreamAction::none:
        break;
    }
    return false;
  }

  bool StreamControls::pump(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height, bool active, bool &statisticsEnabled) {
    // Sampling and transport enqueueing share one owner so capture cannot race a stale gameplay packet.
    SDL_PumpEvents();
    bool stop = false;
    const bool wasVisible = visible();
    SDL_Event event {};
    const Uint32 eventStart = SDL_GetTicks();
    for (int count = 0; count < 128 && (count == 0 || SDL_GetTicks() - eventStart < 4) && SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0; ++count) {
      const bool triggerAxis = event.type == SDL_CONTROLLERAXISMOTION && (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
      const bool gamepadActivity = event.type == SDL_CONTROLLERBUTTONDOWN || (event.type == SDL_CONTROLLERAXISMOTION && (triggerAxis ? event.caxis.value >= 16000 : std::abs(static_cast<int>(event.caxis.value)) >= input::StreamMenu::WHEEL_DEADZONE));
      if (visible() && gamepadActivity && menu_.owner < 0) {
        menu_.owner = event.type == SDL_CONTROLLERBUTTONDOWN ? event.cbutton.which : event.caxis.which;
      }
      if (visible() && gamepadActivity && inputDevice_ != input::UiInputDevice::gamepad) {
        inputDevice_ = input::UiInputDevice::gamepad;
        keyboardNavigation_.reset();
        dirty_ = true;
      }
      if (visible() && menu_.page == input::StreamMenuPage::keyboard && event.type == SDL_CONTROLLERAXISMOTION && event.caxis.which == menu_.owner) {
        const bool pressed = event.caxis.value >= 16000;
        if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
          if (pressed && !keyboardPageTriggerDown_ && armed_) {
            menu_.toggle_keyboard_page();
            dirty_ = true;
          }
          keyboardPageTriggerDown_ = pressed;
        } else if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
          if (pressed && !keyboardDockTriggerDown_ && armed_) {
            keyboardTop_ = !keyboardTop_;
            dirty_ = true;
          }
          keyboardDockTriggerDown_ = pressed;
        }
      }
      if (event.type == SDL_CONTROLLERDEVICEADDED) {
        open_if_needed(event.cdevice.which);
      } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        close_if_removed(event.cdevice.which);
      } else if (event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP) {
        for (auto &device : devices_) {
          if (device.handle == nullptr || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(device.handle)) != event.jbutton.which) {
            continue;
          }
          const int flag = joystick_chord_flag(device.handle, event.jbutton.button);
          if (flag != 0) {
            const bool down = device.chord.update_button(flag, event.jbutton.state == SDL_PRESSED);
            if (down && !device.chordDown) {
              open_menu(event.jbutton.which);
            }
            device.chordDown = down;
          }
        }
      } else if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
        const auto &button = event.cbutton;
        for (auto &device : devices_) {
          if (device.handle == nullptr || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(device.handle)) != button.which) {
            continue;
          }
          const int flag = controller_flag(button.button);
          const bool down = device.chord.update_button(flag, button.state == SDL_PRESSED);
          if (down && !device.chordDown) {
            open_menu(button.which);
          }
          device.chordDown = down;
          if (button.state == SDL_PRESSED) {
            device.sample.buttonFlags |= flag;
          } else {
            device.sample.buttonFlags &= ~flag;
          }
          if (active) {
            router_.connect(button.which);
            router_.controller(button.which, device.sample);
          }
        }
        if (visible() && armed_ && button.state == SDL_RELEASED && button.which == menu_.owner && menu_.page != input::StreamMenuPage::radial) {
          for (const auto &device : devices_) {
            if (device.handle != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(device.handle)) == button.which) {
              navigate_controller(device.sample, SDL_GetTicks());
            }
          }
        }
        if (visible() && armed_ && button.state == SDL_PRESSED) {
          if (menu_.owner < 0) {
            menu_.owner = button.which;
          }
          if (button.which != menu_.owner) {
            continue;
          }
          switch (button.button) {
            case SDL_CONTROLLER_BUTTON_A:
              stop = menu_key(SDLK_RETURN, backend, active, statisticsEnabled) || stop;
              break;
            case SDL_CONTROLLER_BUTTON_B:
              stop = menu_key(SDLK_ESCAPE, backend, active, statisticsEnabled) || stop;
              break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
              if (menu_.page == input::StreamMenuPage::radial) {
                menu_key(button.button == SDL_CONTROLLER_BUTTON_DPAD_UP ? SDLK_UP : button.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? SDLK_DOWN :
                                                                                  button.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT   ? SDLK_LEFT :
                                                                                                                                       SDLK_RIGHT,
                         backend,
                         active,
                         statisticsEnabled);
              } else {
                for (const auto &device : devices_) {
                  if (device.handle != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(device.handle)) == button.which) {
                    navigate_controller(device.sample, SDL_GetTicks());
                  }
                }
              }
              break;
            case SDL_CONTROLLER_BUTTON_X:
              if (active && menu_.page == input::StreamMenuPage::keyboard) {
                tap_key(0x08);
              }
              break;
            case SDL_CONTROLLER_BUTTON_Y:
              if (active && menu_.page == input::StreamMenuPage::keyboard) {
                tap_key(0x20);
              }
              break;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK:
              if (menu_.page == input::StreamMenuPage::keyboard) {
                menu_.caps = !menu_.caps;
                dirty_ = true;
              }
              break;
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
              if (menu_.page == input::StreamMenuPage::keyboard) {
                menu_.toggle_shift();
                dirty_ = true;
              }
              break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
              if (active && menu_.page == input::StreamMenuPage::keyboard) {
                tap_key(button.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ? 0x125 : 0x127);
              }
              break;
            case SDL_CONTROLLER_BUTTON_START:
              if (active && menu_.page == input::StreamMenuPage::keyboard) {
                tap_key(0x0D);
              }
              break;
            default:
              break;
          }
        }
      } else if (event.type == SDL_CONTROLLERAXISMOTION && visible() && armed_ && event.caxis.which == menu_.owner) {
        for (auto &device : devices_) {
          if (device.handle == nullptr || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(device.handle)) != event.caxis.which) {
            continue;
          }
          // Preserve the event's deflection even when a queued center event follows it.
          auto &sample = device.sample;
          switch (event.caxis.axis) {
            case SDL_CONTROLLER_AXIS_LEFTX:
              sample.leftStickX = event.caxis.value;
              break;
            case SDL_CONTROLLER_AXIS_LEFTY:
              sample.leftStickY = input::invert_stick_axis(event.caxis.value);
              break;
            case SDL_CONTROLLER_AXIS_RIGHTX:
              sample.rightStickX = event.caxis.value;
              break;
            case SDL_CONTROLLER_AXIS_RIGHTY:
              sample.rightStickY = input::invert_stick_axis(event.caxis.value);
              break;
            default:
              break;
          }
          if (event.caxis.axis <= SDL_CONTROLLER_AXIS_RIGHTY) {
            if (menu_.page == input::StreamMenuPage::radial) {
              dirty_ = menu_.sample_sticks(sample.leftStickX, sample.leftStickY, sample.rightStickX, sample.rightStickY) || dirty_;
            } else {
              navigate_controller(sample, SDL_GetTicks());
            }
          }
        }
      } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        const bool down = event.type == SDL_KEYDOWN;
        const auto &key = event.key;
        const bool shortcut = key.keysym.scancode == SDL_SCANCODE_Q && (key.keysym.mod & KMOD_CTRL) != 0 && (key.keysym.mod & KMOD_ALT) != 0 && (key.keysym.mod & KMOD_SHIFT) != 0;
        if (down && key.repeat == 0 && shortcut) {
          open_menu(-1);
          armed_ = true;
        } else if (visible()) {
          if (!down) {
            keyboardNavigation_.release(key.keysym.scancode);
          }
          if (down && key.repeat == 0) {
            if (inputDevice_ != input::UiInputDevice::keyboard) {
              inputDevice_ = input::UiInputDevice::keyboard;
              dirty_ = true;
            }
            const auto command = menu_navigation_command(key.keysym.sym);
            if (command != input::UiCommand::none) {
              if (keyboardNavigation_.press(key.keysym.scancode, command, SDL_GetTicks()) != input::UiCommand::none) {
                menu_key(key.keysym.sym, backend, active, statisticsEnabled);
              }
            } else {
              if (key.keysym.sym == SDLK_RETURN || key.keysym.sym == SDLK_SPACE || key.keysym.sym == SDLK_ESCAPE) {
                keyboardNavigation_.reset();
              }
              stop = menu_key(key.keysym.sym, backend, active, statisticsEnabled) || stop;
            }
          }
        } else if (active && key.repeat == 0) {
          const int mods = ((key.keysym.mod & KMOD_SHIFT) != 0 ? MODIFIER_SHIFT : 0) | ((key.keysym.mod & KMOD_CTRL) != 0 ? MODIFIER_CTRL : 0) | ((key.keysym.mod & KMOD_ALT) != 0 ? MODIFIER_ALT : 0) | ((key.keysym.mod & KMOD_GUI) != 0 ? MODIFIER_META : 0);
          router_.key(0, input::keyboard_virtual_key(key.keysym.scancode), down, mods);
        }
      } else if (active && event.type == SDL_MOUSEMOTION) {
        router_.mouse_move(event.motion.xrel, event.motion.yrel);
      } else if (active && (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)) {
        const int button = event.button.button;
        const int hostButton = button == SDL_BUTTON_LEFT ? BUTTON_LEFT : button == SDL_BUTTON_RIGHT ? BUTTON_RIGHT :
                                                                       button == SDL_BUTTON_MIDDLE  ? BUTTON_MIDDLE :
                                                                       button == SDL_BUTTON_X1      ? BUTTON_X1 :
                                                                       button == SDL_BUTTON_X2      ? BUTTON_X2 :
                                                                                                      0;
        router_.mouse_button(1, hostButton, event.type == SDL_MOUSEBUTTONDOWN);
      } else if (active && event.type == SDL_MOUSEWHEEL) {
        const int direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
        router_.scroll(event.wheel.x * direction, event.wheel.y * direction);
      } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        focused_ = false;
        keyboardNavigation_.reset();
        router_.set_captured(true);
      } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
        focused_ = true;
        router_.set_captured(visible());
      } else if (event.type == SDL_QUIT) {
        stop = true;
      }
    }
    if (stop) {
      router_.set_captured(true);
      return true;
    }
    poll();
    const Uint32 now = SDL_GetTicks();
    if (menu_.page == input::StreamMenuPage::logs && now - logRefreshTick_ >= 1000) {
      logRefreshTick_ = now;
      std::vector<std::string> lines;
      for (const auto &entry : logging::snapshot()) {
        lines.push_back(logging::format_entry(entry));
      }
      if (!lines.empty() && lines != logs_) {
        // Preserve the same entries when reading older output; offset zero follows live output.
        if (menu_.logOffset > 0) {
          menu_.logOffset += std::max(0, static_cast<int>(lines.size()) - static_cast<int>(logs_.size()));
        }
        logs_ = std::move(lines);
        dirty_ = true;
      }
    }
    if (visible() && inputDevice_ == input::UiInputDevice::keyboard) {
      const auto repeatedKey = menu_navigation_key(keyboardNavigation_.poll(now));
      if (repeatedKey != SDLK_UNKNOWN) {
        menu_key(repeatedKey, backend, active, statisticsEnabled);
      }
    }
    for (auto &device : devices_) {
      if (device.handle == nullptr) {
        continue;
      }
      const auto instance = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(device.handle));
      const bool present = SDL_GameControllerGetAttached(device.handle) != SDL_FALSE;
      const bool down = device.chord.update(device.sample.buttonFlags, present);
      if (down && !device.chordDown) {
        open_menu(instance);
      }
      device.chordDown = down;
      if (active) {
        if (present) {
          router_.connect(instance);
          router_.controller(instance, device.sample);
        } else {
          router_.disconnect(instance);
        }
      }
      if (!visible() || instance != menu_.owner || inputDevice_ != input::UiInputDevice::gamepad) {
        continue;
      }
      const auto &sample = device.sample;
      if (sample.buttonFlags == 0 && sample.leftTrigger == 0 && sample.rightTrigger == 0) {
        armed_ = true;
      }
      if (!armed_) {
        continue;
      }
      if (menu_.page == input::StreamMenuPage::radial) {
        // Poll on every pump as well as consuming events: Xbox input can change
        // between queued axis events, and a selected item must not freeze polling.
        dirty_ = menu_.sample_sticks(sample.leftStickX, sample.leftStickY, sample.rightStickX, sample.rightStickY) || dirty_;
      } else {
        navigate_controller(sample, now);
      }
    }
    if (pulseSlot_ >= 0 && (!visible() || static_cast<Sint32>(now - pulseUntil_) >= 0 || (router_.active_mask() & (1U << pulseSlot_)) == 0)) {
      send_packet({input::StreamPacketType::controller, pulseSlot_, router_.active_mask()});
      pulseSlot_ = -1;
    }
    if (!visible() && (wasVisible || dirty_)) {
      keyboardNavigation_.reset();
      router_.set_captured(!focused_);
      backend.set_statistics_bitmap(0, 0, {});
      clear_panel();
      dirty_ = false;
    }
    backend.local_overlay_size(width, height);
    if (visible() && (width != menuWidth_ || height != menuHeight_)) {
      dirty_ = true;
      menuWidth_ = width;
      menuHeight_ = height;
    }
    if (visible() && dirty_) {
      render_menu(backend, font, width, height, statisticsEnabled);
    }
    if (active) {
      router_.tick(now - lastTick_);
    }
    lastTick_ = now;
    return stop;
  }

  void StreamControls::release_host_input() {
    router_.release_all();
    if (pulseSlot_ >= 0) {
      send_packet({input::StreamPacketType::controller, pulseSlot_, router_.active_mask()});
      pulseSlot_ = -1;
    }
  }

  bool StreamControls::render_icon(SDL_Renderer *renderer, const char *asset, SDL_Rect bounds) {
    const int size = std::min(bounds.w, bounds.h);
    if (size <= 0) {
      return false;
    }
    const auto key = std::make_pair(std::string(asset), size);
    auto found = icons_.find(key);
    if (found == icons_.end()) {
      const std::string path = ui::build_asset_path(std::string("icons/") + asset);
      std::shared_ptr<SDL_Surface> raster(ui::load_svg_surface(path, size, size), SDL_FreeSurface);
      if (!raster) {
        logging::warn("ui", "Unable to load stream icon " + path + ": " + SDL_GetError());
      }
      found = icons_.emplace(key, std::move(raster)).first;
    }
    if (!found->second) {
      return false;
    }
    const auto &surface = found->second;
    auto texture = iconTextures_.find(key);
    if (texture == iconTextures_.end()) {
      auto loaded = std::shared_ptr<SDL_Texture>(SDL_CreateTextureFromSurface(renderer, surface.get()), SDL_DestroyTexture);
      if (!loaded) {
        return false;
      }
      texture = iconTextures_.emplace(key, std::move(loaded)).first;
    }
    SDL_Rect destination {bounds.x + (bounds.w - surface->w) / 2, bounds.y + (bounds.h - surface->h) / 2, surface->w, surface->h};
    return SDL_RenderCopy(renderer, texture->second.get(), nullptr, &destination) == 0;
  }

  void StreamControls::render_hint(SDL_Renderer *renderer, TTF_Font *font, const char *asset, const char *key, const char *text, SDL_Rect bounds, bool compact) {
    if (bounds.w <= 0 || bounds.h <= 0) {
      return;
    }
    const bool keyboard = inputDevice_ == input::UiInputDevice::keyboard;
    const int size = std::min(bounds.h, compact ? std::max(12, TTF_FontLineSkip(font) * 3 / 4) : TTF_FontLineSkip(font) + 12);
    int textWidth = 0;
    int textHeight = 0;
    TTF_SizeUTF8(font, text, &textWidth, &textHeight);
    const int capWidth = std::min(bounds.w, keyboard ? std::max(size, static_cast<int>(std::char_traits<char>::length(key)) * size / 2 + 8) : size);
    const int groupWidth = std::min(bounds.w, capWidth + 4 + textWidth);
    const int left = bounds.x + (bounds.w - groupWidth) / 2;
    SDL_Rect icon {left, bounds.y + (bounds.h - size) / 2, capWidth, size};
    if (keyboard) {
      SDL_SetRenderDrawColor(renderer, 85, 85, 85, 255);
      SDL_RenderDrawRect(renderer, &icon);
      label(renderer, font, key, {icon.x + 2, icon.y + 2, icon.w - 4, icon.h - 4}, false, false, true);
    } else {
      render_icon(renderer, asset, icon);
    }
    label(renderer, font, text, {left + capWidth + 4, bounds.y, std::max(1, groupWidth - capWidth - 4), bounds.h}, false, false, true);
  }

  void StreamControls::render_keyboard(SDL_Renderer *renderer, TTF_Font *font, int width, int height, int rowHeight) {
    const auto &cells = menu_.keyboard_cells();
    constexpr int padding = 8;
    const int header = TTF_FontLineSkip(font) + 4;
    const auto &focusedCell = cells[menu_.keyIndex];
    std::string description = menu_.keyboardLayout == 0 ? "Keyboard / ABC" : menu_.keyboardLayout == 1 ? "Keyboard / ?123" :
                                                                                                         "Keyboard / PC keys";
    if (focusedCell.key >= 106 && focusedCell.key < static_cast<int>(input::StreamMenu::KEYS.size())) {
      static const std::array<const char *, 12> shortcuts {"Copy: Ctrl+C", "Paste: Ctrl+V", "Cut: Ctrl+X", "Undo: Ctrl+Z", "Redo: Ctrl+Y", "Select all: Ctrl+A", "Switch app: Alt+Tab", "Close app: Alt+F4", "Ctrl+Alt+Delete", "Desktop: Win+D", "Explorer: Win+E", "Run: Win+R"};
      description = shortcuts[focusedCell.key - 106];
    }
    label(renderer, font, description, {padding, 2, width - padding * 2, header}, false, false, true);
    const int gridWidth = width - padding * 2;
    const bool keyboardInput = inputDevice_ == input::UiInputDevice::keyboard;
    for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
      const auto &cell = cells[i];
      const int left = padding + cell.column * gridWidth / input::StreamMenu::KEYBOARD_COLUMNS;
      const int right = padding + (cell.column + cell.width) * gridWidth / input::StreamMenu::KEYBOARD_COLUMNS;
      SDL_Rect key {left + 1, padding + header + cell.row * rowHeight + 1, right - left - 3, cell.height * rowHeight - 3};
      const bool modifier = cell.key < input::StreamMenu::KEYBOARD_PAGE_KEY && input::StreamMenu::KEYS[cell.key].code < 0;
      const bool latched = cell.key == input::StreamMenu::KEYBOARD_CAPS_KEY ? menu_.caps : cell.key == 40 ? menu_.shiftOnce :
                                                                                                            modifier && (menu_.modifiers & -input::StreamMenu::KEYS[cell.key].code) != 0;
      const int gray = i == menu_.keyIndex ? 54 : latched      ? 65 :
                                                cell.width > 2 ? 42 :
                                                                 30;
      SDL_SetRenderDrawColor(renderer, gray, gray, gray, 255);
      SDL_RenderFillRect(renderer, &key);
      std::string text = modifier ? input::StreamMenu::KEYS[cell.key].label : menu_.key_label(i);

      const char *shortcutIcon = nullptr;
      const char *shortcutKey = "";
      switch (cell.key) {
        case input::StreamMenu::KEYBOARD_PAGE_KEY:
          shortcutIcon = "button-lt.svg";
          shortcutKey = "Tab";
          break;
        case 47:
          shortcutIcon = "button-x.svg";
          text = "Bksp";
          break;
        case 57:
          shortcutIcon = "button-lb.svg";
          text = "<";
          break;
        case 59:
          shortcutIcon = "button-rb.svg";
          text = ">";
          break;
        case input::StreamMenu::KEYBOARD_CAPS_KEY:
          shortcutIcon = "button-left-stick.svg";
          shortcutKey = "Caps";
          break;
        case 44:
          shortcutIcon = "button-y.svg";
          break;
        case 46:
          shortcutIcon = "button-start.svg";
          text = "Enter";
          break;
        default:
          break;
      }
      if (cell.width > 2 && shortcutIcon != nullptr && (!keyboardInput || *shortcutKey != '\0')) {
        if (cell.height > 1) {
          label(renderer, font, text, {key.x + 2, key.y + 2, key.w - 4, key.h / 2}, false, false, true);
          render_hint(renderer, font, shortcutIcon, shortcutKey, "", {key.x + 2, key.y + key.h / 2, key.w - 4, key.h / 2 - 2}, cell.key != input::StreamMenu::KEYBOARD_CAPS_KEY && cell.key != 46);
        } else {
          render_hint(renderer, font, shortcutIcon, shortcutKey, text.c_str(), {key.x + 2, key.y + 2, key.w - 4, key.h - 4}, true);
        }
      } else {
        label(renderer, font, text, {key.x + 2, key.y + 2, key.w - 4, key.h - 4}, false, false, true);
      }
      if (i == menu_.keyIndex) {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect focus {key.x - 1, key.y - 1, key.w + 2, key.h + 2};
        SDL_RenderDrawRect(renderer, &focus);
        focus = {key.x - 2, key.y - 2, key.w + 4, key.h + 4};
        SDL_RenderDrawRect(renderer, &focus);
      }
    }
    const int line = TTF_FontLineSkip(font) + 8;
    const int footerY = padding + header + input::StreamMenu::KEYBOARD_ROWS * rowHeight + 4;
    const int footerHeight = std::min(line + 4, height - footerY);
    const std::array<const char *, 4> assets {"button-a.svg", "button-b.svg", "button-right-stick.svg", "button-rt.svg"};
    const std::array<const char *, 4> keys {"Enter", "Esc", "Shift", "F2"};
    const std::array<const char *, 4> actions {"Select", "Back", menu_.shiftOnce ? "Shift: On" : "Shift", keyboardTop_ ? "Move down" : "Move up"};
    for (int i = 0; i < 4; ++i) {
      render_hint(renderer, font, assets[i], keys[i], actions[i], {i * width / 4 + 4, footerY, width / 4 - 8, footerHeight});
    }
  }

  void StreamControls::render_menu(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height, bool stats) {
    if (font == nullptr) {
      return;
    }
    const bool keyboard = menu_.page == input::StreamMenuPage::keyboard;
    const int rowHeight = std::max(TTF_FontLineSkip(font) + 8, width * 9 / 200);
    const int safeBottom = std::max(8, height / 40);
    width = keyboard ? width * 9 / 10 : width;
    height = keyboard ? std::min(height - VideoOverlay::MARGIN * 2, rowHeight * input::StreamMenu::KEYBOARD_ROWS + 28 + TTF_FontLineSkip(font) + (TTF_FontLineSkip(font) + 8) + safeBottom) : height;
    if (width <= 0 || height <= 0 || font == nullptr) {
      return;
    }
    if (!panel_ || panel_->w != width || panel_->h != height) {
      clear_panel();
      panel_.reset(SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888));
      if (panel_) {
        panelRenderer_.reset(SDL_CreateSoftwareRenderer(panel_.get()));
      }
    }
    if (!panelRenderer_) {
      return;
    }
    if (text_.size() > 256) {
      text_.clear();
      textBytes_ = 0;
    }
    auto &panel = panel_;
    auto &renderer = panelRenderer_;
    const Uint8 background = keyboard ? 16 : 0;
    SDL_SetRenderDrawColor(renderer.get(), background, background, background, keyboard ? 255 : 150);
    SDL_RenderClear(renderer.get());
    const int line = TTF_FontLineSkip(font) + 8;
    if (menu_.page == input::StreamMenuPage::radial) {
      label(renderer.get(), font, "Stream controls", {8, 4, width - 16, line}, false, false);
    }
    if (keyboard) {
      render_keyboard(renderer.get(), font, width, height, rowHeight);
    } else if (menu_.page == input::StreamMenuPage::radial) {
      const int cy = height / 2;
      const int radius = std::min(width / 5, (height - line * 4) / 2);
      for (int wheel = 0; wheel < 2; ++wheel) {
        const int cx = width * (wheel == 0 ? 1 : 3) / 4;
        SDL_RenderFlush(renderer.get());
        rasterize_menu_wheel(static_cast<std::uint8_t *>(panel->pixels), static_cast<std::size_t>(panel->pitch) * height, panel->pitch, width, height, cx, cy, radius, static_cast<int>(menu_.selected) / 4 == wheel ? static_cast<int>(menu_.selected) % 4 : -1);
        render_icon(renderer.get(), wheel == 0 ? "button-left-stick.svg" : "button-right-stick.svg", {cx - line, cy - line, line * 2, line * 2});
        constexpr std::array<int, 4> dx {0, 1, 0, -1};
        constexpr std::array<int, 4> dy {-1, 0, 1, 0};
        for (int sector = 0; sector < 4; ++sector) {
          const auto action = static_cast<input::StreamAction>(wheel * 4 + sector);
          const int labelWidth = std::min(line * 6, width / 5);
          const int x = std::clamp(cx + dx[sector] * radius * 2 / 3 - labelWidth / 2, 0, width - labelWidth);
          const int iconSize = std::max(18, line);
          const int groupY = cy + dy[sector] * radius * 2 / 3 - (iconSize + line * 2) / 2;
          constexpr std::array<const char *, 8> icons {"keyboard.svg", "mouse.svg", "scroll-text.svg", "power.svg", "button-guide.svg", "button-share.svg", "camera.svg", "chart-no-axes-combined.svg"};
          render_icon(renderer.get(), icons[wheel * 4 + sector], {x + (labelWidth - iconSize) / 2, groupY, iconSize, iconSize});
          label(renderer.get(), font, input::stream_action_label(action, mouseDevice_ >= 0, stats), {x, groupY + iconSize + 2, labelWidth, line * 2}, false, false);
        }
      }
      const bool keyboardHints = inputDevice_ == input::UiInputDevice::keyboard;
      const std::array<const char *, 4> assets {"button-left-stick.svg", "button-right-stick.svg", "button-a.svg", "button-b.svg"};
      const std::array<const char *, 4> keys {"Arrows", "Tab", "Enter", "Esc"};
      const std::array<const char *, 4> actions {keyboardHints ? "Select" : "Left wheel", keyboardHints ? "Next" : "Right wheel", "Choose", "Resume"};
      for (int i = 0; i < 4; ++i) {
        render_hint(renderer.get(), font, assets[i], keys[i], actions[i], {i * width / 4 + 4, height - line * 2, width / 4 - 8, line + 4});
      }
    } else if (menu_.page == input::StreamMenuPage::logs) {
      const SDL_Rect card {width / 8, line / 2, width * 7 / 8 - line / 2, height - line};
      ui::draw_dialog_surface(renderer.get(), card);
      label(renderer.get(), font, "Stream logs", {card.x + 20, card.y + 28, card.w - 40, line}, false, false);
      const int row = line * 2;
      const int count = std::max(1, (card.h - line * 4) / row);
      menu_.logOffset = std::min(menu_.logOffset, std::max(0, static_cast<int>(logs_.size()) - count));
      const int end = static_cast<int>(logs_.size()) - menu_.logOffset;
      const int start = std::max(0, end - count);
      for (int i = start; i < end; ++i) {
        const auto &text = logs_[i];
        const SDL_Color color = text.find("[ERROR]") != std::string::npos || text.find("[FATAL]") != std::string::npos ? SDL_Color {255, 125, 125, 255} : text.find("[WARN") != std::string::npos ? SDL_Color {255, 211, 115, 255} :
                                                                                                                                                                                                    SDL_Color {224, 232, 241, 255};
        label(renderer.get(), font, text, {card.x + 16, card.y + line * 2 + (i - start) * row, card.w - 42, row}, true, false, false, color);
      }
      const int trackHeight = count * row;
      const int thumbHeight = std::max(8, trackHeight * count / std::max(count, static_cast<int>(logs_.size())));
      SDL_Rect thumb {card.x + card.w - 12, card.y + line * 2 + (trackHeight - thumbHeight) * start / std::max(1, static_cast<int>(logs_.size()) - count), 3, thumbHeight};
      SDL_SetRenderDrawColor(renderer.get(), 130, 180, 255, 255);
      SDL_RenderFillRect(renderer.get(), &thumb);
      if (logs_.empty()) {
        label(renderer.get(), font, "No retained log entries.", {card.x + 16, card.y + line * 2, card.w - 32, line}, false, false);
      }
      render_hint(renderer.get(), font, "button-left-stick.svg", "Arrows", menu_.logOffset == 0 ? "Live / scroll" : "Scroll", {card.x + 8, card.y + card.h - line - 12, card.w / 2 - 16, line});
      render_hint(renderer.get(), font, "button-b.svg", "Esc", "Back", {card.x + card.w / 2, card.y + card.h - line - 12, card.w / 2 - 16, line});
    } else {
      const int cardWidth = std::min(width - 32, std::max(380, width * 3 / 5));
      const int cardHeight = line * 7;
      const SDL_Rect card {(width - cardWidth) / 2, (height - cardHeight) / 2, cardWidth, cardHeight};
      ui::draw_dialog_surface(renderer.get(), card);
      label(renderer.get(), font, "Disconnect from stream?", {card.x + 20, card.y + 28, card.w - 40, line}, false, false);
      label(renderer.get(), font, "The app will keep running on your PC.", {card.x + 20, card.y + 28 + line, card.w - 40, line * 2}, false, false);
      for (int i = 0; i < 2; ++i) {
        SDL_Rect button {card.x + 20 + i * (card.w - 32) / 2, card.y + card.h - line * 2, (card.w - 48) / 2, line + 12};
        ui::draw_dialog_round_rect(renderer.get(), button, {28, 40, 61, 255}, 6);
        render_hint(renderer.get(), font, i == 0 ? "button-b.svg" : "button-a.svg", i == 0 ? "Esc" : "Enter", i == 0 ? "Cancel" : "Disconnect", {button.x + 4, button.y, button.w - 8, button.h});
      }
    }
    if (!message_.empty() && menu_.page == input::StreamMenuPage::radial) {
      label(renderer.get(), font, message_, {4, line + 4, width - 8, line * 2}, false, false);
    }
    SDL_RenderPresent(renderer.get());
    auto pixels = extract_overlay_bitmap(static_cast<const std::uint8_t *>(panel->pixels), static_cast<std::size_t>(panel->pitch) * height, panel->pitch, width, height, keyboard);
    backend.set_color_overlay(width, height, std::move(pixels), keyboard ? (keyboardTop_ ? VideoOverlayPlacement::dock_top : VideoOverlayPlacement::dock_bottom) : VideoOverlayPlacement::full_screen);
    dirty_ = false;
  }
}  // namespace streaming
