/**
 * @file src/streaming/session.h
 * @brief Declares the Xbox streaming session runtime.
 */
#pragma once

// standard includes
#include <string>
#include <vector>

// local includes
#include "src/app/client_state.h"
#include "src/app/host_records.h"
#include "src/network/host_pairing.h"
#include "src/startup/video_mode.h"

struct SDL_Window;
typedef struct _SDL_GameController SDL_GameController;  ///< SDL controller handle shared with the shell without including SDL platform headers.

namespace streaming {

  /**
   * @brief Result returned to the shell after the stream resources are released.
   */
  struct StreamSessionReport {
    SDL_GameController *controller = nullptr;  ///< Owned controller reference to keep or close on return, or nullptr.
    std::string message;  ///< Final session status or reason startup failed.
    std::vector<std::string> summaryLines;  ///< Outcome and statistics for the app picker's summary dialog.
  };

  /**
   * @brief Run one Xbox streaming session for the selected host app.
   *
   * The session displays a progress dialog while launching or resuming the selected app,
   * establishing the connection and waiting for video. It starts the
   * Moonlight transport runtime, decodes negotiated video and Opus audio with
   * FFmpeg, forwards controller input, renders the latest decoded frame with a
   * lightweight overlay, and returns once the user stops streaming or the host
   * terminates the session.
   * The shell retains its controller reference until its own resources have been
   * restored; the session acquires a separate reference to that same device. This
   * keeps the Xbox USB input pipe alive across both render-resource transitions.
   *
   * @param window Shared SDL window reused from the shell.
   * @param videoMode Active Xbox video mode.
   * @param settings Active stream preferences; receives OSK placement changes for persistence on return to the shell.
   * @param host Selected paired host.
   * @param app Selected host app.
   * @param clientIdentity Paired client identity used for authenticated launch requests.
   * @param preferredController Controller retained by the shell throughout the session, or nullptr to use the first available device.
   * @param report Output receiving the final status, summary and an owned controller reference when available.
   * @return True when the stream session started successfully.
   */
  bool run_stream_session(
    SDL_Window *window,
    const VIDEO_MODE &videoMode,
    app::SettingsState &settings,
    const app::HostRecord &host,
    const app::HostAppRecord &app,
    const network::PairingIdentity &clientIdentity,
    SDL_GameController *preferredController,
    StreamSessionReport &report
  );

}  // namespace streaming
