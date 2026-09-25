#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <memory>

#include "platform/i_overlay_window.h"

namespace sz::platform::win32 {

class Win32Dx11Renderer;

// Win32 implementation of IOverlayWindow: a layered, topmost, per-pixel-alpha
// window sized to a display, rendered via Direct3D 11 + Dear ImGui. Follows
// the lazy-create-then-hide/show lifecycle described in the architecture
// doc: EnsureCreated() only does real work on its first call per process.
class Win32OverlayWindow final : public IOverlayWindow {
public:
    // Declared (instead of implicitly generated) and defined out-of-line in
    // the .cpp: renderer_ is a unique_ptr to a forward-declared type, so its
    // destructor must be instantiated where Win32Dx11Renderer is complete,
    // not wherever a Win32OverlayWindow happens to be constructed/destroyed
    // (e.g. as a by-value member of Win32PlatformHost in a different
    // translation unit). The constructor needs the same treatment: even
    // default-constructing renderer_ requires the compiler to generate
    // exception-unwind cleanup code that references Win32Dx11Renderer's
    // destructor.
    Win32OverlayWindow();
    ~Win32OverlayWindow() override;

    // Stores the module instance for later window-class registration; does
    // not create any window or GPU resources yet.
    void Initialize(HINSTANCE instance);

    bool EnsureCreated(const DisplayInfo& display) override;
    void MoveToDisplay(const DisplayInfo& display) override;
    void SetDisplaysChangedCallback(std::function<void()> callback) override;
    // What a close asked of this window from outside does - see WM_CLOSE
    // in HandleMessage. The host's own exit, which it sets.
    void SetCloseRequestedCallback(std::function<void()> callback) { closeRequestedCallback_ = std::move(callback); }
    void Show() override;
    void ShowClickThrough() override;
    void Hide() override;
    bool IsVisible() const override;
    int ScalePercent() const override;
    ForegroundApp UnderlyingApplication() const override;
    void SetInputPassthrough(bool enabled) override;
    void SetEditModeInput(const EditModeInputOptions& options) override;
    void SetInputOptionsHudDigits(int digitCount) override;
    void SetEditModeNoActivate(bool enabled) override;
    void SetCursorShape(CursorShape shape) override;
    void RequestTextInput() override;
    void ReleaseTextInput() override;
    void SetFrameCallback(FrameCallback callback) override;
    void SetFramePacing(FramePacing pacing) override;
    void SetMouseCallback(MouseCallback callback) override;
    CaptureResult CaptureRegion(const Rect& rect) override;
    InputGrabDiagnostics GetInputGrabDiagnostics() const override;
    uint64_t CreateTextureFromPixels(const uint8_t* pixelsRGBA, int width, int height) override;
    bool UpdateTextureRegion(uint64_t textureHandle, const uint8_t* pixelsRGBA, int sourceWidth, int x, int y,
                              int w, int h) override;
    void ReleaseTexture(uint64_t textureHandle) override;
    uint64_t TextureGeneration() const override;
    DrawCallback ImageFilterCallback() const override;
    void Destroy() override;

    // Called from the host's event loop while visible: pumps one ImGui +
    // D3D11 frame (NewFrame -> FrameCallback -> Render -> Present).
    void RenderFrame();

    // For the host's event loop, which is what paces frames - see
    // SetFramePacing. Whether to draw on this pass, given whether any
    // message was just dispatched (any of which may have changed what is
    // shown); and, when not, how long the loop may sleep before the next
    // idle frame is due. A message wakes it earlier.
    bool WantsFrame(bool messagesDispatched) const;
    DWORD MillisecondsUntilIdleFrame() const;

private:
    // Both public Show variants, which differ only in whether the window
    // is allowed to take focus on the way up.
    void ShowInternal(bool activate);
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void EmitMouseEvent(const Vec2& position, MouseButton button, MouseEventKind kind);
    // Recomputes whether the input grab may run right now (visible, and in
    // edit mode rather than click-through view-only) and applies it.
    void RefreshEditModeInput();
    // Puts the game's camera back and gives it time to show that, before a
    // hide or a switch to view-only uncovers the game - see its definition.
    void SettleCameraBeforeReveal();
    // See its definition: whether another always-on-top window is above us and
    // covering something, which the taskbar does whenever it is the foreground
    // window as the overlay comes up.
    bool CoveredByAnotherTopmostWindow() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    // The desktop rectangle of the display the window covers - see
    // MoveToDisplay. Also handed to the input grab, as where its pointer may
    // go.
    RECT displayRect_{};
    std::function<void()> displaysChangedCallback_;
    std::function<void()> closeRequestedCallback_;
    // Where the last per-frame Move was emitted for the software pointer,
    // so a frame in which it didn't move emits nothing - see RenderFrame.
    POINT lastEmittedMove_{LONG_MIN, LONG_MIN};
    HWND previousForegroundWindow_ = nullptr;
    bool visible_ = false;
    bool inputPassthrough_ = false;
    // See SetEditModeInput. Held here so Show/Hide/SetInputPassthrough can
    // re-derive whether the grab should currently be running at all -
    // the options say what is wanted, the window says when it applies.
    EditModeInputOptions editModeInput_;
    // See SetCursorShape / the WM_SETCURSOR handling in HandleMessage.
    CursorShape cursorShape_ = CursorShape::Default;
    // The stock cursor for a shape, or the pen this app draws itself, at
    // the display scale `scalePercent` (see ScalePercent).
    static HCURSOR CursorFor(CursorShape shape, int scalePercent);
    // Built on first use at each scale, from the glyph in the .cpp -
    // Windows has no stock pen, and scales its own cursors with the
    // display, which a bitmap of one size would not. Null if it couldn't be
    // created, which CursorFor treats as "use the crosshair instead" rather
    // than as a failure worth reporting. Kept per scale, null included, so
    // a failure isn't retried on every mouse move; there are only ever as
    // many as there are scales among the displays.
    static HCURSOR PenCursor(int scalePercent);
    static inline std::map<int, HCURSOR> penCursors_;
    // See SetEditModeNoActivate's doc comment. Consulted at EnsureCreated()
    // time (baked into the window's creation style - SetEditModeNoActivate
    // also live-restyles hwnd_ directly if it already exists by then) and
    // by Show()/SetInputPassthrough(false)/ReleaseTextInput() to decide
    // whether they should be touching OS focus at all.
    bool noActivate_ = false;
    // Whether RequestTextInput took WS_EX_NOACTIVATE off to borrow real keyboard
    // focus, so ReleaseTextInput knows to put it back - see both.
    bool focusBorrowed_ = false;
    // Seconds since the last z-order check - see RenderFrame.
    float topmostCheckSeconds_ = 0.0f;
    // Seconds since the last show, for the same check - see RenderFrame.
    float shownSeconds_ = 0.0f;
    // Whether the next frame puts ImGui's pointer where the cursor is - the
    // first after a show; see RenderFrame.
    bool seedPointerFromCursor_ = false;
    // See SetFramePacing.
    FramePacing framePacing_ = FramePacing::EveryFrame;
    FrameCallback frameCallback_;
    MouseCallback mouseCallback_;
    std::unique_ptr<Win32Dx11Renderer> renderer_;
    // The texture generations of renderers since destroyed, which took
    // every texture they made with them. See TextureGeneration.
    uint64_t pastTextureGenerations_ = 0;
    LARGE_INTEGER lastFrameTime_{};
    LARGE_INTEGER perfFrequency_{};
};

}  // namespace sz::platform::win32
