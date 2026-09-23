#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "platform/i_platform_host.h"

namespace sz::test {

// In-memory IOverlayWindow: records what was called instead of doing
// anything OS-specific, so core logic can be tested without a real window.
class FakeOverlayWindow final : public platform::IOverlayWindow {
public:
    bool EnsureCreated(const platform::DisplayInfo& display) override {
        ++ensureCreatedCallCount;
        if (createSucceeds && !created) {
            created = true;
            onDisplay = display;
        }
        return createSucceeds;
    }

    // Like the real thing, only a created window has anywhere to move.
    void MoveToDisplay(const platform::DisplayInfo& display) override {
        if (created) {
            onDisplay = display;
        }
    }

    // Fired by a test to stand for the OS reporting that the displays changed.
    void SetDisplaysChangedCallback(std::function<void()> callback) override {
        displaysChangedCallback = std::move(callback);
    }
    std::function<void()> displaysChangedCallback;

    void Show() override {
        ++showCallCount;
        visible = true;
    }

    void ShowWithoutActivating() override {
        ++showWithoutActivatingCallCount;
        Show();
    }

    void Hide() override {
        ++hideCallCount;
        visible = false;
        if (forgetUnderlyingAppOnHide) {
            underlyingApp = {};
        }
    }

    // Models what a real window does when it hides itself mid-session: the
    // foreground moves on, and by the time anyone asks again there may be
    // nothing identifiable underneath - especially when the overlay took the
    // foreground itself on the way out. Off by default, because most tests
    // want a stable answer; on for the ones about surviving a restart. See
    // app::TrayController::sessionApp_.
    bool forgetUnderlyingAppOnHide = false;

    bool IsVisible() const override { return visible; }

    // Settable, so a test can say what the overlay is up over - see
    // ForegroundApp.
    platform::ForegroundApp underlyingApp;
    platform::ForegroundApp UnderlyingApplication() const override { return underlyingApp; }

    void SetInputPassthrough(bool enabled) override { inputPassthrough = enabled; }

    void SetEditModeNoActivate(bool enabled) override {
        ++setEditModeNoActivateCallCount;
        editModeNoActivate = enabled;
    }

    void SetEditModeInput(const platform::EditModeInputOptions& options) override {
        ++setEditModeInputCallCount;
        editModeInput = options;
    }

    void SetInputOptionsHudDigits(int /*digitCount*/) override {}

    void SetCursorShape(platform::CursorShape shape) override {
        ++setCursorShapeCallCount;
        cursorShape = shape;
    }

    void RequestTextInput() override { ++requestTextInputCallCount; }
    void ReleaseTextInput() override { ++releaseTextInputCallCount; }

    void SetFrameCallback(platform::FrameCallback callback) override { frameCallback = std::move(callback); }
    void SetFramePacing(platform::FramePacing pacing) override { framePacing = pacing; }
    platform::FramePacing framePacing = platform::FramePacing::EveryFrame;
    void SetMouseCallback(platform::MouseCallback callback) override { mouseCallback = std::move(callback); }

    platform::InputGrabDiagnostics GetInputGrabDiagnostics() const override { return {}; }

    platform::CaptureResult CaptureRegionAsTexture(const platform::Rect& rect) override {
        ++captureCallCount;
        lastCaptureRect = rect;
        platform::CaptureResult result;
        result.textureHandle = captureReturnsHandle;
        result.pixelsRGBA = captureReturnsPixelsRGBA;
        result.width = captureReturnsWidth;
        result.height = captureReturnsHeight;
        return result;
    }

    uint64_t CreateTextureFromPixels(const uint8_t* /*pixelsRGBA*/, int /*width*/, int /*height*/) override {
        return createTextureFromPixelsReturnsHandle;
    }

    bool UpdateTextureRegion(uint64_t textureHandle, const uint8_t* /*pixelsRGBA*/, int /*sourceWidth*/, int /*x*/,
                              int /*y*/, int /*w*/, int /*h*/) override {
        return textureHandle != 0;
    }

    void ReleaseTexture(uint64_t /*textureHandle*/) override { ++releaseTextureCallCount; }

    // Never run - nothing renders the fake's draw lists - only looked for
    // among their commands.
    static void FakeImageFilterCallback(const ImDrawList* /*parentList*/, const ImDrawCmd* /*cmd*/) {}
    platform::DrawCallback ImageFilterCallback() const override { return &FakeImageFilterCallback; }

    void Destroy() override {
        created = false;
        visible = false;
    }

    bool created = false;
    bool visible = false;
    bool inputPassthrough = false;
    bool editModeNoActivate = false;
    int setEditModeNoActivateCallCount = 0;
    platform::EditModeInputOptions editModeInput;
    int setEditModeInputCallCount = 0;
    int setCursorShapeCallCount = 0;
    // A text field borrows the keyboard from the game for as long as it is
    // open; the two have to balance, or the game is left without it.
    int requestTextInputCallCount = 0;
    int releaseTextInputCallCount = 0;
    platform::CursorShape cursorShape = platform::CursorShape::Default;
    // Off, the window is never made: what the OS answers when it will not
    // give the overlay a window, and every show and capture has to cope.
    bool createSucceeds = true;
    int ensureCreatedCallCount = 0;
    int showCallCount = 0;
    // Counted separately, though it shows the same way here: which of the
    // two a caller used is the whole difference between taking focus and
    // leaving it alone on the real thing.
    int showWithoutActivatingCallCount = 0;
    int hideCallCount = 0;
    // The display the window covers: the one it was created on, until moved.
    platform::DisplayInfo onDisplay{};
    platform::FrameCallback frameCallback;
    platform::MouseCallback mouseCallback;

    // What a capture comes back with. 0 and nothing by default: a backend
    // that cannot capture, which is the ordinary "couldn't" every caller
    // has to take. A handle alone is a capture with nothing to save; with
    // pixels and a size it is what a real backend returns, and those go to
    // the library as the snippet's image.
    uint64_t captureReturnsHandle = 0;
    std::vector<uint8_t> captureReturnsPixelsRGBA;
    int captureReturnsWidth = 0;
    int captureReturnsHeight = 0;
    int captureCallCount = 0;
    platform::Rect lastCaptureRect{};
    int releaseTextureCallCount = 0;
    // 0 by default, same as a capture: an upload that fails. A crop out of
    // a frozen screen is only kept when its upload succeeds.
    uint64_t createTextureFromPixelsReturnsHandle = 0;
};

// In-memory IPlatformHost paired with FakeOverlayWindow. Exposes
// TriggerHotkey()/TriggerTrayCommand() so tests can simulate the events a
// real Win32PlatformHost would deliver.
class FakePlatformHost final : public platform::IPlatformHost {
public:
    bool Initialize(const std::string& /*appName*/) override { return true; }

    // On by default; a test turns it off to stand for another copy of the
    // app already running.
    bool AcquireSingleInstance() override { return singleInstanceAvailable; }
    bool singleInstanceAvailable = true;

    bool ShowTrayIcon() override {
        trayIconShown = showTrayIconSucceeds;
        return showTrayIconSucceeds;
    }

    void RemoveTrayIcon() override { trayIconShown = false; }

    void SetTrayCommandCallback(platform::TrayCommandCallback callback) override {
        trayCallback = std::move(callback);
    }

    // Supports any number of simultaneously-registered hotkeys (like every
    // real backend does), each independently triggerable via
    // TriggerHotkey(id) - id is whatever this call returns.
    int RegisterGlobalHotkey(const platform::KeyCombo& combo, platform::HotkeyCallback callback) override {
        if (!registerHotkeySucceeds) {
            return 0;
        }
        const int id = nextHotkeyId++;
        registeredCombos[id] = combo;
        hotkeyCallbacks[id] = std::move(callback);
        return id;
    }

    void UnregisterGlobalHotkey(int hotkeyId) override {
        hotkeyCallbacks.erase(hotkeyId);
        registeredCombos.erase(hotkeyId);
    }

    platform::IOverlayWindow& GetOverlayWindow() override { return overlayWindow; }

    std::vector<platform::DisplayInfo> ListDisplays() const override { return displays; }

    // Empty by default, same "no persistence" convention as
    // dataDirectoryPath just below (see TrayController::OnSettingsChanged)
    // - a test opts in by pointing this at a real (typically temporary)
    // file path.
    std::filesystem::path GetConfigFilePath() const override { return configFilePath; }
    std::filesystem::path configFilePath;

    // Empty by default: TrayController treats this as "no persistence"
    // (see its own Initialize) unless a test explicitly points it at a
    // real (typically temporary) directory.
    std::filesystem::path GetDataDirectoryPath() const override { return dataDirectoryPath; }
    std::filesystem::path dataDirectoryPath;

    // Recorded rather than run: a test fires it with FireBackgroundTimer,
    // standing for the interval having passed. 0 means none is set.
    void SetBackgroundTimer(int intervalMs, std::function<void()> callback) override {
        backgroundTimerIntervalMs = intervalMs > 0 && callback ? intervalMs : 0;
        backgroundTimerCallback = backgroundTimerIntervalMs > 0 ? std::move(callback) : nullptr;
    }
    void FireBackgroundTimer() {
        if (backgroundTimerCallback) {
            backgroundTimerCallback();
        }
    }
    int backgroundTimerIntervalMs = 0;
    std::function<void()> backgroundTimerCallback;

    void SetSessionEndCallback(std::function<void()> callback) override { sessionEndCallback = std::move(callback); }
    // What the OS does on logoff or shutdown.
    void TriggerSessionEnd() {
        if (sessionEndCallback) {
            sessionEndCallback();
        }
    }
    std::function<void()> sessionEndCallback;

    int RunEventLoop() override { return exitCode; }

    void Quit(int code) override {
        quitCalled = true;
        exitCode = code;
    }

    // Everything a controller registered while it was alive: its hotkeys,
    // and the overlay window's frame/mouse callbacks. Called between a
    // shutdown and a restart of the app under test - the registrations
    // hold pointers into the controller that has just been destroyed, and
    // a hotkey fired through a stale id runs into freed memory. Leaves
    // what a *test* configured alone (the displays, what is
    // underneath, which calls are made to fail).
    void ForgetRegistrations() {
        hotkeyCallbacks.clear();
        registeredCombos.clear();
        trayCallback = nullptr;
        overlayWindow.frameCallback = nullptr;
        overlayWindow.mouseCallback = nullptr;
        overlayWindow.displaysChangedCallback = nullptr;
        backgroundTimerCallback = nullptr;
        backgroundTimerIntervalMs = 0;
        sessionEndCallback = nullptr;
    }

    // Test-only helpers to simulate what a real backend would deliver.
    void TriggerHotkey(int hotkeyId) {
        auto it = hotkeyCallbacks.find(hotkeyId);
        if (it != hotkeyCallbacks.end() && it->second) {
            it->second();
        }
    }
    void TriggerTrayCommand(platform::TrayCommand command) {
        if (trayCallback) {
            trayCallback(command);
        }
    }

    FakeOverlayWindow overlayWindow;
    platform::TrayCommandCallback trayCallback;
    std::unordered_map<int, platform::HotkeyCallback> hotkeyCallbacks;
    std::unordered_map<int, platform::KeyCombo> registeredCombos;
    // One ordinary display unless a test attaches more.
    std::vector<platform::DisplayInfo> displays{
        platform::DisplayInfo{"fake-primary", "Fake Display", 0, 0, 1920, 1080, true, 60, 100}};
    bool trayIconShown = false;
    bool showTrayIconSucceeds = true;
    bool registerHotkeySucceeds = true;
    int nextHotkeyId = 1;
    bool quitCalled = false;
    int exitCode = 0;
};

}  // namespace sz::test
