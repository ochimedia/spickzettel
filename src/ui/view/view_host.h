#pragma once

// What an owner of a surface may ask of the rest of the view -
// docs/VIEW_LAYER.md, section 7. An owner draws its own surfaces and
// changes only its own state; anything else is asked for here, of
// OverlayApp, which is the only object that knows every owner. It reads the
// session, the settings and the editor directly, and changes them only
// through an action or through its widgets' own values (section 6).

#include <string>
#include <vector>

#include "core/session/actions.h"
#include "platform/i_overlay_window.h"
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

protected:
    ~ViewHost() = default;
};

}  // namespace sz::ui
