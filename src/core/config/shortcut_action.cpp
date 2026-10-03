#include "core/config/shortcut_action.h"

namespace sz::core {

std::string_view ShortcutActionKey(ShortcutAction action) {
    switch (action) {
        case ShortcutAction::Draw:
            return "pen";
        case ShortcutAction::Erase:
            return "eraser";
        case ShortcutAction::Text:
            return "text";
        case ShortcutAction::Select:
            return "select";
        case ShortcutAction::NewScreenshot:
            return "newScreenshot";
        case ShortcutAction::NewDrawing:
            return "newDrawing";
        case ShortcutAction::NewCanvas:
            return "newCanvas";
        case ShortcutAction::NewCanvasWithSelection:
            return "newCanvasWithSelection";
        case ShortcutAction::SelectAll:
            return "selectAll";
        case ShortcutAction::Copy:
            return "copy";
        case ShortcutAction::Cut:
            return "cut";
        case ShortcutAction::Paste:
            return "paste";
        case ShortcutAction::PasteInPlace:
            return "pasteInPlace";
        case ShortcutAction::Duplicate:
            return "duplicate";
        case ShortcutAction::CheatSheet:
            return "cheatSheet";
    }
    return "";
}

std::optional<ShortcutAction> ShortcutActionFromKey(std::string_view key) {
    for (const ShortcutAction action : kAllShortcutActions) {
        if (ShortcutActionKey(action) == key) {
            return action;
        }
    }
    return std::nullopt;
}

ShortcutBindings DefaultShortcuts() {
    ShortcutBindings bindings{};
    const auto bind = [&bindings](ShortcutAction action, char key) {
        bindings[ShortcutActionIndex(action)] = platform::KeyCombo{/*ctrl=*/false, /*alt=*/false,
                                                                   /*shift=*/false, /*key=*/key};
    };
    const auto bindWithCtrl = [&bindings](ShortcutAction action, char key) {
        bindings[ShortcutActionIndex(action)] = platform::KeyCombo{/*ctrl=*/true, /*alt=*/false,
                                                                   /*shift=*/false, /*key=*/key};
    };
    bind(ShortcutAction::NewScreenshot, 'S');
    bind(ShortcutAction::NewDrawing, 'D');
    bind(ShortcutAction::Erase, 'E');
    bind(ShortcutAction::Draw, 'P');
    bindWithCtrl(ShortcutAction::SelectAll, 'A');
    bindWithCtrl(ShortcutAction::Copy, 'C');
    bindWithCtrl(ShortcutAction::Cut, 'X');
    bindWithCtrl(ShortcutAction::Paste, 'V');
    bindWithCtrl(ShortcutAction::Duplicate, 'D');
    bindWithCtrl(ShortcutAction::CheatSheet, 'H');
    bindings[ShortcutActionIndex(ShortcutAction::PasteInPlace)] =
        platform::KeyCombo{/*ctrl=*/true, /*alt=*/false, /*shift=*/true, /*key=*/'V'};
    // Ctrl+Shift+N rather than Ctrl+N: the plain chord is "new" in every
    // browser and editor there is, and this one takes the selection with
    // it, which is the heavier of the two things to do by accident.
    bindings[ShortcutActionIndex(ShortcutAction::NewCanvasWithSelection)] =
        platform::KeyCombo{/*ctrl=*/true, /*alt=*/false, /*shift=*/true, /*key=*/'N'};
    return bindings;
}

}  // namespace sz::core
