#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/config/shortcut_action.h"
#include "platform/platform_types.h"

namespace sz::core {

// A per-application override set: what to do differently while the overlay
// is up over one particular thing.
//
// Resolution is `defaults -> the matching profile's overrides`, and that is
// the whole of it: two levels, so "where did this value come from" has two
// possible answers and "which profile am I in" has exactly one.
// Deliberately no inheritance between profiles - see docs/ARCHITECTURE.md.

// The two halves of what a profile can override, which are also the two
// Settings sections they are edited in.
enum class ProfileGroup { Input, Shortcuts };

struct ProfileMatch {
    // Lowercased executable names, matched exactly. Several because one
    // application can arrive under more than one (a launcher and the game
    // it starts, an anti-cheat wrapper and what it wraps).
    std::vector<std::string> executables;
    // Case-insensitive substrings of the window title, for the case the
    // executable can't be read at all - a process owned by another
    // account, or a game shielded by an anti-cheat driver, both of which
    // refuse to be opened. Being elevated does not do it: an elevated
    // Task Manager reports its executable name like anything else.
    // Coarser and easier to get wrong, so it is the fallback rather than
    // the first thing offered.
    std::vector<std::string> titleContains;

    bool Empty() const { return executables.empty() && titleContains.empty(); }
    bool Matches(const platform::ForegroundApp& app) const;
    bool operator==(const ProfileMatch&) const = default;
};

// Exactly the settings a profile may override: the config file's `input`
// and `shortcuts` groups, the ones that are about the machine in front of
// you rather than about you. Colours and the rest are deliberately absent
// - making them per-application would be a settings maze for no gain.
//
// std::optional throughout, because "this profile says nothing about it"
// has to be a value. For a shortcut that means three states: nullopt is
// "inherit", a default-constructed KeyCombo is "explicitly unbound", and
// anything else is a binding - the same distinction the file draws between
// an absent key and a null one.
struct ProfileOverrides {
    std::optional<bool> dontStealFocus;
    std::optional<bool> takeFocusOverElevated;
    std::optional<bool> softwarePointer;
    std::optional<bool> rawMouseInput;
    std::optional<bool> dontForwardKeystrokes;
    std::optional<bool> counterRawMouseInput;
    std::optional<bool> freezeScreen;
    std::array<std::optional<platform::KeyCombo>, kShortcutActionCount> shortcuts;

    bool Empty() const;
    // How many settings of one group this profile states for itself - what
    // the Settings panel counts to say "it sets 3 of these". Per group and
    // not in total, because the two are edited in different sections: a
    // profile that only rebinds keys would otherwise claim to have set
    // something on the Behavior section with nothing marked there.
    size_t OverriddenCount(ProfileGroup group) const;
    bool operator==(const ProfileOverrides&) const = default;
};

// The same settings as concrete values: the defaults, or the result of
// resolving a profile against them. Flat rather than mirroring AppConfig's
// nesting so that a UI row can name its field once, as a pair of pointers
// to member - `&ProfileableSettings::freezeScreen` alongside
// `&ProfileOverrides::freezeScreen` - instead of a near-identical block of
// plumbing per setting.
struct ProfileableSettings {
    bool dontStealFocus = true;
    // Qualifies the one above, and only ever in the direction of taking
    // focus: over an application at a higher integrity level nothing else
    // in this struct can work, because Windows delivers that application's
    // input to no lower-integrity process. Per-application because the one
    // good reason to turn it off is per-application - an elevated game,
    // where a dead overlay still shows pinned snippets and still captures,
    // and where taking focus is the one thing that must not happen. See
    // docs/ARCHITECTURE.md.
    bool takeFocusOverElevated = true;
    bool softwarePointer = true;
    bool rawMouseInput = true;
    bool dontForwardKeystrokes = true;
    bool counterRawMouseInput = true;
    bool freezeScreen = true;
    ShortcutBindings shortcuts = DefaultShortcuts();

    // The four that travel together as one platform type - see
    // platform::EditModeInputOptions, whose precondition helpers the
    // Settings panel and the input grab both ask.
    platform::EditModeInputOptions InputOptions() const;
    void SetInputOptions(const platform::EditModeInputOptions& options);

    bool operator==(const ProfileableSettings&) const = default;
};

// The pair of pointers that names one overridable boolean: where its
// concrete value lives, and where a profile's answer about it lives. Held
// together so a caller can't accidentally pair "freeze screen" with
// "software pointer".
struct ProfileableField {
    bool ProfileableSettings::*value;
    std::optional<bool> ProfileOverrides::*override;
};

// Every one of them, in no particular order - for the code that has to
// treat them uniformly (applying a profile, counting what it overrides).
// The Settings panel does *not* walk this: its rows are as many different
// explanations, not a list.
inline constexpr ProfileableField kProfileableFields[] = {
    {&ProfileableSettings::dontStealFocus, &ProfileOverrides::dontStealFocus},
    {&ProfileableSettings::takeFocusOverElevated, &ProfileOverrides::takeFocusOverElevated},
    {&ProfileableSettings::softwarePointer, &ProfileOverrides::softwarePointer},
    {&ProfileableSettings::rawMouseInput, &ProfileOverrides::rawMouseInput},
    {&ProfileableSettings::dontForwardKeystrokes, &ProfileOverrides::dontForwardKeystrokes},
    {&ProfileableSettings::counterRawMouseInput, &ProfileOverrides::counterRawMouseInput},
    {&ProfileableSettings::freezeScreen, &ProfileOverrides::freezeScreen},
};

struct Profile {
    // What it is called. Free text, and the handle a person uses - which is
    // why matching is on the fields below rather than on this.
    std::string name;
    ProfileMatch match;
    ProfileOverrides overrides;

    bool operator==(const Profile&) const = default;
};

// The first profile in `profiles` that matches, or nullopt. First rather
// than best: the order is the user's own and visible, and a "most specific
// wins" rule that has to be inferred is a rule nobody can predict.
std::optional<size_t> FindMatchingProfile(const std::vector<Profile>& profiles,
                                           const platform::ForegroundApp& app);

// `base` with the profile at `index` applied. An index past the end returns
// `base` unchanged rather than failing: a hand-editable file can say
// anything, and the overlay still has to come up.
//
// There is no companion for "what this profile inherits", because the
// answer is `base` itself - which is what a row of the Settings panel falls
// back to when its override is cleared.
ProfileableSettings ResolveProfile(const ProfileableSettings& base, const std::vector<Profile>& profiles,
                                    size_t index);

// The whole thing in one call: match, then resolve. `base` unchanged when
// nothing matches, which is the ordinary case and not a failure.
ProfileableSettings ResolveForApplication(const ProfileableSettings& base,
                                           const std::vector<Profile>& profiles,
                                           const platform::ForegroundApp& app);

// A name no existing profile uses, derived from `wanted` - "Elden Ring",
// then "Elden Ring 2", and so on. Nothing resolves by name, but a list with
// two identically named rows is a list you can't act on: the picker at the
// top of Input and Shortcuts names the target it is editing.
std::string UniqueProfileName(const std::vector<Profile>& profiles, std::string_view wanted);

}  // namespace sz::core
