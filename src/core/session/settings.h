#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "core/config/app_config.h"
#include "core/config/profile.h"
#include "core/config/setting.h"
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

    // ===== Edits =====
    //
    // Every change to the stored settings is one of these - docs/SETTINGS.md,
    // section 6. An edit names its row in the catalog (settings_catalog.h),
    // and is held to the row's rule: a value the rule rejects changes
    // nothing, and the edit returns false. Then the edit repairs of the
    // invariants it touches are applied (section 5), and the edit commits:
    // resolved again, and the listener told.

    // A Global row's value.
    template <typename Rule>
    const typename Rule::Value& Get(const GlobalSetting<Rule>& row) const {
        return *row.at(const_cast<AppConfig&>(stored_));  // `at` only finds the field
    }
    // A Profile row's value as `target` resolves it: the defaults for
    // nullopt, else that profile's, inherited or its own.
    template <typename Rule>
    typename Rule::Value Get(const ProfileSetting<Rule>& row, std::optional<size_t> target) const {
        return ResolvedFor(target).*row.value;
    }

    template <typename Rule>
    bool Set(const GlobalSetting<Rule>& row, typename Rule::Value value) {
        if (!Store(row, std::move(value))) {
            return false;
        }
        CommitNow();
        return true;
    }
    // Into the defaults for nullopt, into that profile's overrides
    // otherwise.
    template <typename Rule>
    bool Set(const ProfileSetting<Rule>& row, typename Rule::Value value, std::optional<size_t> target) {
        std::optional<typename Rule::Value> held = Hold(row.rule, std::move(value));
        if (!held) {
            return false;
        }
        if (IsProfile(target)) {
            stored_.profiles[*target].overrides.*row.override = std::move(*held);
        } else {
            stored_.profileable.*row.value = std::move(*held);
        }
        CommitNow();
        return true;
    }
    // A value shown before it is finished - a color or a slider while it is
    // dragged: held and stored, so that everything drawn shows it, but not
    // committed, so the file is not written once a frame. Global rows only:
    // what a profile resolves to is decided at the commit.
    template <typename Rule>
    bool Preview(const GlobalSetting<Rule>& row, typename Rule::Value value) {
        if (!Store(row, std::move(value))) {
            return false;
        }
        previewing_ = true;
        return true;
    }
    // Finishes the previews: commits them, if there are any.
    void CommitPreviews() {
        if (previewing_) {
            CommitNow();
        }
    }
    bool Previewing() const { return previewing_; }

    // Whether `profile` states the row for itself rather than inheriting it.
    // Always false for the defaults, which inherit from nothing.
    template <typename Rule>
    bool IsOverridden(const ProfileSetting<Rule>& row, std::optional<size_t> profile) const {
        return IsProfile(profile) && (stored_.profiles[*profile].overrides.*row.override).has_value();
    }
    // The profile inherits the row again, and the edit commits. Nothing for
    // the defaults, which have nothing to clear.
    template <typename Rule>
    void ClearOverride(const ProfileSetting<Rule>& row, std::optional<size_t> profile) {
        if (!IsProfile(profile)) {
            return;
        }
        (stored_.profiles[*profile].overrides.*row.override).reset();
        CommitNow();
    }

    // ===== Profiles =====

    // What the overlay is up over, and so which profile applies. Resolves,
    // but notifies nobody: nothing was edited.
    void SetUnderlyingApplication(platform::ForegroundApp app);
    const platform::ForegroundApp& UnderlyingApplication() const { return underlyingApp_; }
    // The profile that matched, if one did.
    std::optional<size_t> ActiveProfile() const { return activeProfile_; }
    const std::vector<Profile>& Profiles() const { return stored_.profiles; }
    // The profileable fields as the defaults have them.
    const ProfileableSettings& Base() const { return stored_.profileable; }
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
    bool IsShortcutOverridden(ShortcutAction action, std::optional<size_t> profile) const;

    // Each writes into the defaults for nullopt, into that profile
    // otherwise, and commits.
    void SetProfileable(std::optional<size_t> target, const ProfileableField& field, bool value);
    void ClearOverride(std::optional<size_t> profile, const ProfileableField& field);
    void SetProfileable(std::optional<size_t> target, const ProfileableIntField& field, int value);
    void ClearOverride(std::optional<size_t> profile, const ProfileableIntField& field);
    // Binds `combo` to `action` in `target`, and takes it off whatever else
    // there held it (the edit repair of "one combination, one action"):
    // refusing instead would leave the user to find the other holder, and
    // two actions on one key is a state where only the first can fire (see
    // Editor::CommandForKey). Judged against what `target` resolves to, not
    // against what is running: a collision inside a profile is one when that
    // profile is active, and a key the defaults use elsewhere is not this
    // profile's to solve. An unbound combo collides with nothing.
    void SetShortcut(ShortcutAction action, platform::KeyCombo combo, std::optional<size_t> target);
    void ClearShortcutOverride(ShortcutAction action, std::optional<size_t> profile);
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
    // The commit: resolved again against the same application, and the
    // listener told. Finishes any preview with it.
    void CommitNow();
    // A Global row's value held to its rule and stored, with the edit repair
    // of any invariant it is part of. False, and nothing stored, for a value
    // the rule rejects.
    template <typename Rule>
    bool Store(const GlobalSetting<Rule>& row, typename Rule::Value value) {
        std::optional<typename Rule::Value> held = Hold(row.rule, std::move(value));
        if (!held) {
            return false;
        }
        typename Rule::Value old = std::exchange(*row.at(stored_), std::move(*held));
        RepairEdit(row, old);
        return true;
    }
    // The edit repairs, by the kind of row edited - docs/SETTINGS.md,
    // section 5. Most rows are part of no invariant.
    template <typename Rule>
    void RepairEdit(const GlobalSetting<Rule>&, const typename Rule::Value&) {}
    // The other creation trigger takes the edited one's old press, rather
    // than both having one.
    void RepairEdit(const GlobalSetting<ChoiceRule<CreationTrigger>>& row, const CreationTrigger& old);
    // Another summon hotkey that had the combination is left unbound.
    void RepairEdit(const GlobalSetting<HotkeyRule>& row, const platform::KeyCombo& old);
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
    // A preview stored and not committed yet - see Preview.
    bool previewing_ = false;
    std::function<void()> changedCallback_;
};

}  // namespace sz::core
