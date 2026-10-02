#include "core/config/app_config.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "support/temp_dir.h"

// Settings files as written, kept beside this test (config_files/) - see
// docs/SETTINGS.md, section 8. Two kinds:
//
// - What a released build wrote: v0.1.0-*.json. Never edited. Each has to
//   read, through whatever migrations there are by then, as the config it
//   was written from - so a key renamed without a migration fails here
//   rather than resetting a setting for everyone who upgrades.
// - What this build writes: current-*.json. The writer has to produce them
//   byte for byte, so a change to the file shows as a diff to review. A
//   change that is meant - a new setting - replaces them with the text this
//   test prints the path of.
namespace sz::core {
namespace {

std::filesystem::path FilesDir() { return std::filesystem::path(SPICKZETTEL_CONFIG_FILES); }

std::string ReadFixture(const char* name) {
    std::ifstream in(FilesDir() / name, std::ios::binary);
    // Read as empty, a missing file would pass every test that expects the
    // defaults.
    EXPECT_TRUE(in.is_open()) << "no " << (FilesDir() / name).string();
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Compares what this build writes with a current-*.json, and on a mismatch
// leaves the text written beside the test's temp files to diff or copy.
void ExpectWrittenAs(const char* name, const AppConfig& config) {
    const std::string written = SerializeConfig(config);
    const std::string fixture = ReadFixture(name);
    if (written == fixture) {
        return;
    }
    const std::filesystem::path actual = sz::test::TempDir() / name;
    std::ofstream(actual, std::ios::binary) << written;
    ADD_FAILURE() << "the settings file written differs from " << (FilesDir() / name).string()
                  << "; what was written is in " << actual.string();
}

// Every setting at a value other than its default, and every way a value
// can be spelled: an unbound hotkey and shortcut, a function key, a mouse
// button, a reordered bar with a button switched off, colors with and
// without alpha, a profile stating every override and one stating none.
// Kept complete by hand until the catalog can check it row by row.
AppConfig Everything() {
    AppConfig c = DefaultConfig();
    c.hotkeyEditMode = platform::KeyCombo{true, false, true, 'D'};
    c.hotkeyViewMode = platform::KeyCombo{false, false, false, platform::KeyCombo::kFunctionKeyBase + 9};
    c.hotkeyQuickCapture = platform::KeyCombo{true, true, true, 'S'};
    c.hotkeySilentCapture = platform::KeyCombo{};
    c.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Draw)] = platform::KeyCombo{false, false, true, 'Q'};
    c.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Erase)] = platform::KeyCombo{};
    c.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Text)] =
        platform::KeyCombo{true, false, false, platform::KeyCombo::kMiddleButton};
    c.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::CheatSheet)] =
        platform::KeyCombo{false, false, false, platform::KeyCombo::kFunctionKeyBase + 1};

    Profile game;
    game.name = "Game";
    game.match.executables = {"game.exe", "launcher.exe"};
    game.match.titleContains = {"Game Window"};
    game.overrides.dontStealFocus = false;
    game.overrides.takeFocusOverElevated = false;
    game.overrides.softwarePointer = false;
    game.overrides.rawMouseInput = false;
    game.overrides.dontForwardKeystrokes = false;
    game.overrides.counterRawMouseInput = true;
    game.overrides.freezeScreen = true;
    game.overrides.counterThreshold = 250;
    game.overrides.shortcuts[ShortcutActionIndex(ShortcutAction::Draw)] = platform::KeyCombo{false, false, false, 'W'};
    game.overrides.shortcuts[ShortcutActionIndex(ShortcutAction::Erase)] = platform::KeyCombo{};
    Profile notes;
    notes.name = "Notes";
    notes.match.titleContains = {"Notepad"};
    c.profiles = {game, notes};

    c.strokeColorRGBA = 0x00FF00FFu;
    c.strokeWidth = 6.5f;
    c.showDebugOverlay = true;
    c.showInputOptionsHud = true;
    c.showFrameGraph = true;
    c.profileable.dontStealFocus = false;
    c.profileable.takeFocusOverElevated = false;
    c.profileable.softwarePointer = false;
    c.profileable.rawMouseInput = false;
    c.profileable.dontForwardKeystrokes = false;
    c.profileable.counterRawMouseInput = true;
    c.profileable.counterThreshold = 40;
    c.showItemBorders = false;
    c.showToastsWhileHidden = false;
    c.accentColorRGBA = 0x8040C0FFu;
    c.uiScalePercent = 150;
    c.itemBorderColorFrontRGBA = 0x11223344u;
    c.itemBorderColorOtherRGBA = 0x55667788u;
    c.itemBorderColorPinnedRGBA = 0x99AABBCCu;
    c.itemBorderSelectedFollowsAccent = false;
    c.itemBorderColorSelectedRGBA = 0xDDEEFF80u;
    c.imageFilter = platform::ImageFilter::Lanczos;
    c.raiseSelectedSnippet = false;
    c.screenshotTrigger = CreationTrigger::Alt;
    c.drawingTrigger = CreationTrigger::Off;
    std::swap(c.snippetBar[0], c.snippetBar[1]);
    c.snippetBar[2].shown = false;
    c.drawingBar[3].shown = false;
    c.overviewShowsStrokes = false;
    c.overviewShowsBitmaps = false;
    c.showCanvasBar = false;
    c.showEditModeBorder = false;
    c.editModeBorderColorRGBA = 0x5AA9FF66u;
    c.editModeBorderWidthPx = 16.0f;
    c.editModeBorderOnlyWhenEmpty = true;
    c.profileable.freezeScreen = true;
    c.purgeDeleted = false;
    c.purgeDeletedAfterDays = 21;
    c.confirmDelete = false;
    c.confirmDeleteForGood = false;
    c.screenshotDefaults = SnippetDefaults{false, 0.5f, 0.75f};
    c.drawingDefaults = SnippetDefaults{false, 0.8f, 0.25f};
    c.drawingBackgroundColorRGBA = 0xFFF0C0FFu;
    c.noteTextSizePx = 28.0f;
    c.noteTextColorRGBA = 0x000000C0u;
    c.overlayDisplayId = "\\\\?\\DISPLAY#TEST#1";
    c.overlayDisplayName = "Test Display";
    c.tutorialProgress = {{"basics", "finished"}, {"drawing", "started"}};
    return c;
}

TEST(ConfigFilesTest, TheDefaultsAreWrittenAsTheyAlwaysHaveBeen) {
    ExpectWrittenAs("current-defaults.json", DefaultConfig());
    EXPECT_EQ(ParseConfig(ReadFixture("current-defaults.json")), DefaultConfig());
}

TEST(ConfigFilesTest, EverySettingChangedIsWrittenAsItAlwaysHasBeen) {
    ExpectWrittenAs("current-everything.json", Everything());
    EXPECT_EQ(ParseConfig(ReadFixture("current-everything.json")), Everything());
    // Nothing to migrate or repair in what this build writes, or every
    // start would write it back.
    EXPECT_FALSE(TryParseConfig(ReadFixture("current-everything.json"))->changed);
}

TEST(ConfigFilesTest, TheFileTheFirstReleaseWroteReadsAsTheDefaults) {
    // It says drawing.paintPixels and diagnostics.showLibraryTreeHud, which
    // are gone, and nothing about the settings added since.
    EXPECT_EQ(ParseConfig(ReadFixture("v0.1.0-defaults.json")), DefaultConfig());
}

TEST(ConfigFilesTest, TheFirstReleasesSettingsAreReadAsItWroteThem) {
    // The same values as Everything(), as far as v0.1.0 had the settings.
    AppConfig expected = Everything();
    const AppConfig defaults = DefaultConfig();
    // No mouse button could be a shortcut yet.
    expected.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Text)] =
        defaults.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Text)];
    // Added since.
    expected.uiScalePercent = defaults.uiScalePercent;
    expected.screenshotDefaults = defaults.screenshotDefaults;
    expected.drawingDefaults = defaults.drawingDefaults;
    expected.drawingBackgroundColorRGBA = defaults.drawingBackgroundColorRGBA;
    expected.noteTextSizePx = defaults.noteTextSizePx;
    expected.noteTextColorRGBA = defaults.noteTextColorRGBA;
    expected.tutorialProgress = defaults.tutorialProgress;
    expected.showFrameGraph = defaults.showFrameGraph;
    expected.itemBorderSelectedFollowsAccent = defaults.itemBorderSelectedFollowsAccent;
    expected.itemBorderColorSelectedRGBA = defaults.itemBorderColorSelectedRGBA;
    EXPECT_EQ(ParseConfig(ReadFixture("v0.1.0-everything.json")), expected);
}

}  // namespace
}  // namespace sz::core
