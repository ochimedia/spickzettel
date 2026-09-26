#pragma once

// The levels of docs/INTERACTIONS.md between the canvas and the gesture:
// what is open over the canvas - a popup, a note being typed - as
// interactions of the machine. Each is the machine's record of something a
// view draws; the view opens and closes the real thing, and tells the
// machine when it has closed by itself (see EditorViews).

#include "core/canvas/item.h"
#include "ui/interaction/machine.h"

namespace sz::ui {

// Every popup the app opens itself, by what it is.
enum class PopupKind { ItemMenu, CanvasMenu, EmptyCanvasMenu, ItemProperties, ColorChooser, ConfirmDelete };
inline constexpr size_t kPopupKindCount = 6;
const char* PopupName(PopupKind kind);

// A popup up over the canvas (section 5): the pointer is its - a press
// inside is its widgets', and one outside closes it and does nothing else,
// both of which ImGui does - and so is every key but the global hotkeys,
// which arrive as hotkey events: undo, a tool, the clipboard would all act
// on a canvas the popup is over, and it would stay up over one that had
// changed under it (decision 2). Escape closes it - or a popup of ImGui's
// own opened inside it, first. It finishes once the view no longer shows
// it, however it closed: a row chosen, a click outside, Escape.
class Popup final : public Interaction {
public:
    explicit Popup(PopupKind kind) : kind_(kind) {}
    Level level() const override { return Level::Popup; }
    const char* Name() const override { return PopupName(kind_); }
    PopupKind Kind() const { return kind_; }
    Answer Offer(const Event& event, Editor& editor) override;
    // Ended from outside - another popup opened, a command whose scope
    // covers popups: the view closes it.
    void Interrupt(Editor& editor) override;
    void Cancel(Editor& editor) override { Interrupt(editor); }

private:
    PopupKind kind_;
};

// A note being typed into (section 5): every key is the text field's, and
// a press is too while it lands on the field - or on any of ImGui's
// windows. A press anywhere else is passed on, and the field, let go of,
// keeps the text. Escape ends the typing, keeping the text too - Undo is
// the way to take typing back (decision 1) - and so does ending it from
// outside. It finishes once the note is no longer being typed into,
// however that ended: the field let go of, the session ending the edit for
// a command.
class TypingNote final : public Interaction {
public:
    explicit TypingNote(core::ItemId item) : item_(item) {}
    Level level() const override { return Level::Text; }
    const char* Name() const override { return "TypingNote"; }
    core::ItemId Item() const { return item_; }
    void Begin(const Event& event, Editor& editor) override;
    Answer Offer(const Event& event, Editor& editor) override;
    void Interrupt(Editor& editor) override;
    void Cancel(Editor& editor) override { Interrupt(editor); }

private:
    core::ItemId item_;
};

}  // namespace sz::ui
