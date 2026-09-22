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
    // A drawing a click made and nothing was put into is taken back by
    // going, not by leaving an empty snippet behind, marked deleted. It is always
    // the most recent thing done on its canvas: anything done since began
    // with a press somewhere else, and that press already settled it.
    if (untouchedDrawing_.has_value() && session_.DiscardIfUntouched(*untouchedDrawing_)) {
        untouchedDrawing_.reset();
        ShowUndoStep(Session::UndoStep{Session::UndoWhat::Create, /*undone=*/true});
        return;
    }
    ShowUndoStep(session_.Undo());
}

void OverlayApp::Redo() { ShowUndoStep(session_.Redo()); }

void OverlayApp::ShowUndoStep(const std::optional<Session::UndoStep>& step) {
    if (!step.has_value()) {
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
        case Session::UndoWhat::Painting:
            what = strings::kUndoPainting;
            break;
        case Session::UndoWhat::Create:
            what = strings::kUndoCreate;
            break;
        case Session::UndoWhat::Placement:
            what = strings::kUndoPlacement;
            break;
    }
    ShowActionToast(std::string(step->undone ? strings::kToastUndidPrefix : strings::kToastRedidPrefix) + what);
}

}  // namespace sz::ui
