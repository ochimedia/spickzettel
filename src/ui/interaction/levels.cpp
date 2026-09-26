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

}  // namespace sz::ui
