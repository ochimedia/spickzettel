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

    virtual bool ShowTrayIcon() = 0;
    virtual void RemoveTrayIcon() = 0;
    virtual void SetTrayCommandCallback(TrayCommandCallback callback) = 0;

    // Registers a global hotkey; returns a non-zero id, or 0 when the
    // combination could not be registered (another application owns it).
    virtual int RegisterGlobalHotkey(const KeyCombo& combo, HotkeyCallback callback) = 0;
    virtual void UnregisterGlobalHotkey(int hotkeyId) = 0;

    virtual IOverlayWindow& GetOverlayWindow() = 0;

    // Every display attached right now. Asked afresh each time rather than
    // cached, since monitors come and go while the app runs. Which one the
    // overlay goes on is decided above this layer - see core::ChooseDisplay.
    virtual std::vector<DisplayInfo> ListDisplays() const = 0;

    // Per-user location of the config file (%APPDATA%\Spickzettel\config.json
    // on Windows). The directory need not exist yet.
    virtual std::filesystem::path GetConfigFilePath() const = 0;

    // The per-user library file, a sibling of the config file
    // (%APPDATA%\Spickzettel\library.db on Windows). Need not exist yet.
    virtual std::filesystem::path GetLibraryPath() const = 0;

    // Calls `callback` on the app thread every `intervalMs`, until called
    // again with 0. The one clock the app has while the overlay is hidden:
    // frames stop with the window, and the debounced autosave runs on
    // frames, so a save that failed on the way to hidden - or a capture
    // taken there - would otherwise wait for the next show to be retried.
    // See TrayController::OnBackgroundTimer.
    virtual void SetBackgroundTimer(int intervalMs, std::function<void()> callback) = 0;

    // Called on the app thread when the OS is ending the user's session -
    // logging off, shutting down - before the process is taken down. The
    // last chance to write what is unsaved; see TrayController::OnSessionEnding.
    virtual void SetSessionEndCallback(std::function<void()> callback) = 0;

    // Runs the OS event loop until Quit() is called; returns the exit code.
    virtual int RunEventLoop() = 0;
    virtual void Quit(int exitCode = 0) = 0;
};

// Implemented once per platform backend.
std::unique_ptr<IPlatformHost> CreatePlatformHost();

}  // namespace sz::platform
