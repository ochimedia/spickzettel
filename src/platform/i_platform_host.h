#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "platform/i_overlay_window.h"
#include "platform/platform_types.h"

namespace sz::platform {

// The platform-specific host of the application: the tray icon, global
// hotkey registration, the overlay window and the OS event loop. Nothing
// above this layer includes an OS header.
class IPlatformHost {
public:
    virtual ~IPlatformHost() = default;

    virtual bool Initialize(const std::string& appName) = 0;

    // Claims this user's one running copy of the app, for the life of the
    // process; false if another copy holds it. Two instances would write
    // one library from two stale pictures of it, through the same
    // temp-file names - so the second never gets as far as loading it.
    // See TrayController::Initialize.
    virtual bool AcquireSingleInstance() = 0;

    // Puts the tray icon up, now or once the taskbar is there to take it:
    // false while it is not up yet. RemoveTrayIcon takes it down, and
    // stops the waiting.
    virtual bool ShowTrayIcon() = 0;
    virtual void RemoveTrayIcon() = 0;
    virtual void SetTrayCommandCallback(TrayCommandCallback callback) = 0;

    // Registers a global hotkey; returns a non-zero id, or 0 when the
    // combination could not be registered (another application owns it).
    virtual int RegisterGlobalHotkey(const KeyCombo& combo, HotkeyCallback callback) = 0;
    virtual void UnregisterGlobalHotkey(int hotkeyId) = 0;
    // While true, no hotkey fires and the press of one reaches the overlay
    // as any other key does - for a row in Settings waiting for a key,
    // which would otherwise never see a combination a hotkey holds, and
    // whose press would run that hotkey. Each keeps its id, and is
    // registered again as the pause ends.
    virtual void SetHotkeysPaused(bool paused) = 0;

    virtual IOverlayWindow& GetOverlayWindow() = 0;

    // Every display attached right now. Asked afresh each time rather than
    // cached, since monitors come and go while the app runs. Which one the
    // overlay goes on is decided above this layer - see core::ChooseDisplay.
    virtual std::vector<DisplayInfo> ListDisplays() const = 0;

    // Per-user location of the config file (%APPDATA%\Spickzettel\config.json
    // on Windows). The directory need not exist yet.
    virtual std::filesystem::path GetConfigFilePath() const = 0;

    // The per-user library file, on this computer: not beside the config
    // file, which roams with the user (%LOCALAPPDATA%\Spickzettel\library.db
    // on Windows). Need not exist yet.
    virtual std::filesystem::path GetLibraryPath() const = 0;
    // Where builds up to 0.2.0 kept the library, beside the config file
    // (%APPDATA%\Spickzettel\library.db on Windows): moved from there at the
    // first start that finds none at GetLibraryPath - see
    // LibraryStore::MoveHereFrom. Empty when there is nowhere to look.
    virtual std::filesystem::path GetFormerLibraryPath() const = 0;

    // Calls `callback` on the app thread every `intervalMs`, until called
    // again with 0. The one clock the app has while the overlay is hidden,
    // when frames stop with the window: what retries a settings file that
    // could not be written. See TrayController::OnBackgroundTimer.
    virtual void SetBackgroundTimer(int intervalMs, std::function<void()> callback) = 0;

    // Called on the app thread when the OS is ending the user's session -
    // logging off, shutting down - before the process is taken down. The
    // last chance to finish what is in flight and write a settings file
    // still owed; see TrayController::OnSessionEnding.
    virtual void SetSessionEndCallback(std::function<void()> callback) = 0;

    // Called on the app thread when the app is started again while this
    // copy runs: the other copy, refused the single instance, handed its
    // start over (see PassOpeningToRunningCopy). See
    // TrayController::OnOpenedAgain.
    virtual void SetOpenedAgainCallback(std::function<void()> callback) = 0;
    // For a copy that could not take the single instance: hands its start
    // to the copy that holds it - the same user's - which is asked to come
    // up, and may take the foreground to do so. False when no running copy
    // could be reached.
    virtual bool PassOpeningToRunningCopy() = 0;

    // Runs `task` on the app thread after the current frame or message, and
    // before the next frame. For what is asked for from inside a frame but
    // changes what the frame is part of - the overlay's state, the window
    // (see docs/OVERLAY_STATES.md, section 8). Tasks run in the order they
    // were posted; one posted by a task runs after the next message.
    virtual void Post(std::function<void()> task) = 0;

    // Runs the OS event loop until Quit() is called; returns the exit code.
    virtual int RunEventLoop() = 0;
    virtual void Quit(int exitCode = 0) = 0;
};

// Implemented once per platform backend. With a `dataDir`, the settings,
// the library and the crash dumps are kept in that one folder rather than
// the user's own (see app::CommandLine::dataDir).
std::unique_ptr<IPlatformHost> CreatePlatformHost(std::filesystem::path dataDir = {});

}  // namespace sz::platform
