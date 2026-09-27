#include "ui/editor.h"

#include <algorithm>

namespace sz::ui {

// ================= Commands =================
//
// Every key, context menu row, selection bar button and global hotkey
// names a Command (see ui/interaction/command.h), and every one of them is
// run here: Dispatch asks whether it can act, ends what its scope covers,
// and runs it. See docs/INTERACTIONS.md, section 7.

bool Editor::Dispatch(const Command& command) {
    // Nothing acts on a snippet that has gone, before or after - an undo or
    // a delete can take a selected one off the screen.
    PruneSelection();
    if (!Available(command)) {
        return false;
    }
    // What the scope covers ends first - see Scope - and a command whose
    // settling could not be written does not run (see Settle).
    if (!Settle(InfoFor(command.id).scope)) {
        PruneSelection();
        return false;
    }
    ++commandsRun_;
    lastCommand_ = command.id;
    Run(command, Filing::Step);
    PruneSelection();
    return true;
}

bool Editor::Step(const Command& command) {
    PruneSelection();
    if (!Available(command)) {
        return false;
    }
    ++commandsRun_;
    lastCommand_ = command.id;
    Run(command, Filing::Burst);
    PruneSelection();
    return true;
}

std::optional<CommandId> Editor::CommandForKey(int key, const platform::Modifiers& held, bool repeat) const {
    if (key == 0) {
        return std::nullopt;
    }
    const ShortcutBindings& shortcuts = settings_.Live().shortcuts;
    for (const CommandInfo& info : kCommands) {
        if (info.hotkey.has_value()) {
            continue;  // the OS hands those to the tray, not to this window
        }
        for (const platform::KeyCombo& binding : KeysFor(info.id, Cfg(), shortcuts)) {
            // Exactly the modifiers the binding names, so a bare "P" does
            // not also fire on Ctrl+P - which is somebody else's chord, even
            // if nothing here claims it yet.
            if (binding.key != key || (!info.anyModifiers && (held.ctrl != binding.ctrl || held.alt != binding.alt ||
                                                              held.shift != binding.shift))) {
                continue;
            }
            // The key belongs to this command, repeating or not.
            if (repeat && !info.repeats) {
                return std::nullopt;
            }
            return info.id;
        }
    }
    return std::nullopt;
}

bool Editor::Available(const Command& command) const {
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
        case CommandId::DrawingMode:
            return item != nullptr;
        case CommandId::LeaveDrawingMode:
            return DrawingItem().has_value();
        case CommandId::ItemMenu:
            return item != nullptr && command.at.has_value();
        case CommandId::EmptyCanvasMenu:
            return command.at.has_value();
        case CommandId::FrameSnippet:
            return command.rect.has_value();
    }
    return false;  // unreachable: the switch names every command
}

void Editor::Run(const Command& command, Filing filing) {
    // The key of the tool already in hand puts it down again - back to
    // Select, the hand at rest (see the Tool enum), which for a marking
    // tool means leaving drawing mode.
    const auto toggleTool = [this](Tool tool) { PickTool(ActiveTool() == tool ? Tool::Select : tool); };
    // A pixel a press, ten with Shift - the way every drawing program
    // nudges.
    const float nudge = held_.shift ? 10.0f : 1.0f;
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
            NudgeSelection(-nudge, 0.0f, filing);
            return;
        case CommandId::NudgeRight:
            NudgeSelection(nudge, 0.0f, filing);
            return;
        case CommandId::NudgeUp:
            NudgeSelection(0.0f, -nudge, filing);
            return;
        case CommandId::NudgeDown:
            NudgeSelection(0.0f, nudge, filing);
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
            if (views_ != nullptr) {
                views_->ToggleCheatSheet();
            }
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
            if (views_ != nullptr) {
                views_->OpenPicker(command.item, /*copy=*/false);
            }
            return;
        case CommandId::Minimize:
            session_.SetMinimized(selection_, true);
            // Off the screen, so out of the selection - PruneSelection would
            // do it next frame; doing it now keeps the bar from showing over
            // nothing for a frame.
            ClearSelection();
            return;
        case CommandId::Pin: {
            // Nothing happens on screen until the overlay is put away - into
            // the pinned view, which is where a pin is acted on (see
            // docs/OVERLAY_STATES.md, "Away"). Not
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
            if (views_ != nullptr) {
                views_->OpenItemProperties(command.item, command.at);
            }
            return;
        // The drawing bar: the tool to draw with, and the color. The tool
        // already in hand is cycled through its shapes instead - pen, line,
        // rectangle; eraser, rectangle eraser - so a plain drag makes them,
        // for a hand with no modifier key to hold (see PenShape).
        case CommandId::PenButton:
            if (DrawingMode* drawing = machine_.As<DrawingMode>(Level::Mode);
                drawing != nullptr && drawing->GetTool() == Tool::Draw) {
                drawing->CyclePenShape();
            } else {
                PickTool(Tool::Draw);
            }
            return;
        case CommandId::EraserButton:
            if (DrawingMode* drawing = machine_.As<DrawingMode>(Level::Mode);
                drawing != nullptr && drawing->GetTool() == Tool::Erase) {
                drawing->CycleEraserShape();
            } else {
                PickTool(Tool::Erase);
            }
            return;
        case CommandId::TextButton:
            PickTool(Tool::Text);
            return;
        case CommandId::ColorButton:
            // The chooser opens next to the button.
            if (views_ != nullptr) {
                views_->OpenColorChooser(*command.at);
            }
            return;
        case CommandId::FullscreenScreenshot:
        case CommandId::FullscreenDrawing:
        case CommandId::FrameSnippet: {
            // A capture leaves the overlay's own window out (see
            // IOverlayWindow::CaptureRegion), whatever asked for it. Made by
            // a press on empty canvas, it is the hand moving on from a
            // snippet it was drawing on.
            const ItemCreationKind kind = command.id == CommandId::FullscreenScreenshot ? ItemCreationKind::Screenshot
                                          : command.id == CommandId::FullscreenDrawing ? ItemCreationKind::Drawing
                                                                                       : command.kind;
            if (command.madeBy == MadeBy::Press) {
                ExitDrawingMode();
            }
            const ItemId made = command.id == CommandId::FrameSnippet ? CreateRegionItem(kind, *command.rect)
                                                                      : CreateFullscreenItem(kind);
            if (made == 0) {
                return;  // too small to be meant, or not written: the tool stays in hand to try again
            }
            // A creation tool places once. A drawing has already handed
            // over to Draw (see HandOverNewItem); a screenshot hands back
            // the tool that was in hand before it.
            if (command.madeBy == MadeBy::Tool && ActiveTool() == Tool::NewScreenshot) {
                PutDownCreationTool();
            }
            return;
        }
        case CommandId::DeleteCanvas:
            // Through the same confirmation the Overview's own delete
            // button asks for, rather than deleting outright: a canvas
            // takes every snippet on it along, and unlike a snippet's own
            // delete there is no undo entry to take it back with.
            if (views_ != nullptr) {
                views_->AskToDeleteCanvas(command.canvas);
            }
            return;
        case CommandId::Overview:
            if (views_ != nullptr) {
                views_->OpenOverview();
            }
            return;
        case CommandId::Settings:
            if (views_ != nullptr) {
                views_->OpenSettings();
            }
            return;
        case CommandId::DrawingMode:
            EnterDrawingMode(command.item);
            return;
        case CommandId::LeaveDrawingMode:
            ExitDrawingMode();
            return;
        case CommandId::ItemMenu:
            if (views_ != nullptr) {
                views_->OpenItemMenu(command.item, *command.at);
            }
            return;
        case CommandId::EmptyCanvasMenu:
            if (views_ != nullptr) {
                views_->OpenEmptyCanvasMenu(*command.at);
            }
            return;
    }
}

// Escape, once everything above the Canvas level has passed it: a creation
// tool in hand has been put down and drawing mode left by then (see
// CreationTool and DrawingMode), and what is left is the canvas's - a cut
// waiting to be pasted is called off, then the selection clears. What a
// stroke or a drag in flight does with it is its own: it is canceled.
void Editor::PutDown() {
    if (clipboardIsCut_ && !clipboard_.empty()) {
        // Never mind the cut: the snippets are still where they were, so
        // this only has to stop them waiting to be moved - the selection
        // they are part of is the next press of Escape's.
        clipboard_.clear();
        clipboardIsCut_ = false;
    } else if (!selection_.empty()) {
        ClearSelection();
    }
}

}  // namespace sz::ui
