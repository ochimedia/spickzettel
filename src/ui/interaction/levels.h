#pragma once

// The levels of docs/INTERACTIONS.md between the canvas and the gesture:
// what the canvas is in - drawing mode, a creation tool in hand - and what
// is open over it - a panel, a popup, text being typed, a key being
// captured - as interactions of the machine. Each is the machine's record of something a
// view draws; the view opens and closes the real thing, and tells the
// machine when it has closed by itself (see EditorViews).

#include <algorithm>
#include <functional>
#include <variant>
#include <vector>

#include "core/canvas/item.h"
#include "core/config/app_config.h"
#include "core/config/shortcut_action.h"
#include "core/session/actions.h"
#include "platform/platform_types.h"
#include "ui/interaction/machine.h"

namespace sz::ui {

// Snippets in drawing mode (section 5) - one, or every one selected when
// the mode was entered from the bar: the tool in hand is a marking tool -
// the pen, the eraser or Text - and what a press on one of them does is
// the recognizer's to say (see RecognizePress, rules 4, 5 and 12), asking
// this. Escape leaves it; Delete and the arrows do nothing in it, the
// snippets being worked in rather than on. Everything else goes on down.
// Ended, it leaves nothing of a stroke on the live layer.
class DrawingMode final : public Interaction {
public:
    DrawingMode(std::vector<core::ItemId> items, core::Tool tool) : items_(std::move(items)), tool_(tool) {}
    Level level() const override { return Level::Mode; }
    const char* Name() const override { return "DrawingMode"; }
    const std::vector<core::ItemId>& Items() const { return items_; }
    bool Holds(core::ItemId item) const;
    // Keeps only the snippets `keep` says to - see Editor::PruneSelection.
    template <typename Keep>
    void KeepOnly(Keep keep) {
        std::erase_if(items_, [&](core::ItemId item) { return !keep(item); });
    }
    core::Tool GetTool() const { return tool_; }
    // Another marking tool in hand. Its shape is the editor's, kept
    // whatever is in hand (see Editor::PenShape).
    void SetTool(core::Tool tool) { tool_ = tool; }
    Answer Offer(const Event& event, Editor& editor) override;
    void Interrupt(Editor& editor) override;
    void Cancel(Editor& editor) override { Interrupt(editor); }

private:
    std::vector<core::ItemId> items_;
    core::Tool tool_;
};

// A creation tool in hand: the next left press places a snippet of its
// kind, anywhere (rule 6). Escape puts it down.
class CreationTool final : public Interaction {
public:
    explicit CreationTool(core::ItemCreationKind kind) : kind_(kind) {}
    Level level() const override { return Level::Mode; }
    const char* Name() const override { return "CreationTool"; }
    core::ItemCreationKind Kind() const { return kind_; }
    Answer Offer(const Event& event, Editor& editor) override;
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}

private:
    core::ItemCreationKind kind_;
};

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
enum class PopupKind {
    ItemMenu,
    CanvasMenu,
    EmptyCanvasMenu,
    ItemProperties,
    ColorChooser,
    ShapeMenu,
    ConfirmDelete,
    LibraryReminder,
};
inline constexpr size_t kPopupKindCount = 8;
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
// get - for a hotkey, Escape only stops waiting. The global hotkeys are
// paused meanwhile, so the combination one holds arrives as a key like
// any other (see IPlatformHost::SetHotkeysPaused); one that fires anyway
// is the press, for a hotkey's row. The pointer is passed on, to the
// panel the row is in.
//
// A row waiting borrows the keyboard as a text field does (see
// IOverlayWindow::RequestTextInput) and pauses the hotkeys, whoever armed
// it; `release` gives both back, however the wait ends - a key bound,
// Escape, or the level ended from outside.
class KeyCapture final : public Interaction {
public:
    using Target = std::variant<core::HotkeySlot, core::ShortcutAction>;
    KeyCapture(Target target, std::function<void(platform::KeyCombo)> bind, std::function<void()> release = {})
        : target_(target), bind_(std::move(bind)), release_(std::move(release)) {}
    Level level() const override { return Level::Text; }
    const char* Name() const override { return "KeyCapture"; }
    const Target& Waiting() const { return target_; }
    Answer Offer(const Event& event, Editor& editor) override;
    void Interrupt(Editor& /*editor*/) override { Release(); }
    void Cancel(Editor& /*editor*/) override { Release(); }

private:
    bool ForHotkey() const { return std::holds_alternative<core::HotkeySlot>(target_); }
    Answer Answered(const Event& event);
    void Release() {
        if (release_) {
            release_();
        }
    }
    Target target_;
    std::function<void(platform::KeyCombo)> bind_;
    std::function<void()> release_;
};

}  // namespace sz::ui
