#include "core/config/profile.h"

#include <gtest/gtest.h>

#include <string>

#include "core/config/app_config.h"

namespace sz::core {
namespace {

platform::ForegroundApp App(std::string executable, std::string title = {}) {
    return platform::ForegroundApp{std::move(executable), std::move(title)};
}

Profile GameProfile(std::string name, std::string executable) {
    Profile profile;
    profile.name = std::move(name);
    profile.match.executables.push_back(std::move(executable));
    return profile;
}

ProfileableSettings Defaults() { return DefaultConfig().profileable; }

TEST(ProfileTest, MatchesOnExecutableNameWhateverItsCase) {
    Profile profile = GameProfile("Elden Ring", "EldenRing.exe");
    EXPECT_TRUE(profile.match.Matches(App("eldenring.exe")));
    EXPECT_FALSE(profile.match.Matches(App("notepad.exe")));
}

TEST(ProfileTest, MatchesAnyOfSeveralExecutables) {
    // A launcher and the game it starts, which is how a lot of them arrive.
    Profile profile = GameProfile("Elden Ring", "eldenring.exe");
    profile.match.executables.push_back("start_protected_game.exe");
    EXPECT_TRUE(profile.match.Matches(App("eldenring.exe")));
    EXPECT_TRUE(profile.match.Matches(App("start_protected_game.exe")));
}

TEST(ProfileTest, FallsBackToTheWindowTitleWhenTheExecutableIsUnreadable) {
    // Exactly the elevated-process case: no executable name at all, only a
    // title. Substring and case-insensitive, because a title carries
    // version numbers and decoration nobody wants to type.
    Profile profile;
    profile.name = "Elden Ring";
    profile.match.titleContains.push_back("ELDEN RING");
    EXPECT_TRUE(profile.match.Matches(App("", "ELDEN RING - v1.10")));
    EXPECT_TRUE(profile.match.Matches(App("", "elden ring")));
    EXPECT_FALSE(profile.match.Matches(App("", "Notepad")));
}

TEST(ProfileTest, AnUnknownApplicationMatchesNothing) {
    const std::vector<Profile> profiles = {GameProfile("Elden Ring", "eldenring.exe")};
    EXPECT_FALSE(FindMatchingProfile(profiles, App("", "")).has_value());
}

TEST(ProfileTest, AProfileWithNoMatchRulesIsNeverActive) {
    Profile unmatched;
    unmatched.name = "Nothing to match on";
    unmatched.overrides.counterRawMouseInput = false;
    ASSERT_TRUE(unmatched.match.Empty());

    const std::vector<Profile> profiles = {unmatched};
    EXPECT_FALSE(FindMatchingProfile(profiles, App("eldenring.exe")).has_value());
    EXPECT_EQ(ResolveForApplication(Defaults(), profiles, App("eldenring.exe")), Defaults());
}

TEST(ProfileTest, FirstMatchWins) {
    std::vector<Profile> profiles = {GameProfile("First", "game.exe"), GameProfile("Second", "game.exe")};
    profiles[0].overrides.freezeScreen = false;
    profiles[1].overrides.freezeScreen = true;

    const auto index = FindMatchingProfile(profiles, App("game.exe"));
    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(*index, 0u);
}

TEST(ProfileTest, OnlyTheSettingsAProfileMentionsAreChanged) {
    ProfileableSettings base = Defaults();
    base.rawMouseInput = true;
    base.freezeScreen = true;

    std::vector<Profile> profiles = {GameProfile("Game", "game.exe")};
    profiles[0].overrides.freezeScreen = false;

    const ProfileableSettings resolved = ResolveForApplication(base, profiles, App("game.exe"));
    EXPECT_FALSE(resolved.freezeScreen);
    // Untouched, because the profile said nothing about it.
    EXPECT_TRUE(resolved.rawMouseInput);
    EXPECT_EQ(resolved.shortcuts, base.shortcuts);
}

TEST(ProfileTest, AProfileCanBindAndUnbindShortcuts) {
    std::vector<Profile> profiles = {GameProfile("Game", "game.exe")};
    profiles[0].overrides.shortcuts[ShortcutActionIndex(ShortcutAction::Text)] =
        platform::KeyCombo{false, false, false, 'T'};
    // Explicitly unbound, which is a different thing from saying nothing.
    profiles[0].overrides.shortcuts[ShortcutActionIndex(ShortcutAction::Draw)] = platform::KeyCombo{};

    const ProfileableSettings resolved = ResolveForApplication(Defaults(), profiles, App("game.exe"));
    EXPECT_EQ(resolved.shortcuts[ShortcutActionIndex(ShortcutAction::Text)].key, 'T');
    EXPECT_EQ(resolved.shortcuts[ShortcutActionIndex(ShortcutAction::Draw)].key, 0);
    // Said nothing about this one.
    EXPECT_EQ(resolved.shortcuts[ShortcutActionIndex(ShortcutAction::Erase)].key, 'E');
}

// A key a profile binds is the profile's, even when the defaults have
// since given the same key to another action: that action goes without it
// while the profile runs, rather than both waiting on one key.
TEST(ProfileTest, AProfilesOwnKeyWinsOverTheSameKeyInherited) {
    ProfileableSettings base = Defaults();
    const platform::KeyCombo key{false, false, false, 'Q'};
    base.shortcuts[ShortcutActionIndex(ShortcutAction::Erase)] = key;
    std::vector<Profile> profiles = {GameProfile("Game", "game.exe")};
    profiles[0].overrides.shortcuts[ShortcutActionIndex(ShortcutAction::Text)] = key;

    const ProfileableSettings resolved = ResolveForApplication(base, profiles, App("game.exe"));
    EXPECT_EQ(resolved.shortcuts[ShortcutActionIndex(ShortcutAction::Text)], key);
    EXPECT_EQ(resolved.shortcuts[ShortcutActionIndex(ShortcutAction::Erase)].key, 0);
    EXPECT_EQ(ResolveForApplication(base, profiles, App("other.exe")).shortcuts, base.shortcuts)
        << "the defaults as they are, where the profile does not run";
}

// Two levels and no more: a profile states some settings, everything else
// is the defaults, and one profile's overrides never reach another.
TEST(ProfileTest, AProfileAppliesItsOwnOverridesAndNothingElses) {
    Profile other;
    other.name = "Somebody else";
    other.overrides.counterRawMouseInput = false;

    Profile game = GameProfile("Elden Ring", "eldenring.exe");
    game.overrides.rawMouseInput = false;

    const std::vector<Profile> profiles = {other, game};
    const ProfileableSettings resolved = ResolveForApplication(Defaults(), profiles, App("eldenring.exe"));
    EXPECT_FALSE(resolved.rawMouseInput);                                     // its own
    EXPECT_EQ(resolved.counterRawMouseInput, Defaults().counterRawMouseInput);  // not the other one's
    EXPECT_EQ(resolved.freezeScreen, Defaults().freezeScreen);                 // said nothing
}

// An override that agrees with what it inherits is still an override: "this
// game, this way" has to survive a later change to the defaults, and that
// is the whole difference between stating a value and leaving it alone.
TEST(ProfileTest, AnOverrideThatMatchesTheDefaultStillPins) {
    ProfileableSettings base = Defaults();
    base.freezeScreen = true;

    std::vector<Profile> profiles = {GameProfile("Game", "game.exe")};
    profiles[0].overrides.freezeScreen = true;  // deliberately the same
    EXPECT_TRUE(ResolveForApplication(base, profiles, App("game.exe")).freezeScreen);

    base.freezeScreen = false;  // the default changes later
    EXPECT_TRUE(ResolveForApplication(base, profiles, App("game.exe")).freezeScreen);
    // ...while a profile that said nothing follows it.
    profiles[0].overrides.freezeScreen.reset();
    EXPECT_FALSE(ResolveForApplication(base, profiles, App("game.exe")).freezeScreen);
}

// Counted per group, because the two are edited in different sections: a
// profile that only rebinds keys must not claim to have set something on
// the Behavior section, where nothing would be marked.
TEST(ProfileTest, OverriddenCountIsPerGroup) {
    Profile profile = GameProfile("Game", "game.exe");
    EXPECT_EQ(profile.overrides.OverriddenCount(ProfileGroup::Behavior), 0u);
    EXPECT_EQ(profile.overrides.OverriddenCount(ProfileGroup::Shortcuts), 0u);

    profile.overrides.shortcuts[ShortcutActionIndex(ShortcutAction::Draw)] = platform::KeyCombo{};
    EXPECT_EQ(profile.overrides.OverriddenCount(ProfileGroup::Behavior), 0u);
    EXPECT_EQ(profile.overrides.OverriddenCount(ProfileGroup::Shortcuts), 1u);

    profile.overrides.freezeScreen = false;
    profile.overrides.dontStealFocus = true;
    EXPECT_EQ(profile.overrides.OverriddenCount(ProfileGroup::Behavior), 2u);
    EXPECT_EQ(profile.overrides.OverriddenCount(ProfileGroup::Shortcuts), 1u);
    EXPECT_FALSE(profile.overrides.Empty());

    profile.overrides.counterThreshold = 50;
    EXPECT_EQ(profile.overrides.OverriddenCount(ProfileGroup::Behavior), 3u);
}

TEST(ProfileTest, ANumberOverrideCountsAndApplies) {
    Profile profile = GameProfile("Game", "game.exe");
    profile.overrides.counterThreshold = 50;
    EXPECT_FALSE(profile.overrides.Empty());

    const ProfileableSettings resolved = ResolveForApplication(Defaults(), {profile}, App("game.exe"));
    EXPECT_EQ(resolved.counterThreshold, 50);
    EXPECT_EQ(resolved.InputOptions().counterThreshold, 50);
    EXPECT_EQ(ResolveForApplication(Defaults(), {profile}, App("other.exe")).counterThreshold,
              Defaults().counterThreshold);
}

TEST(ProfileTest, InputOptionsRoundTripThroughTheFlatFields) {
    ProfileableSettings settings;
    platform::EditModeInputOptions options;
    options.useSoftwarePointer = false;
    options.useRawMouseInput = true;
    options.counterRawMouseInput = true;
    settings.SetInputOptions(options);
    EXPECT_EQ(settings.InputOptions(), options);
    EXPECT_FALSE(settings.softwarePointer);
    EXPECT_TRUE(settings.rawMouseInput);
}

TEST(ProfileTest, ProfilesRoundTripThroughTheConfigFile) {
    AppConfig config = DefaultConfig();

    Profile preset;
    preset.name = "Unreal games";
    preset.overrides.counterRawMouseInput = false;

    Profile game = GameProfile("Elden Ring", "eldenring.exe");
    game.match.executables.push_back("start_protected_game.exe");
    game.match.titleContains.push_back("ELDEN RING");
    game.overrides.dontStealFocus = true;
    game.overrides.shortcuts[ShortcutActionIndex(ShortcutAction::Draw)] = platform::KeyCombo{};
    game.overrides.shortcuts[ShortcutActionIndex(ShortcutAction::Text)] =
        platform::KeyCombo{true, false, false, 'L'};

    config.profiles = {preset, game};

    const AppConfig reparsed = ParseConfig(SerializeConfig(config));
    EXPECT_EQ(reparsed.profiles, config.profiles);
    EXPECT_EQ(reparsed, config);
}

// A name cleared by hand costs the name, not the profile: its match and
// overrides come back under the name a new profile would get.
TEST(ProfileTest, AProfileWithoutANameIsKeptAndNamed) {
    AppConfig config = DefaultConfig();
    Profile nameless = GameProfile("", "game.exe");
    nameless.overrides.freezeScreen = true;
    config.profiles = {GameProfile("Profile", "other.exe"), nameless};

    const AppConfig reparsed = ParseConfig(SerializeConfig(config));
    ASSERT_EQ(reparsed.profiles.size(), 2u);
    EXPECT_EQ(reparsed.profiles[1].name, "Profile 2");
    EXPECT_EQ(reparsed.profiles[1].match, nameless.match);
    EXPECT_EQ(reparsed.profiles[1].overrides, nameless.overrides);
}

// A name that is not UTF-8 - cut through the middle of a character - is
// written with U+FFFD where it breaks. Writing it threw, and nothing on the
// way from a settings edit caught that: the app ended.
TEST(ProfileTest, ANameCutThroughACharacterIsStillWritten) {
    AppConfig config = DefaultConfig();
    // "Café", cut after the first of the two bytes of its "é".
    config.profiles = {GameProfile("Caf\xC3", "game.exe")};

    std::string written;
    ASSERT_NO_THROW(written = SerializeConfig(config));
    const AppConfig reparsed = ParseConfig(written);
    ASSERT_EQ(reparsed.profiles.size(), 1u);
    EXPECT_EQ(reparsed.profiles[0].name, "Caf\xEF\xBF\xBD");
    EXPECT_EQ(reparsed.profiles[0].match, config.profiles[0].match);
}

// Two profiles of one name - typed so by hand - come back as two that can
// be told apart, each with its own match and overrides.
TEST(ProfileTest, AProfileNamedLikeAnotherIsRenamed) {
    AppConfig config = DefaultConfig();
    Profile second = GameProfile("Game", "other.exe");
    second.overrides.freezeScreen = true;
    config.profiles = {GameProfile("Game", "game.exe"), second};

    const AppConfig reparsed = ParseConfig(SerializeConfig(config));
    ASSERT_EQ(reparsed.profiles.size(), 2u);
    EXPECT_EQ(reparsed.profiles[0].name, "Game");
    EXPECT_EQ(reparsed.profiles[1].name, "Game 2");
    EXPECT_EQ(reparsed.profiles[1].match, second.match);
    EXPECT_EQ(reparsed.profiles[1].overrides, second.overrides);
}

// The duplicate is renamed around every name in the file, the later ones
// too - not into a name a later profile already has, which that profile
// then lost for one it was never given.
TEST(ProfileTest, ADuplicateIsNotRenamedIntoALaterProfilesName) {
    AppConfig config = DefaultConfig();
    config.profiles = {GameProfile("Game", "a.exe"), GameProfile("Game", "b.exe"), GameProfile("Game 2", "c.exe")};

    const AppConfig reparsed = ParseConfig(SerializeConfig(config));
    ASSERT_EQ(reparsed.profiles.size(), 3u);
    EXPECT_EQ(reparsed.profiles[0].name, "Game");
    EXPECT_EQ(reparsed.profiles[1].name, "Game 3");
    EXPECT_EQ(reparsed.profiles[2].name, "Game 2") << "as named";
}

TEST(ProfileTest, AnOverrideOfEveryKindSurvivesTheFile) {
    AppConfig config = DefaultConfig();
    Profile profile = GameProfile("Game", "game.exe");
    profile.overrides.dontStealFocus = false;
    profile.overrides.takeFocusOverElevated = false;
    profile.overrides.softwarePointer = false;
    profile.overrides.rawMouseInput = false;
    profile.overrides.counterRawMouseInput = false;
    profile.overrides.freezeScreen = false;
    profile.overrides.counterThreshold = 75;
    config.profiles = {profile};

    const AppConfig reparsed = ParseConfig(SerializeConfig(config));
    ASSERT_EQ(reparsed.profiles.size(), 1u);
    EXPECT_EQ(reparsed.profiles[0].overrides, profile.overrides);
}

TEST(ProfileTest, AProfileThatOverridesNothingWritesNoGroupsAndComesBackEmpty) {
    AppConfig config = DefaultConfig();
    config.profiles = {GameProfile("Game", "game.exe")};
    ASSERT_TRUE(config.profiles[0].overrides.Empty());

    const std::string text = SerializeConfig(config);
    EXPECT_EQ(text.find("\"behavior\": {}"), std::string::npos);

    const AppConfig reparsed = ParseConfig(text);
    ASSERT_EQ(reparsed.profiles.size(), 1u);
    EXPECT_TRUE(reparsed.profiles[0].overrides.Empty());
}

TEST(ProfileTest, GarbageInTheProfilesArrayIsSkippedNotFatal) {
    const AppConfig config = ParseConfig(R"({"profiles": [
        42, "nonsense", null,
        {"name": "Kept", "match": {"exe": ["game.exe", 7, ""], "titleContains": "not a list"}}]})");
    ASSERT_EQ(config.profiles.size(), 1u);
    EXPECT_EQ(config.profiles[0].match.executables, std::vector<std::string>{"game.exe"});
    EXPECT_TRUE(config.profiles[0].match.titleContains.empty());
}

TEST(ProfileTest, ProfilesOfTheWrongShapeLeaveTheListEmpty) {
    EXPECT_TRUE(ParseConfig(R"({"profiles": {"name": "not an array"}})").profiles.empty());
    EXPECT_TRUE(ParseConfig(R"({"profiles": 5})").profiles.empty());
}

TEST(ProfileTest, UniqueProfileNameAvoidsCollisions) {
    const std::vector<Profile> profiles = {GameProfile("Elden Ring", "a.exe"),
                                            GameProfile("Elden Ring 2", "b.exe")};
    EXPECT_EQ(UniqueProfileName(profiles, "Elden Ring"), "Elden Ring 3");
    EXPECT_EQ(UniqueProfileName(profiles, "Doom"), "Doom");
    EXPECT_EQ(UniqueProfileName(profiles, ""), "Profile");
}

}  // namespace
}  // namespace sz::core
