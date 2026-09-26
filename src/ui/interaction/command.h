#pragma once

// Everything the app can be told to do in one step, and what reaches it -
// see docs/INTERACTIONS.md, section 7. A key, a context menu row, a
// button on the selection bar and a global hotkey all name a Command, and
// every one of them is run by Editor::Dispatch, which settles what the
// command's scope covers first. No ImGui in here: what a command is and
// which keys reach it are the app's words, not its widgets'.

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "core/config/app_config.h"
#include "core/config/shortcut_action.h"
#include "core/session/actions.h"
#include "platform/platform_types.h"

namespace sz::ui {

// Grouped by what reaches them. Fixed keys first, so a fixed key and a
// chosen one that share a combination go to the fixed one (see
// KeysFor): the chosen ones are what a person can move.
enum class CommandId {
    // Fixed keys.
    Undo,
    Redo,
    PutDown,  // Escape: the tool down, then drawing mode, a cut, the selection
    DeleteSelection,
    NudgeLeft,
    NudgeRight,
    NudgeUp,
    NudgeDown,
    // Keys a person chooses (see core::ShortcutAction). A tool's key puts
    // the tool down again when it is in hand already.
    DrawTool,
    EraseTool,
    TextTool,
    SelectTool,
    NewScreenshotTool,
    NewDrawingTool,
    NewCanvas,
    NewCanvasWithSelection,
    Copy,
    Cut,
    Paste,
    Duplicate,
    CheatSheet,
    // Global hotkeys, which are the tray's to run (see
    // OverlayApp::SetAppCommandCallback).
    ToggleEditMode,
    ToggleViewMode,
    QuickCapture,
    SilentCapture,
    // Reached from a menu or the selection bar only - and the two
    // fullscreen snippets from a gesture too, a double-click or a hold.
    ToggleFullscreen,
    ToggleFullscreenStretched,  // the menu row with Shift held
    ResetSize,
    ClearDrawing,
    SendBackward,
    BringForward,
    MoveToCanvas,
    Minimize,
    Pin,
    Properties,
    PenButton,     // the pen, or its next shape when it is in hand
    EraserButton,  // the eraser, or its next shape when it is in hand
    TextButton,
    ColorButton,
    FullscreenScreenshot,
    FullscreenDrawing,
    DeleteCanvas,
    Overview,
    Settings,
    // Reached from a gesture only: what a press on the canvas means once
    // the recognizer knows (see docs/INTERACTIONS.md, section 6.5).
    DrawingMode,       // on `item` - a double-click, a hold
    LeaveDrawingMode,  // a press elsewhere, a right click on the snippet
    ItemMenu,          // `item`'s context menu, at `at` - a right click
    EmptyCanvasMenu,   // at `at` - a right click on empty canvas
    FrameSnippet,      // of `kind`, at `rect` - a drag on empty canvas, or with a creation tool
};

// What a command ends before it runs - see OverlayApp::SettleHand, and
// docs/INTERACTIONS.md, section 4.2. Both end the hand and a note being
// typed today; Canvas is kept apart because it is the scope of whatever
// switches or empties the canvas, which will end more than the hand.
enum class Scope { Hand, Canvas };

// Who asked for a snippet to be made: a menu row or a key, the creation
// tool in hand, or a press on empty canvas - which decides what happens
// around it (see Editor::Run): the screenshot tool is put down once it has
// placed, and a drawing a press made is watched until something goes
// into it (see Editor::UntouchedDrawing).
enum class MadeBy { Asking, Tool, Press };

// One command, with what it is about. Most take the selection as it is
// and need nothing else; a menu row names the snippet or the canvas it
// was opened on, and a bar button where it sits, for what it opens. A
// snippet made by a gesture says of what kind, where, and who made it.
struct Command {
    CommandId id = CommandId::Undo;
    core::ItemId item = 0;
    core::CanvasId canvas = 0;
    std::optional<platform::Vec2> at;
    std::optional<core::Rect> rect;
    core::ItemCreationKind kind = core::ItemCreationKind::Screenshot;
    MadeBy madeBy = MadeBy::Asking;
};

struct CommandInfo {
    CommandId id;
    // A stable name, for tests and logs.
    std::string_view name;
    Scope scope = Scope::Hand;
    // The key a person chooses for it, stored under this name (see
    // core::ShortcutAction) - or none.
    std::optional<core::ShortcutAction> shortcut;
    // The global hotkey that runs it (see AppConfig::hotkeyEditMode and
    // the three after it) - or none.
    std::optional<core::HotkeySlot> hotkey;
    // Keys of its own that nobody rebinds: undo's Ctrl+Z, Escape, the
    // arrows. A key of 0 is none.
    std::array<platform::KeyCombo, 2> keys{};
    // Whether those match with any modifiers held, as Escape, Delete and
    // the arrows always have - a nudge reads Shift itself, for ten pixels.
    bool anyModifiers = false;
    // Whether a key held down runs it again at the key's repeat rate.
    bool repeats = false;
};

namespace command_detail {
using platform::KeyCombo;
constexpr KeyCombo Key(int key, bool ctrl = false, bool shift = false) { return KeyCombo{ctrl, false, shift, key}; }
// The four kinds of row: a command on keys of its own, on a key a person
// chooses, on a global hotkey, and on nothing but a menu or the bar.
constexpr CommandInfo Fixed(CommandId id, std::string_view name, std::array<KeyCombo, 2> keys, bool anyModifiers,
                            bool repeats) {
    return CommandInfo{id, name, Scope::Hand, std::nullopt, std::nullopt, keys, anyModifiers, repeats};
}
constexpr CommandInfo Chosen(CommandId id, std::string_view name, core::ShortcutAction shortcut,
                             Scope scope = Scope::Hand) {
    return CommandInfo{id, name, scope, shortcut};
}
constexpr CommandInfo Hotkey(CommandId id, std::string_view name, core::HotkeySlot slot, Scope scope = Scope::Hand) {
    return CommandInfo{id, name, scope, std::nullopt, slot};
}
constexpr CommandInfo Clicked(CommandId id, std::string_view name) { return CommandInfo{id, name}; }
}  // namespace command_detail

// The table, in CommandId order - checked below, so that a command added
// without a row, or a row out of place, does not compile.
inline constexpr std::array kCommands = [] {
    using namespace command_detail;
    using core::HotkeySlot;
    using core::ShortcutAction;
    constexpr bool kAny = true;      // anyModifiers
    constexpr bool kRepeats = true;  // repeats
    return std::array{
        Fixed(CommandId::Undo, "undo", {Key('Z', /*ctrl=*/true)}, !kAny, kRepeats),
        Fixed(CommandId::Redo, "redo", {Key('Y', true), Key('Z', true, /*shift=*/true)}, !kAny, kRepeats),
        Fixed(CommandId::PutDown, "putDown", {Key(KeyCombo::kEscape)}, kAny, kRepeats),
        Fixed(CommandId::DeleteSelection, "deleteSelection", {Key(KeyCombo::kDelete), Key(KeyCombo::kBackspace)},
              kAny, !kRepeats),
        Fixed(CommandId::NudgeLeft, "nudgeLeft", {Key(KeyCombo::kLeftArrow)}, kAny, kRepeats),
        Fixed(CommandId::NudgeRight, "nudgeRight", {Key(KeyCombo::kRightArrow)}, kAny, kRepeats),
        Fixed(CommandId::NudgeUp, "nudgeUp", {Key(KeyCombo::kUpArrow)}, kAny, kRepeats),
        Fixed(CommandId::NudgeDown, "nudgeDown", {Key(KeyCombo::kDownArrow)}, kAny, kRepeats),
        Chosen(CommandId::DrawTool, "drawTool", ShortcutAction::Draw),
        Chosen(CommandId::EraseTool, "eraseTool", ShortcutAction::Erase),
        Chosen(CommandId::TextTool, "textTool", ShortcutAction::Text),
        Chosen(CommandId::SelectTool, "selectTool", ShortcutAction::Select),
        Chosen(CommandId::NewScreenshotTool, "newScreenshotTool", ShortcutAction::NewScreenshot),
        Chosen(CommandId::NewDrawingTool, "newDrawingTool", ShortcutAction::NewDrawing),
        Chosen(CommandId::NewCanvas, "newCanvas", ShortcutAction::NewCanvas, Scope::Canvas),
        Chosen(CommandId::NewCanvasWithSelection, "newCanvasWithSelection", ShortcutAction::NewCanvasWithSelection,
               Scope::Canvas),
        Chosen(CommandId::Copy, "copy", ShortcutAction::Copy),
        Chosen(CommandId::Cut, "cut", ShortcutAction::Cut),
        Chosen(CommandId::Paste, "paste", ShortcutAction::Paste),
        Chosen(CommandId::Duplicate, "duplicate", ShortcutAction::Duplicate),
        Chosen(CommandId::CheatSheet, "cheatSheet", ShortcutAction::CheatSheet),
        Hotkey(CommandId::ToggleEditMode, "toggleEditMode", HotkeySlot::EditMode),
        Hotkey(CommandId::ToggleViewMode, "toggleViewMode", HotkeySlot::ViewMode),
        Hotkey(CommandId::QuickCapture, "quickCapture", HotkeySlot::QuickCapture, Scope::Canvas),
        Hotkey(CommandId::SilentCapture, "silentCapture", HotkeySlot::SilentCapture, Scope::Canvas),
        Clicked(CommandId::ToggleFullscreen, "toggleFullscreen"),
        Clicked(CommandId::ToggleFullscreenStretched, "toggleFullscreenStretched"),
        Clicked(CommandId::ResetSize, "resetSize"),
        Clicked(CommandId::ClearDrawing, "clearDrawing"),
        Clicked(CommandId::SendBackward, "sendBackward"),
        Clicked(CommandId::BringForward, "bringForward"),
        Clicked(CommandId::MoveToCanvas, "moveToCanvas"),
        Clicked(CommandId::Minimize, "minimize"),
        Clicked(CommandId::Pin, "pin"),
        Clicked(CommandId::Properties, "properties"),
        Clicked(CommandId::PenButton, "penButton"),
        Clicked(CommandId::EraserButton, "eraserButton"),
        Clicked(CommandId::TextButton, "textButton"),
        Clicked(CommandId::ColorButton, "colorButton"),
        Clicked(CommandId::FullscreenScreenshot, "fullscreenScreenshot"),
        Clicked(CommandId::FullscreenDrawing, "fullscreenDrawing"),
        Clicked(CommandId::DeleteCanvas, "deleteCanvas"),
        Clicked(CommandId::Overview, "overview"),
        Clicked(CommandId::Settings, "settings"),
        Clicked(CommandId::DrawingMode, "drawingMode"),
        Clicked(CommandId::LeaveDrawingMode, "leaveDrawingMode"),
        Clicked(CommandId::ItemMenu, "itemMenu"),
        Clicked(CommandId::EmptyCanvasMenu, "emptyCanvasMenu"),
        Clicked(CommandId::FrameSnippet, "frameSnippet"),
    };
}();
inline constexpr size_t kCommandCount = kCommands.size();

namespace command_detail {
constexpr bool InIdOrder() {
    for (size_t i = 0; i < kCommands.size(); ++i) {
        if (static_cast<size_t>(kCommands[i].id) != i) {
            return false;
        }
    }
    return kCommands.back().id == CommandId::FrameSnippet;  // the last id: none left without a row
}
}  // namespace command_detail
static_assert(command_detail::InIdOrder(), "kCommands must hold one row per CommandId, in CommandId order");

constexpr const CommandInfo& InfoFor(CommandId id) { return kCommands[static_cast<size_t>(id)]; }

// The command a chosen key, a global hotkey or a selection bar button
// runs. Every ShortcutAction and every HotkeySlot names exactly one (see
// the tests); every bar button is one.
std::optional<CommandId> CommandForShortcut(core::ShortcutAction action);
std::optional<CommandId> CommandForHotkey(core::HotkeySlot slot);
CommandId CommandForBarButton(core::ChromeButton button);

// The global hotkey `slot` is set to in `config`.
const platform::KeyCombo& HotkeyCombo(const core::AppConfig& config, core::HotkeySlot slot);

// Every key that runs `id` right now: its fixed keys, the key chosen for
// it in `shortcuts` (the live bindings, profile and all), and its global
// hotkey in `config`. Unbound ones are left out; nothing for a command
// only a menu or the bar reaches.
std::vector<platform::KeyCombo> KeysFor(CommandId id, const core::AppConfig& config,
                                        const core::ShortcutBindings& shortcuts);

}  // namespace sz::ui
