/**
 * @file src/streaming/stream_controls.cpp
 * @brief Implements dual-stick stream controls and SDL-to-Moonlight input routing.
 */
// class header include
#include "src/streaming/stream_controls.h"

// standard includes
#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <utility>

// lib includes
#include <Limelight.h>

// local includes
#include "src/logging/log_file.h"
#include "src/logging/logger.h"
#include "src/streaming/ffmpeg_stream_backend.h"
#include "src/ui/asset_path.h"
#include "src/ui/brand_colors.h"
#include "src/ui/dialog_surface.h"
#include "src/ui/sdl_render.h"
#include "src/ui/shell_layout.h"
#include "src/ui/svg_assets.h"

#ifdef SUNSHINE_DESKTOP
  #include "src/ui/desktop_window.h"
#endif

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
      case SDL_GAMEPAD_BUTTON_SOUTH:
        return A_FLAG;
      case SDL_GAMEPAD_BUTTON_EAST:
        return B_FLAG;
      case SDL_GAMEPAD_BUTTON_WEST:
        return X_FLAG;
      case SDL_GAMEPAD_BUTTON_NORTH:
        return Y_FLAG;
      case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return UP_FLAG;
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return DOWN_FLAG;
      case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return LEFT_FLAG;
      case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return RIGHT_FLAG;
      case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return LB_FLAG;
      case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return RB_FLAG;
      case SDL_GAMEPAD_BUTTON_START:
        return PLAY_FLAG;
      case SDL_GAMEPAD_BUTTON_BACK:
        return BACK_FLAG;
      case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return LS_CLK_FLAG;
      case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return RS_CLK_FLAG;
      case SDL_GAMEPAD_BUTTON_GUIDE:
        return SPECIAL_FLAG;
#ifdef SUNSHINE_NATIVE_CLIENT
      case SDL_GAMEPAD_BUTTON_MISC1:
        return MISC_FLAG;
      case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return TOUCHPAD_FLAG;
      case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
        return PADDLE1_FLAG;
      case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
        return PADDLE2_FLAG;
      case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
        return PADDLE3_FLAG;
      case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
        return PADDLE4_FLAG;
#endif
      default:
        return 0;
    }
  }

#ifdef SUNSHINE_NATIVE_CLIENT
  /**
   * @brief Translate SDL battery state and send it to the host.
   * @param slot Host controller slot.
   * @param state SDL battery power state.
   * @param percent State of charge, or a negative value if unknown.
   */
  void send_gamepad_battery(int slot, SDL_PowerState state, int percent) {
    std::uint8_t status = LI_BATTERY_STATE_UNKNOWN;
    switch (state) {
      case SDL_POWERSTATE_ON_BATTERY:
        status = LI_BATTERY_STATE_DISCHARGING;
        break;
      case SDL_POWERSTATE_NO_BATTERY:
        status = LI_BATTERY_STATE_NOT_PRESENT;
        break;
      case SDL_POWERSTATE_CHARGING:
        status = LI_BATTERY_STATE_CHARGING;
        break;
      case SDL_POWERSTATE_CHARGED:
        status = LI_BATTERY_STATE_FULL;
        break;
      default:
        break;
    }
    LiSendControllerBatteryEvent(slot, status, percent < 0 ? LI_BATTERY_PERCENTAGE_UNKNOWN : std::min(percent, 100));
  }

  /**
   * @brief Select the protocol controller family from SDL's detected type.
   * @param pad Connected SDL gamepad.
   * @return Host controller family.
   */
  std::uint8_t controller_type(SDL_Gamepad *pad) {
    switch (SDL_GetRealGamepadType(pad)) {
      case SDL_GAMEPAD_TYPE_XBOX360:
      case SDL_GAMEPAD_TYPE_XBOXONE:
        return LI_CTYPE_XBOX;
      case SDL_GAMEPAD_TYPE_PS3:
      case SDL_GAMEPAD_TYPE_PS4:
      case SDL_GAMEPAD_TYPE_PS5:
        return LI_CTYPE_PS;
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return LI_CTYPE_NINTENDO;
      case SDL_GAMEPAD_TYPE_STEAM:
        return LI_CTYPE_STEAM;
      default:
        return LI_CTYPE_UNKNOWN;
    }
  }

  /**
   * @brief Report the buttons and feedback available on a mapped SDL gamepad.
   * @param packet Arrival packet with the stable slot and active mask.
   * @param pad Local gamepad assigned to the slot.
   */
  void send_gamepad_arrival(const input::StreamPacket &packet, SDL_Gamepad *pad) {
    std::uint32_t buttons = 0;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
      if (SDL_GamepadHasButton(pad, static_cast<SDL_GamepadButton>(button))) {
        buttons |= controller_flag(button);
      }
    }
    const auto properties = SDL_GetGamepadProperties(pad);
    const auto touchpads = SDL_GetNumGamepadTouchpads(pad);
    const auto capabilities = LI_CCAP_ANALOG_TRIGGERS |
                              (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? LI_CCAP_RUMBLE : 0) |
                              (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false) ? LI_CCAP_TRIGGER_RUMBLE : 0) |
                              (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false) ? LI_CCAP_RGB_LED : 0) |
                              (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) ? LI_CCAP_ACCEL : 0) |
                              (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) ? LI_CCAP_GYRO : 0) |
                              (touchpads > 0 ? LI_CCAP_TOUCHPAD : 0) | (touchpads > 1 ? LI_CCAP_DUAL_TOUCHPAD : 0) |
                              LI_CCAP_BATTERY_STATE;
    LiSendControllerArrivalEvent(packet.slot, packet.mask, controller_type(pad), buttons, capabilities);
    int percentage = -1;
    const auto power = SDL_GetGamepadPowerInfo(pad, &percentage);
    send_gamepad_battery(packet.slot, power, percentage);
  }
#endif

  /**
   * @brief Cache the raw buttons bound to the three menu shortcut buttons.
   * @param gamepad Open gamepad with a current SDL mapping.
   * @return Raw button indices and their corresponding menu flags.
   */
  std::vector<std::pair<int, int>> shortcut_bindings(SDL_Gamepad *gamepad) {
    int count = 0;
    const std::unique_ptr<SDL_GamepadBinding *, decltype(&SDL_free)> bindings(SDL_GetGamepadBindings(gamepad, &count), SDL_free);
    std::vector<std::pair<int, int>> buttons;
    for (int i = 0; bindings && i < count; ++i) {
      const auto &binding = *bindings.get()[i];
      if (binding.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON && binding.output_type == SDL_GAMEPAD_BINDTYPE_BUTTON) {
        const auto mapped = binding.output.button;
        if (mapped == SDL_GAMEPAD_BUTTON_BACK || mapped == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || mapped == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
          buttons.emplace_back(binding.input.button, controller_flag(mapped));
        }
      }
    }
    return buttons;
  }

  /**
   * @brief Read an SDL gamepad with saturated Y inversion and raw shortcut bindings.
   * @param handle SDL gamepad reference.
   * @param shortcuts Raw shortcut buttons to sample across UI handoffs.
   * @return Complete host-oriented controller sample.
   */
  input::ControllerState sample_controller(SDL_Gamepad *handle, const std::vector<std::pair<int, int>> &shortcuts) {
    input::ControllerState state;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
      if (SDL_GetGamepadButton(handle, static_cast<SDL_GamepadButton>(button))) {
        state.buttonFlags |= controller_flag(button);
      }
    }
    SDL_Joystick *joystick = SDL_GetGamepadJoystick(handle);
    for (const auto &[button, flags] : shortcuts) {
      if (SDL_GetJoystickButton(joystick, button)) {
        state.buttonFlags |= flags;
      }
    }
    state.leftTrigger = std::clamp(static_cast<int>(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)), 0, 32767) * 255 / 32767;
    state.rightTrigger = std::clamp(static_cast<int>(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)), 0, 32767) * 255 / 32767;
    state.leftStickX = SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_LEFTX);
    state.leftStickY = input::invert_stick_axis(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_LEFTY));
    state.rightStickX = SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_RIGHTX);
    state.rightStickY = input::invert_stick_axis(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_RIGHTY));
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
  StreamControls::EventHandler::EventHandler(StreamControls &controls) noexcept:
      controls_(controls) {}

  void StreamControls::PanelCache::clear() {
    text.clear();
    textBytes = 0;
    iconTextures.clear();
    renderer.reset();
    surface.reset();
    content.reset();
    damage.clear();
  }

  void StreamControls::invalidate_panel(FfmpegStreamBackend &backend) {
    backend.presentation().set_statistics_bitmap(0, 0, {});
    panel_.clear();
    panel_.dirty = true;
  }

  void StreamControls::PanelCache::label(SDL_Renderer *target, TTF_Font *font, const std::string &caption, SDL_Rect bounds, const StreamLabelStyle &style) {
    const auto &[leading, background, fitSingleLine, foreground] = style;
    if (background) {
      SDL_SetRenderDrawColor(target, 16, 16, 16, 255);
      ui::fill_rect(target, bounds);
    }
    if (caption.empty()) {
      return;
    }
    const Uint32 color = static_cast<Uint32>(foreground.r) | (static_cast<Uint32>(foreground.g) << 8) | (static_cast<Uint32>(foreground.b) << 16) | (static_cast<Uint32>(foreground.a) << 24);
    const auto key = std::make_tuple(font, caption, fitSingleLine ? 0 : std::max(1, bounds.w - 4), color);
    auto found = text.find(key);
    if (found == text.end()) {
      std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> rasterSurface(fitSingleLine ? TTF_RenderText_Blended(font, caption.c_str(), 0, foreground) : TTF_RenderText_Blended_Wrapped(font, caption.c_str(), 0, foreground, std::max(1, bounds.w - 4)), &SDL_DestroySurface);
      if (!rasterSurface) {
        return;
      }
      const auto bytes = static_cast<std::size_t>(rasterSurface->w) * rasterSurface->h * 4;
      if (textBytes + bytes > 1024U * 1024U) {
        SDL_FlushRenderer(target);
        text.clear();
        textBytes = 0;
      }
      std::shared_ptr<SDL_Texture> texture(SDL_CreateTextureFromSurface(target, rasterSurface.get()), SDL_DestroyTexture);
      if (!texture) {
        return;
      }
      found = text.try_emplace(key, CachedText {rasterSurface->w, rasterSurface->h, std::move(texture)}).first;
      textBytes += bytes;
    }
    const auto &raster = found->second;
    // Crop the cached raster to the label's bounds.
    auto layout = layout_overlay_label(raster.width, raster.height, {bounds.x, bounds.y, bounds.w, bounds.h}, fitSingleLine);
    if (leading) {
      layout.source.x = 0;
      layout.source.y = 0;
      layout.destination.x = bounds.x;
      layout.destination.y = bounds.y;
    }
    SDL_Rect source {layout.source.x, layout.source.y, layout.source.width, layout.source.height};
    SDL_Rect destination {layout.destination.x, layout.destination.y, layout.destination.width, layout.destination.height};
    ui::render_texture(target, raster.texture.get(), &source, &destination);
  }

  void StreamControls::EventHandler::navigate_controller(const input::ControllerState &sample, Uint32 now) const {
    if (controls_.menu_.page != input::StreamMenuPage::keyboard && controls_.menu_.page != input::StreamMenuPage::logs) {
      controls_.navigation_.controller.reset();
      return;
    }
    int x = (sample.buttonFlags & RIGHT_FLAG) != 0;
    int y = (sample.buttonFlags & DOWN_FLAG) != 0;
    if (x == 0 && (sample.buttonFlags & LEFT_FLAG) != 0) {
      x = -1;
    }
    if (y == 0 && (sample.buttonFlags & UP_FLAG) != 0) {
      y = -1;
    }
    if (controls_.navigation_.neutralRequired) {
      if (x == 0 && y == 0 && std::max(std::abs(static_cast<int>(sample.leftStickX)), std::abs(static_cast<int>(sample.leftStickY))) < input::STICK_NAVIGATION_RELEASE_THRESHOLD) {
        controls_.navigation_.neutralRequired = false;
      }
      return;
    }
    const auto command = input::direction_command(x, y);
    const auto step = x == 0 && y == 0 ? controls_.navigation_.controller.update_stick(sample.leftStickX, sample.leftStickY, now) : controls_.navigation_.controller.update(command, now);
    const auto key = menu_navigation_key(step);
    if (key != SDLK_UNKNOWN && !controls_.navigation_.applied) {
      controls_.navigation_.applied = true;
      const auto [dx, dy] = input::navigation_step(menu_navigation_command(key));
      controls_.menu_.navigate(dx, dy);
      controls_.panel_.dirty = true;
    }
  }

  StreamControls::StreamControls():
      router_([this](const input::StreamPacket &packet) {
#ifdef SUNSHINE_NATIVE_CLIENT
        if (packet.type == input::StreamPacketType::arrival) {
          if (SDL_Gamepad *pad = SDL_GetGamepadFromID(router_.slot_device(packet.slot)); pad != nullptr) {
            send_gamepad_arrival(packet, pad);
            return;
          }
        }
#endif
        auto outgoing = packet;
        if (packet.type == input::StreamPacketType::controller && packet.slot >= 0 && packet.slot < 4) {
          latestHostController_[packet.slot] = packet.controller;
          if (packet.slot == pulseSlot_) {
            outgoing.controller.buttonFlags |= pulseButtons_;
          }
        }
        send_packet(outgoing);
      }) {
  }

#ifdef SUNSHINE_NATIVE_CLIENT
  int StreamControls::gamepad_slot(SDL_JoystickID device) const {
    for (int slot = 0; slot < static_cast<int>(devices_.size()); ++slot) {
      if (router_.slot_device(slot) == device) {
        return slot;
      }
    }
    return -1;
  }

  void StreamControls::set_motion_reporting(unsigned int slot, std::uint8_t motionType, std::uint16_t rate) {
    const auto sensor = motionType == LI_MOTION_TYPE_ACCEL ? SDL_SENSOR_ACCEL : SDL_SENSOR_GYRO;
    const auto index = motionType == LI_MOTION_TYPE_ACCEL ? 0 : 1;
    for (auto &device : devices_) {
      if (device.handle != nullptr && SDL_GetGamepadID(device.handle) == router_.slot_device(slot)) {
        if (SDL_SetGamepadSensorEnabled(device.handle, sensor, rate != 0)) {
          device.motionRate[index] = rate;
          device.lastMotionTimestamp[index] = 0;
        }
        return;
      }
    }
  }

  void StreamControls::set_controller_led(unsigned int slot, std::uint32_t rgb) {
    if (SDL_Gamepad *pad = SDL_GetGamepadFromID(router_.slot_device(slot)); pad != nullptr) {
      SDL_SetGamepadLED(pad, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    }
  }

  void StreamControls::forward_gamepad_event(const SDL_Event &event) {
    switch (event.type) {
      case SDL_EVENT_JOYSTICK_BATTERY_UPDATED:
        if (const int slot = gamepad_slot(event.jbattery.which); slot >= 0) {
          send_gamepad_battery(slot, event.jbattery.state, event.jbattery.percent);
        }
        break;
      case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
      case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
      case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        if (const int slot = gamepad_slot(event.gtouchpad.which); slot >= 0 && event.gtouchpad.touchpad < 2) {
          const auto &touch = event.gtouchpad;
          const auto type = event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN ? LI_TOUCH_EVENT_DOWN :
                                                                            (event.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP ? LI_TOUCH_EVENT_UP : LI_TOUCH_EVENT_MOVE);
          LiSendControllerTouchEvent2(slot, type, touch.touchpad, touch.finger, touch.x, touch.y, touch.pressure);
        }
        break;
      case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        {
          const auto &sensor = event.gsensor;
          if (sensor.sensor != SDL_SENSOR_ACCEL && sensor.sensor != SDL_SENSOR_GYRO) {
            break;
          }
          const int slot = gamepad_slot(sensor.which);
          if (slot < 0) {
            break;
          }
          const int index = sensor.sensor == SDL_SENSOR_ACCEL ? 0 : 1;
          for (auto &device : devices_) {
            if (device.handle == nullptr || SDL_GetGamepadID(device.handle) != sensor.which) {
              continue;
            }
            const auto rate = device.motionRate[index];
            auto &last = device.lastMotionTimestamp[index];
            if (rate != 0 && (last == 0 || sensor.timestamp - last >= SDL_NS_PER_SECOND / rate)) {
              constexpr float radiansToDegrees = 57.295779513f;
              const float scale = index == 0 ? 1.0f : radiansToDegrees;
              LiSendControllerMotionEvent(slot, index == 0 ? LI_MOTION_TYPE_ACCEL : LI_MOTION_TYPE_GYRO, sensor.data[0] * scale, sensor.data[1] * scale, sensor.data[2] * scale);
              last = sensor.timestamp;
            }
            break;
          }
          break;
        }
      default:
        break;
    }
  }

  void StreamControls::rumble(unsigned int slot, std::uint16_t first, std::uint16_t second, bool triggers) {
    const auto device = router_.slot_device(slot);
    if (device < 0) {
      return;
    }
    if (SDL_Gamepad *pad = SDL_GetGamepadFromID(device); pad != nullptr) {
      constexpr Uint32 effectDuration = 30000;
      if (triggers) {
        SDL_RumbleGamepadTriggers(pad, first, second, effectDuration);
      } else {
        SDL_RumbleGamepad(pad, first, second, effectDuration);
      }
    }
  }
#endif

  StreamControls::~StreamControls() {
    close();
  }

  void StreamControls::reset(SDL_Gamepad *controller) {
    close();
    devices_[0].handle = controller;
    if (controller != nullptr) {
      devices_[0].shortcuts = shortcut_bindings(controller);
    }
    open_connected_gamepads();
    poll();
  }

  void StreamControls::configure(std::string screenshotDirectory, bool keyboardTop, app::ScreenshotSource source) {
    screenshotDirectory_ = std::move(screenshotDirectory);
    keyboardTop_ = keyboardTop;
    screenshotSource_ = std::move(source);
  }

  bool StreamControls::keyboard_docked_top() const {
    return keyboardTop_;
  }

  bool StreamControls::quit_app_requested() const {
    return menu_.disconnectAction == input::StreamDisconnectAction::quit_app;
  }

  bool StreamControls::has_overlay() const {
    return visible() || screenshotNotification_.visible;
  }

  void StreamControls::EventHandler::release_pulse() const {
    if (controls_.pulseSlot_ >= 0) {
      input::StreamPacket packet {input::StreamPacketType::controller, controls_.pulseSlot_, controls_.router_.active_mask()};
      packet.controller = controls_.latestHostController_[controls_.pulseSlot_];
      controls_.pulseSlot_ = -1;
      controls_.pulseButtons_ = 0;
      send_packet(packet);
    }
  }

  void StreamControls::close() {
    panel_.clear();
    panel_.icons.clear();
    for (auto &device : devices_) {
      if (device.handle != nullptr) {
#ifdef SUNSHINE_NATIVE_CLIENT
        SDL_RumbleGamepad(device.handle, 0, 0, 0);
        SDL_RumbleGamepadTriggers(device.handle, 0, 0, 0);
        SDL_SetGamepadSensorEnabled(device.handle, SDL_SENSOR_ACCEL, false);
        SDL_SetGamepadSensorEnabled(device.handle, SDL_SENSOR_GYRO, false);
#endif
        SDL_CloseGamepad(device.handle);
      }
      device = {};
    }
  }

  void StreamControls::open_connected_gamepads() {
    int count = 0;
    const std::unique_ptr<SDL_JoystickID, decltype(&SDL_free)> ids(SDL_GetGamepads(&count), &SDL_free);
    for (int i = 0; ids && i < count; ++i) {
      open_if_needed(ids.get()[i]);
    }
  }

  void StreamControls::open_if_needed(SDL_JoystickID instance) {
    for (const auto &device : devices_) {
      if (device.handle != nullptr && SDL_GetGamepadID(device.handle) == instance) {
        return;
      }
    }
    for (auto &device : devices_) {
      if (device.handle == nullptr) {
        device = {};
        device.handle = SDL_OpenGamepad(instance);
        if (device.handle != nullptr) {
          device.shortcuts = shortcut_bindings(device.handle);
          logging::info("input", "Opened stream gamepad " + std::to_string(instance));
        }
        return;
      }
    }
  }

  bool StreamControls::poll() {
    bool chord = false;
    for (auto &device : devices_) {
      const bool present = device.handle != nullptr && SDL_GamepadConnected(device.handle);
      device.sample = present ? sample_controller(device.handle, device.shortcuts) : input::ControllerState {};
      chord = input::is_stream_exit_requested(device.sample.buttonFlags, present) || chord;
    }
    return chord;
  }

  SDL_Gamepad *StreamControls::take_controller_for_shell() {
    poll();
#ifdef SUNSHINE_NATIVE_CLIENT
    // The shell retains the SDL handle, so closing the stream cannot clear feedback for it.
    for (auto &device : devices_) {
      if (device.handle != nullptr) {
        SDL_RumbleGamepad(device.handle, 0, 0, 0);
        SDL_RumbleGamepadTriggers(device.handle, 0, 0, 0);
        SDL_SetGamepadSensorEnabled(device.handle, SDL_SENSOR_ACCEL, false);
        SDL_SetGamepadSensorEnabled(device.handle, SDL_SENSOR_GYRO, false);
      }
    }
#endif
    for (auto &device : devices_) {
      if (device.handle != nullptr && (device.sample.buttonFlags != 0 || device.sample.leftTrigger != 0 || device.sample.rightTrigger != 0)) {
        return std::exchange(device.handle, nullptr);
      }
    }
    for (auto &device : devices_) {
      if (device.handle != nullptr && SDL_GamepadConnected(device.handle)) {
        return std::exchange(device.handle, nullptr);
      }
    }
    return nullptr;
  }

  std::vector<std::string> StreamControls::diagnostic_lines() const {
    std::vector<std::string> lines {"STREAM MENU: Back + Black + White"};
    for (const auto &device : devices_) {
      if (device.handle != nullptr) {
        lines.push_back("Pad " + std::to_string(SDL_GetGamepadID(device.handle)) + " buttons=" + std::to_string(device.sample.buttonFlags));
      }
    }
    return lines;
  }

  bool StreamControls::visible() const {
    return menu_.page != input::StreamMenuPage::closed;
  }

  void StreamControls::EventHandler::open_menu(std::int64_t device) const {
    if (controls_.menu_.page == input::StreamMenuPage::radial) {
      return;
    }
    controls_.menu_.open(device);
    controls_.navigation_.keyboard.reset();
    controls_.navigation_.controller.reset();
    controls_.navigation_.neutralRequired = false;
    controls_.navigation_.pageTriggerDown = false;
    controls_.navigation_.dockTriggerDown = false;
    controls_.navigation_.inputDevice = device < 0 ? input::UiInputDevice::keyboard : input::UiInputDevice::gamepad;
    controls_.router_.set_captured(true);
    controls_.navigation_.armed = false;
    controls_.panel_.dirty = true;
    controls_.panel_.message.clear();
    logging::info("input", "Opened stream menu from " + (device < 0 ? std::string("keyboard") : "gamepad " + std::to_string(device)));
  }

  void StreamControls::EventHandler::tap_key(int code) const {
    if (code == 0) {
      return;
    }
    const int keyModifiers = controls_.menu_.key_modifiers(code);
    // No gameplay sample is forwarded during this event-thread-only local key tap.
    controls_.router_.set_captured(false);
    constexpr std::array<int, 4> modifiers {MODIFIER_SHIFT, MODIFIER_CTRL, MODIFIER_ALT, MODIFIER_META};
    constexpr std::array<int, 4> keys {0xA0, 0xA2, 0xA4, 0x15B};
    for (std::size_t i = 0; i < modifiers.size(); ++i) {
      if ((keyModifiers & modifiers[i]) != 0) {
        controls_.router_.key(-3, keys[i], true);
      }
    }
    controls_.router_.key(-3, code, true, keyModifiers);
    controls_.router_.key(-3, code, false, keyModifiers);
    controls_.router_.release_source(-3);
    controls_.router_.set_captured(true);
    controls_.menu_.key_sent();
    controls_.panel_.dirty = true;
  }

  bool StreamControls::EventHandler::confirm_exit_key(SDL_Keycode key) const {
    if (key == SDLK_RETURN) {
      return controls_.menu_.confirm_disconnect(input::StreamDisconnectAction::disconnect);
    }
    if (key == SDLK_Q) {
      return controls_.menu_.confirm_disconnect(input::StreamDisconnectAction::quit_app);
    }
    if (key == SDLK_ESCAPE) {
      controls_.panel_.dirty = true;
      return controls_.menu_.confirm_disconnect(input::StreamDisconnectAction::cancel);
    }
    return false;
  }

  bool StreamControls::EventHandler::handle_keyboard_shortcut(SDL_Keycode key, bool active) const {
    if (key == SDLK_TAB) {
      controls_.menu_.toggle_keyboard_page();
      return true;
    }
    if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) {
      controls_.menu_.toggle_shift();
      return true;
    }
    if (key == SDLK_CAPSLOCK) {
      controls_.menu_.caps = !controls_.menu_.caps;
      return true;
    }
    if (key == SDLK_F2) {
      controls_.keyboardTop_ = !controls_.keyboardTop_;
      return true;
    }
    if (key == SDLK_BACKSPACE || key == SDLK_SPACE) {
      if (active) {
        tap_key(key == SDLK_SPACE ? 0x20 : 0x08);
      }
      return true;
    }
    return false;
  }

  void StreamControls::EventHandler::activate_stream_action(FfmpegStreamBackend &backend, bool active, bool &stats) const {
    switch (controls_.menu_.selected) {
      case input::StreamAction::quit:
        controls_.menu_.page = input::StreamMenuPage::quit_confirmation;
        break;
      case input::StreamAction::keyboard:
        controls_.menu_.page = input::StreamMenuPage::keyboard;
        controls_.navigation_.controller.reset();
        controls_.navigation_.neutralRequired = true;
        break;
      case input::StreamAction::logs:
        controls_.menu_.page = input::StreamMenuPage::logs;
        controls_.navigation_.controller.reset();
        controls_.navigation_.neutralRequired = true;
        controls_.panel_.logs.clear();
        for (const auto &entry : logging::snapshot()) {
          controls_.panel_.logs.push_back(logging::format_entry(entry));
        }
        if (controls_.panel_.logs.empty()) {
          controls_.panel_.logs = logging::load_log_file().lines;
        }
        break;
      case input::StreamAction::mouse:
        if (controls_.mouseDevice_ < 0 && controls_.menu_.owner < 0) {
          controls_.panel_.message = "Open the menu with a controller to enable mouse mode.";
          break;
        }
        controls_.mouseDevice_ = controls_.mouseDevice_ < 0 ? controls_.menu_.owner : -1;
        controls_.router_.set_mouse_device(controls_.mouseDevice_);
        controls_.menu_.close();
        break;
      case input::StreamAction::statistics:
        stats = !stats;
        controls_.menu_.close();
        break;
      case input::StreamAction::screenshot:
        {
          if (backend.request_screenshot(controls_.screenshotDirectory_, controls_.screenshotSource_)) {
            controls_.screenshotNotification_.begin();
          } else if (backend.screenshot_progress() < 0) {
            controls_.screenshotNotification_.finish(false, "Could not start capture. Check the stream log for details.", SDL_GetTicks());
          }
          controls_.panel_.message = controls_.screenshotNotification_.title();
          logging::info("stream", controls_.panel_.message);
          controls_.menu_.close();
          break;
        }
      case input::StreamAction::guide:
      case input::StreamAction::share:
        {
          const int slot = active ? controls_.router_.connect(controls_.menu_.owner) : -1;
          if (slot < 0) {
            controls_.panel_.message = "A connected controller and active stream are required.";
            break;
          }
          release_pulse();
          controls_.pulseSlot_ = slot;
          controls_.pulseUntil_ = SDL_GetTicks() + 120;
          controls_.pulseButtons_ = controls_.menu_.selected == input::StreamAction::guide ? SPECIAL_FLAG : MISC_FLAG;
          input::StreamPacket packet {input::StreamPacketType::controller, slot, controls_.router_.active_mask()};
          packet.controller = controls_.latestHostController_[slot];
          packet.controller.buttonFlags |= controls_.pulseButtons_;
          send_packet(packet);
          controls_.panel_.message = controls_.menu_.selected == input::StreamAction::guide ? "Guide sent" : "Share sent (requires host support)";
          controls_.menu_.close();
          break;
        }
      case input::StreamAction::none:
        break;
    }
  }

  bool StreamControls::EventHandler::menu_key(SDL_Keycode key, FfmpegStreamBackend &backend, bool active, bool &stats) const {
    if (controls_.menu_.page == input::StreamMenuPage::quit_confirmation) {
      return confirm_exit_key(key);
    }
    controls_.panel_.dirty = true;
    if (key == SDLK_ESCAPE) {
      if (controls_.menu_.page == input::StreamMenuPage::radial) {
        controls_.menu_.close();
      } else {
        controls_.menu_.page = input::StreamMenuPage::radial;
        controls_.menu_.modifiers = 0;
        controls_.menu_.shiftOnce = false;
      }
      return false;
    }
    if (controls_.menu_.page == input::StreamMenuPage::keyboard && handle_keyboard_shortcut(key, active)) {
      return false;
    }
    if (key != SDLK_RETURN && key != SDLK_SPACE) {
      if (key == SDLK_TAB && controls_.menu_.page == input::StreamMenuPage::radial) {
        controls_.menu_.selected = controls_.menu_.selected == input::StreamAction::none ? input::StreamAction::keyboard : static_cast<input::StreamAction>((static_cast<int>(controls_.menu_.selected) + 1) % 8);
      } else {
        const auto [dx, dy] = input::navigation_step(menu_navigation_command(key));
        controls_.menu_.navigate(dx, dy);
      }
      return false;
    }
    if (controls_.menu_.page == input::StreamMenuPage::keyboard) {
      if (active) {
        tap_key(controls_.menu_.keyboard_key());
      }
      return false;
    }
    if (controls_.menu_.page != input::StreamMenuPage::radial) {
      return false;
    }
    activate_stream_action(backend, active, stats);
    return false;
  }

  /**
   * @brief Translate the four directional controller buttons to local navigation keys.
   * @param button SDL controller button.
   * @return Corresponding arrow key, or unknown for other buttons.
   */
  SDL_Keycode controller_navigation_key(Uint8 button) {
    switch (button) {
      case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return SDLK_UP;
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return SDLK_DOWN;
      case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return SDLK_LEFT;
      case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return SDLK_RIGHT;
      default:
        return SDLK_UNKNOWN;
    }
  }

  void StreamControls::EventHandler::note_gamepad_activity(const SDL_Event &event) const {
    const bool triggerAxis = event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    const bool gamepadActivity = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (triggerAxis ? event.gaxis.value >= 16000 : std::abs(static_cast<int>(event.gaxis.value)) >= input::StreamMenu::WHEEL_DEADZONE));
    if (controls_.visible() && gamepadActivity && controls_.menu_.owner < 0) {
      controls_.menu_.owner = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? event.gbutton.which : event.gaxis.which;
    }
    if (controls_.visible() && gamepadActivity && controls_.navigation_.inputDevice != input::UiInputDevice::gamepad) {
      controls_.navigation_.inputDevice = input::UiInputDevice::gamepad;
      controls_.navigation_.keyboard.reset();
      controls_.panel_.dirty = true;
    }
    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
      keyboard_trigger(event.gaxis);
    }
  }

  void StreamControls::EventHandler::keyboard_trigger(const SDL_GamepadAxisEvent &axis) const {
    if (!controls_.visible() || controls_.menu_.page != input::StreamMenuPage::keyboard || axis.which != controls_.menu_.owner) {
      return;
    }
    const bool pressed = axis.value >= 16000;
    if (axis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
      if (pressed && !controls_.navigation_.pageTriggerDown && controls_.navigation_.armed) {
        controls_.menu_.toggle_keyboard_page();
        controls_.panel_.dirty = true;
      }
      controls_.navigation_.pageTriggerDown = pressed;
    } else if (axis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
      if (pressed && !controls_.navigation_.dockTriggerDown && controls_.navigation_.armed) {
        controls_.keyboardTop_ = !controls_.keyboardTop_;
        controls_.panel_.dirty = true;
      }
      controls_.navigation_.dockTriggerDown = pressed;
    }
  }

  void StreamControls::EventHandler::joystick_button(const SDL_JoyButtonEvent &button) const {
    for (auto &device : controls_.devices_) {
      if (device.handle == nullptr || SDL_GetGamepadID(device.handle) != button.which) {
        continue;
      }
      int flag = 0;
      for (const auto &[raw, mapped] : device.shortcuts) {
        if (raw == button.button) {
          flag |= mapped;
        }
      }
      if (flag != 0) {
        const bool down = device.chord.update_button(flag, button.down);
        if (down && !device.chordDown) {
          open_menu(button.which);
        }
        device.chordDown = down;
      }
    }
  }

  void StreamControls::EventHandler::route_controller_button(const SDL_GamepadButtonEvent &button, bool active) const {
    for (auto &device : controls_.devices_) {
      if (device.handle == nullptr || SDL_GetGamepadID(device.handle) != button.which) {
        continue;
      }
      const int flag = controller_flag(button.button);
      const bool down = device.chord.update_button(flag, button.down);
      if (down && !device.chordDown) {
        open_menu(button.which);
      }
      device.chordDown = down;
      if (button.down) {
        device.sample.buttonFlags |= flag;
      } else {
        device.sample.buttonFlags &= ~flag;
      }
      if (active) {
        controls_.router_.connect(button.which);
        controls_.router_.controller(button.which, device.sample);
      }
    }
  }

  void StreamControls::EventHandler::navigate_owner_controller() const {
    for (const auto &device : controls_.devices_) {
      if (device.handle != nullptr && SDL_GetGamepadID(device.handle) == controls_.menu_.owner) {
        navigate_controller(device.sample, SDL_GetTicks());
      }
    }
  }

  bool StreamControls::EventHandler::controller_button(const SDL_GamepadButtonEvent &button, FfmpegStreamBackend &backend, bool active, bool &statisticsEnabled) const {
    route_controller_button(button, active);
    if (controls_.visible() && controls_.navigation_.armed && !button.down && button.which == controls_.menu_.owner && controls_.menu_.page != input::StreamMenuPage::radial) {
      navigate_owner_controller();
    }
    if (controls_.visible() && controls_.navigation_.armed && button.down) {
      if (controls_.menu_.owner < 0) {
        controls_.menu_.owner = button.which;
      }
      if (button.which != controls_.menu_.owner) {
        return false;
      }
      return activate_controller_button(button.button, backend, active, statisticsEnabled);
    }
    return false;
  }

  bool StreamControls::EventHandler::activate_controller_button(Uint8 button, FfmpegStreamBackend &backend, bool active, bool &statisticsEnabled) const {
    switch (button) {
      case SDL_GAMEPAD_BUTTON_SOUTH:
        return menu_key(SDLK_RETURN, backend, active, statisticsEnabled);
      case SDL_GAMEPAD_BUTTON_EAST:
        return menu_key(SDLK_ESCAPE, backend, active, statisticsEnabled);
      case SDL_GAMEPAD_BUTTON_DPAD_UP:
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
      case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
      case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        if (controls_.menu_.page == input::StreamMenuPage::radial) {
          menu_key(controller_navigation_key(button), backend, active, statisticsEnabled);
        } else {
          navigate_owner_controller();
        }
        break;
      case SDL_GAMEPAD_BUTTON_WEST:
        if (controls_.menu_.page == input::StreamMenuPage::quit_confirmation) {
          return menu_key(SDLK_Q, backend, active, statisticsEnabled);
        } else if (active && controls_.menu_.page == input::StreamMenuPage::keyboard) {
          tap_key(0x08);
        }
        break;
      case SDL_GAMEPAD_BUTTON_NORTH:
        if (active && controls_.menu_.page == input::StreamMenuPage::keyboard) {
          tap_key(0x20);
        }
        break;
      case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        if (controls_.menu_.page == input::StreamMenuPage::keyboard) {
          controls_.menu_.caps = !controls_.menu_.caps;
          controls_.panel_.dirty = true;
        }
        break;
      case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        if (controls_.menu_.page == input::StreamMenuPage::keyboard) {
          controls_.menu_.toggle_shift();
          controls_.panel_.dirty = true;
        }
        break;
      case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
      case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        if (active && controls_.menu_.page == input::StreamMenuPage::keyboard) {
          tap_key(button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ? 0x125 : 0x127);
        }
        break;
      case SDL_GAMEPAD_BUTTON_START:
        if (active && controls_.menu_.page == input::StreamMenuPage::keyboard) {
          tap_key(0x0D);
        }
        break;
      default:
        break;
    }
    return false;
  }

  void StreamControls::EventHandler::controller_axis(const SDL_GamepadAxisEvent &axis) const {
    if (!controls_.visible() || !controls_.navigation_.armed || axis.which != controls_.menu_.owner) {
      return;
    }
    for (auto &device : controls_.devices_) {
      if (device.handle == nullptr || SDL_GetGamepadID(device.handle) != axis.which) {
        continue;
      }
      // Preserve the event's deflection even when a queued center event follows it.
      auto &sample = device.sample;
      switch (axis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
          sample.leftStickX = axis.value;
          break;
        case SDL_GAMEPAD_AXIS_LEFTY:
          sample.leftStickY = input::invert_stick_axis(axis.value);
          break;
        case SDL_GAMEPAD_AXIS_RIGHTX:
          sample.rightStickX = axis.value;
          break;
        case SDL_GAMEPAD_AXIS_RIGHTY:
          sample.rightStickY = input::invert_stick_axis(axis.value);
          break;
        default:
          break;
      }
      if (axis.axis <= SDL_GAMEPAD_AXIS_RIGHTY) {
        if (controls_.menu_.page == input::StreamMenuPage::radial) {
          controls_.panel_.dirty = controls_.menu_.sample_sticks(sample.leftStickX, sample.leftStickY, sample.rightStickX, sample.rightStickY) || controls_.panel_.dirty;
        } else {
          navigate_controller(sample, SDL_GetTicks());
        }
      }
    }
  }

  /**
   * @brief Translate physical keyboard modifier flags to the host protocol.
   * @param flags SDL modifier bits.
   * @return Moonlight Shift, Ctrl, Alt and Meta modifier mask.
   */
  int host_keyboard_modifiers(Uint16 flags) {
    return ((flags & SDL_KMOD_SHIFT) != 0 ? MODIFIER_SHIFT : 0) | ((flags & SDL_KMOD_CTRL) != 0 ? MODIFIER_CTRL : 0) | ((flags & SDL_KMOD_ALT) != 0 ? MODIFIER_ALT : 0) | ((flags & SDL_KMOD_GUI) != 0 ? MODIFIER_META : 0);
  }

  /**
   * @brief Match a fresh Ctrl+Alt+Shift press of a local stream shortcut key.
   * @param key SDL keyboard event.
   * @param scancode Physical key assigned to the shortcut.
   * @return True when the event should trigger the local action.
   */
  bool matches_stream_shortcut(const SDL_KeyboardEvent &key, SDL_Scancode scancode) {
    return key.type == SDL_EVENT_KEY_DOWN && key.repeat == 0 && key.scancode == scancode && (key.mod & SDL_KMOD_CTRL) != 0 && (key.mod & SDL_KMOD_ALT) != 0 && (key.mod & SDL_KMOD_SHIFT) != 0;
  }

  bool StreamControls::EventHandler::keyboard_event(const SDL_KeyboardEvent &key, FfmpegStreamBackend &backend, bool active, bool &statisticsEnabled) const {
    bool stop = false;
    const bool down = key.type == SDL_EVENT_KEY_DOWN;
#ifdef SUNSHINE_DESKTOP
    if (matches_stream_shortcut(key, SDL_SCANCODE_X)) {
      ui::toggle_desktop_fullscreen(key.windowID);
      return false;
    }
#endif
    if (matches_stream_shortcut(key, SDL_SCANCODE_Q)) {
      open_menu(-1);
      controls_.navigation_.armed = true;
      return false;
    }
    if (controls_.visible()) {
      if (!down) {
        controls_.navigation_.keyboard.release(key.scancode);
      }
      if (!down || key.repeat != 0) {
        return false;
      }
      {
        if (controls_.navigation_.inputDevice != input::UiInputDevice::keyboard) {
          controls_.navigation_.inputDevice = input::UiInputDevice::keyboard;
          controls_.panel_.dirty = true;
        }
        const auto command = menu_navigation_command(key.key);
        if (command != input::UiCommand::none) {
          if (controls_.navigation_.keyboard.press(key.scancode, command, SDL_GetTicks()) != input::UiCommand::none) {
            menu_key(key.key, backend, active, statisticsEnabled);
          }
        } else {
          if (key.key == SDLK_RETURN || key.key == SDLK_SPACE || key.key == SDLK_ESCAPE) {
            controls_.navigation_.keyboard.reset();
          }
          stop = menu_key(key.key, backend, active, statisticsEnabled) || stop;
        }
      }
      return stop;
    }
    if (active && key.repeat == 0) {
      const int mods = host_keyboard_modifiers(key.mod);
      controls_.router_.key(0, input::keyboard_virtual_key(key.scancode), down, mods);
    }
    return stop;
  }

  /**
   * @brief Translate supported physical mouse buttons to host protocol buttons.
   * @param button SDL mouse button.
   * @return Host button, or zero for an unsupported button.
   */
  int host_mouse_button(Uint8 button) {
    switch (button) {
      case SDL_BUTTON_LEFT:
        return BUTTON_LEFT;
      case SDL_BUTTON_RIGHT:
        return BUTTON_RIGHT;
      case SDL_BUTTON_MIDDLE:
        return BUTTON_MIDDLE;
      case SDL_BUTTON_X1:
        return BUTTON_X1;
      case SDL_BUTTON_X2:
        return BUTTON_X2;
      default:
        return 0;
    }
  }

  void StreamControls::EventHandler::close_if_removed(SDL_JoystickID instance) const {
    release_pulse();
    controls_.router_.disconnect(instance);
    if (controls_.mouseDevice_ == instance) {
      controls_.mouseDevice_ = -1;
    }
    if (controls_.menu_.owner == instance) {
      controls_.menu_.owner = -1;
      controls_.navigation_.armed = true;
    }
    for (auto &device : controls_.devices_) {
      if (device.handle != nullptr && SDL_GetGamepadID(device.handle) == instance) {
        SDL_CloseGamepad(device.handle);
        device = {};
      }
    }
    // Recover a fifth device that was waiting when the four slots were occupied.
    controls_.open_connected_gamepads();
  }

  bool StreamControls::EventHandler::handle_event(const SDL_Event &event, FfmpegStreamBackend &backend, bool active, bool &statisticsEnabled) const {
#ifdef SUNSHINE_NATIVE_CLIENT
    if (active && controls_.focused_ && !controls_.visible()) {
      controls_.forward_gamepad_event(event);
    }
#endif
    note_gamepad_activity(event);
    switch (event.type) {
      case SDL_EVENT_GAMEPAD_ADDED:
        controls_.open_if_needed(event.gdevice.which);
        break;
      case SDL_EVENT_GAMEPAD_REMOVED:
        close_if_removed(event.gdevice.which);
        break;
      case SDL_EVENT_GAMEPAD_REMAPPED:
        for (auto &device : controls_.devices_) {
          if (device.handle != nullptr && SDL_GetGamepadID(device.handle) == event.gdevice.which) {
            device.shortcuts = shortcut_bindings(device.handle);
          }
        }
        break;
      case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
      case SDL_EVENT_JOYSTICK_BUTTON_UP:
        joystick_button(event.jbutton);
        break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return controller_button(event.gbutton, backend, active, statisticsEnabled);
      case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        controller_axis(event.gaxis);
        break;
      case SDL_EVENT_KEY_DOWN:
#ifdef __ANDROID__
        if (event.key.key == SDLK_AC_BACK) {
          return true;
        }
#endif
        [[fallthrough]];
      case SDL_EVENT_KEY_UP:
        return keyboard_event(event.key, backend, active, statisticsEnabled);
      case SDL_EVENT_MOUSE_MOTION:
        if (active) {
          controls_.router_.mouse_move(static_cast<int>(event.motion.xrel), static_cast<int>(event.motion.yrel));
        }
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (active) {
          controls_.router_.mouse_button(1, host_mouse_button(event.button.button), event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        if (active) {
          const int direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
          controls_.router_.scroll(event.wheel.integer_x * direction, event.wheel.integer_y * direction);
        }
        break;
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        controls_.focused_ = false;
        controls_.navigation_.keyboard.reset();
        controls_.router_.set_captured(true);
        break;
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
        controls_.focused_ = true;
        controls_.router_.set_captured(controls_.visible());
        break;
#ifdef SUNSHINE_NATIVE_CLIENT
      case SDL_EVENT_DID_ENTER_BACKGROUND:
#endif
      case SDL_EVENT_QUIT:
        return true;
      default:
        break;
    }
    return false;
  }

  void StreamControls::PanelCache::refresh_logs(input::StreamMenu &menu, Uint32 now) {
    if (menu.page == input::StreamMenuPage::logs && now - logRefreshTick >= 1000) {
      logRefreshTick = now;
      std::vector<std::string> lines;
      for (const auto &entry : logging::snapshot()) {
        lines.push_back(logging::format_entry(entry));
      }
      if (!lines.empty() && lines != logs) {
        // Preserve the same entries when reading older output; offset zero follows live output.
        if (menu.logOffset > 0) {
          menu.logOffset += std::max(0, static_cast<int>(lines.size()) - static_cast<int>(logs.size()));
        }
        logs = std::move(lines);
        dirty = true;
      }
    }
  }

  void StreamControls::EventHandler::update_device(Device &device, bool active, Uint32 now) const {
    if (device.handle == nullptr) {
      return;
    }
    const auto instance = SDL_GetGamepadID(device.handle);
    const bool present = SDL_GamepadConnected(device.handle);
    const bool down = device.chord.update(device.sample.buttonFlags, present);
    if (down && !device.chordDown) {
      open_menu(instance);
    }
    device.chordDown = down;
    if (active) {
      if (present) {
        controls_.router_.connect(instance);
        controls_.router_.controller(instance, device.sample);
      } else {
        controls_.router_.disconnect(instance);
      }
    }
    if (!controls_.visible() || instance != controls_.menu_.owner || controls_.navigation_.inputDevice != input::UiInputDevice::gamepad) {
      return;
    }
    const auto &sample = device.sample;
    if (sample.buttonFlags == 0 && sample.leftTrigger == 0 && sample.rightTrigger == 0) {
      controls_.navigation_.armed = true;
    }
    if (!controls_.navigation_.armed) {
      return;
    }
    if (controls_.menu_.page == input::StreamMenuPage::radial) {
      // Poll on every pump as well as consuming events: Xbox input can change
      // between queued axis events, and a selected item must not freeze polling.
      controls_.panel_.dirty = controls_.menu_.sample_sticks(sample.leftStickX, sample.leftStickY, sample.rightStickX, sample.rightStickY) || controls_.panel_.dirty;
    } else {
      navigate_controller(sample, now);
    }
  }

  void StreamControls::refresh_overlay(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height, bool wasVisible, bool screenshotChanged, bool stats) {
    if (!visible() && (wasVisible || panel_.dirty)) {
      navigation_.keyboard.reset();
      router_.set_captured(!focused_);
      if (!screenshotNotification_.visible) {
        backend.presentation().set_statistics_bitmap(0, 0, {});
      }
      panel_.clear();
      panel_.dirty = false;
      screenshotChanged = true;
    }
    const int scale = controls_raster_scale(height);
    backend.presentation().local_overlay_size(width, height);
    if (!visible() && screenshotChanged) {
      if (screenshotNotification_.visible) {
        render_screenshot_notification(backend, font, width, height);
      } else {
        backend.presentation().set_statistics_bitmap(0, 0, {});
        panel_.clear();
      }
    }
    if (visible() && screenshotNotification_.visible && panel_.message != screenshotNotification_.title()) {
      panel_.message = screenshotNotification_.title();
      panel_.dirty = true;
    }
    if (visible() && (width != panel_.width || height != panel_.height)) {
      panel_.dirty = true;
      panel_.width = width;
      panel_.height = height;
    }
    if (visible() && panel_.dirty && !render_menu(backend, font, width, height, stats, scale)) {
      // An invisible menu must not keep host input captured.
      menu_.close();
      navigation_.keyboard.reset();
      router_.set_captured(!focused_);
      backend.presentation().set_statistics_bitmap(0, 0, {});
      panel_.clear();
      panel_.dirty = false;
    }
  }

  bool StreamControls::pump(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height, bool active, bool &statisticsEnabled) {
    bool screenshotChanged = false;
    if (const auto result = backend.take_screenshot_result(); !result.empty()) {
      logging::info("screenshots", result);
      screenshotNotification_.finish(result.rfind("Saved ", 0) == 0, result, SDL_GetTicks());
      screenshotChanged = true;
    } else {
      screenshotChanged = screenshotNotification_.update(backend.screenshot_progress());
    }
    // Sampling and transport enqueueing share one owner so capture cannot race a stale gameplay packet.
    navigation_.applied = false;
    SDL_PumpEvents();
    bool stop = false;
    const bool wasVisible = visible();
    SDL_Event event;  // SDL_PeepEvents fills the event before each use.
    const Uint64 eventStart = SDL_GetTicks();
    for (int count = 0; count < 128 && (count == 0 || SDL_GetTicks() - eventStart < 4) && SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0; ++count) {
      stop = events_.handle_event(event, backend, active, statisticsEnabled) || stop;
    }
    if (stop) {
      router_.set_captured(true);
      return true;
    }
    poll();
    const Uint32 now = SDL_GetTicks();
    screenshotChanged = screenshotNotification_.tick(now) || screenshotChanged;
    panel_.refresh_logs(menu_, now);
    if (visible() && navigation_.inputDevice == input::UiInputDevice::keyboard) {
      const auto repeatedKey = menu_navigation_key(navigation_.keyboard.poll(now));
      if (repeatedKey != SDLK_UNKNOWN) {
        events_.menu_key(repeatedKey, backend, active, statisticsEnabled);
      }
    }
    for (auto &device : devices_) {
      events_.update_device(device, active, now);
    }
    if (pulseSlot_ >= 0 && (static_cast<Sint32>(now - pulseUntil_) >= 0 || (router_.active_mask() & (1U << pulseSlot_)) == 0)) {
      events_.release_pulse();
    }
    refresh_overlay(backend, font, width, height, wasVisible, screenshotChanged, statisticsEnabled);
    if (active) {
      router_.tick(now - lastTick_);
    }
    lastTick_ = now;
    return stop;
  }

  void StreamControls::release_host_input() {
    router_.release_all();
    events_.release_pulse();
  }

  void StreamControls::render_screenshot_notification(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height) {
    if (font == nullptr || width <= 0 || height <= 0) {
      return;
    }
    const int line = TTF_GetFontLineSkip(font);
    const auto title = screenshotNotification_.title();
    const std::string detail(screenshotNotification_.body());
    int titleWidth = 0;
    int detailWidth = 0;
    TTF_GetStringSize(font, title.c_str(), 0, &titleWidth, nullptr);
    if (!detail.empty()) {
      TTF_GetStringSize(font, detail.c_str(), 0, &detailWidth, nullptr);
    }
    const auto layout = layout_screenshot_notification(width, height, line, titleWidth, detailWidth, screenshotNotification_.saving);
    const int panelWidth = layout.width;
    const int panelHeight = layout.height;
    if (panelWidth <= 0 || panelHeight <= 0) {
      return;
    }
    if (!panel_.prepare(backend, panelWidth, panelHeight)) {
      return;
    }
    auto *renderer = panel_.renderer.get();
    SDL_SetRenderDrawColor(renderer, ui::brand::PANEL_ALT.r, ui::brand::PANEL_ALT.g, ui::brand::PANEL_ALT.b, 170);
    SDL_RenderClear(renderer);
    panel_.label(renderer, font, title, {12, 8, panelWidth - 24, line}, {false, false, true});
    if (!detail.empty()) {
      // Fit the complete path on one line within the screenshot notification.
      panel_.label(renderer, font, detail, {12, line + 12, panelWidth - 24, line}, {false, false, true});
    }
    SDL_Rect bar {12, panelHeight - 12, panelWidth - 24, 4};
    SDL_SetRenderDrawColor(renderer, ui::brand::PANEL.r, ui::brand::PANEL.g, ui::brand::PANEL.b, ui::brand::PANEL.a);
    ui::fill_rect(renderer, bar);
    bar.w = bar.w * screenshotNotification_.percent / 100;
    const auto accent = screenshotNotification_.saving || screenshotNotification_.saved ? ui::brand::ACCENT : SDL_Color {255, 110, 110, 255};
    SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, accent.a);
    ui::fill_rect(renderer, bar);
    SDL_RenderPresent(renderer);
    backend.presentation().set_color_overlay_surface(panel_.surface, VideoOverlayPlacement::bottom_center);
  }

  bool StreamControls::PanelCache::render_icon(SDL_Renderer *target, const char *asset, SDL_Rect bounds, const SDL_Rect *clip) {
    const int size = std::min(bounds.w, bounds.h);
    if (size <= 0) {
      return false;
    }
    const auto key = std::make_pair(std::string(asset), size);
    auto found = icons.find(key);
    if (found == icons.end()) {
      const std::string path = ui::build_asset_path(std::string("icons/") + asset);
      std::shared_ptr<SDL_Surface> raster(ui::load_svg_surface(path, size, size), SDL_DestroySurface);
      if (!raster) {
        logging::warn("ui", "Unable to load stream icon " + path + ": " + SDL_GetError());
      }
      found = icons.try_emplace(key, std::move(raster)).first;
    }
    if (!found->second) {
      return false;
    }
    const auto &raster = found->second;
    auto texture = iconTextures.find(key);
    if (texture == iconTextures.end()) {
      auto loaded = std::shared_ptr<SDL_Texture>(SDL_CreateTextureFromSurface(target, raster.get()), SDL_DestroyTexture);
      if (!loaded) {
        return false;
      }
      texture = iconTextures.try_emplace(key, std::move(loaded)).first;
    }
    SDL_Rect destination {bounds.x + (bounds.w - raster->w) / 2, bounds.y + (bounds.h - raster->h) / 2, raster->w, raster->h};
    if (clip != nullptr) {
      SDL_Rect intersection {};
      if (!SDL_GetRectIntersection(&destination, clip, &intersection)) {
        return true;
      }
      const SDL_Rect source {intersection.x - destination.x, intersection.y - destination.y, intersection.w, intersection.h};
      return ui::render_texture(target, texture->second.get(), &source, &intersection);
    }
    return ui::render_texture(target, texture->second.get(), nullptr, &destination);
  }

  void StreamControls::render_hint(SDL_Renderer *renderer, TTF_Font *font, const char *asset, const char *key, const char *text, SDL_Rect bounds, bool compact) {
    if (bounds.w <= 0 || bounds.h <= 0) {
      return;
    }
    const bool keyboard = navigation_.inputDevice == input::UiInputDevice::keyboard;
    const int size = ui::control_hint_glyph_size(TTF_GetFontLineSkip(font), bounds.h, compact);
    int textWidth = 0;
    int textHeight = 0;
    TTF_GetStringSize(font, text, 0, &textWidth, &textHeight);
    const int capWidth = std::min(bounds.w, keyboard ? std::max(size, static_cast<int>(std::char_traits<char>::length(key)) * size / 2 + 8) : size);
    const int groupWidth = std::min(bounds.w, capWidth + 4 + textWidth);
    const int left = bounds.x + (bounds.w - groupWidth) / 2;
    SDL_Rect icon {left, bounds.y + (bounds.h - size) / 2, capWidth, size};
    if (keyboard) {
      SDL_SetRenderDrawColor(renderer, 85, 85, 85, 255);
      ui::draw_rect(renderer, icon);
      panel_.label(renderer, font, key, {icon.x + 2, icon.y + 2, icon.w - 4, icon.h - 4}, {false, false, true});
    } else {
      panel_.render_icon(renderer, asset, icon);
    }
    panel_.label(renderer, font, text, {left + capWidth + 4, bounds.y, std::max(1, groupWidth - capWidth - 4), bounds.h}, {false, false, true});
  }

  void StreamControls::render_keyboard_key(SDL_Renderer *renderer, TTF_Font *font, int i, SDL_Rect key) {
    const auto &cell = menu_.keyboard_cells()[i];
    const bool keyboardInput = navigation_.inputDevice == input::UiInputDevice::keyboard;
    const bool modifier = cell.key < input::StreamMenu::KEYBOARD_PAGE_KEY && input::StreamMenu::KEYS[cell.key].code < 0;
    bool latched = modifier && (menu_.modifiers & -input::StreamMenu::KEYS[cell.key].code) != 0;
    if (cell.key == input::StreamMenu::KEYBOARD_CAPS_KEY) {
      latched = menu_.caps;
    } else if (cell.key == 40) {
      latched = menu_.shiftOnce;
    }
    int gray = cell.width > 2 ? 42 : 30;
    if (latched) {
      gray = 65;
    }
    if (i == menu_.keyIndex) {
      gray = 54;
    }
    SDL_SetRenderDrawColor(renderer, gray, gray, gray, 255);
    ui::fill_rect(renderer, key);
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
        panel_.label(renderer, font, text, {key.x + 2, key.y + 2, key.w - 4, key.h / 2}, {false, false, true});
        render_hint(renderer, font, shortcutIcon, shortcutKey, "", {key.x + 2, key.y + key.h / 2, key.w - 4, key.h / 2 - 2}, cell.key != input::StreamMenu::KEYBOARD_CAPS_KEY && cell.key != 46);
      } else {
        render_hint(renderer, font, shortcutIcon, shortcutKey, text.c_str(), {key.x + 2, key.y + 2, key.w - 4, key.h - 4}, true);
      }
    } else {
      panel_.label(renderer, font, text, {key.x + 2, key.y + 2, key.w - 4, key.h - 4}, {false, false, true});
    }
    if (i == menu_.keyIndex) {
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_Rect focus {key.x - 1, key.y - 1, key.w + 2, key.h + 2};
      ui::draw_rect(renderer, focus);
      focus = {key.x - 2, key.y - 2, key.w + 4, key.h + 4};
      ui::draw_rect(renderer, focus);
    }
  }

  void StreamControls::render_keyboard(SDL_Renderer *renderer, TTF_Font *font, int width, int height, int rowHeight, bool focusOnly) {
    const auto &cells = menu_.keyboard_cells();
    constexpr int padding = 8;
    const int header = TTF_GetFontLineSkip(font) + 4;
    const auto &focusedCell = cells[menu_.keyIndex];
    std::string description = "Keyboard / PC keys";
    if (menu_.keyboardLayout == 0) {
      description = "Keyboard / ABC";
    } else if (menu_.keyboardLayout == 1) {
      description = "Keyboard / ?123";
    }
    if (focusedCell.key >= 106 && focusedCell.key < static_cast<int>(input::StreamMenu::KEYS.size())) {
      static const std::array<const char *, 12> shortcuts {"Copy: Ctrl+C", "Paste: Ctrl+V", "Cut: Ctrl+X", "Undo: Ctrl+Z", "Redo: Ctrl+Y", "Select all: Ctrl+A", "Switch app: Alt+Tab", "Close app: Alt+F4", "Ctrl+Alt+Delete", "Desktop: Win+D", "Explorer: Win+E", "Run: Win+R"};
      description = shortcuts[focusedCell.key - 106];
    }
    const int gridWidth = width - padding * 2;
    // Erase both outlines before painting either key: adjacent focus rectangles
    // touch the same gap pixels. No SDL renderer clipping commands are needed.
    if (focusOnly) {
      SDL_SetRenderDrawColor(renderer, 16, 16, 16, 255);
      for (const int index : {panel_.selection, menu_.keyIndex}) {
        const auto &cell = cells[index];
        const int left = padding + cell.column * gridWidth / input::StreamMenu::KEYBOARD_COLUMNS;
        const int right = padding + (cell.column + cell.width) * gridWidth / input::StreamMenu::KEYBOARD_COLUMNS;
        SDL_Rect area {left - 1, padding + header + cell.row * rowHeight - 1, right - left + 1, cell.height * rowHeight + 1};
        ui::fill_rect(renderer, area);
        panel_.damage.push_back({area.x, area.y, area.w, area.h});
      }
    }
    if (!focusOnly || menu_.keyboardLayout == 2) {
      SDL_Rect area {0, 0, width, header + 4};
      if (focusOnly) {
        SDL_SetRenderDrawColor(renderer, 16, 16, 16, 255);
        ui::fill_rect(renderer, area);
        panel_.damage.push_back({area.x, area.y, area.w, area.h});
      }
      panel_.label(renderer, font, description, {padding, 2, width - padding * 2, header}, {false, false, true});
    }
    for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
      if (focusOnly && i != panel_.selection && i != menu_.keyIndex) {
        continue;
      }
      const auto &cell = cells[i];
      const int left = padding + cell.column * gridWidth / input::StreamMenu::KEYBOARD_COLUMNS;
      const int right = padding + (cell.column + cell.width) * gridWidth / input::StreamMenu::KEYBOARD_COLUMNS;
      SDL_Rect key {left + 1, padding + header + cell.row * rowHeight + 1, right - left - 3, cell.height * rowHeight - 3};
      render_keyboard_key(renderer, font, i, key);
    }
    if (focusOnly) {
      return;
    }
    const int line = TTF_GetFontLineSkip(font) + 8;
    const int footerY = padding + header + input::StreamMenu::KEYBOARD_ROWS * rowHeight + 4;
    const int footerHeight = std::min(line + 4, height - footerY);
    const std::array<const char *, 4> assets {"button-a.svg", "button-b.svg", "button-right-stick.svg", "button-rt.svg"};
    const std::array<const char *, 4> keys {"Enter", "Esc", "Shift", "F2"};
    const std::array<const char *, 4> actions {"Select", "Back", menu_.shiftOnce ? "Shift: On" : "Shift", keyboardTop_ ? "Move down" : "Move up"};
    for (int i = 0; i < 4; ++i) {
      render_hint(renderer, font, assets[i], keys[i], actions[i], {i * width / 4 + 4, footerY, width / 4 - 8, footerHeight});
    }
  }

  /**
   * @brief Bound one wheel sector for incremental overlay publication.
   * @param wheel Wheel geometry.
   * @param sector Clockwise sector from up.
   * @return Conservative sector rectangle.
   */
  VideoRectangle wheel_sector_bounds(const MenuWheel &wheel, int sector) {
    const int cx = wheel.centerX;
    const int cy = wheel.centerY;
    const int radius = wheel.radius;
    switch (sector) {
      case 0:
        return {cx - radius, cy - radius, radius * 2 + 1, radius + 1};
      case 1:
        return {cx, cy - radius, radius + 1, radius * 2 + 1};
      case 2:
        return {cx - radius, cy, radius * 2 + 1, radius + 1};
      default:
        return {cx - radius, cy - radius, radius + 1, radius * 2 + 1};
    }
  }

  void StreamControls::render_wheel_sector(SDL_Renderer *renderer, TTF_Font *font, const WheelPaint &paint, int sector) {
    const auto &panel = panel_.surface;
    const auto &[cx, cy, radius, selected] = paint.geometry;
    const int width = paint.target.width;
    const int height = paint.target.height;
    const int line = paint.line;
    const bool focusOnly = paint.focusOnly;
    const bool stats = paint.stats;
    constexpr std::array<int, 4> dx {0, 1, 0, -1};
    constexpr std::array<int, 4> dy {-1, 0, 1, 0};
    const auto action = static_cast<input::StreamAction>(paint.index * 4 + sector);
    const int labelWidth = std::min(line * 6, width / 5);
    const int x = std::clamp(cx + dx[sector] * radius * 2 / 3 - labelWidth / 2, 0, width - labelWidth);
    const int iconSize = std::max(18, line);
    const int groupY = cy + dy[sector] * radius * 2 / 3 - (iconSize + line * 2) / 2;
    if (focusOnly) {
      // Restore the label's complete background, including any pixels in
      // the central hole, before blending its antialiased text again.
      const SDL_Rect group {x, groupY, labelWidth, iconSize + 2 + line * 2};
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
      ui::fill_rect(renderer, group);
      SDL_FlushRenderer(renderer);
      rasterize_menu_wheel({static_cast<std::uint8_t *>(panel->pixels), static_cast<std::size_t>(panel->pitch) * height, panel->pitch, width, height}, {cx, cy, radius, selected}, sector);
      const VideoRectangle clip {group.x, group.y, group.w, group.h};
      rasterize_menu_wheel({static_cast<std::uint8_t *>(panel->pixels), static_cast<std::size_t>(panel->pitch) * height, panel->pitch, width, height}, {cx, cy, radius, selected}, -1, &clip);
      panel_.render_icon(renderer, paint.index == 0 ? "button-left-stick.svg" : "button-right-stick.svg", {cx - line, cy - line, line * 2, line * 2}, &group);
      const VideoRectangle area = wheel_sector_bounds(paint.geometry, sector);
      const int left = std::min(area.x, clip.x);
      const int top = std::min(area.y, clip.y);
      panel_.damage.push_back({left, top, std::max(area.x + area.width, clip.x + clip.width) - left, std::max(area.y + area.height, clip.y + clip.height) - top});
    }
    constexpr std::array<const char *, 8> icons {"keyboard.svg", "mouse.svg", "scroll-text.svg", "log-out.svg", "button-guide.svg", "button-share.svg", "camera.svg", "chart-no-axes-combined.svg"};
    panel_.render_icon(renderer, icons[paint.index * 4 + sector], {x + (labelWidth - iconSize) / 2, groupY, iconSize, iconSize});
    panel_.label(renderer, font, input::stream_action_label(action, mouseDevice_ >= 0, stats), {x, groupY + iconSize + 2, labelWidth, line * 2}, {false, false});
  }

  void StreamControls::render_wheels(SDL_Renderer *renderer, TTF_Font *font, int width, int height, bool stats, bool focusOnly) {
    const auto &panel = panel_.surface;
    const int line = TTF_GetFontLineSkip(font) + 8;
    const int cy = height / 2;
    const int radius = std::min(width / 5, (height - line * 4) / 2);
    for (int wheel = 0; wheel < 2; ++wheel) {
      const int cx = width * (wheel == 0 ? 1 : 3) / 4;
      const int selected = static_cast<int>(menu_.selected) / 4 == wheel ? static_cast<int>(menu_.selected) % 4 : -1;
      const WheelPaint paint {{static_cast<std::uint8_t *>(panel->pixels), static_cast<std::size_t>(panel->pitch) * height, panel->pitch, width, height}, {cx, cy, radius, selected}, wheel, line, focusOnly, stats};
      if (!focusOnly) {
        SDL_FlushRenderer(renderer);
        rasterize_menu_wheel(paint.target, paint.geometry);
        panel_.render_icon(renderer, wheel == 0 ? "button-left-stick.svg" : "button-right-stick.svg", {cx - line, cy - line, line * 2, line * 2});
      }
      for (int sector = 0; sector < 4; ++sector) {
        if (focusOnly && wheel * 4 + sector != panel_.selection && wheel * 4 + sector != static_cast<int>(menu_.selected)) {
          continue;
        }
        render_wheel_sector(renderer, font, paint, sector);
      }
    }
    const bool keyboardHints = navigation_.inputDevice == input::UiInputDevice::keyboard;
    const std::array<const char *, 4> assets {"button-left-stick.svg", "button-right-stick.svg", "button-a.svg", "button-b.svg"};
    const std::array<const char *, 4> keys {"Arrows", "Tab", "Enter", "Esc"};
    const std::array<const char *, 4> actions {keyboardHints ? "Select" : "Left wheel", keyboardHints ? "Next" : "Right wheel", "Choose", "Resume"};
    for (int i = 0; !focusOnly && i < 4; ++i) {
      render_hint(renderer, font, assets[i], keys[i], actions[i], {i * width / 4 + 4, height - line * 2, width / 4 - 8, line + 4});
    }
  }

  /**
   * @brief Choose a readable severity color for a formatted log entry.
   * @param text Formatted log line.
   * @return Error, warning or normal text color.
   */
  SDL_Color stream_log_color(std::string_view text) {
    if (text.find("[ERROR]") != std::string_view::npos || text.find("[FATAL]") != std::string_view::npos) {
      return {255, 125, 125, 255};
    }
    if (text.find("[WARN") != std::string_view::npos) {
      return {255, 211, 115, 255};
    }
    return {224, 232, 241, 255};
  }

  void StreamControls::render_logs(SDL_Renderer *renderer, TTF_Font *font, int width, int height) {
    const int line = TTF_GetFontLineSkip(font) + 8;
    const SDL_Rect card {width / 8, line / 2, width * 7 / 8 - line / 2, height - line};
    ui::draw_dialog_surface(renderer, card);
    panel_.label(renderer, font, "Stream logs", {card.x + 20, card.y + 28, card.w - 40, line}, {false, false});
    const int row = line * 2;
    const int count = std::max(1, (card.h - line * 4) / row);
    menu_.logOffset = std::min(menu_.logOffset, std::max(0, static_cast<int>(panel_.logs.size()) - count));
    const int end = static_cast<int>(panel_.logs.size()) - menu_.logOffset;
    const int start = std::max(0, end - count);
    for (int i = start; i < end; ++i) {
      const auto &text = panel_.logs[i];
      const SDL_Color color = stream_log_color(text);
      panel_.label(renderer, font, text, {card.x + 16, card.y + line * 2 + (i - start) * row, card.w - 42, row}, {true, false, false, color});
    }
    const int trackHeight = count * row;
    const int thumbHeight = std::max(8, trackHeight * count / std::max(count, static_cast<int>(panel_.logs.size())));
    SDL_Rect thumb {card.x + card.w - 12, card.y + line * 2 + (trackHeight - thumbHeight) * start / std::max(1, static_cast<int>(panel_.logs.size()) - count), 3, thumbHeight};
    SDL_SetRenderDrawColor(renderer, ui::brand::ACCENT.r, ui::brand::ACCENT.g, ui::brand::ACCENT.b, ui::brand::ACCENT.a);
    ui::fill_rect(renderer, thumb);
    if (panel_.logs.empty()) {
      panel_.label(renderer, font, "No retained log entries.", {card.x + 16, card.y + line * 2, card.w - 32, line}, {false, false});
    }
    render_hint(renderer, font, "button-left-stick.svg", "Arrows", menu_.logOffset == 0 ? "Live / scroll" : "Scroll", {card.x + 8, card.y + card.h - line - 12, card.w / 2 - 16, line});
    render_hint(renderer, font, "button-b.svg", "Esc", "Back", {card.x + card.w / 2, card.y + card.h - line - 12, card.w / 2 - 16, line});
  }

  void StreamControls::render_exit_confirmation(SDL_Renderer *renderer, TTF_Font *font, int width, int height) {
    const int line = TTF_GetFontLineSkip(font) + 8;
    const int cardWidth = std::min(width - 32, std::max(560, width * 4 / 5));
    const int cardHeight = line * 7;
    const SDL_Rect card {(width - cardWidth) / 2, (height - cardHeight) / 2, cardWidth, cardHeight};
    ui::draw_dialog_surface(renderer, card);
    panel_.label(renderer, font, "Exit stream?", {card.x + 20, card.y + 28, card.w - 40, line}, {false, false});
    panel_.label(renderer, font, "Disconnect keeps the app running. Quit app closes it on the PC; unsaved progress may be lost.", {card.x + 20, card.y + 28 + line, card.w - 40, line * 2}, {false, false});
    constexpr std::array<const char *, 3> buttons {"button-b.svg", "button-a.svg", "button-x.svg"};
    constexpr std::array<const char *, 3> keys {"Esc", "Enter", "Q"};
    constexpr std::array<const char *, 3> labels {"Cancel", "Disconnect", "Quit app"};
    for (int i = 0; i < 3; ++i) {
      SDL_Rect button {card.x + 16 + i * (card.w - 24) / 3, card.y + card.h - line * 2, (card.w - 48) / 3, line + 12};
      ui::draw_dialog_round_rect(renderer, button, {28, 40, 61, 255}, 6);
      render_hint(renderer, font, buttons[i], keys[i], labels[i], {button.x + 4, button.y, button.w - 8, button.h});
    }
  }

  bool StreamControls::PanelCache::prepare(FfmpegStreamBackend &backend, int panelWidth, int panelHeight) {
    if (!surface || surface->w != panelWidth || surface->h != panelHeight) {
      // Release both owners before allocating another full-size raster.
      backend.presentation().set_statistics_bitmap(0, 0, {});
      clear();
      if (auto *createdSurface = SDL_CreateSurface(panelWidth, panelHeight, SDL_PIXELFORMAT_ARGB8888); createdSurface != nullptr) {
        surface.reset(createdSurface, SDL_DestroySurface);
      }
    }
    if (surface && !renderer) {
      renderer.reset(SDL_CreateSoftwareRenderer(surface.get()));
    }
    if (!renderer) {
      return false;
    }
    if (text.size() > 256) {
      text.clear();
      textBytes = 0;
    }
    return true;
  }

  void StreamControls::PanelCache::publish(FfmpegStreamBackend &backend, bool keyboard, bool dockTop, bool focusOnly, int scale) const {
    SDL_RenderPresent(renderer.get());
    if (focusOnly) {
      for (const auto &area : damage) {
        backend.presentation().update_color_overlay(area);
      }
    } else {
      VideoOverlayPlacement placement = VideoOverlayPlacement::full_screen;
      if (keyboard) {
        placement = dockTop ? VideoOverlayPlacement::dock_top : VideoOverlayPlacement::dock_bottom;
      }
      backend.presentation().set_color_overlay_surface(surface, placement, keyboard, scale);
    }
  }

  bool StreamControls::render_menu(FfmpegStreamBackend &backend, TTF_Font *font, int width, int height, bool stats, int scale) {
    if (font == nullptr) {
      return false;
    }
    width /= scale;
    height /= scale;
    const bool keyboard = menu_.page == input::StreamMenuPage::keyboard;
    const int rowHeight = std::max(TTF_GetFontLineSkip(font) + 8, width * 9 / 200);
    const int safeBottom = std::max(8, height / 40);
    width = keyboard ? width * 9 / 10 : width;
    height = keyboard ? std::min(height - VideoOverlay::MARGIN * 2, rowHeight * input::StreamMenu::KEYBOARD_ROWS + 28 + TTF_GetFontLineSkip(font) + (TTF_GetFontLineSkip(font) + 8) + safeBottom) : height;
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (!panel_.prepare(backend, width, height)) {
      logging::warn("input", "Unable to allocate the " + std::to_string(width) + "x" + std::to_string(height) + " stream controls panel: " + SDL_GetError());
      return false;
    }
    const auto &renderer = panel_.renderer;
    const PanelContent content {font, navigation_.inputDevice, menu_.page, menu_.keyboardLayout, menu_.modifiers, menu_.caps, menu_.shiftOnce, stats, mouseDevice_, keyboardTop_, panel_.message};
    const bool focusOnly = panel_.content && *panel_.content == content && (keyboard || menu_.page == input::StreamMenuPage::radial);
    if (focusOnly && panel_.selection == (keyboard ? menu_.keyIndex : static_cast<int>(menu_.selected))) {
      panel_.dirty = false;
      return true;
    }
    panel_.damage.clear();
    const Uint8 background = keyboard ? 16 : 0;
    if (!focusOnly) {
      SDL_SetRenderDrawColor(renderer.get(), background, background, background, keyboard ? 255 : 150);
      SDL_RenderClear(renderer.get());
    }
    const int line = TTF_GetFontLineSkip(font) + 8;
    if (!focusOnly && menu_.page == input::StreamMenuPage::radial) {
      panel_.label(renderer.get(), font, "Stream controls", {8, 4, width - 16, line}, {false, false});
    }
    if (keyboard) {
      render_keyboard(renderer.get(), font, width, height, rowHeight, focusOnly);
    } else if (menu_.page == input::StreamMenuPage::radial) {
      render_wheels(renderer.get(), font, width, height, stats, focusOnly);
    } else if (menu_.page == input::StreamMenuPage::logs) {
      render_logs(renderer.get(), font, width, height);
    } else {
      render_exit_confirmation(renderer.get(), font, width, height);
    }
    if (!focusOnly && !panel_.message.empty() && menu_.page == input::StreamMenuPage::radial) {
      panel_.label(renderer.get(), font, panel_.message, {4, line + 4, width - 8, line * 2}, {false, false});
    }
    panel_.publish(backend, keyboard, keyboardTop_, focusOnly, scale);
    panel_.content = content;
    panel_.selection = keyboard ? menu_.keyIndex : static_cast<int>(menu_.selected);
    panel_.dirty = false;
    return true;
  }
}  // namespace streaming
