#pragma once

// What the machine of docs/INTERACTIONS.md is offered: one thing that
// happened, with its time and the modifiers held at that moment - section
// 3. The window's input stream (platform::InputEvent) plus what the app
// itself tells: a global hotkey, and the overlay coming and going.

#include <cstdint>

#include "platform/platform_types.h"
#include "ui/interaction/command.h"

namespace sz::ui {

// Every interaction answers each of these in a switch with no default
// (see Interaction::Offer): a kind added here is a compile error in every
// one of them until someone decides what it means there.
enum class EventKind {
    PointerDown,
    PointerMove,
    PointerUp,
    Wheel,
    KeyDown,
    KeyUp,
    Modifiers,
    // Once a frame: what timeouts run on - a press held still for a hold.
    Tick,
    // A global hotkey, from the tray - `command` is what it runs. May
    // arrive while the overlay is hidden, with no frames.
    Hotkey,
};

struct Event {
    EventKind kind = EventKind::Tick;
    double seconds = 0.0;
    platform::Modifiers modifiers;
    platform::Vec2 position;
    platform::MouseButton button = platform::MouseButton::Left;
    uint8_t buttons = 0;  // held, for a PointerMove
    float wheel = 0.0f;   // notches
    int key = 0;          // KeyCombo's encoding
    bool repeat = false;
    // For a Hotkey: the command, and the combination it is bound to - a
    // key being captured in Settings takes the combination instead.
    CommandId command = CommandId::Undo;
    platform::KeyCombo combo;

    static Event FromInput(const platform::InputEvent& input);
};

}  // namespace sz::ui
