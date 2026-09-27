#pragma once

#include <windows.h>

#include <deque>
#include <string>
#include <unordered_map>

#include "platform/i_platform_host.h"
#include "platform/win32/win32_overlay_window.h"

namespace sz::platform::win32 {

// Win32 implementation of IPlatformHost: owns a hidden top-level window
// that receives tray-icon callback messages, global hotkey messages and
// the OS's session-end broadcast (top-level rather than message-only for
// that last one - see Initialize), the tray icon itself, and the
// single-threaded event loop that keeps idle CPU usage near zero (see
// RunEventLoop).
class Win32PlatformHost final : public IPlatformHost {
public:
    Win32PlatformHost() = default;
    ~Win32PlatformHost() override;

    bool Initialize(const std::string& appName) override;
    bool AcquireSingleInstance() override;
    bool ShowTrayIcon() override;
    void RemoveTrayIcon() override;
    void SetTrayCommandCallback(TrayCommandCallback callback) override;
    int RegisterGlobalHotkey(const KeyCombo& combo, HotkeyCallback callback) override;
    void UnregisterGlobalHotkey(int hotkeyId) override;
    IOverlayWindow& GetOverlayWindow() override;
    std::vector<DisplayInfo> ListDisplays() const override;
    std::filesystem::path GetConfigFilePath() const override;
    std::filesystem::path GetLibraryPath() const override;
    void SetBackgroundTimer(int intervalMs, std::function<void()> callback) override;
    void SetSessionEndCallback(std::function<void()> callback) override;
    void Post(std::function<void()> task) override;
    int RunEventLoop() override;
    void Quit(int exitCode) override;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ShowTrayContextMenu();
    // Exits as the tray menu's Exit does, for a close asked from outside.
    void Exit();

    std::string appName_;
    HWND hwnd_ = nullptr;
    // See AcquireSingleInstance: held, never released, until the process
    // ends - Windows abandons it for us.
    HANDLE instanceMutex_ = nullptr;
    bool trayIconVisible_ = false;
    // "TaskbarCreated", as registered - see Initialize.
    UINT taskbarCreatedMessage_ = 0;
    TrayCommandCallback trayCallback_;
    std::unordered_map<int, HotkeyCallback> hotkeyCallbacks_;
    std::function<void()> backgroundTimerCallback_;
    std::function<void()> sessionEndCallback_;
    // See Post: waiting for the message each one posted.
    std::deque<std::function<void()>> posted_;
    int nextHotkeyId_ = 1;
    Win32OverlayWindow overlayWindow_;
    bool running_ = false;
    int exitCode_ = 0;
};

// The name of the mutex AcquireSingleInstance claims for appName: in this
// session's namespace, and named for the user this process runs as.
std::wstring InstanceMutexName(const std::string& appName);

// The host's one tray icon, as the shell knows it: this id on the host
// window.
inline constexpr UINT kTrayIconId = 1;

}  // namespace sz::platform::win32
