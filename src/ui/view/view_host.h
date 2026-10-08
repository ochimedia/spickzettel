#pragma once

// What an owner of a surface may ask of the rest of the view -
// docs/VIEW_LAYER.md, section 7. An owner draws its own surfaces and
// changes only its own state; anything else is asked for here, of
// OverlayApp, which is the only object that knows every owner. It reads the
// session, the settings and the editor directly, and changes them only
// through an action or through its widgets' own values (section 6).

#include <string>
#include <vector>

#include <imgui.h>

#include "core/canvas/canvas.h"
#include "core/drawing/stroke_mesh_cache.h"
#include "core/session/actions.h"
#include "platform/i_overlay_window.h"
#include "ui/item_painting.h"
#include "ui/view/anchors.h"
#include "ui/view_action.h"

namespace sz::ui {

class ViewHost {
public:
    // What a widget asks for, done once the frame is drawn - see ViewAction.
    virtual void Act(ViewAction action) = 0;
    // A message, for the toast.
    virtual void Say(std::string text) = 0;
    // The window the overlay is drawn in, or none before it is attached.
    virtual platform::IOverlayWindow* Window() const = 0;
    // The displays the overlay may be put on, as the host lists them - empty
    // where there is no host to ask.
    virtual std::vector<platform::DisplayInfo> ListDisplays() = 0;
    // Offers a new combination for a global hotkey to the OS, and stores it
    // if the OS takes it - see OverlayApp::SetHotkeyChangeCallback. False
    // when it did not.
    virtual bool ChangeHotkey(core::HotkeySlot slot, platform::KeyCombo combo) = 0;
    // No hotkey fires while `paused`, and the press of one is a key like
    // any other - for a row waiting for a key (see OverlayApp::
    // SetHotkeysPausedCallback).
    virtual void PauseHotkeys(bool paused) = 0;
    // A delete of `target`: the confirmation, unless Settings > Behavior
    // says not to ask, and then the delete itself, as an action.
    virtual void AskToDelete(DeleteTarget target) = 0;
    // The overlay hidden and shown again, after this frame - for an option
    // that takes effect only on entry to edit mode (see OverlayApp::
    // SetRestartOverlayCallback).
    virtual void RestartOverlay() = 0;
    // A canvas bar tile's menu, for `canvas`, at `at`.
    virtual void OpenCanvasMenu(core::CanvasId canvas, ImVec2 at) = 0;
    // An anchored widget, drawn this frame at `min`-`max` - see
    // AnchorBoard. What the owner drawing it says, and all it knows of
    // what points at it.
    virtual void Mark(Anchor anchor, ImVec2 min, ImVec2 max) = 0;

    // What a canvas's preview is drawn with - see DrawCanvasPreview: each
    // picture's thumbnail-sized pixels, the previews' own mesh cache, and
    // the renderer's hooks. The textures and the caches are the canvas view's.
    struct PreviewDrawing {
        PreviewTextureFn textures;
        core::StrokeMeshSlot meshes;
        PaintHooks hooks;
    };
    // Asked once a frame by what draws previews, which starts that frame's
    // budget for reading pictures over.
    virtual PreviewDrawing Previews() = 0;

protected:
    ~ViewHost() = default;
};

}  // namespace sz::ui
