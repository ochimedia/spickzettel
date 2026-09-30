#include "ui/interaction/levels.h"

#include "ui/editor.h"
#include "ui/interaction/gestures.h"

namespace sz::ui {

// ================= DrawingMode =================

bool DrawingMode::Holds(core::ItemId item) const {
    return std::find(items_.begin(), items_.end(), item) != items_.end();
}

void DrawingMode::SetTool(core::Tool tool) {
    if (tool != tool_) {
        penShape_ = core::DrawShape::Freehand;
        eraserShape_ = core::DrawShape::Freehand;
    }
    tool_ = tool;
}

Answer DrawingMode::Offer(const Event& event, Editor& editor) {
    switch (event.kind) {
        case EventKind::KeyDown: {
            if (event.key == platform::KeyCombo::kEscape) {
                Interrupt(editor);
                return Answer::Finish();
            }
            const std::optional<CommandId> command = editor.CommandForKey(event.key, event.modifiers, event.repeat);
            const bool onTheSnippet =
                command.has_value() && (*command == CommandId::DeleteSelection || IsNudge(*command));
            return onTheSnippet ? Answer::Claim() : Answer::Pass();
        }
        case EventKind::PointerDown:
        case EventKind::PointerMove:
        case EventKind::PointerUp:
        case EventKind::Wheel:
        case EventKind::KeyUp:
        case EventKind::Modifiers:
        case EventKind::Tick:
        case EventKind::Hotkey:
            return Answer::Pass();
    }
    return Answer::Pass();
}

void DrawingMode::Interrupt(Editor& editor) { editor.GetSession().LiveLayer().Clear(); }

// ================= CreationTool =================

Answer CreationTool::Offer(const Event& event, Editor& /*editor*/) {
    switch (event.kind) {
        case EventKind::KeyDown:
            return event.key == platform::KeyCombo::kEscape ? Answer::Finish() : Answer::Pass();
        case EventKind::PointerDown:
        case EventKind::PointerMove:
        case EventKind::PointerUp:
        case EventKind::Wheel:
        case EventKind::KeyUp:
        case EventKind::Modifiers:
        case EventKind::Tick:
        case EventKind::Hotkey:
            return Answer::Pass();
    }
    return Answer::Pass();
}

// ================= Panel =================

Answer Panel::Offer(const Event& event, Editor& editor) {
    switch (event.kind) {
        case EventKind::PointerDown:
            // A mouse button bound to the panel's own command is its key.
            if (event.button != platform::MouseButton::Left && event.button != platform::MouseButton::Right &&
                kind_ == PanelKind::CheatSheet &&
                editor.CommandForKey(ComboKeyForMouseButton(event.button), event.modifiers, false) ==
                    CommandId::CheatSheet) {
                return Answer::Pass();
            }
            if (event.button == platform::MouseButton::Left || event.button == platform::MouseButton::Right) {
                return Answer::Start(std::nullopt, std::make_unique<Widget>(event));  // its widgets'
            }
            return Answer::Claim();
        case EventKind::PointerMove:
        case EventKind::PointerUp:
        case EventKind::Wheel:
            return Answer::Claim();
        case EventKind::KeyDown:
            if (event.key == platform::KeyCombo::kEscape) {
                if (editor.PopupOpen()) {
                    editor.Views().CloseInnermostPopup();
                    return Answer::Claim();
                }
                return Answer::Cancel();
            }
            if (kind_ == PanelKind::CheatSheet &&
                editor.CommandForKey(event.key, event.modifiers, event.repeat) == CommandId::CheatSheet) {
                return Answer::Pass();  // its own key, which closes it
            }
            return Answer::Claim();
        case EventKind::KeyUp:
        case EventKind::Modifiers:
        case EventKind::Tick:
        case EventKind::Hotkey:
            return Answer::Pass();
    }
    return Answer::Pass();
}

void Panel::Interrupt(Editor& editor) { editor.Views().ClosePanel(kind_); }

// ================= Popup =================

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
        case PopupKind::ShapeMenu:
            return "ShapeMenu";
        case PopupKind::ConfirmDelete:
            return "ConfirmDelete";
    }
    return "Popup";
}

Answer Popup::Offer(const Event& event, Editor& editor) {
    switch (event.kind) {
        case EventKind::PointerDown:
            // Its widgets', or a click outside that closes it.
            if (event.button == platform::MouseButton::Left || event.button == platform::MouseButton::Right) {
                return Answer::Start(std::nullopt, std::make_unique<Widget>(event));
            }
            return Answer::Claim();
        case EventKind::PointerMove:
        case EventKind::PointerUp:
        case EventKind::Wheel:
            return Answer::Claim();
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
            return Answer::Pass();
    }
    return Answer::Pass();
}

void TypingNote::Interrupt(Editor& editor) {
    if (editor.EditingNote() == item_) {
        editor.CommitNoteBeingEdited();
    }
}

// ================= NameEdit =================

Answer NameEdit::Offer(const Event& event, Editor& /*editor*/) {
    switch (event.kind) {
        case EventKind::PointerDown:
        case EventKind::PointerMove:
        case EventKind::PointerUp:
        case EventKind::Wheel:
            return Answer::Pass();
        case EventKind::KeyDown:
        case EventKind::KeyUp:
            return Answer::Claim();
        case EventKind::Modifiers:
            return Answer::Pass();
        case EventKind::Tick:
            return editing_() ? Answer::Pass() : Answer::Finish(/*usedUp=*/false);
        case EventKind::Hotkey:
            return Answer::Pass();
    }
    return Answer::Pass();
}

// ================= KeyCapture =================

Answer KeyCapture::Offer(const Event& event, Editor& /*editor*/) {
    const auto combo = [&event](int key) {
        return platform::KeyCombo{event.modifiers.ctrl, event.modifiers.alt, event.modifiers.shift, key};
    };
    switch (event.kind) {
        case EventKind::PointerDown:
            if (event.button == platform::MouseButton::Left || event.button == platform::MouseButton::Right) {
                return Answer::Pass();  // the panel's - its button disarms the row
            }
            if (ForHotkey()) {
                return Answer::Claim();  // a hotkey is a key
            }
            bind_(combo(ComboKeyForMouseButton(event.button)));
            return Answer::Finish();
        case EventKind::PointerMove:
        case EventKind::PointerUp:
        case EventKind::Wheel:
            return Answer::Pass();
        case EventKind::KeyDown: {
            if (event.key == platform::KeyCombo::kEscape) {
                if (!ForHotkey()) {
                    bind_(platform::KeyCombo{});
                }
                return Answer::Finish();
            }
            if (event.key == platform::KeyCombo::kBackspace || event.key == platform::KeyCombo::kDelete) {
                if (ForHotkey()) {
                    return Answer::Claim();
                }
                bind_(platform::KeyCombo{});
                return Answer::Finish();
            }
            const platform::KeyCombo pressed = combo(event.key);
            if (!pressed.IsValid()) {
                return Answer::Claim();  // not a key a row can hold
            }
            bind_(pressed);
            return Answer::Finish();
        }
        case EventKind::KeyUp:
            return Answer::Claim();
        case EventKind::Modifiers:
        case EventKind::Tick:
            return Answer::Pass();
        case EventKind::Hotkey:
            if (!ForHotkey()) {
                return Answer::Pass();
            }
            bind_(event.combo);
            return Answer::Finish();
    }
    return Answer::Pass();
}

}  // namespace sz::ui
