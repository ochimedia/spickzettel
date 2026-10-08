#pragma once

#include <windows.h>

#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

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
    // Keeping the settings, the library and the crash dumps in `dataDir`
    // rather than the user's folders - see CreatePlatformHost.
    explicit Win32PlatformHost(std::filesystem::path dataDir) : dataDir_(std::move(dataDir)) {}
    ~Win32PlatformHost() override;

    bool Initialize(const std::string& appName) override;
    bool AcquireSingleInstance() override;
    bool ShowTrayIcon() override;
    void RemoveTrayIcon() override;
    void SetTrayCommandCallback(TrayCommandCallback callback) override;
    int RegisterGlobalHotkey(const KeyCombo& combo, HotkeyCallback callback) override;
    void UnregisterGlobalHotkey(int hotkeyId) override;
    void SetHotkeysPaused(bool paused) override;
    IOverlayWindow& GetOverlayWindow() override;
    std::vector<DisplayInfo> ListDisplays() const override;
    std::filesystem::path GetConfigFilePath() const override;
    std::filesystem::path GetLibraryPath() const override;
    std::filesystem::path GetFormerLibraryPath() const override;
    void SetBackgroundTimer(int intervalMs, std::function<void()> callback) override;
    void SetSessionEndCallback(std::function<void()> callback) override;
    void SetOpenedAgainCallback(std::function<void()> callback) override;
    bool PassOpeningToRunningCopy() override;
    void Post(std::function<void()> task) override;
    int RunEventLoop() override;
    void Quit(int exitCode) override;

    // The next `count` tray icon adds fail as the shell's would with no
    // taskbar up yet - for the tests, which have a taskbar.
    void FailTrayIconAddsForTesting(int count) { failTrayIconAdds_ = count; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ShowTrayContextMenu();
    // Hands the shell the icon, as ShowTrayIcon asked. A failed add is
    // tried again: on the TaskbarCreated a starting taskbar sends, and
    // every kTrayRetryMs until one lands.
    bool AddTrayIcon();
    // Exits as the tray menu's Exit does, for a close asked from outside.
    void Exit();

    std::string appName_;
    // Empty for the user's own folders.
    std::filesystem::path dataDir_;
    HWND hwnd_ = nullptr;
    // See AcquireSingleInstance: held, never released, until the process
    // ends - Windows abandons it for us.
    HANDLE instanceMutex_ = nullptr;
    bool trayIconVisible_ = false;
    // Asked for by ShowTrayIcon and not taken back by RemoveTrayIcon:
    // what a failed add is tried again for - see AddTrayIcon.
    bool trayIconWanted_ = false;
    int failTrayIconAdds_ = 0;
    // "TaskbarCreated", as registered - see Initialize.
    UINT taskbarCreatedMessage_ = 0;
    // What a copy started again posts - see PassOpeningToRunningCopy.
    UINT openedAgainMessage_ = 0;
    TrayCommandCallback trayCallback_;
    std::unordered_map<int, HotkeyCallback> hotkeyCallbacks_;
    // What each was registered with - RegisterHotKey's modifiers and key -
    // to register it again after a pause.
    std::unordered_map<int, std::pair<UINT, UINT>> hotkeyKeys_;
    bool hotkeysPaused_ = false;
    std::function<void()> backgroundTimerCallback_;
    std::function<void()> sessionEndCallback_;
    std::function<void()> openedAgainCallback_;
    // See Post: waiting for the message each one posted.
    std::deque<std::function<void()>> posted_;
    int nextHotkeyId_ = 1;
    Win32OverlayWindow overlayWindow_;
    // Set by Quit, and by a WM_QUIT: the loop ends, or does not start.
    bool quitting_ = false;
    int exitCode_ = 0;
};

// The name of the mutex AcquireSingleInstance claims for appName: in this
// session's namespace, and named for the user this process runs as.
std::wstring InstanceMutexName(const std::string& appName);
// The host window's title: the instance's name, without the namespace -
// what a copy started again looks for (see PassOpeningToRunningCopy).
std::string InstanceWindowTitle(const std::string& appName);

// The host's one tray icon, as the shell knows it: this id on the host
// window.
inline constexpr UINT kTrayIconId = 1;
// See SetBackgroundTimer: a WM_TIMER on the message window, which is
// pumped whether or not the overlay is up.
inline constexpr UINT_PTR kBackgroundTimerId = 1;
// The WM_TIMER that tries a failed tray icon add again, and how often.
inline constexpr UINT_PTR kTrayRetryTimerId = 2;
inline constexpr UINT kTrayRetryMs = 5000;

}  // namespace sz::platform::win32
