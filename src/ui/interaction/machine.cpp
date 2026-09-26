#include "ui/interaction/machine.h"

#include <cassert>

#include "ui/editor.h"
#include "ui/interaction/gestures.h"

namespace sz::ui {

const char* LevelName(Level level) {
    switch (level) {
        case Level::Canvas:
            return "Canvas";
        case Level::Mode:
            return "Mode";
        case Level::Panel:
            return "Panel";
        case Level::Popup:
            return "Popup";
        case Level::Text:
            return "Text";
        case Level::Gesture:
            return "Gesture";
    }
    return "?";
}

Event Event::FromInput(const platform::InputEvent& input) {
    Event event;
    switch (input.kind) {
        case platform::InputEventKind::PointerDown:
            event.kind = EventKind::PointerDown;
            break;
        case platform::InputEventKind::PointerMove:
            event.kind = EventKind::PointerMove;
            break;
        case platform::InputEventKind::PointerUp:
            event.kind = EventKind::PointerUp;
            break;
        case platform::InputEventKind::Wheel:
            event.kind = EventKind::Wheel;
            break;
        case platform::InputEventKind::KeyDown:
            event.kind = EventKind::KeyDown;
            break;
        case platform::InputEventKind::KeyUp:
            event.kind = EventKind::KeyUp;
            break;
        case platform::InputEventKind::Modifiers:
            event.kind = EventKind::Modifiers;
            break;
        case platform::InputEventKind::Tick:
            event.kind = EventKind::Tick;
            break;
    }
    event.seconds = input.seconds;
    event.modifiers = input.modifiers;
    event.position = input.position;
    event.button = input.button;
    event.buttons = input.buttons;
    event.wheel = input.wheel;
    event.key = input.key;
    event.repeat = input.repeat;
    return event;
}

void Machine::SetRoot(std::unique_ptr<Interaction> root) {
    assert(root != nullptr && root->level() == Level::Canvas);
    stack_[static_cast<size_t>(Level::Canvas)] = std::move(root);
}

void Machine::Offer(const Event& event) {
    const bool gestureButton =
        event.button == platform::MouseButton::Left || event.button == platform::MouseButton::Right;
    if (event.kind == EventKind::PointerDown && gestureButton) {
        held_ |= platform::ButtonBit(event.button);
        lastPressed_ = event.button;
    } else if (event.kind == EventKind::PointerUp && gestureButton) {
        held_ &= static_cast<uint8_t>(~platform::ButtonBit(event.button));
    }
    Route(event);
    LeaveSpentIfHeld();
    CheckLevels();
}

void Machine::Route(const Event& event) {
    for (size_t index = kLevelCount; index-- > 0;) {
        Interaction* offered = stack_[index].get();
        if (offered == nullptr) {
            continue;
        }
        Answer answer = offered->Offer(event, editor_);
        // An interaction does not change the stack under its own answer.
        assert(stack_[index].get() == offered && "an interaction changed the stack while answering");
        switch (answer.kind) {
            case Answer::Kind::Claim:
                return;
            case Answer::Kind::Pass:
                continue;
            case Answer::Kind::Finish:
                // Popped as it is: it has ended itself, as it meant to.
                stack_[index].reset();
                if (answer.command.has_value()) {
                    editor_.Dispatch(*answer.command);
                }
                if (answer.usedUp) {
                    return;
                }
                continue;
            case Answer::Kind::Cancel: {
                const std::unique_ptr<Interaction> cancelled = std::move(stack_[index]);
                cancelled->Cancel(editor_);
                return;
            }
            case Answer::Kind::Start:
                if (answer.command.has_value()) {
                    editor_.Dispatch(*answer.command);
                }
                if (answer.push != nullptr) {
                    // Section 4.2: what is on the new interaction's level and
                    // above it ends first - see Push. The levels between it
                    // and the one that started it passed the event, and stay.
                    Push(std::move(answer.push), event);
                }
                return;
        }
    }
}

void Machine::Push(std::unique_ptr<Interaction> interaction, const Event& cause) {
    const auto level = static_cast<size_t>(interaction->level());
    assert(level != static_cast<size_t>(Level::Canvas) && "the Canvas level is the root's");
    for (size_t index = kLevelCount; index-- > level;) {
        InterruptAt(index);
    }
    stack_[level] = std::move(interaction);
    stack_[level]->Begin(cause, editor_);
    LeaveSpentIfHeld();
    CheckLevels();
}

void Machine::EndFor(Scope scope) {
    switch (scope) {
        case Scope::Hand:
            End(Level::Gesture);
            End(Level::Text);
            return;
        case Scope::Canvas:
            End(Level::Gesture);
            End(Level::Text);
            End(Level::Popup);
            return;
        case Scope::All:
            End(Level::Gesture);
            End(Level::Text);
            End(Level::Popup);
            End(Level::Panel);
            End(Level::Mode);
            return;
    }
}

void Machine::End(Level level) {
    assert(level != Level::Canvas && "the Canvas level is always there");
    InterruptAt(static_cast<size_t>(level));
    LeaveSpentIfHeld();
    CheckLevels();
}

void Machine::Forget() {
    held_ = 0;
    InterruptAt(static_cast<size_t>(Level::Gesture));
    CheckLevels();
}

void Machine::LeaveSpentIfHeld() {
    std::unique_ptr<Interaction>& gesture = stack_[static_cast<size_t>(Level::Gesture)];
    if (gesture != nullptr || held_ == 0) {
        return;
    }
    const platform::MouseButton button =
        (held_ & platform::ButtonBit(lastPressed_)) != 0
            ? lastPressed_
            : ((held_ & platform::ButtonBit(platform::MouseButton::Left)) != 0 ? platform::MouseButton::Left
                                                                                : platform::MouseButton::Right);
    gesture = std::make_unique<Spent>(button);
}

void Machine::InterruptAt(size_t index) {
    if (stack_[index] == nullptr) {
        return;
    }
    // Off the stack before it is told, so that whatever it does on the way
    // out sees it gone.
    const std::unique_ptr<Interaction> ended = std::move(stack_[index]);
    ended->Interrupt(editor_);
}

std::string Machine::Describe() const {
    std::string text;
    for (size_t index = 0; index < kLevelCount; ++index) {
        if (index > 0) {
            text += " / ";
        }
        text += stack_[index] != nullptr ? stack_[index]->Name() : "-";
    }
    return text;
}

void Machine::CheckLevels() const {
#ifndef NDEBUG
    for (size_t index = 0; index < kLevelCount; ++index) {
        assert((stack_[index] == nullptr || static_cast<size_t>(stack_[index]->level()) == index) &&
               "an interaction is on a level not its own");
    }
#endif
}

}  // namespace sz::ui
