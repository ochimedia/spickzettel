#pragma once

// The levels of docs/INTERACTIONS.md between the canvas and the gesture:
// what is open over the canvas - a panel, a popup, text being typed, a key
// being captured - as interactions of the machine. Each is the machine's record of something a
// view draws; the view opens and closes the real thing, and tells the
// machine when it has closed by itself (see EditorViews).

#include <functional>
#include <variant>

#include "core/canvas/item.h"
#include "core/config/app_config.h"
#include "core/config/shortcut_action.h"
#include "core/session/actions.h"
#include "platform/platform_types.h"
#include "ui/interaction/machine.h"

namespace sz::ui {

// The panels that cover the canvas.
enum class PanelKind { Overview, CheatSheet };

// A panel over the canvas (section 5): the Overview, the cheat sheet. The
// pointer and the wheel are its - it covers the canvas, and its widgets are
// ImGui's - and so is every key but the global hotkeys and its own: the
// cheat sheet's key closes the cheat sheet. Escape closes it - or a popup
// of ImGui's own open inside it, first. Its own widgets close it too, which
// the view does (see Machine::End).
class Panel final : public Interaction {
public:
    explicit Panel(PanelKind kind) : kind_(kind) {}
    Level level() const override { return Level::Panel; }
    const char* Name() const override { return kind_ == PanelKind::Overview ? "Overview" : "CheatSheet"; }
    PanelKind Kind() const { return kind_; }
    Answer Offer(const Event& event, Editor& editor) override;
    // Ended however - Escape, something started below it, the overlay
    // going away: the view closes it.
    void Interrupt(Editor& editor) override;
    void Cancel(Editor& editor) override { Interrupt(editor); }

private:
    PanelKind kind_;
};

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

// A name being edited in the Overview - a folder's or a canvas's, in a
// field of ImGui's: every key is the field's, including Escape, which gives
// up the edit there. A press is passed on to the panel, whose field lets go
// of it. It finishes once the view no longer edits a name, and ended from
// outside it asks the view to stop, keeping nothing.
class NameEdit final : public Interaction {
public:
    NameEdit(std::function<bool()> editing, std::function<void()> stop)
        : editing_(std::move(editing)), stop_(std::move(stop)) {}
    Level level() const override { return Level::Text; }
    const char* Name() const override { return "NameEdit"; }
    Answer Offer(const Event& event, Editor& editor) override;
    void Interrupt(Editor& /*editor*/) override { stop_(); }
    void Cancel(Editor& /*editor*/) override { stop_(); }

private:
    std::function<bool()> editing_;
    std::function<void()> stop_;
};

// A Settings row waiting for the key a person wants (section 7): a global
// hotkey's, or the key of a command a person chooses. The next key that
// can be one - a letter, a digit or a function key, with whatever
// modifiers are held - is bound, and for a chosen key the middle or a side
// mouse button as well. Escape, and for a chosen key Backspace and Delete,
// bind nothing: a row showing "(none)" is what a hand reaches for them to
// get - for a hotkey, Escape only stops waiting. A global hotkey that
// fires meanwhile is the press, for a hotkey's row: Windows hands a
// registered combination to its hotkey and to nothing else, so it never
// arrives as a key. The pointer is passed on, to the panel the row is in.
class KeyCapture final : public Interaction {
public:
    using Target = std::variant<core::HotkeySlot, core::ShortcutAction>;
    KeyCapture(Target target, std::function<void(platform::KeyCombo)> bind)
        : target_(target), bind_(std::move(bind)) {}
    Level level() const override { return Level::Text; }
    const char* Name() const override { return "KeyCapture"; }
    const Target& Waiting() const { return target_; }
    Answer Offer(const Event& event, Editor& editor) override;
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}

private:
    bool ForHotkey() const { return std::holds_alternative<core::HotkeySlot>(target_); }
    Target target_;
    std::function<void(platform::KeyCombo)> bind_;
};

}  // namespace sz::ui
