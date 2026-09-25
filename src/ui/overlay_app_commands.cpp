#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Commands =================
//
// Every key, context menu row, selection bar button and global hotkey
// names a Command (see ui/interaction/command.h), and every one of them is
// run here: Dispatch asks whether it can act, ends what its scope covers,
// and runs it. See docs/INTERACTIONS.md, section 7.

bool OverlayApp::Dispatch(const Command& command) {
    if (!Available(command)) {
        return false;
    }
    // Both scopes end the hand and a note being typed today - see Scope.
    switch (InfoFor(command.id).scope) {
        case Scope::Hand:
        case Scope::Canvas:
            SettleHand();
            break;
    }
    ++commandsRun_;
    lastCommand_ = command.id;
    Run(command);
    return true;
}

bool OverlayApp::Available(const Command& command) const {
    const Item* item = command.item != 0 ? Manager().FindItemAnywhere(command.item) : nullptr;
    switch (command.id) {
        case CommandId::Undo:
        case CommandId::Redo:
        case CommandId::PutDown:
        case CommandId::DrawTool:
        case CommandId::EraseTool:
        case CommandId::TextTool:
        case CommandId::SelectTool:
        case CommandId::NewScreenshotTool:
        case CommandId::NewDrawingTool:
        case CommandId::NewCanvas:
        case CommandId::NewCanvasWithSelection:
        case CommandId::CheatSheet:
        case CommandId::ToggleEditMode:
        case CommandId::ToggleViewMode:
        case CommandId::QuickCapture:
        case CommandId::SilentCapture:
        case CommandId::FullscreenScreenshot:
        case CommandId::FullscreenDrawing:
        case CommandId::Overview:
        case CommandId::Settings:
            return true;
        case CommandId::DeleteSelection:
        case CommandId::NudgeLeft:
        case CommandId::NudgeRight:
        case CommandId::NudgeUp:
        case CommandId::NudgeDown:
        case CommandId::Copy:
        case CommandId::Cut:
        case CommandId::Duplicate:
        case CommandId::Minimize:
        case CommandId::Pin:
        case CommandId::PenButton:
        case CommandId::EraserButton:
        case CommandId::TextButton:
            return !selection_.empty();
        case CommandId::ColorButton:
            return !selection_.empty() && command.at.has_value();
        case CommandId::Paste:
            return !clipboard_.empty() && Manager().CurrentOrNull() != nullptr;
        case CommandId::ToggleFullscreen:
        case CommandId::ToggleFullscreenStretched:
        case CommandId::ResetSize:
        case CommandId::Properties:
            return item != nullptr;
        case CommandId::ClearDrawing:
            return item != nullptr && !item->strokes.empty();
        case CommandId::SendBackward:
            return item != nullptr && Manager().CanMoveItemLayer(command.item, -1);
        case CommandId::BringForward:
            return item != nullptr && Manager().CanMoveItemLayer(command.item, 1);
        case CommandId::MoveToCanvas: {
            // Somewhere to move it to: a canvas the picker shows, which a
            // deleted one is not - counted, they opened a picker with
            // nothing in it.
            const CanvasId here = Manager().CurrentCanvasId();
            return item != nullptr &&
                   std::any_of(Manager().Canvases().begin(), Manager().Canvases().end(),
                               [&](const Canvas& c) { return c.id != here && !Manager().IsDeleted(c); });
        }
        case CommandId::DeleteCanvas: {
            const Canvas* canvas = Manager().FindCanvas(command.canvas);
            return canvas != nullptr && !Manager().IsDeleted(*canvas);
        }
    }
    return false;  // unreachable: the switch names every command
}

void OverlayApp::Run(const Command& command) {
    // The key of the tool already in hand puts it down again - back to
    // Select, the hand at rest (see the Tool enum), which for a marking
    // tool means leaving drawing mode.
    const auto toggleTool = [this](Tool tool) { PickTool(activeTool_ == tool ? Tool::Select : tool); };
    // A pixel a press, ten with Shift - the way every drawing program
    // nudges.
    const float nudge = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
    switch (command.id) {
        case CommandId::Undo:
            Undo();
            return;
        case CommandId::Redo:
            Redo();
            return;
        case CommandId::PutDown:
            PutDown();
            return;
        case CommandId::DeleteSelection:
            DeleteSelection();
            return;
        case CommandId::NudgeLeft:
            NudgeSelection(-nudge, 0.0f);
            return;
        case CommandId::NudgeRight:
            NudgeSelection(nudge, 0.0f);
            return;
        case CommandId::NudgeUp:
            NudgeSelection(0.0f, -nudge);
            return;
        case CommandId::NudgeDown:
            NudgeSelection(0.0f, nudge);
            return;
        case CommandId::DrawTool:
            toggleTool(Tool::Draw);
            return;
        case CommandId::EraseTool:
            toggleTool(Tool::Erase);
            return;
        case CommandId::TextTool:
            toggleTool(Tool::Text);
            return;
        case CommandId::SelectTool:
            toggleTool(Tool::Select);
            return;
        case CommandId::NewScreenshotTool:
            toggleTool(Tool::NewScreenshot);
            return;
        case CommandId::NewDrawingTool:
            toggleTool(Tool::NewDrawing);
            return;
        case CommandId::NewCanvas:
            CreateAndSwitchToNewCanvas();
            return;
        case CommandId::NewCanvasWithSelection:
            MoveSelectionToNewCanvas();
            return;
        case CommandId::Copy:
            CopySelectionToClipboard(/*cut=*/false);
            return;
        case CommandId::Cut:
            CopySelectionToClipboard(/*cut=*/true);
            return;
        case CommandId::Paste:
            PasteFromClipboard();
            return;
        case CommandId::Duplicate:
            DuplicateSelection();
            return;
        case CommandId::CheatSheet:
            cheatSheetOpen_ = !cheatSheetOpen_;
            return;
        case CommandId::ToggleEditMode:
        case CommandId::ToggleViewMode:
        case CommandId::QuickCapture:
        case CommandId::SilentCapture:
            if (appCommandCallback_) {
                appCommandCallback_(command.id);
            }
            return;
        case CommandId::ToggleFullscreen:
            ToggleFullscreenUndoably(command.item, /*stretch=*/false);
            return;
        case CommandId::ToggleFullscreenStretched:
            ToggleFullscreenUndoably(command.item, /*stretch=*/true);
            return;
        case CommandId::ResetSize:
            ResetToNativeSizeUndoably(command.item);
            return;
        case CommandId::ClearDrawing:
            ClearItemDrawing(command.item);
            return;
        case CommandId::SendBackward:
            session_.MoveItemLayer(command.item, -1);
            return;
        case CommandId::BringForward:
            session_.MoveItemLayer(command.item, 1);
            return;
        case CommandId::MoveToCanvas:
            // Opens the Overview in picker mode to choose a destination.
            OpenPicker(command.item, /*isCopy=*/false);
            return;
        case CommandId::Minimize:
            session_.SetMinimized(selection_, true);
            // Off the screen, so out of the selection - PruneSelection would
            // do it next frame; doing it now keeps the bar from showing over
            // nothing for a frame.
            ClearSelection();
            return;
        case CommandId::Pin: {
            // Nothing happens on screen until the overlay is put away - see
            // TrayController::PutAway, which is where a pin is acted on. Not
            // an undo step, the same as Minimize: it changes where the
            // snippet is shown, not what it holds. One press pins the whole
            // selection, or unpins it when every one of it is pinned.
            bool allPinned = true;
            for (const ItemId id : selection_) {
                const Item* item = Manager().FindItemAnywhere(id);
                allPinned = allPinned && item != nullptr && item->pinned;
            }
            session_.SetPinned(selection_, !allPinned);
            return;
        }
        case CommandId::Properties:
            // A request flag rather than ImGui::OpenPopup directly - see
            // colorChooserRequested_'s own doc comment: a bar button fires
            // outside any frame, with no current window for a popup to be
            // scoped to.
            itemPropertiesPopoverItemId_ = command.item;
            itemPropertiesPopoverRequested_ = true;
            if (command.at.has_value()) {
                itemPropertiesPopoverAnchor_ = ImVec2(command.at->x, command.at->y);
            }
            return;
        // The drawing bar: the tool to draw with, and the color. The tool
        // already in hand is cycled through its shapes instead - pen, line,
        // rectangle; eraser, rectangle eraser - so a plain drag makes them,
        // for a hand with no modifier key to hold (see penShape_).
        case CommandId::PenButton:
            if (activeTool_ != Tool::Draw) {
                PickTool(Tool::Draw);
            } else {
                penShape_ = penShape_ == DrawShape::Freehand ? DrawShape::Line
                            : penShape_ == DrawShape::Line   ? DrawShape::Rectangle
                                                             : DrawShape::Freehand;
            }
            return;
        case CommandId::EraserButton:
            if (activeTool_ != Tool::Erase) {
                PickTool(Tool::Erase);
            } else {
                eraserShape_ = eraserShape_ == DrawShape::Rectangle ? DrawShape::Freehand : DrawShape::Rectangle;
            }
            return;
        case CommandId::TextButton:
            PickTool(Tool::Text);
            return;
        case CommandId::ColorButton:
            // The chooser opens next to the button - asked for here, opened
            // on the next frame (see OpenColorChooser).
            OpenColorChooser(ImVec2(command.at->x, command.at->y));
            return;
        case CommandId::FullscreenScreenshot:
        case CommandId::FullscreenDrawing: {
            // Asked for, so a drawing made this way is not watched as a
            // stray the way a double-click's is (see untouchedDrawing_) - as
            // with a creation tool. A capture leaves the overlay's own
            // window out (see IOverlayWindow::CaptureRegion), whatever menu
            // it was chosen from.
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            CreateFullscreenItem(command.id == CommandId::FullscreenScreenshot ? ItemCreationKind::Screenshot
                                                                               : ItemCreationKind::Drawing,
                                 display.x, display.y);
            return;
        }
        case CommandId::DeleteCanvas:
            // Through the same confirmation the Overview's own delete
            // button asks for, rather than deleting outright: a canvas
            // takes every snippet on it along, and unlike a snippet's own
            // delete there is no undo entry to take it back with.
            confirmDeleteTarget_ = ConfirmDeleteTarget{ConfirmDeleteTarget::Kind::Canvas, command.canvas,
                                                       Manager().FindCanvas(command.canvas)->name};
            confirmDeletePopoverRequested_ = true;
            return;
        case CommandId::Overview:
            OpenOverview();
            return;
        case CommandId::Settings:
            OpenOverview();
            SwitchOverviewTab(OverviewTab::Settings);
            return;
    }
}

// Escape puts the hand down, in stages: a creation tool in hand goes
// back to Select, the hand at rest (see the Tool enum), then drawing mode
// ends, then a cut waiting to be pasted is called off, then a selection
// clears. What a stroke or a drag in flight does with it is Dispatch's:
// it is settled first, as for every command.
void OverlayApp::PutDown() {
    if (CreationKindFor(activeTool_).has_value()) {
        PickTool(Tool::Select);
    } else if (drawingItem_.has_value()) {
        ExitDrawingMode();
    } else if (clipboardIsCut_ && !clipboard_.empty()) {
        // Never mind the cut: the snippets are still where they were, so
        // this only has to stop them waiting to be moved - the selection
        // they are part of is the next press of Escape's.
        clipboard_.clear();
        clipboardIsCut_ = false;
    } else if (!selection_.empty()) {
        ClearSelection();
    }
}

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
        // HandleCommandKeys).
        case CommandId::PutDown:
            return !PanelOpen() && !popupOpen;
        // And not in drawing mode, where the snippet is being worked in,
        // not on.
        case CommandId::DeleteSelection:
        case CommandId::NudgeLeft:
        case CommandId::NudgeRight:
        case CommandId::NudgeUp:
        case CommandId::NudgeDown:
            return !PanelOpen() && !popupOpen && !drawingItem_.has_value();
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
            return false;
    }
    return false;  // unreachable: the switch names every command
}

bool OverlayApp::Pressed(const platform::KeyCombo& key, bool repeats) const {
    if (const std::optional<ImGuiMouseButton> button = ImGuiMouseButtonForCombo(key)) {
        // Reaching its command as a key would, over a panel too - no panel
        // here does anything with these buttons, and taken for the panel's,
        // the button that opened the cheat sheet could not close it. Only
        // while a gesture is in flight does it wait: it is on the mouse
        // holding the gesture (see Hand::ignoredButton).
        return ImGui::IsMouseClicked(*button, false) && !GestureInFlight();
    }
    const ImGuiKey imguiKey = ImGuiKeyForCombo(key);
    return imguiKey != ImGuiKey_None && ImGui::IsKeyPressed(imguiKey, repeats);
}

void OverlayApp::HandleCommandKeys() {
    const ImGuiIO& io = ImGui::GetIO();
    // An open popover takes Escape before any command sees it, and closes.
    bool escapeTaken = false;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !io.WantTextInput && !PanelOpen() && CloseTopmostPopover()) {
        escapeTaken = true;
    }
    // Each key to one command: the first in the table that it is bound to,
    // which puts the fixed keys ahead of the chosen ones and, among those,
    // the table's order ahead of a profile that bound one key twice.
    std::vector<platform::KeyCombo> taken;
    const ShortcutBindings& shortcuts = settings_.Live().shortcuts;
    for (const CommandInfo& info : kCommands) {
        if (info.hotkey.has_value()) {
            continue;  // the OS hands those to the tray, not to this window
        }
        for (const platform::KeyCombo& key : KeysFor(info.id, Cfg(), shortcuts)) {
            if (!Pressed(key, info.repeats)) {
                continue;
            }
            // Exactly the modifiers the binding names, so a bare "P" does
            // not also fire on Ctrl+P - which is somebody else's chord, even
            // if nothing here claims it yet. The keys that never cared -
            // Escape, Delete, the arrows - still don't.
            if (!info.anyModifiers && (io.KeyCtrl != key.ctrl || io.KeyAlt != key.alt || io.KeyShift != key.shift)) {
                continue;
            }
            if (std::find(taken.begin(), taken.end(), key) != taken.end()) {
                continue;
            }
            taken.push_back(key);
            if (key.key == platform::KeyCombo::kEscape && escapeTaken) {
                continue;
            }
            if (KeyReaches(info.id)) {
                Dispatch(Command{info.id});
            }
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
