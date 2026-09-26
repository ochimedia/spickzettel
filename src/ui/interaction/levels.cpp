#include "ui/interaction/levels.h"

#include "ui/editor.h"

namespace sz::ui {

const char* PopupName(PopupKind kind) {
    switch (kind) {
        case PopupKind::ItemMenu:
            return "ItemMenu";
        case PopupKind::CanvasMenu:
            return "CanvasMenu";
        case PopupKind::EmptyCanvasMenu:
            return "EmptyCanvasMenu";
        case PopupKind::ItemProperties:
            return "ItemProperties";
        case PopupKind::ColorChooser:
            return "ColorChooser";
        case PopupKind::ConfirmDelete:
            return "ConfirmDelete";
    }
    return "Popup";
}

Answer Popup::Offer(const Event& event, Editor& editor) {
    switch (event.kind) {
        case EventKind::PointerDown:
        case EventKind::PointerMove:
        case EventKind::PointerUp:
        case EventKind::Wheel:
            return Answer::Claim();  // its widgets', or a click outside that closes it
        case EventKind::KeyDown:
            if (event.key == platform::KeyCombo::kEscape) {
                editor.Views().CloseInnermostPopup();
            }
            return Answer::Claim();
        case EventKind::KeyUp:
        case EventKind::Modifiers:
            return Answer::Pass();
        case EventKind::Tick:
            if (!editor.Views().PopupShowing(kind_)) {
                return Answer::Finish(/*usedUp=*/false);  // closed by itself, however
            }
            return Answer::Pass();
        case EventKind::Hotkey:
        case EventKind::Lifecycle:
            return Answer::Pass();
    }
    return Answer::Pass();
}

void Popup::Interrupt(Editor& editor) { editor.Views().ClosePopup(kind_); }

void TypingNote::Begin(const Event& /*event*/, Editor& editor) { editor.BeginEditingNote(item_); }

Answer TypingNote::Offer(const Event& event, Editor& editor) {
    switch (event.kind) {
        case EventKind::PointerDown:
            if (event.button != platform::MouseButton::Left && event.button != platform::MouseButton::Right) {
                return Answer::Claim();  // no binding acts under the field
            }
            if (editor.PointerOverView()) {
                return Answer::Claim();  // the field's, or a panel's
            }
            // Outside: not the note's. It goes on to do what it does - with
            // the note still open, which is what keeps it from making a
            // snippet (see RecognizePress) - and the field, let go of,
            // commits the text, which ends this at the next tick.
            return Answer::Pass();
        case EventKind::PointerMove:
        case EventKind::PointerUp:
        case EventKind::Wheel:
            return Answer::Claim();
        case EventKind::KeyDown:
            if (event.key == platform::KeyCombo::kEscape) {
                editor.CommitNoteBeingEdited();
                return Answer::Finish();
            }
            return Answer::Claim();
        case EventKind::KeyUp:
            return Answer::Claim();
        case EventKind::Modifiers:
            return Answer::Pass();
        case EventKind::Tick:
            if (editor.EditingNote() != item_) {
                return Answer::Finish(/*usedUp=*/false);
            }
            return Answer::Pass();
        case EventKind::Hotkey:
        case EventKind::Lifecycle:
            return Answer::Pass();
    }
    return Answer::Pass();
}

void TypingNote::Interrupt(Editor& editor) {
    if (editor.EditingNote() == item_) {
        editor.CommitNoteBeingEdited();
    }
}

}  // namespace sz::ui
