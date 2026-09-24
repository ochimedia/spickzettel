#include "core/session/settings.h"

#include <gtest/gtest.h>

namespace sz::core {
namespace {

// A config with one profile that matches "game.exe" and overrides
// dontStealFocus to the opposite of the default.
AppConfig ConfigWithOneProfile() {
    AppConfig config = DefaultConfig();
    Profile profile;
    profile.name = "Game";
    profile.match.executables.push_back("game.exe");
    profile.overrides.dontStealFocus = !config.editModeNoActivate;
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
    EXPECT_EQ(settings.Live().dontStealFocus, DefaultConfig().editModeNoActivate);

    settings.SetUnderlyingApplication(Game());
    ASSERT_EQ(settings.ActiveProfile(), std::optional<size_t>(0));
    EXPECT_NE(settings.Live().dontStealFocus, DefaultConfig().editModeNoActivate);
    EXPECT_EQ(settings.Stored().editModeNoActivate, DefaultConfig().editModeNoActivate)
        << "matching a profile resolves; it writes nothing";
}

TEST(SettingsTest, AnEditGoesWhereItIsAimed) {
    Settings settings(ConfigWithOneProfile());
    settings.SetUnderlyingApplication(Game());
    const ProfileableField freeze{&ProfileableSettings::freezeScreen, &ProfileOverrides::freezeScreen};
    const bool defaultFreeze = DefaultConfig().freezeScreenInEditMode;

    settings.SetProfileable(0u, freeze, !defaultFreeze);
    EXPECT_TRUE(settings.IsOverridden(0u, freeze));
    EXPECT_EQ(settings.Stored().freezeScreenInEditMode, defaultFreeze) << "the defaults are untouched";
    EXPECT_EQ(settings.Live().freezeScreen, !defaultFreeze) << "and what runs is the profile's";

    settings.ClearOverride(0u, freeze);
    EXPECT_FALSE(settings.IsOverridden(0u, freeze));
    EXPECT_EQ(settings.Live().freezeScreen, defaultFreeze);

    settings.SetProfileable(std::nullopt, freeze, !defaultFreeze);
    EXPECT_EQ(settings.Stored().freezeScreenInEditMode, !defaultFreeze) << "nullopt is the defaults";
    EXPECT_FALSE(settings.IsOverridden(std::nullopt, freeze)) << "which override nothing";
}

TEST(SettingsTest, ANumberEditGoesWhereItIsAimed) {
    Settings settings(ConfigWithOneProfile());
    settings.SetUnderlyingApplication(Game());
    const ProfileableIntField threshold{&ProfileableSettings::counterThreshold, &ProfileOverrides::counterThreshold};
    const int defaultThreshold = DefaultConfig().editModeInput.counterThreshold;

    settings.SetProfileable(0u, threshold, 40);
    EXPECT_TRUE(settings.IsOverridden(0u, threshold));
    EXPECT_EQ(settings.Stored().editModeInput.counterThreshold, defaultThreshold);
    EXPECT_EQ(settings.Live().counterThreshold, 40);

    settings.ClearOverride(0u, threshold);
    EXPECT_FALSE(settings.IsOverridden(0u, threshold));
    EXPECT_EQ(settings.Live().counterThreshold, defaultThreshold);

    settings.SetProfileable(std::nullopt, threshold, 90);
    EXPECT_EQ(settings.Stored().editModeInput.counterThreshold, 90);
    EXPECT_EQ(settings.Live().counterThreshold, 90) << "a profile that says nothing inherits it";
}

TEST(SettingsTest, EveryEditCommitsAndACommitResolvesAgain) {
    Settings settings(ConfigWithOneProfile());
    int commits = 0;
    settings.SetChangedCallback([&commits] { ++commits; });

    settings.SetUnderlyingApplication(Game());
    EXPECT_EQ(commits, 0) << "what the overlay is up over is not an edit";

    // A plain field, edited in place and then committed.
    settings.Mutable().showItemBorders = !settings.Stored().showItemBorders;
    settings.Commit();
    EXPECT_EQ(commits, 1);

    // Replacing the profiles can change which one matches.
    std::vector<Profile> none;
    settings.SetProfiles(none);
    EXPECT_EQ(commits, 2);
    EXPECT_FALSE(settings.ActiveProfile().has_value());
    EXPECT_EQ(settings.Live().dontStealFocus, settings.Stored().editModeNoActivate);
}

TEST(SettingsTest, AShortcutOverrideIsPerProfile) {
    Settings settings(ConfigWithOneProfile());
    settings.SetUnderlyingApplication(Game());
    const ShortcutAction action = ShortcutAction::Draw;
    const platform::KeyCombo combo{/*ctrl=*/false, /*alt=*/false, /*shift=*/true, /*key=*/'Q'};

    settings.SetShortcut(0u, action, combo);
    EXPECT_TRUE(settings.IsShortcutOverridden(0u, action));
    EXPECT_EQ(settings.Live().shortcuts[ShortcutActionIndex(action)], combo);
    EXPECT_NE(settings.ResolvedFor(std::nullopt).shortcuts[ShortcutActionIndex(action)], combo);

    settings.ClearShortcutOverride(0u, action);
    EXPECT_FALSE(settings.IsShortcutOverridden(0u, action));
}

}  // namespace
}  // namespace sz::core
