#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "core/config/app_config.h"
#include "core/config/profile.h"
#include "core/config/shortcut_action.h"
#include "platform/platform_types.h"

namespace sz::core {

// The settings, as one object with one owner: what config.json holds, and
// what that resolves to over whatever application the overlay is up over.
//
// One object rather than a copy in the UI and a copy in the controller
// kept in step by hand: both read this, and the UI edits it the way it
// edits anything else.
//
// Two kinds of field, and the difference is the whole of profiles: most
// settings are plain - Stored() is the value, Mutable() edits it - while the
// input options and the tool shortcuts may be overridden per application.
// Those are read through Live(), resolved against the profile that matched
// what the overlay came up over, and edited through the profile-aware
// setters below, which say which of the defaults or the profiles an edit
// goes into.
class Settings {
public:
    explicit Settings(AppConfig stored);

    // The config as written: the defaults, the profiles, and everything no
    // profile can touch. What the tray controller writes to config.json.
    const AppConfig& Stored() const { return stored_; }
    // For the plain fields. A UI may edit through this directly - a slider
    // bound to a field shows its value live while it is dragged - and calls
    // Commit once the edit is done, which is what gets it saved.
    AppConfig& Mutable() { return stored_; }
    // Re-resolves against the current application and tells whoever
    // listens (see SetChangedCallback) that something was edited.
    void Commit();
    // Called on Commit. The tray controller's: apply what changed to the
    // window, write config.json.
    void SetChangedCallback(std::function<void()> callback) { changedCallback_ = std::move(callback); }

    // ===== Profiles =====

    // What the overlay is up over, and so which profile applies. Resolves,
    // but notifies nobody: nothing was edited.
    void SetUnderlyingApplication(platform::ForegroundApp app);
    const platform::ForegroundApp& UnderlyingApplication() const { return underlyingApp_; }
    // The profile that matched, if one did.
    std::optional<size_t> ActiveProfile() const { return activeProfile_; }
    const std::vector<Profile>& Profiles() const { return stored_.profiles; }
    // The profileable fields as the defaults have them.
    ProfileableSettings Base() const { return ProfileableFrom(stored_); }
    // The profileable fields in effect: the defaults, resolved against the
    // active profile.
    const ProfileableSettings& Live() const { return live_; }
    // As they would be under `profile` - the defaults for nullopt. What a
    // settings panel showing one profile's values displays.
    ProfileableSettings ResolvedFor(std::optional<size_t> profile) const;
    // Whether `profile` sets the field itself rather than inheriting it.
    // Always false for the defaults, which inherit from nothing.
    bool IsOverridden(std::optional<size_t> profile, const ProfileableField& field) const;
    bool IsOverridden(std::optional<size_t> profile, const ProfileableIntField& field) const;
    bool IsShortcutOverridden(std::optional<size_t> profile, ShortcutAction action) const;

    // Each writes into the defaults for nullopt, into that profile
    // otherwise, and commits.
    void SetProfileable(std::optional<size_t> target, const ProfileableField& field, bool value);
    void ClearOverride(std::optional<size_t> profile, const ProfileableField& field);
    void SetProfileable(std::optional<size_t> target, const ProfileableIntField& field, int value);
    void ClearOverride(std::optional<size_t> profile, const ProfileableIntField& field);
    void SetShortcut(std::optional<size_t> target, ShortcutAction action, platform::KeyCombo combo);
    void ClearShortcutOverride(std::optional<size_t> profile, ShortcutAction action);
    // Replaces the whole list - adding, renaming, deleting, editing what a
    // profile matches - and commits. Which profile matches may change.
    void SetProfiles(std::vector<Profile> profiles);

    // How many times the settings have been resolved - for the input
    // options HUD, which shows it so that a toggle that seems to undo itself
    // can be told apart from one that never happened.
    int ResolveCount() const { return resolveCount_; }

private:
    bool IsProfile(std::optional<size_t> index) const { return index && *index < stored_.profiles.size(); }
    void Resolve();
    // The bodies of the overloads above, one per kind of field.
    template <typename Field>
    bool IsOverriddenImpl(std::optional<size_t> profile, const Field& field) const;
    template <typename Field, typename Value>
    void SetProfileableImpl(std::optional<size_t> target, const Field& field, Value value);
    template <typename Field>
    void ClearOverrideImpl(std::optional<size_t> profile, const Field& field);

    AppConfig stored_;
    platform::ForegroundApp underlyingApp_;
    std::optional<size_t> activeProfile_;
    ProfileableSettings live_;
    int resolveCount_ = 0;
    std::function<void()> changedCallback_;
};

}  // namespace sz::core
