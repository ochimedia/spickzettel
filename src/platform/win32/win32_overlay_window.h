#pragma once

#include <windows.h>

#include <bitset>
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
    void Present(Presentation presentation) override;
    bool IsVisible() const override;
    int ScalePercent() const override;
    ForegroundApp UnderlyingApplication() const override;
    void SetEditModeInput(const EditModeInputOptions& options) override;
    void SetPanelDigits(int digitCount) override;
    void SetEditModeNoActivate(bool enabled) override;
    void SetCursorShape(CursorShape shape) override;
    void RequestTextInput() override;
    void ReleaseTextInput() override;
    void SetFrameCallback(FrameCallback callback) override;
    void SetFramePacing(FramePacing pacing) override;
    void SetInputCallback(InputCallback callback) override;
    CaptureResult CaptureRegion(const Rect& rect) override;
    InputGrabDiagnostics GetInputGrabDiagnostics() const override;
    uint64_t CreateTextureFromPixels(const uint8_t* pixelsRGBA, int width, int height) override;
    bool UpdateTextureRegion(uint64_t textureHandle, const uint8_t* pixelsRGBA, int sourceWidth, int x, int y,
                              int w, int h) override;
    void ReleaseTexture(uint64_t textureHandle) override;
    uint64_t TextureGeneration() const override;
    void SetMipmapsWanted(bool wanted) override;
    DrawCallback ImageFilterCallback() const override;
    DrawCallback StrokeDepthCallback() const override;
    DrawCallback StrokeLayerCallback() const override;
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
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    // Hands `event` to the input callback, stamped with the time and the
    // modifiers held - after a Modifiers event of its own, if those
    // changed since the last one. See SetInputCallback.
    void Emit(InputEvent event);
    void EmitPointer(InputEventKind kind, const Vec2& position, MouseButton button, uint8_t buttons = 0);
    // The left or right button's up at `position`, unless it was told
    // already - see WM_CAPTURECHANGED in HandleMessage.
    void EmitButtonUp(MouseButton button, const Vec2& position);
    void EmitKey(WPARAM virtualKey, LPARAM lParam, bool down);
    // A Modifiers event, if `held` is not what the last event carried.
    void EmitModifiersIfChanged(const Modifiers& held, double seconds);
    // The modifiers held right now, from the same two sources RenderFrame
    // gives ImGui - see there.
    static Modifiers HeldModifiers();
    double NowSeconds() const;
    // One step of a presentation plan - see Present. `heldFocus` is whether
    // this window held focus when the change began.
    void Carry(PresentationStep step, bool heldFocus);
    // Takes focus, noting the window it is taken from unless this window
    // holds it already - the one way this window takes focus. See
    // focusTakenFrom_.
    void TakeFocus();
    // RequestTextInput's way by focus - see its definition.
    void TakeTextInputFocus();
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
    // The window focus was taken from, noted at the moment it was taken
    // (see TakeFocus) and dropped once it is handed back - so only while
    // this window may hold focus it took. Where focus goes back to on going
    // click-through or hidden, and when a text field that borrowed it
    // closes. Noted at the show, as it once was, it could be hours old by
    // then, or a window the user had since left.
    HWND focusTakenFrom_ = nullptr;
    bool visible_ = false;
    bool inputPassthrough_ = false;
    // See SetEditModeInput. Held here so a presentation can
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
    // by Present to decide whether it should be taking focus at all.
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
    InputCallback inputCallback_;
    // The modifiers the last event carried - see Emit.
    Modifiers emittedModifiers_;
    // The keys whose down was delivered and whose up was not yet, so a
    // down for one of them is its repeat - see EmitKey.
    std::bitset<256> keysDown_;
    // The left and right buttons whose down was delivered and whose up
    // not yet, as ButtonBit's bits - the ones a lost capture ends.
    uint8_t buttonsHeld_ = 0;
    // Where the last pointer event was: where a button the window lets go
    // of itself goes up.
    Vec2 lastPointerPosition_{};
    std::unique_ptr<Win32Dx11Renderer> renderer_;
    // The texture generations of renderers since destroyed, which took
    // every texture they made with them, and one for every switch of the
    // mip chain (SetMipmapsWanted). See TextureGeneration.
    uint64_t pastTextureGenerations_ = 0;
    // See SetMipmapsWanted; handed to every renderer made.
    bool mipmapsWanted_ = false;
    LARGE_INTEGER lastFrameTime_{};
    LARGE_INTEGER perfFrequency_{};
};

// KeyCombo's encoding of a Win32 virtual key - a letter, digit, function
// or named key - or 0 for one it has no name for.
int KeyForVirtualKey(UINT virtualKey);

}  // namespace sz::platform::win32
