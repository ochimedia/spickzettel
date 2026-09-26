#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include "platform/platform_types.h"

namespace sz::core {

// Everything a key can be bound to while the overlay is up in edit mode:
// the drawing tools, the create actions, what the clipboard does with
// the selection, and the cheat sheet. One flat list rather than the two enums the app itself uses
// (`Tool`, `CreateAction`), for two reasons: a shortcut list is one list to
// the person reading it, and this is the layer that persists it - config
// sits below the app and cannot see those enums at all.
//
// The command table (ui/interaction/command.h) is the single place the two
// sides meet: each command names the action it is stored under, by name
// rather than by numbering, so adding a tool cannot silently bind the
// wrong thing.
//
// `ShortcutActionKey` is what ends up in config.json, so the *names* are
// the compatibility surface and the order below is only the order the
// Shortcuts tab lists them in. Draw and Erase are stored as "pen" and
// "eraser", the names of the buttons they light.
enum class ShortcutAction {
    Draw,
    Erase,
    Text,
    Select,
    NewScreenshot,
    NewDrawing,
    NewCanvas,
    NewCanvasWithSelection,
    Copy,
    Cut,
    Paste,
    Duplicate,
    CheatSheet,
};

inline constexpr std::array<ShortcutAction, 13> kAllShortcutActions = {
    ShortcutAction::Draw,      ShortcutAction::Erase,    ShortcutAction::Text,
    ShortcutAction::Select,    ShortcutAction::NewScreenshot, ShortcutAction::NewDrawing,
    ShortcutAction::NewCanvas, ShortcutAction::NewCanvasWithSelection, ShortcutAction::Copy,
    ShortcutAction::Cut,       ShortcutAction::Paste,    ShortcutAction::Duplicate,
    ShortcutAction::CheatSheet,
};
inline constexpr size_t kShortcutActionCount = kAllShortcutActions.size();

// One combo per action, indexed by the action's own position in
// `kAllShortcutActions`. An unset entry is a default-constructed KeyCombo
// (`key == 0`), which is also what unbinding writes - there is no separate
// "bound to nothing" flag to keep in step.
using ShortcutBindings = std::array<platform::KeyCombo, kShortcutActionCount>;

constexpr size_t ShortcutActionIndex(ShortcutAction action) { return static_cast<size_t>(action); }

// The token this action is written as in config.json's "shortcuts" object.
std::string_view ShortcutActionKey(ShortcutAction action);
std::optional<ShortcutAction> ShortcutActionFromKey(std::string_view key);

// What ships bound: the four marks-on-screen actions that get reached for
// constantly, on the letters that name them, and the clipboard's three on
// Ctrl+C/X/V, which every application on the machine has already taught
// the hand. Duplicate and "new canvas with the selection" ship on Ctrl+D
// and Ctrl+Shift+N for the same reason - both are chords rather than
// letters, so neither can fire from ordinary typing. Everything else
// starts unset rather than being given a letter nobody asked for - a
// shortcut that fires a tool you didn't want is worse than no shortcut.
ShortcutBindings DefaultShortcuts();

}  // namespace sz::core
