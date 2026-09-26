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
enum class ProfileGroup { Behavior, Shortcuts };

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

// Exactly the settings a profile may override: the config file's `behavior`
// and `shortcuts` groups, the ones that are about the machine in front of
// you rather than about you. Colors and the rest are deliberately absent
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
    std::optional<int> counterThreshold;
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

// The same settings as concrete values: the defaults (AppConfig::
// profileable), or the result of resolving a profile against them. Named
// as config.json names them, and flat, so that a setting is one pair of
// member pointers - `&ProfileableSettings::freezeScreen` alongside
// `&ProfileOverrides::freezeScreen` - in its catalog row (see
// ProfileSetting).
struct ProfileableSettings {
    // true (default): the overlay window never steals OS input focus just
    // from being shown or clicked in edit mode (WS_EX_NOACTIVATE on
    // Windows) - drawing/item interaction all work purely via
    // mouse routing while the game underneath keeps keyboard focus and
    // doesn't see a focus-loss event, so it won't pause/throttle the way it
    // does on view-only mode's own hotkey otherwise. The one exception is
    // typing into a rename field (see OverlayApp's folder/canvas rename),
    // which briefly requests real focus for as long as that field is open,
    // then hands it back.
    //
    // This is the fundamental decision the input options below exist to
    // make good on. On its own it has a real, measured cost: since the game
    // stays the OS foreground window, and most games gate raw/relative
    // mouse input (the kind used for camera-look) on being foreground
    // rather than on being covered or z-order, the game goes on receiving
    // full mouse input at the same time as the overlay - a dragged stroke
    // can simultaneously spin the camera. That is exactly what the input
    // options take back, which is why they default on together; turning
    // this off makes every one of them a no-op, since a game that has lost
    // focus has already stopped receiving input.
    //
    // See IOverlayWindow::SetEditModeNoActivate for how a change reaches an
    // already-created window live.
    bool dontStealFocus = true;
    // true (default): `dontStealFocus` is overruled, for one showing, when
    // the application in front is at a higher integrity level than this
    // process - something started as administrator, which on an account
    // with admin rights includes Task Manager. Windows hands such an
    // application's input to no lower-integrity process at all, so leaving
    // it focused costs not just the grab but every shortcut the overlay
    // has, and nothing else in this struct can work; taking focus is the
    // only thing that restores either. Only a *positive* reading acts: a
    // process that refuses the question is left alone, because a game
    // behind an anti-cheat driver refuses it the same way and is the one
    // thing that must keep focus. Per-application because the one good
    // reason to turn it off is per-application - an elevated game, where a
    // dead overlay still shows pinned snippets and still captures, and
    // where taking focus is the one thing that must not happen. See
    // docs/ARCHITECTURE.md.
    bool takeFocusOverElevated = true;
    // The input options: what edit mode does with physical input while
    // `dontStealFocus` is leaving the game focused. They reach the window
    // together as platform::EditModeInputOptions (InputOptions below),
    // which documents each part and what it costs; docs/ARCHITECTURE.md
    // has the measurements. Each stays individually switchable because
    // which combination is right still depends on the game.
    bool softwarePointer = true;
    bool rawMouseInput = true;
    bool dontForwardKeystrokes = true;
    bool counterRawMouseInput = false;
    // Freeze the screen while editing: on entering edit mode, grab what is
    // on screen and draw that instead of letting the live application show
    // through. For annotating over a game, this is the one thing that
    // reliably works. The camera underneath still turns while you draw -
    // nothing outside the game's process can stop that - but you no longer
    // have to watch it happen, which was most of the problem. Pairs with
    // counterRawMouseInput, whose job then becomes leaving the view roughly
    // where you found it rather than holding it still on screen.
    //
    // Off by default: it changes what the overlay fundamentally is, from a
    // sheet of glass into an opaque page, and most of what the overlay is
    // up over is not a game that turns under the mouse. Worth switching on
    // in a game's profile. A region capture taken while the screen is
    // frozen crops the frozen image rather than re-capturing the live
    // screen, so a snippet matches what you were looking at when you
    // dragged it out - see Session::CaptureShotItem.
    bool freezeScreen = false;
    int counterThreshold = platform::EditModeInputOptions{}.counterThreshold;
    // What each tool and create action is bound to while the overlay is up
    // in edit mode - see ShortcutAction, and Settings > Hotkeys, which is
    // where these are edited.
    //
    // Not OS-level hotkeys, and deliberately not stored alongside the
    // summon hotkeys (AppConfig::hotkeyEditMode and the rest): these are
    // plain keys the overlay reads from its own frame, they only do
    // anything while it is showing and taking input, and nothing about
    // them can fail the way registering a global hotkey can. That is also
    // why they're allowed to be bare letters - "P" costs nothing outside
    // edit mode.
    ShortcutBindings shortcuts = DefaultShortcuts();

    // The five that travel together as one platform type - see
    // platform::EditModeInputOptions, whose precondition helpers the
    // Settings panel and the input grab both ask.
    platform::EditModeInputOptions InputOptions() const;
    void SetInputOptions(const platform::EditModeInputOptions& options);

    bool operator==(const ProfileableSettings&) const = default;
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
