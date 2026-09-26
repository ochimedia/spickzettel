#include "ui/interaction/canvas.h"

#include <optional>

#include "ui/editor.h"
#include "ui/interaction/bursts.h"
#include "ui/interaction/recognizer.h"

namespace sz::ui {

namespace {
// The command `key` is bound to, started - or nothing, for a key bound to
// none. An arrow begins a nudge burst, when there is something to nudge.
Answer Bound(int key, const Event& event, Editor& editor) {
    const std::optional<CommandId> id = editor.CommandForKey(key, event.modifiers, event.repeat);
    if (!id.has_value()) {
        return Answer::Claim();
    }
    if (IsNudge(*id)) {
        return editor.Available(Command{*id}) ? Answer::Start(std::nullopt, std::make_unique<NudgeBurst>())
                                              : Answer::Claim();
    }
    return Answer::Start(Command{*id});
}
}  // namespace

Answer CanvasLevel::Offer(const Event& event, Editor& editor) {
    switch (event.kind) {
        case EventKind::PointerDown:
            if (event.button == platform::MouseButton::Left || event.button == platform::MouseButton::Right) {
                return RecognizePress(event, editor);
            }
            // A shortcut the button may be - never while a gesture is in
            // flight, which the Gesture level above sees to.
            return Bound(ComboKeyForMouseButton(event.button), event, editor);
        case EventKind::PointerMove:
        case EventKind::PointerUp:
            return Answer::Claim();  // dropped
        case EventKind::Wheel: {
            // The selection's size or opacity comes in bursts, when there
            // is a selection to change; the rest are steps of their own.
            const Editor::WheelKind kind = editor.KindOfWheel();
            if ((kind == Editor::WheelKind::SelectionSize || kind == Editor::WheelKind::SelectionOpacity) &&
                editor.SelectionLive() && !editor.Selection().empty()) {
                return Answer::Start(std::nullopt, std::make_unique<WheelBurst>(kind));
            }
            editor.Wheel(event.wheel);
            return Answer::Claim();
        }
        case EventKind::KeyDown:
            // The input options HUD's number keys, while it is up, ahead of
            // any command they might be bound to.
            if (editor.Views().InputOptionsKey(event)) {
                return Answer::Claim();
            }
            return Bound(event.key, event, editor);
        case EventKind::KeyUp:
        case EventKind::Modifiers:
        case EventKind::Tick:
        case EventKind::Lifecycle:
            return Answer::Claim();  // nothing waits on these here
        case EventKind::Hotkey:
            return Answer::Start(Command{event.command});
    }
    return Answer::Claim();
}

}  // namespace sz::ui
