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
    if (!Available(command)) {
        return false;
    }
    // What the scope covers ends first - see Scope. The gestures and a note
    // being typed are still the view's, which ends them for either scope.
    machine_.EndFor(InfoFor(command.id).scope);
    if (views_ != nullptr) {
        views_->SettleHand();
    }
    ++commandsRun_;
    lastCommand_ = command.id;
    Run(command);
    return true;
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
    }
    return false;  // unreachable: the switch names every command
}

void Editor::Run(const Command& command) {
    // The key of the tool already in hand puts it down again - back to
    // Select, the hand at rest (see the Tool enum), which for a marking
    // tool means leaving drawing mode.
    const auto toggleTool = [this](Tool tool) { PickTool(activeTool_ == tool ? Tool::Select : tool); };
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
            if (views_ != nullptr) {
                views_->OpenItemProperties(command.item, command.at);
            }
            return;
        // The drawing bar: the tool to draw with, and the color. The tool
        // already in hand is cycled through its shapes instead - pen, line,
        // rectangle; eraser, rectangle eraser - so a plain drag makes them,
        // for a hand with no modifier key to hold (see PenShape).
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
            // The chooser opens next to the button.
            if (views_ != nullptr) {
                views_->OpenColorChooser(*command.at);
            }
            return;
        case CommandId::FullscreenScreenshot:
        case CommandId::FullscreenDrawing:
            // Asked for, so a drawing made this way is not watched as a
            // stray the way a double-click's is (see UntouchedDrawing) - as
            // with a creation tool. A capture leaves the overlay's own
            // window out (see IOverlayWindow::CaptureRegion), whatever menu
            // it was chosen from.
            CreateFullscreenItem(command.id == CommandId::FullscreenScreenshot ? ItemCreationKind::Screenshot
                                                                               : ItemCreationKind::Drawing);
            return;
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
    }
}

// Escape puts the hand down, in stages: a creation tool in hand goes
// back to Select, the hand at rest (see the Tool enum), then drawing mode
// ends, then a cut waiting to be pasted is called off, then a selection
// clears. What a stroke or a drag in flight does with it is Dispatch's:
// it is settled first, as for every command.
void Editor::PutDown() {
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

}  // namespace sz::ui
