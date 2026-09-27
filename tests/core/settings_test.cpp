#include "core/session/settings.h"

#include <gtest/gtest.h>

#include "core/config/settings_catalog.h"

namespace sz::core {
namespace {

// A config with one profile that matches "game.exe" and overrides
// dontStealFocus to the opposite of the default.
AppConfig ConfigWithOneProfile() {
    AppConfig config = DefaultConfig();
    Profile profile;
    profile.name = "Game";
    profile.match.executables.push_back("game.exe");
    profile.overrides.dontStealFocus = !config.profileable.dontStealFocus;
    config.profiles.push_back(profile);
    return config;
}

platform::ForegroundApp Game() {
    platform::ForegroundApp app;
    app.executable = "game.exe";
    return app;
}

TEST(SettingsTest, LiveIsTheDefaultsUntilAProfileMatches) {
    Settings settings(ConfigWithOneProfile());
    EXPECT_FALSE(settings.ActiveProfile().has_value());
    EXPECT_EQ(settings.Live().dontStealFocus, DefaultConfig().profileable.dontStealFocus);

    settings.SetUnderlyingApplication(Game());
    ASSERT_EQ(settings.ActiveProfile(), std::optional<size_t>(0));
    EXPECT_NE(settings.Live().dontStealFocus, DefaultConfig().profileable.dontStealFocus);
    EXPECT_EQ(settings.Stored().profileable.dontStealFocus, DefaultConfig().profileable.dontStealFocus)
        << "matching a profile resolves; it writes nothing";
}

TEST(SettingsTest, AnEditGoesWhereItIsAimed) {
    Settings settings(ConfigWithOneProfile());
    settings.SetUnderlyingApplication(Game());
    const bool defaultFreeze = DefaultConfig().profileable.freezeScreen;

    EXPECT_TRUE(settings.Set(setting::kFreezeScreen, !defaultFreeze, 0u));
    EXPECT_TRUE(settings.IsOverridden(setting::kFreezeScreen, 0u));
    EXPECT_EQ(settings.Stored().profileable.freezeScreen, defaultFreeze) << "the defaults are untouched";
    EXPECT_EQ(settings.Live().freezeScreen, !defaultFreeze) << "and what runs is the profile's";
    EXPECT_EQ(settings.Get(setting::kFreezeScreen, 0u), !defaultFreeze);
    EXPECT_EQ(settings.Get(setting::kFreezeScreen, std::nullopt), defaultFreeze);

    settings.ClearOverride(setting::kFreezeScreen, 0u);
    EXPECT_FALSE(settings.IsOverridden(setting::kFreezeScreen, 0u));
    EXPECT_EQ(settings.Live().freezeScreen, defaultFreeze);

    settings.Set(setting::kFreezeScreen, !defaultFreeze, std::nullopt);
    EXPECT_EQ(settings.Stored().profileable.freezeScreen, !defaultFreeze) << "nullopt is the defaults";
    EXPECT_FALSE(settings.IsOverridden(setting::kFreezeScreen, std::nullopt)) << "which override nothing";
}

// An edit is held to its row's rule, as the file is: pulled into the band,
// or rejected and nothing changed - and a rejected edit commits nothing.
TEST(SettingsTest, AnEditIsHeldToItsRowsRule) {
    Settings settings(ConfigWithOneProfile());
    int commits = 0;
    settings.SetChangedCallback([&commits] { ++commits; });

    EXPECT_TRUE(settings.Set(setting::kPurgeDeletedAfterDays, 99999));
    EXPECT_EQ(settings.Stored().purgeDeletedAfterDays, kPurgeDeletedAfterDaysMax);
    EXPECT_TRUE(settings.Set(setting::kCounterThreshold, 1, 0u));
    EXPECT_EQ(settings.Stored().profiles[0].overrides.counterThreshold,
              platform::EditModeInputOptions::kCounterThresholdMin);
    EXPECT_EQ(commits, 2);

    EXPECT_FALSE(settings.Set(setting::kEditModeBorderWidth, 0.0f)) << "a zero width is no width";
    EXPECT_EQ(settings.Stored().editModeBorderWidthPx, DefaultConfig().editModeBorderWidthPx);
    EXPECT_EQ(commits, 2);
}

TEST(SettingsTest, EveryEditCommitsAndACommitResolvesAgain) {
    Settings settings(ConfigWithOneProfile());
    int commits = 0;
    settings.SetChangedCallback([&commits] { ++commits; });

    settings.SetUnderlyingApplication(Game());
    EXPECT_EQ(commits, 0) << "what the overlay is up over is not an edit";

    settings.Set(setting::kShowItemBorders, !settings.Stored().showItemBorders);
    EXPECT_EQ(commits, 1);

    // Replacing the profiles can change which one matches.
    std::vector<Profile> none;
    settings.SetProfiles(none);
    EXPECT_EQ(commits, 2);
    EXPECT_FALSE(settings.ActiveProfile().has_value());
    EXPECT_EQ(settings.Live().dontStealFocus, settings.Stored().profileable.dontStealFocus);
}

// A value being dragged is stored as it moves, for everything drawn to show
// it, and committed once, when the drag is finished.
TEST(SettingsTest, APreviewIsShownAtOnceAndCommittedWhenFinished) {
    Settings settings(DefaultConfig());
    int commits = 0;
    settings.SetChangedCallback([&commits] { ++commits; });

    settings.Preview(setting::kAccentColor, 0x112233FFu);
    settings.Preview(setting::kAccentColor, 0x445566FFu);
    EXPECT_EQ(settings.Get(setting::kAccentColor), 0x445566FFu);
    EXPECT_TRUE(settings.Previewing());
    EXPECT_EQ(commits, 0);

    settings.CommitPreviews();
    EXPECT_EQ(commits, 1);
    EXPECT_FALSE(settings.Previewing());
    settings.CommitPreviews();
    EXPECT_EQ(commits, 1) << "nothing left to finish";

    // Any other edit's commit takes a preview with it.
    settings.Preview(setting::kEditModeBorderOpacity, 0.5f);
    settings.Set(setting::kShowCanvasBar, false);
    EXPECT_EQ(commits, 2);
    EXPECT_FALSE(settings.Previewing());
}

// Called off - Escape during the drag - every row previewed is back to what
// it was before the first preview, and nothing is committed.
TEST(SettingsTest, APreviewCalledOffIsPutBack) {
    Settings settings(DefaultConfig());
    int commits = 0;
    settings.SetChangedCallback([&commits] { ++commits; });
    const uint32_t accent = settings.Get(setting::kAccentColor);
    const float opacity = settings.Get(setting::kEditModeBorderOpacity);

    settings.Preview(setting::kAccentColor, 0x112233FFu);
    settings.Preview(setting::kAccentColor, 0x445566FFu);
    settings.Preview(setting::kEditModeBorderOpacity, 0.5f);
    settings.CancelPreviews();
    EXPECT_EQ(settings.Get(setting::kAccentColor), accent);
    EXPECT_EQ(settings.Get(setting::kEditModeBorderOpacity), opacity);
    EXPECT_FALSE(settings.Previewing());
    settings.CommitPreviews();
    EXPECT_EQ(commits, 0);

    // And the next drag's starts from there.
    settings.Preview(setting::kAccentColor, 0x778899FFu);
    settings.CancelPreviews();
    EXPECT_EQ(settings.Get(setting::kAccentColor), accent);
}

// One press cannot make both kinds: choosing the press the other has swaps
// the two, and both may be off.
TEST(SettingsTest, ChoosingTheOtherTriggersPressSwapsThem) {
    Settings settings(DefaultConfig());
    ASSERT_EQ(settings.Stored().screenshotTrigger, CreationTrigger::Plain);
    ASSERT_EQ(settings.Stored().drawingTrigger, CreationTrigger::Ctrl);

    settings.Set(setting::kScreenshotTrigger, CreationTrigger::Ctrl);
    EXPECT_EQ(settings.Stored().screenshotTrigger, CreationTrigger::Ctrl);
    EXPECT_EQ(settings.Stored().drawingTrigger, CreationTrigger::Plain);

    settings.Set(setting::kDrawingTrigger, CreationTrigger::Off);
    settings.Set(setting::kScreenshotTrigger, CreationTrigger::Off);
    EXPECT_EQ(settings.Stored().screenshotTrigger, CreationTrigger::Off);
    EXPECT_EQ(settings.Stored().drawingTrigger, CreationTrigger::Off);
}

// One combination summons one thing: the hotkey that had it is unbound.
// Two unbound hotkeys share nothing.
TEST(SettingsTest, AHotkeyGivenAnothersCombinationLeavesThatOneUnbound) {
    Settings settings(DefaultConfig());
    const platform::KeyCombo view = settings.Stored().hotkeyViewMode;

    settings.Set(setting::kHotkeyEditMode, view);
    EXPECT_EQ(settings.Stored().hotkeyEditMode, view);
    EXPECT_FALSE(settings.Stored().hotkeyViewMode.IsValid());

    settings.Set(setting::kHotkeyQuickCapture, platform::KeyCombo{});
    EXPECT_FALSE(settings.Stored().hotkeyQuickCapture.IsValid());
    EXPECT_EQ(settings.Stored().hotkeyEditMode, view) << "unbound collides with nothing";
}

// A rename to a name another profile has is refused, as is an empty one,
// and the profile keeps its own; a free name is taken, and committed.
TEST(SettingsTest, AProfileIsRenamedOnlyToAFreeName) {
    AppConfig config = ConfigWithOneProfile();
    Profile other;
    other.name = "Other";
    config.profiles.push_back(other);
    Settings settings(config);
    int commits = 0;
    settings.SetChangedCallback([&commits] { ++commits; });

    EXPECT_FALSE(settings.RenameProfile(0, "Other"));
    EXPECT_FALSE(settings.RenameProfile(0, ""));
    EXPECT_EQ(settings.Profiles()[0].name, "Game");
    EXPECT_EQ(commits, 0);

    EXPECT_TRUE(settings.RenameProfile(0, "Elden Ring"));
    EXPECT_EQ(settings.Profiles()[0].name, "Elden Ring");
    EXPECT_EQ(commits, 1);
    EXPECT_TRUE(settings.RenameProfile(0, "Elden Ring")) << "its own name is not another's";
}

// A list handed over whole is held to the same invariant a file is.
TEST(SettingsTest, ProfilesSetWholeHaveNamesThatAreNotEmptyAndUnique) {
    Settings settings(DefaultConfig());
    std::vector<Profile> profiles(3);
    profiles[0].name = "Game";
    profiles[1].name = "Game";
    settings.SetProfiles(profiles);
    EXPECT_EQ(settings.Profiles()[0].name, "Game");
    EXPECT_EQ(settings.Profiles()[1].name, "Game 2");
    EXPECT_EQ(settings.Profiles()[2].name, "Profile");
}

TEST(SettingsTest, AShortcutOverrideIsPerProfile) {
    Settings settings(ConfigWithOneProfile());
    settings.SetUnderlyingApplication(Game());
    const ShortcutAction action = ShortcutAction::Draw;
    const platform::KeyCombo combo{/*ctrl=*/false, /*alt=*/false, /*shift=*/true, /*key=*/'Q'};

    settings.SetShortcut(action, combo, 0u);
    EXPECT_TRUE(settings.IsShortcutOverridden(action, 0u));
    EXPECT_EQ(settings.Live().shortcuts[ShortcutActionIndex(action)], combo);
    EXPECT_NE(settings.ResolvedFor(std::nullopt).shortcuts[ShortcutActionIndex(action)], combo);

    settings.ClearShortcutOverride(action, 0u);
    EXPECT_FALSE(settings.IsShortcutOverridden(action, 0u));
}

// A key another action has in the same target moves over, and that action
// is left unbound there - in a profile, as an override of its own, leaving
// the defaults as they were.
TEST(SettingsTest, AShortcutTakesItsKeyFromTheActionThatHadIt) {
    Settings settings(ConfigWithOneProfile());
    const platform::KeyCombo newScreenshotKey =
        settings.Stored().profileable.shortcuts[ShortcutActionIndex(ShortcutAction::NewScreenshot)];
    ASSERT_TRUE(newScreenshotKey.IsValid());

    settings.SetShortcut(ShortcutAction::Text, newScreenshotKey, 0u);
    const ProfileableSettings game = settings.ResolvedFor(0u);
    EXPECT_EQ(game.shortcuts[ShortcutActionIndex(ShortcutAction::Text)], newScreenshotKey);
    EXPECT_FALSE(game.shortcuts[ShortcutActionIndex(ShortcutAction::NewScreenshot)].IsValid());
    EXPECT_TRUE(settings.IsShortcutOverridden(ShortcutAction::NewScreenshot, 0u));
    EXPECT_EQ(settings.Stored().profileable.shortcuts[ShortcutActionIndex(ShortcutAction::NewScreenshot)], newScreenshotKey)
        << "the defaults keep it";

    settings.SetShortcut(ShortcutAction::Text, newScreenshotKey, std::nullopt);
    EXPECT_FALSE(settings.Stored().profileable.shortcuts[ShortcutActionIndex(ShortcutAction::NewScreenshot)].IsValid());
}

}  // namespace
}  // namespace sz::core
