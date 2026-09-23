#include "ui/overlay_app_internal.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/config/app_config.h"

namespace sz::ui {
namespace {

using overlay_detail::BuildCheatSheet;
using overlay_detail::CheatSheetRow;
using overlay_detail::CheatSheetSection;

// Every row, whichever group it is in.
std::vector<CheatSheetRow> AllRows(const std::vector<CheatSheetSection>& sections) {
    std::vector<CheatSheetRow> rows;
    for (const CheatSheetSection& section : sections) {
        rows.insert(rows.end(), section.rows.begin(), section.rows.end());
    }
    return rows;
}

// What the row for `what` says to press, or "" for no such row.
std::string KeysFor(const std::vector<CheatSheetSection>& sections, const char* what) {
    for (const CheatSheetRow& row : AllRows(sections)) {
        if (row.what == what) {
            return row.keys;
        }
    }
    return "";
}

TEST(CheatSheetTest, ShowsTheKeysAsShipped) {
    const AppConfig config = DefaultConfig();
    const auto sheet = BuildCheatSheet(config, config.toolShortcuts);
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetShowHide), "Ctrl+Alt+O");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetSelf), "Ctrl+H");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetPen), "P");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetCopy), "Ctrl+C");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetScreenshotArea), "Drag");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetDrawingArea), "Ctrl+Drag");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetText), "") << "Text ships unbound, so has no row";
}

// The sheet reads the bindings, not the defaults: a rebound key shows as
// rebound, an unbound one drops its row, and so does a kind of snippet no
// press on empty canvas makes.
TEST(CheatSheetTest, FollowsTheBindingsAsTheyAre) {
    AppConfig config = DefaultConfig();
    config.hotkeyEditMode = platform::KeyCombo{/*ctrl=*/false, /*alt=*/false, /*shift=*/false,
                                               /*key=*/platform::KeyCombo::kFunctionKeyBase + 9};
    config.screenshotTrigger = CreationTrigger::Alt;
    config.drawingTrigger = CreationTrigger::Off;
    ShortcutBindings shortcuts = config.toolShortcuts;
    shortcuts[ShortcutActionIndex(ShortcutAction::Draw)] = platform::KeyCombo{false, false, false, 'Q'};
    shortcuts[ShortcutActionIndex(ShortcutAction::Copy)] = platform::KeyCombo{};

    const auto sheet = BuildCheatSheet(config, shortcuts);
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetShowHide), "F9");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetPen), "Q");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetCopy), "");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetScreenshotArea), "Alt+Drag");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetScreenshotFull), "Alt+Double-click or hold");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetDrawingArea), "");
    EXPECT_EQ(KeysFor(sheet, strings::kCheatSheetDrawingFull), "");
}

// No group is left empty or unnamed, and nothing promises a key that is
// not there.
TEST(CheatSheetTest, EveryGroupHasATitleAndRows) {
    AppConfig config = DefaultConfig();
    ShortcutBindings none{};
    for (const ShortcutBindings& shortcuts : {config.toolShortcuts, none}) {
        const auto sheet = BuildCheatSheet(config, shortcuts);
        ASSERT_EQ(sheet.size(), 6u);
        for (const CheatSheetSection& section : sheet) {
            EXPECT_NE(std::string(section.title), "");
            EXPECT_FALSE(section.rows.empty()) << section.title;
        }
        for (const CheatSheetRow& row : AllRows(sheet)) {
            EXPECT_NE(row.keys, "(none)") << row.what;
            EXPECT_FALSE(row.what.empty()) << row.keys;
        }
    }
}

}  // namespace
}  // namespace sz::ui
