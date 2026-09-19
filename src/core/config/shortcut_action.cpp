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
        case ShortcutAction::Copy:
            return "copy";
        case ShortcutAction::Cut:
            return "cut";
        case ShortcutAction::Paste:
            return "paste";
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
    bindWithCtrl(ShortcutAction::Copy, 'C');
    bindWithCtrl(ShortcutAction::Cut, 'X');
    bindWithCtrl(ShortcutAction::Paste, 'V');
    return bindings;
}

}  // namespace sz::core
