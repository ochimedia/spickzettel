#include "ui/interaction/command.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/config/app_config.h"
#include "core/config/shortcut_action.h"
#include "ui/overlay_app_internal.h"

namespace sz::ui {
namespace {

std::string NameOf(CommandId id) { return std::string(InfoFor(id).name); }

// A key a person chooses reaches one command, and so does a hotkey: two
// would be a key that does two things, none a Settings row that binds a
// key to nothing.
TEST(CommandTest, EveryChosenKeyAndHotkeyRunsExactlyOneCommand) {
    for (const ShortcutAction action : kAllShortcutActions) {
        const auto count = std::count_if(kCommands.begin(), kCommands.end(),
                                         [&](const CommandInfo& info) { return info.shortcut == action; });
        EXPECT_EQ(count, 1) << ShortcutActionKey(action);
    }
    for (const HotkeySlot slot :
         {HotkeySlot::EditMode, HotkeySlot::ViewMode, HotkeySlot::QuickCapture, HotkeySlot::SilentCapture}) {
        const auto count = std::count_if(kCommands.begin(), kCommands.end(),
                                         [&](const CommandInfo& info) { return info.hotkey == slot; });
        EXPECT_EQ(count, 1) << static_cast<int>(slot);
    }
}

// What each Settings row binds is the key of the command it names.
TEST(CommandTest, EverySettingsRowBindsTheKeyOfItsCommand) {
    const std::pair<Tool, CommandId> tools[] = {
        {Tool::Draw, CommandId::DrawTool},
        {Tool::Erase, CommandId::EraseTool},
        {Tool::Text, CommandId::TextTool},
        {Tool::Select, CommandId::SelectTool},
        {Tool::NewScreenshot, CommandId::NewScreenshotTool},
        {Tool::NewDrawing, CommandId::NewDrawingTool},
    };
    for (const auto& [tool, command] : tools) {
        EXPECT_EQ(CommandForShortcut(overlay_detail::ShortcutForTool(tool)), command) << NameOf(command);
    }
    EXPECT_EQ(CommandForShortcut(overlay_detail::ShortcutForCreateAction(CreateAction::NewCanvas)),
              CommandId::NewCanvas);
    EXPECT_EQ(CommandForShortcut(overlay_detail::ShortcutForCreateAction(CreateAction::NewCanvasWithSelection)),
              CommandId::NewCanvasWithSelection);
    const std::pair<ClipboardAction, CommandId> clipboard[] = {
        {ClipboardAction::Copy, CommandId::Copy},
        {ClipboardAction::Cut, CommandId::Cut},
        {ClipboardAction::Paste, CommandId::Paste},
        {ClipboardAction::Duplicate, CommandId::Duplicate},
    };
    for (const auto& [action, command] : clipboard) {
        EXPECT_EQ(CommandForShortcut(overlay_detail::ShortcutForClipboardAction(action)), command) << NameOf(command);
    }
}

// A fixed key belongs to one command, and none of the keys that ship for
// the chosen ones is a fixed key: out of the box, every key does one
// thing.
TEST(CommandTest, NoKeyShipsOnTwoCommands) {
    std::vector<std::pair<platform::KeyCombo, CommandId>> seen;
    const AppConfig config = DefaultConfig();
    const ShortcutBindings shortcuts = DefaultShortcuts();
    for (const CommandInfo& info : kCommands) {
        for (const platform::KeyCombo& key : KeysFor(info.id, config, shortcuts)) {
            const auto it = std::find_if(seen.begin(), seen.end(), [&](const auto& s) { return s.first == key; });
            EXPECT_TRUE(it == seen.end()) << NameOf(info.id) << " shares a key with "
                                          << (it == seen.end() ? "" : NameOf(it->second));
            seen.emplace_back(key, info.id);
        }
    }
}

TEST(CommandTest, KeysForListsFixedChosenAndHotkeyKeys) {
    AppConfig config = DefaultConfig();
    ShortcutBindings shortcuts = DefaultShortcuts();
    const auto keys = [&](CommandId id) { return KeysFor(id, config, shortcuts); };

    ASSERT_EQ(keys(CommandId::Redo).size(), 2u);
    EXPECT_EQ(keys(CommandId::Redo)[0], (platform::KeyCombo{true, false, false, 'Y'}));
    EXPECT_EQ(keys(CommandId::Redo)[1], (platform::KeyCombo{true, false, true, 'Z'}));
    EXPECT_EQ(keys(CommandId::Copy),
              std::vector<platform::KeyCombo>{shortcuts[ShortcutActionIndex(ShortcutAction::Copy)]});
    EXPECT_EQ(keys(CommandId::ToggleEditMode), std::vector<platform::KeyCombo>{config.hotkeyEditMode});
    EXPECT_TRUE(keys(CommandId::ToggleFullscreen).empty()) << "a menu row's, which no key reaches";

    // Unbound is nothing, not a key of 0.
    shortcuts[ShortcutActionIndex(ShortcutAction::Copy)] = platform::KeyCombo{};
    config.hotkeyEditMode = platform::KeyCombo{};
    EXPECT_TRUE(keys(CommandId::Copy).empty());
    EXPECT_TRUE(keys(CommandId::ToggleEditMode).empty());
}

// Every selection bar button is a command, and no two are the same one -
// the bar layout setting stores buttons, and each has to do its own thing.
TEST(CommandTest, EveryBarButtonIsACommandOfItsOwn) {
    std::vector<CommandId> commands;
    for (const ChromeButton button : {ChromeButton::Close, ChromeButton::Maximize, ChromeButton::Minimize,
                                      ChromeButton::More, ChromeButton::Pin, ChromeButton::Pen, ChromeButton::Eraser,
                                      ChromeButton::Text, ChromeButton::Color}) {
        const CommandId command = CommandForBarButton(button);
        EXPECT_TRUE(std::find(commands.begin(), commands.end(), command) == commands.end()) << NameOf(command);
        commands.push_back(command);
    }
}

}  // namespace
}  // namespace sz::ui
