#pragma once

#include <algorithm>
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
    // The calls that bring the window up, take it down and decide how it
    // takes input, in the order they were made - see docs/OVERLAY_STATES.md,
    // section 6. The order is what a count cannot tell: a show followed by
    // passthrough is edit mode in between. Frame-by-frame calls (pacing, the
    // cursor, textures) are left out. A test clears it before the request
    // it looks at. Mutable because UnderlyingApplication, a const call, is
    // logged too.
    mutable std::vector<std::string> calls;

    bool EnsureCreated(const platform::DisplayInfo& display) override {
        calls.push_back("EnsureCreated");
        ++ensureCreatedCallCount;
        if (createSucceeds && !created) {
            created = true;
            onDisplay = display;
        }
        return createSucceeds;
    }

    // Like the real thing, only a created window has anywhere to move.
    void MoveToDisplay(const platform::DisplayInfo& display) override {
        calls.push_back("MoveToDisplay");
        if (created) {
            onDisplay = display;
        }
    }

    // Fired by a test to stand for the OS reporting that the displays changed.
    void SetDisplaysChangedCallback(std::function<void()> callback) override {
        displaysChangedCallback = std::move(callback);
    }
    std::function<void()> displaysChangedCallback;

    void Present(platform::Presentation presentation) override {
        switch (presentation) {
            case platform::Presentation::Hidden:
                calls.push_back("Present(Hidden)");
                break;
            case platform::Presentation::ClickThrough:
                calls.push_back("Present(ClickThrough)");
                break;
            case platform::Presentation::Interactive:
                calls.push_back("Present(Interactive)");
                break;
        }
        if (!created) {
            return;  // like the real thing: nothing to present
        }
        const bool up = presentation != platform::Presentation::Hidden;
        if (up && !visible) {
            ++showCallCount;
            if (presentation == platform::Presentation::ClickThrough) {
                ++showClickThroughCallCount;
            }
        }
        if (!up && visible) {
            ++hideCallCount;
            if (forgetUnderlyingAppOnHide) {
                underlyingApp = {};
            }
        }
        visible = up;
        if (up) {
            inputPassthrough = presentation == platform::Presentation::ClickThrough;
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
    int ScalePercent() const override { return scalePercent; }

    // Settable, so a test can say what the overlay is up over - see
    // ForegroundApp.
    platform::ForegroundApp underlyingApp;
    platform::ForegroundApp UnderlyingApplication() const override {
        calls.push_back("UnderlyingApplication");
        return underlyingApp;
    }

    void SetEditModeNoActivate(bool enabled) override {
        calls.push_back(enabled ? "NoActivate(on)" : "NoActivate(off)");
        ++setEditModeNoActivateCallCount;
        editModeNoActivate = enabled;
    }

    void SetEditModeInput(const platform::EditModeInputOptions& options) override {
        calls.push_back("EditModeInput");
        ++setEditModeInputCallCount;
        editModeInput = options;
    }

    void SetCursorShape(platform::CursorShape shape) override {
        ++setCursorShapeCallCount;
        cursorShape = shape;
    }

    void RequestTextInput() override { ++requestTextInputCallCount; }
    void ReleaseTextInput() override { ++releaseTextInputCallCount; }

    void SetFrameCallback(platform::FrameCallback callback) override { frameCallback = std::move(callback); }
    void SetFramePacing(platform::FramePacing pacing) override { framePacing = pacing; }
    platform::FramePacing framePacing = platform::FramePacing::EveryFrame;
    void SetInputCallback(platform::InputCallback callback) override { inputCallback = std::move(callback); }

    platform::InputGrabDiagnostics GetInputGrabDiagnostics() const override { return {}; }

    platform::CaptureResult CaptureRegion(const platform::Rect& rect) override {
        calls.push_back("Capture");
        ++captureCallCount;
        lastCaptureRect = rect;
        platform::CaptureResult result;
        result.pixelsRGBA = captureReturnsPixelsRGBA;
        result.width = captureReturnsWidth;
        result.height = captureReturnsHeight;
        return result;
    }

    // Each upload a handle of its own, held in liveTextures - with the
    // device it was made on - until it is released.
    uint64_t CreateTextureFromPixels(const uint8_t* pixelsRGBA, int width, int height) override {
        if (!uploadsSucceed || pixelsRGBA == nullptr || width <= 0 || height <= 0) {
            return 0;
        }
        ++uploadCount;
        const uint64_t handle = nextTextureHandle++;
        liveTextures.emplace(handle, textureGeneration);
        return handle;
    }

    bool UpdateTextureRegion(uint64_t textureHandle, const uint8_t* /*pixelsRGBA*/, int /*sourceWidth*/, int /*x*/,
                              int /*y*/, int /*w*/, int /*h*/) override {
        const auto it = liveTextures.find(textureHandle);
        if (it == liveTextures.end() || it->second != textureGeneration) {
            ++badTextureUses;  // freed, or made on a device that is gone
            return false;
        }
        return true;
    }

    void ReleaseTexture(uint64_t textureHandle) override {
        if (textureHandle == 0) {
            return;
        }
        ++releaseTextureCallCount;
        if (liveTextures.erase(textureHandle) == 0) {
            ++badTextureUses;  // released twice, or never made
        }
    }

    uint64_t TextureGeneration() const override { return textureGeneration; }
    // As the real window: a change makes every texture again.
    void SetMipmapsWanted(bool wanted) override {
        if (wanted != mipmapsWanted) {
            mipmapsWanted = wanted;
            ++textureGeneration;
        }
    }
    bool mipmapsWanted = false;

    // Whether `handle` is one that may be drawn with now: made, not
    // released, and on the device there is.
    bool IsDrawable(uint64_t handle) const {
        const auto it = liveTextures.find(handle);
        return it != liveTextures.end() && it->second == textureGeneration;
    }

    // Never run - nothing renders the fake's draw lists - only looked for
    // among their commands, by address. Each counts into a counter of its
    // own so that no two have the same body: a release build folds
    // identical functions into one (/OPT:ICF), and three empty ones came
    // to one address, which told a stroke's depth from its layer no more.
    static inline int callbacksRun[3] = {};
    static void FakeImageFilterCallback(const ImDrawList* /*parentList*/, const ImDrawCmd* /*cmd*/) {
        ++callbacksRun[0];
    }
    platform::DrawCallback ImageFilterCallback() const override { return &FakeImageFilterCallback; }
    static void FakeStrokeDepthCallback(const ImDrawList* /*parentList*/, const ImDrawCmd* /*cmd*/) {
        ++callbacksRun[1];
    }
    platform::DrawCallback StrokeDepthCallback() const override { return &FakeStrokeDepthCallback; }
    static void FakeStrokeLayerCallback(const ImDrawList* /*parentList*/, const ImDrawCmd* /*cmd*/) {
        ++callbacksRun[2];
    }
    platform::DrawCallback StrokeLayerCallback() const override { return &FakeStrokeLayerCallback; }

    void Destroy() override {
        calls.push_back("Destroy");
        created = false;
        visible = false;
    }

    bool created = false;
    bool visible = false;
    // What the system's display scale is, for a test of the UI scale.
    int scalePercent = 100;
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
    // How often the window came up, how often of those click-through - the
    // whole difference between taking focus and leaving it alone on the
    // real thing - and how often it went down.
    int showCallCount = 0;
    int showClickThroughCallCount = 0;
    int hideCallCount = 0;
    // The display the window covers: the one it was created on, until moved.
    platform::DisplayInfo onDisplay{};
    platform::FrameCallback frameCallback;
    platform::InputCallback inputCallback;

    // What a capture comes back with. Nothing by default: a backend that
    // cannot capture, which is the ordinary "couldn't" every caller has to
    // take. With pixels and a size it is what a real backend returns, and
    // those go to the library as the snippet's image.
    std::vector<uint8_t> captureReturnsPixelsRGBA;
    int captureReturnsWidth = 0;
    int captureReturnsHeight = 0;
    // Captures that come back with a picture - a small gray one, whatever
    // was asked for - for the tests that need a capture to have worked.
    void CapturesTakePictures() {
        captureReturnsWidth = 4;
        captureReturnsHeight = 4;
        captureReturnsPixelsRGBA.assign(4 * 4 * 4, 128);
    }
    int captureCallCount = 0;
    platform::Rect lastCaptureRect{};
    // Off by default, like a capture: no device to upload to.
    bool uploadsSucceed = false;
    uint64_t nextTextureHandle = 1;
    int uploadCount = 0;
    int releaseTextureCallCount = 0;
    // Every texture made and not yet released, with the device (the
    // generation) it was made on.
    std::unordered_map<uint64_t, uint64_t> liveTextures;
    // A texture released twice, or updated after it was released or its
    // device replaced. Never anything but 0.
    int badTextureUses = 0;
    // Moved on by a test to lose every texture, as a device reset does.
    uint64_t textureGeneration = 0;
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
    // TriggerHotkey(id) - id is whatever this call returns. An unset combo
    // is refused, as Win32PlatformHost refuses it: accepted here, a test
    // could unbind a hotkey through ChangeHotkey that the app cannot.
    int RegisterGlobalHotkey(const platform::KeyCombo& combo, platform::HotkeyCallback callback) override {
        if (!registerHotkeySucceeds || !combo.IsValid()) {
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

    // Empty by default, same "no persistence" convention as libraryPath
    // (see TrayController::PersistConfig) - a test opts in by pointing this
    // at a real (typically temporary) file path.
    std::filesystem::path GetConfigFilePath() const override { return configFilePath; }
    std::filesystem::path configFilePath;

    // Empty by default: TrayController treats this as "no persistence"
    // (see its own Initialize) unless a test explicitly points it at a
    // real (typically temporary) file.
    std::filesystem::path GetLibraryPath() const override { return libraryPath; }
    std::filesystem::path libraryPath;
    // Empty by default: nowhere to move a library from.
    std::filesystem::path GetFormerLibraryPath() const override { return formerLibraryPath; }
    std::filesystem::path formerLibraryPath;

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

    void SetOpenedAgainCallback(std::function<void()> callback) override { openedAgainCallback = std::move(callback); }
    // What a copy started again sets off in the one running.
    void TriggerOpenedAgain() {
        if (openedAgainCallback) {
            openedAgainCallback();
        }
    }
    std::function<void()> openedAgainCallback;
    bool PassOpeningToRunningCopy() override { return runningCopyAnswers; }
    bool runningCopyAnswers = false;

    // Held until a test runs them - the headless app does after every frame
    // (see HeadlessAppTest::StepFrame), as the real loop does before the
    // next one.
    void Post(std::function<void()> task) override { posted.push_back(std::move(task)); }
    void RunPostedTasks() {
        while (!posted.empty()) {
            const std::function<void()> task = std::move(posted.front());
            posted.erase(posted.begin());
            task();
        }
    }
    std::vector<std::function<void()>> posted;

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
        overlayWindow.inputCallback = nullptr;
        overlayWindow.displaysChangedCallback = nullptr;
        backgroundTimerCallback = nullptr;
        backgroundTimerIntervalMs = 0;
        sessionEndCallback = nullptr;
        openedAgainCallback = nullptr;
        posted.clear();
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
