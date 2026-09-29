#include "ui/interaction/command.h"

#include "core/config/settings_catalog.h"
#include "core/session/settings.h"

namespace sz::ui {

std::optional<CommandId> CommandForShortcut(core::ShortcutAction action) {
    for (const CommandInfo& info : kCommands) {
        if (info.shortcut == action) {
            return info.id;
        }
    }
    return std::nullopt;
}

std::optional<CommandId> CommandForHotkey(core::HotkeySlot slot) {
    for (const CommandInfo& info : kCommands) {
        if (info.hotkey == slot) {
            return info.id;
        }
    }
    return std::nullopt;
}

CommandId CommandForBarButton(core::ChromeButton button) {
    switch (button) {
        case core::ChromeButton::Close:
            return CommandId::DeleteSelection;
        case core::ChromeButton::Maximize:
            return CommandId::ToggleFullscreen;
        case core::ChromeButton::Minimize:
            return CommandId::Minimize;
        case core::ChromeButton::More:
            return CommandId::Properties;
        case core::ChromeButton::Pin:
            return CommandId::Pin;
        case core::ChromeButton::Pen:
            return CommandId::PenButton;
        case core::ChromeButton::Eraser:
            return CommandId::EraserButton;
        case core::ChromeButton::Text:
            return CommandId::TextButton;
        case core::ChromeButton::Color:
            return CommandId::ColorButton;
    }
    return CommandId::DeleteSelection;  // unreachable: the switch names every button
}

std::optional<CommandId> MenuForBarButton(core::ChromeButton button) {
    switch (button) {
        case core::ChromeButton::Pen:
            return CommandId::PenMenu;
        case core::ChromeButton::Eraser:
            return CommandId::EraserMenu;
        case core::ChromeButton::Close:
        case core::ChromeButton::Maximize:
        case core::ChromeButton::Minimize:
        case core::ChromeButton::More:
        case core::ChromeButton::Pin:
        case core::ChromeButton::Text:
        case core::ChromeButton::Color:
            break;
    }
    return std::nullopt;
}

int ComboKeyForMouseButton(platform::MouseButton button) {
    switch (button) {
        case platform::MouseButton::Middle:
            return platform::KeyCombo::kMiddleButton;
        case platform::MouseButton::X1:
            return platform::KeyCombo::kX1Button;
        case platform::MouseButton::X2:
            return platform::KeyCombo::kX2Button;
        case platform::MouseButton::Left:
        case platform::MouseButton::Right:
            return 0;
    }
    return 0;
}

const platform::KeyCombo& HotkeyCombo(const core::AppConfig& config, core::HotkeySlot slot) {
    return core::ValueIn(core::HotkeySetting(slot), config);
}

std::vector<platform::KeyCombo> KeysFor(CommandId id, const core::AppConfig& config,
                                        const core::ShortcutBindings& shortcuts) {
    const CommandInfo& info = InfoFor(id);
    std::vector<platform::KeyCombo> keys;
    for (const platform::KeyCombo& key : info.keys) {
        if (key.key != 0) {
            keys.push_back(key);
        }
    }
    if (info.shortcut.has_value()) {
        if (const platform::KeyCombo& chosen = shortcuts[core::ShortcutActionIndex(*info.shortcut)]; chosen.key != 0) {
            keys.push_back(chosen);
        }
    }
    if (info.hotkey.has_value()) {
        if (const platform::KeyCombo& hotkey = HotkeyCombo(config, *info.hotkey); hotkey.key != 0) {
            keys.push_back(hotkey);
        }
    }
    return keys;
}

}  // namespace sz::ui
