#include "ui/interaction/bursts.h"

#include <optional>

namespace sz::ui {

namespace {
// Which of the arrows `key` is, as a bit - 0 for any other key.
uint8_t ArrowBit(int key) {
    switch (key) {
        case platform::KeyCombo::kLeftArrow:
            return 1;
        case platform::KeyCombo::kRightArrow:
            return 2;
        case platform::KeyCombo::kUpArrow:
            return 4;
        case platform::KeyCombo::kDownArrow:
            return 8;
        default:
            return 0;
    }
}

// A burst's step is the last it takes when a second passes without
// another - the wheel has no "up" to end it, and a run of arrow presses
// is one undo, as it has been.
bool Lapsed(const Event& event, double lastStep) { return event.seconds - lastStep >= kBurstSeconds; }

// Whether a burst's steps hold anything open on the session to take back.
bool HoldsOpen(Editor& editor, bool style) {
    return style ? editor.GetSession().StyleEditOpen() : editor.GetSession().PlacementOpen();
}
}  // namespace

// ================= NudgeBurst =================

void NudgeBurst::Begin(const Event& event, Editor& editor) { Nudge(event, editor); }

bool NudgeBurst::Nudge(const Event& event, Editor& editor) {
    const std::optional<CommandId> id = editor.CommandForKey(event.key, event.modifiers, event.repeat);
    if (!id.has_value() || !IsNudge(*id)) {
        return false;
    }
    held_ |= ArrowBit(event.key);
    if (editor.Step(Command{*id})) {
        lastStep_ = event.seconds;
    }
    return true;
}

Answer NudgeBurst::Offer(const Event& event, Editor& editor) {
    switch (event.kind) {
        case EventKind::PointerDown:
        case EventKind::Wheel:
            // Something else begins: the burst is done, and the event is
            // that something's.
            Interrupt(editor);
            return Answer::Finish(/*usedUp=*/false);
        case EventKind::PointerMove:
        case EventKind::PointerUp:
            return Answer::Pass();
        case EventKind::KeyDown:
            if (event.key == platform::KeyCombo::kEscape) {
                if (held_ != 0 && HoldsOpen(editor, /*style=*/false)) {
                    return Answer::Cancel();
                }
                Interrupt(editor);
                return Answer::Finish(/*usedUp=*/false);
            }
            // An arrow is the next nudge; any other key goes on, and a
            // command it reaches ends the burst first.
            return Nudge(event, editor) ? Answer::Claim() : Answer::Pass();
        case EventKind::KeyUp:
            held_ &= static_cast<uint8_t>(~ArrowBit(event.key));
            return Answer::Pass();
        case EventKind::Tick:
            if (Lapsed(event, lastStep_)) {
                Interrupt(editor);
                return Answer::Finish(/*usedUp=*/false);
            }
            return Answer::Pass();
        case EventKind::Modifiers:
        case EventKind::Hotkey:
        case EventKind::Lifecycle:
            return Answer::Pass();
    }
    return Answer::Pass();
}

void NudgeBurst::Interrupt(Editor& editor) { editor.GetSession().EndPlacement(); }

void NudgeBurst::Cancel(Editor& editor) { editor.GetSession().CancelPlacement(); }

// ================= WheelBurst =================

void WheelBurst::Begin(const Event& event, Editor& editor) {
    editor.Wheel(event.wheel);
    lastStep_ = event.seconds;
}

Answer WheelBurst::Offer(const Event& event, Editor& editor) {
    switch (event.kind) {
        case EventKind::Wheel:
            if (editor.KindOfWheel() == kind_) {
                editor.Wheel(event.wheel);
                lastStep_ = event.seconds;
                return Answer::Claim();
            }
            Interrupt(editor);
            return Answer::Finish(/*usedUp=*/false);  // a notch of another kind, and its own
        case EventKind::PointerDown:
            Interrupt(editor);
            return Answer::Finish(/*usedUp=*/false);
        case EventKind::PointerMove:
        case EventKind::PointerUp:
            return Answer::Pass();
        case EventKind::KeyDown:
            if (event.key == platform::KeyCombo::kEscape) {
                if (HoldsOpen(editor, /*style=*/kind_ == Editor::WheelKind::SelectionOpacity)) {
                    return Answer::Cancel();
                }
                Interrupt(editor);
                return Answer::Finish(/*usedUp=*/false);
            }
            return Answer::Pass();
        case EventKind::Tick:
            if (Lapsed(event, lastStep_)) {
                Interrupt(editor);
                return Answer::Finish(/*usedUp=*/false);
            }
            return Answer::Pass();
        case EventKind::KeyUp:
        case EventKind::Modifiers:
        case EventKind::Hotkey:
        case EventKind::Lifecycle:
            return Answer::Pass();
    }
    return Answer::Pass();
}

void WheelBurst::Interrupt(Editor& editor) {
    if (kind_ == Editor::WheelKind::SelectionOpacity) {
        editor.GetSession().EndStyleEdit();
    } else {
        editor.GetSession().EndPlacement();
    }
}

void WheelBurst::Cancel(Editor& editor) {
    if (kind_ == Editor::WheelKind::SelectionOpacity) {
        editor.GetSession().CancelStyleEdit();
    } else {
        editor.GetSession().CancelPlacement();
    }
}

}  // namespace sz::ui
