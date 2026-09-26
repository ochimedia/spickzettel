#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Keys =================

bool OverlayApp::KeyReaches(CommandId id) const {
    const ImGuiIO& io = ImGui::GetIO();
    // A field being typed into - a note, a name - takes every key.
    if (io.WantTextInput) {
        return false;
    }
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    switch (id) {
        // Not under a panel: the change would be to a canvas nobody can
        // see - while a shortcut is being bound, or, in the move picker, to
        // the very snippet being moved.
        case CommandId::Undo:
        case CommandId::Redo:
            return !PanelOpen();
        // Not under a popup either, which Escape closes first (see
        // HandleCommandKey).
        case CommandId::PutDown:
            return !PanelOpen() && !popupOpen;
        // And not in drawing mode, where the snippet is being worked in,
        // not on.
        case CommandId::DeleteSelection:
        case CommandId::NudgeLeft:
        case CommandId::NudgeRight:
        case CommandId::NudgeUp:
        case CommandId::NudgeDown:
            return !PanelOpen() && !popupOpen && !editor_.DrawingItem().has_value();
        // Not while the Overview is up - where the Shortcuts tab itself is,
        // and where a stray "P" would change the tool underneath the panel
        // while the user is in the middle of binding one. Over the cheat
        // sheet, only its own key, which closes it: a tool picked up under
        // a panel that hides the canvas would be a change nobody saw.
        case CommandId::DrawTool:
        case CommandId::EraseTool:
        case CommandId::TextTool:
        case CommandId::SelectTool:
        case CommandId::NewScreenshotTool:
        case CommandId::NewDrawingTool:
        case CommandId::NewCanvas:
        case CommandId::NewCanvasWithSelection:
        case CommandId::Copy:
        case CommandId::Cut:
        case CommandId::Paste:
        case CommandId::Duplicate:
            return !overviewOpen_ && !cheatSheetOpen_;
        case CommandId::CheatSheet:
            return !overviewOpen_;
        // The global hotkeys come from the OS, to the tray; the rest no key
        // reaches at all.
        case CommandId::ToggleEditMode:
        case CommandId::ToggleViewMode:
        case CommandId::QuickCapture:
        case CommandId::SilentCapture:
        case CommandId::ToggleFullscreen:
        case CommandId::ToggleFullscreenStretched:
        case CommandId::ResetSize:
        case CommandId::ClearDrawing:
        case CommandId::SendBackward:
        case CommandId::BringForward:
        case CommandId::MoveToCanvas:
        case CommandId::Minimize:
        case CommandId::Pin:
        case CommandId::Properties:
        case CommandId::PenButton:
        case CommandId::EraserButton:
        case CommandId::TextButton:
        case CommandId::ColorButton:
        case CommandId::FullscreenScreenshot:
        case CommandId::FullscreenDrawing:
        case CommandId::DeleteCanvas:
        case CommandId::Overview:
        case CommandId::Settings:
        case CommandId::DrawingMode:
        case CommandId::LeaveDrawingMode:
        case CommandId::ItemMenu:
        case CommandId::EmptyCanvasMenu:
        case CommandId::FrameSnippet:
            return false;
    }
    return false;  // unreachable: the switch names every command
}

void OverlayApp::HandleCommandKey(int key, bool repeat) {
    if (key == 0) {
        return;
    }
    // Reaching its command as a key would, over a panel too, for a mouse
    // button - no panel here does anything with those, and taken for the
    // panel's, the button that opened the cheat sheet could not close it.
    // An open popup never lets a key this far: the machine's Popup level
    // claims it (see Popup).
    // Each key to one command: the first in the table that it is bound to,
    // which puts the fixed keys ahead of the chosen ones and, among those,
    // the table's order ahead of a profile that bound one key twice.
    const ShortcutBindings& shortcuts = settings_.Live().shortcuts;
    for (const CommandInfo& info : kCommands) {
        if (info.hotkey.has_value()) {
            continue;  // the OS hands those to the tray, not to this window
        }
        for (const platform::KeyCombo& binding : KeysFor(info.id, Cfg(), shortcuts)) {
            // Exactly the modifiers the binding names, so a bare "P" does
            // not also fire on Ctrl+P - which is somebody else's chord, even
            // if nothing here claims it yet. The keys that never cared -
            // Escape, Delete, the arrows - still don't.
            if (binding.key != key || (!info.anyModifiers && (editor_.Held().ctrl != binding.ctrl ||
                                                              editor_.Held().alt != binding.alt ||
                                                              editor_.Held().shift != binding.shift))) {
                continue;
            }
            if ((!repeat || info.repeats) && KeyReaches(info.id)) {
                // Nothing acts on a snippet that has gone, before or after -
                // an undo or a delete can take a selected one off the screen.
                editor_.PruneSelection();
                Dispatch(Command{info.id});
                editor_.PruneSelection();
            }
            return;
        }
    }
}

// ================= Menu rows =================

std::string OverlayApp::MenuShortcutLabel(CommandId id) const {
    // Live, not Stored: what is bound right now, profile and all, is what
    // the row has to promise.
    const std::vector<platform::KeyCombo> keys = KeysFor(id, Cfg(), settings_.Live().shortcuts);
    return keys.empty() ? std::string() : FormatKeyComboLabel(keys.front());
}

ContextMenuEntry OverlayApp::MenuRow(const Command& command, const char* id, const Icon* icon, const char* label,
                                     bool separatorAbove) const {
    return ContextMenuEntry{static_cast<int>(command.id), id, icon, label, MenuShortcutLabel(command.id),
                            Available(command), separatorAbove};
}

}  // namespace sz::ui
