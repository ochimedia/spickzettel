#pragma once

#include <filesystem>
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

    // Per-user root of the persisted library, a sibling of the config file
    // (%APPDATA%\Spickzettel\library\ on Windows). Need not exist yet.
    virtual std::filesystem::path GetDataDirectoryPath() const = 0;

    // Runs the OS event loop until Quit() is called; returns the exit code.
    virtual int RunEventLoop() = 0;
    virtual void Quit(int exitCode = 0) = 0;
};

// Implemented once per platform backend.
std::unique_ptr<IPlatformHost> CreatePlatformHost();

}  // namespace sz::platform
