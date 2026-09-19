#include "core/session/settings.h"

#include <cmath>
#include <utility>

namespace sz::core {

Settings::Settings(AppConfig stored) : stored_(std::move(stored)) { Resolve(); }

void Settings::Resolve() {
    ++resolveCount_;
    activeProfile_ = FindMatchingProfile(stored_.profiles, underlyingApp_);
    const ProfileableSettings base = Base();
    live_ = activeProfile_ ? ResolveProfile(base, stored_.profiles, *activeProfile_) : base;
}

void Settings::Commit() {
    Resolve();
    if (changedCallback_) {
        changedCallback_();
    }
}

void Settings::SetUnderlyingApplication(platform::ForegroundApp app) {
    underlyingApp_ = std::move(app);
    Resolve();
}

ProfileableSettings Settings::ResolvedFor(std::optional<size_t> profile) const {
    return IsProfile(profile) ? ResolveProfile(Base(), stored_.profiles, *profile) : Base();
}

bool Settings::IsOverridden(std::optional<size_t> profile, const ProfileableField& field) const {
    return IsProfile(profile) && (stored_.profiles[*profile].overrides.*field.override).has_value();
}

bool Settings::IsShortcutOverridden(std::optional<size_t> profile, ShortcutAction action) const {
    return IsProfile(profile) &&
           stored_.profiles[*profile].overrides.shortcuts[ShortcutActionIndex(action)].has_value();
}

void Settings::SetProfileable(std::optional<size_t> target, const ProfileableField& field, bool value) {
    if (IsProfile(target)) {
        stored_.profiles[*target].overrides.*field.override = value;
    } else {
        ProfileableSettings base = Base();
        base.*field.value = value;
        ApplyProfileable(base, stored_);
    }
    Commit();
}

void Settings::ClearOverride(std::optional<size_t> profile, const ProfileableField& field) {
    if (!IsProfile(profile)) {
        return;  // the defaults have nothing to clear
    }
    (stored_.profiles[*profile].overrides.*field.override).reset();
    Commit();
}

void Settings::SetShortcut(std::optional<size_t> target, ShortcutAction action, platform::KeyCombo combo) {
    if (IsProfile(target)) {
        stored_.profiles[*target].overrides.shortcuts[ShortcutActionIndex(action)] = combo;
    } else {
        ProfileableSettings base = Base();
        base.shortcuts[ShortcutActionIndex(action)] = combo;
        ApplyProfileable(base, stored_);
    }
    Commit();
}

void Settings::ClearShortcutOverride(std::optional<size_t> profile, ShortcutAction action) {
    if (!IsProfile(profile)) {
        return;
    }
    stored_.profiles[*profile].overrides.shortcuts[ShortcutActionIndex(action)].reset();
    Commit();
}

void Settings::SetProfiles(std::vector<Profile> profiles) {
    stored_.profiles = std::move(profiles);
    Commit();
}

}  // namespace sz::core
