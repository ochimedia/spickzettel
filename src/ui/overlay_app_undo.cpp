#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <string>

namespace sz::ui {

using namespace overlay_detail;

// ================= Undo =================
//
// The history itself is the session's (see Session::Undo); what is left
// here is saying what a step did.

void OverlayApp::Undo() {
    // Mid-drag or mid-stroke, what the hand has done so far is the most
    // recent thing done, and this takes it back; carrying on, a drag would
    // overwrite whatever the undo restored and drop that step from the
    // history unseen. See SettleHand.
    SettleHand();
    // A drawing a click made and nothing was put into is taken back by
    // going, not by leaving an empty snippet behind, marked deleted - when
    // it is the most recent thing done on its canvas. A press anywhere else
    // settles it as it happens, but a key does not: a paste made after it
    // is the step to take back, and the hand has moved on from the drawing
    // - which goes the way moving on takes it, without being the undo.
    if (untouchedDrawing_.has_value() && session_.HistoryRevision() != untouchedDrawingRevision_) {
        SettleUntouchedDrawing();
    }
    if (untouchedDrawing_.has_value() && session_.DiscardIfUntouched(*untouchedDrawing_)) {
        untouchedDrawing_.reset();
        ShowUndoStep(Session::UndoStep{Session::UndoWhat::Create, /*undone=*/true});
        return;
    }
    ShowUndoStep(session_.Undo());
}

void OverlayApp::Redo() {
    // See Undo. A drag that moved anything is a new step, so this finds
    // nothing left to redo - as it would once the drag was let go.
    SettleHand();
    ShowUndoStep(session_.Redo());
}

void OverlayApp::ShowUndoStep(const std::optional<Session::UndoStep>& step) {
    if (!step.has_value()) {
        return;
    }
    if (step->intoDeletedCanvas != 0) {
        // Where it went is out of sight, and the toast says where to find
        // it - see Session::UndoStep::intoDeletedCanvas.
        const Canvas* canvas = Manager().FindCanvas(step->intoDeletedCanvas);
        ShowActionToast(std::string(strings::kToastSentToDeletedCanvasPrefix) +
                        (canvas != nullptr ? canvas->name : std::string()));
        return;
    }
    // What the toast calls it - chosen once per kind, so the two directions
    // can't name the same thing differently.
    const char* what = strings::kUndoStroke;
    switch (step->what) {
        case Session::UndoWhat::Stroke:
            what = strings::kUndoStroke;
            break;
        case Session::UndoWhat::Erase:
            what = strings::kUndoErase;
            break;
        case Session::UndoWhat::Delete:
            what = strings::kUndoDelete;
            break;
        case Session::UndoWhat::TextEdit:
            what = strings::kUndoTextEdit;
            break;
        case Session::UndoWhat::Create:
            what = strings::kUndoCreate;
            break;
        case Session::UndoWhat::Placement:
            what = strings::kUndoPlacement;
            break;
        case Session::UndoWhat::Paste:
            what = strings::kUndoPaste;
            break;
        case Session::UndoWhat::Duplicate:
            what = strings::kUndoDuplicate;
            break;
        case Session::UndoWhat::Style:
            what = strings::kUndoStyle;
            break;
        case Session::UndoWhat::Move:
            what = strings::kUndoMove;
            break;
        case Session::UndoWhat::CopyTo:
            what = strings::kUndoCopyTo;
            break;
    }
    ShowActionToast(std::string(step->undone ? strings::kToastUndidPrefix : strings::kToastRedidPrefix) + what);
}

}  // namespace sz::ui
