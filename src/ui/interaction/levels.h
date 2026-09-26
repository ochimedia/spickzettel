#pragma once

// The levels of docs/INTERACTIONS.md between the canvas and the gesture:
// what is open over the canvas - a popup so far - as interactions of the
// machine. Each is the machine's record of something a view draws; the
// view opens and closes the real thing, and tells the machine when it has
// closed by itself (see EditorViews).

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

}  // namespace sz::ui
