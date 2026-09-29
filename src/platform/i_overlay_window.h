#pragma once

#include <cstdint>
#include <functional>

#include "platform/platform_types.h"
#include "platform/presentation.h"

// Declared, not included: the one ImGui type this header names is the
// signature of a draw callback, which the UI hands to ImDrawList::
// AddCallback as it is.
struct ImDrawList;
struct ImDrawCmd;

namespace sz::platform {

// ImGui's ImDrawCallback, spelled out.
using DrawCallback = void (*)(const ImDrawList* parentList, const ImDrawCmd* cmd);

// What IOverlayWindow::StrokeLayerCallback is told, copied into the draw
// command (ImDrawList::AddCallback with its size).
struct StrokeLayerStep {
    // True opens the layer, false closes it and lays it down.
    bool open = true;
    // How much of the finished layer is laid down; read on closing.
    float opacity = 1.0f;
};

// A fullscreen overlay window covering one display. Created lazily and
// then hidden and shown without tearing down GPU resources, so that
// toggling it by hotkey many times a session costs nothing after the first.
class IOverlayWindow {
public:
    virtual ~IOverlayWindow() = default;

    // ===== Lifetime and placement =====

    // Creates the window and its rendering resources on first call,
    // covering `display`; a cheap no-op afterwards, which ignores
    // `display`. Returns true if the window is ready to show.
    virtual bool EnsureCreated(const DisplayInfo& display) = 0;

    // Moves the window to cover `display` instead, together with everything
    // the backend does in desktop coordinates on its behalf (where a capture
    // is taken from, where a grabbed pointer may go). Safe while visible or
    // hidden; a no-op before EnsureCreated or when already there.
    virtual void MoveToDisplay(const DisplayInfo& display) = 0;

    // Invoked when the displays change under the window - one attached or
    // removed, a resolution changed - so that whoever chooses the display
    // can choose again. The window never moves itself.
    virtual void SetDisplaysChangedCallback(std::function<void()> callback) = 0;

    // The scale the system gives the display the window is on, in percent:
    // 150 where Windows is set to 150%. Asked every frame, so the answer
    // follows the window to another display and a scale changed while it is
    // up. 100 before EnsureCreated.
    virtual int ScalePercent() const = 0;

    // Releases everything (window, device, swapchain). Called once, on exit.
    virtual void Destroy() = 0;

    // ===== Showing =====

    // Makes the window what `presentation` says, from whatever it is -
    // hidden, up click-through, or up interactive - in the order only the
    // window knows (see PresentationSteps, and docs/OVERLAY_STATES.md,
    // section 7). A no-op before EnsureCreated, and for what it already is.
    //
    // Click-through, every mouse event over the window goes to whatever is
    // beneath it, and the window never takes focus. Interactive, it takes
    // focus unless SetEditModeNoActivate(true) is in effect. Focus taken is
    // handed back, on going click-through or hidden, to the window it was
    // taken from - and only if this window still holds it.
    virtual void Present(Presentation presentation) = 0;
    virtual bool IsVisible() const = 0;

    // Whether frames are wanted at the display's refresh rate or only now
    // and then - see FramePacing. Idle lets the backend draw a few times a
    // second plus once per event that could have changed the picture,
    // instead of redrawing an unchanging view-only overlay at 120 Hz over a
    // game for hours. Told from the frame callback whenever it changes.
    virtual void SetFramePacing(FramePacing pacing) = 0;

    // ===== Focus and input =====

    // Whether showing the window, or clicking it in edit mode, ever gives it
    // OS input focus. Enabled, it never does: mouse routing works as usual
    // while keyboard focus stays with the application underneath, which
    // therefore sees no focus-loss event. Safe to call at any time; takes
    // effect immediately. See RequestTextInput for the one thing that still
    // needs the keyboard regardless.
    virtual void SetEditModeNoActivate(bool enabled) = 0;

    // A text field is open and needs keystrokes even though the window may
    // be holding no focus: the backend either grabs the keyboard for the
    // duration or borrows real focus, whichever it can. Release gives back
    // whichever was taken. Both are no-ops when the window holds focus the
    // ordinary way.
    virtual void RequestTextInput() = 0;
    virtual void ReleaseTextInput() = 0;

    // How much physical input the overlay takes away from the foreground
    // application while it is up in edit mode - see EditModeInputOptions.
    // Safe to call at any time; applies only while presented interactive,
    // never while hidden or click-through.
    virtual void SetEditModeInput(const EditModeInputOptions& options) = 0;

    // The input options HUD is up: its rows are toggled by number keys, and
    // an overlay deliberately holding no keyboard focus can only be handed
    // those by the backend's keyboard grab, so that grab has to stay
    // available even with dontForwardKeystrokes off. 0 when the HUD closes.
    virtual void SetInputOptionsHudDigits(int digitCount) = 0;

    // Which application the overlay is up over - see ForegroundApp. Asked
    // at the moment the overlay is shown, when the answer means something.
    virtual ForegroundApp UnderlyingApplication() const = 0;

    // Overrides the cursor over this window for the shapes ImGui does not
    // have - see CursorShape. Called once per frame with whatever is wanted,
    // so a shape ImGui quietly overwrote from its own NewFrame is
    // reinstated; must be idempotent and cheap on an unchanged shape.
    virtual void SetCursorShape(CursorShape shape) = 0;

    // Debug scaffolding for the input options HUD. Backends without an
    // input grab return a default-constructed value.
    virtual InputGrabDiagnostics GetInputGrabDiagnostics() const = 0;

    // ===== Callbacks =====

    // Invoked once per rendered frame while visible; the UI issues its draw
    // calls from within it.
    virtual void SetFrameCallback(FrameCallback callback) = 0;
    // Invoked for every input event received while visible - the pointer,
    // the wheel, keys and the modifiers, and a Tick once a frame - one at a
    // time, in the order they happened, as each arrives rather than at the
    // next frame. Nothing
    // received while hidden. What ImGui needs for its widgets it is fed
    // separately, as before.
    virtual void SetInputCallback(InputCallback callback) = 0;

    // ===== Textures and capture =====

    // Captures `rect` (in this window's own coordinates) as it appears with
    // this window's own content excluded. The pixels are empty if the
    // backend cannot capture or the OS refused; callers then fall back to a
    // placeholder.
    virtual CaptureResult CaptureRegion(const Rect& rect) = 0;

    // The texture calls below are core::TextureCache's, which is the only
    // caller: everything else asks it for a texture by what it shows (see
    // TextureCache for why).
    //
    // Uploads RGBA8 pixels (the layout CaptureResult uses) as a new
    // texture. The handle is an ImTextureID kept as a bare uint64_t so this
    // header stays free of ImGui; the caller casts. Returns 0 if the
    // backend has no device to create one with.
    virtual uint64_t CreateTextureFromPixels(const uint8_t* pixelsRGBA, int width, int height) = 0;

    // Replaces a rectangle of an existing texture's pixels in place.
    // `pixelsRGBA` is the *whole* image the texture was created from, with
    // `sourceWidth` its width; x/y/w/h select the changed part. Returns false
    // for an unknown handle, a rectangle outside the texture, or a backend
    // that cannot do this. What a rasterized stroke list is brought up to
    // date with (see TextureCache::Get).
    virtual bool UpdateTextureRegion(uint64_t textureHandle, const uint8_t* pixelsRGBA, int sourceWidth,
                                      int x, int y, int w, int h) = 0;

    // Releases a texture from either call above. No-op for 0.
    virtual void ReleaseTexture(uint64_t textureHandle) = 0;

    // Changes when every texture handed out before is lost: the GPU device
    // was reset or replaced - a driver update, or a driver that stopped
    // responding and was restarted. The handles from before draw nothing
    // and must not be updated; each is still given back through
    // ReleaseTexture, and what it showed uploaded again.
    virtual uint64_t TextureGeneration() const = 0;

    // A draw callback that makes the pictures drawn after it resample with
    // the ImageFilter carried as its user data (the enum's value cast to a
    // pointer), until ImGui's DrawCallback_ResetRenderState puts the
    // default back. Null from a backend that draws nothing, whose pictures
    // then just keep the default.
    virtual DrawCallback ImageFilterCallback() const = 0;

    // A draw callback around one stroke: with non-null user data it gives the triangles after it a depth of
    // their own, nearer than every stroke's before, and a test that lets
    // only the first of them reach each pixel; with null user data it
    // takes the test away again. Null from a backend that draws nothing,
    // or that has no depth buffer, and the stroke is drawn untested.
    virtual DrawCallback StrokeDepthCallback() const = 0;

    // A draw callback around one snippet's strokes, below full opacity,
    // with a StrokeLayerStep as its data. Opening, what is drawn
    // after it goes into a layer of its own, cleared within the command's
    // clip rectangle; closing, the layer is laid over what was there within
    // that rectangle, once, at the step's opacity. ImGui's
    // DrawCallback_ResetRenderState has to follow the closing one. Null
    // from a backend that draws nothing, and the strokes are drawn straight
    // onto the frame, each at the opacity.
    virtual DrawCallback StrokeLayerCallback() const = 0;
};

}  // namespace sz::platform
