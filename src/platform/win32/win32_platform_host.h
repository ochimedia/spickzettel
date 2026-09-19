#pragma once

#include <windows.h>

#include <string>
#include <unordered_map>

#include "platform/i_platform_host.h"
#include "platform/win32/win32_overlay_window.h"

namespace sz::platform::win32 {

// Win32 implementation of IPlatformHost: owns a hidden message-only window
// that receives tray-icon callback messages and global hotkey messages, the
// tray icon itself, and the single-threaded event loop that keeps idle CPU
// usage near zero (see RunEventLoop).
class Win32PlatformHost final : public IPlatformHost {
public:
    Win32PlatformHost() = default;
    ~Win32PlatformHost() override;

    bool Initialize(const std::string& appName) override;
    bool ShowTrayIcon() override;
    void RemoveTrayIcon() override;
    void SetTrayCommandCallback(TrayCommandCallback callback) override;
    int RegisterGlobalHotkey(const KeyCombo& combo, HotkeyCallback callback) override;
    void UnregisterGlobalHotkey(int hotkeyId) override;
    IOverlayWindow& GetOverlayWindow() override;
    std::vector<DisplayInfo> ListDisplays() const override;
    std::filesystem::path GetConfigFilePath() const override;
    std::filesystem::path GetDataDirectoryPath() const override;
    int RunEventLoop() override;
    void Quit(int exitCode) override;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ShowTrayContextMenu();

    std::string appName_;
    HWND hwnd_ = nullptr;
    bool trayIconVisible_ = false;
    TrayCommandCallback trayCallback_;
    std::unordered_map<int, HotkeyCallback> hotkeyCallbacks_;
    int nextHotkeyId_ = 1;
    Win32OverlayWindow overlayWindow_;
    bool running_ = false;
    int exitCode_ = 0;
};

}  // namespace sz::platform::win32
