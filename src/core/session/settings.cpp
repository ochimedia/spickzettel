#include "core/session/settings.h"

#include <cmath>
#include <utility>

#include "core/config/settings_catalog.h"

namespace sz::core {

Settings::Settings(AppConfig stored) : stored_(std::move(stored)) { Resolve(); }

void Settings::Resolve() {
    ++resolveCount_;
    activeProfile_ = FindMatchingProfile(stored_.profiles, underlyingApp_);
    live_ = activeProfile_ ? ResolveProfile(Base(), stored_.profiles, *activeProfile_) : Base();
}

void Settings::Commit() { CommitNow(); }

void Settings::CommitNow() {
    previewing_ = false;
    Resolve();
    if (changedCallback_) {
        changedCallback_();
    }
}

void Settings::RepairEdit(const GlobalSetting<ChoiceRule<CreationTrigger>>& row, const CreationTrigger& old) {
    const bool screenshot = &row == &setting::kScreenshotTrigger;
    const CreationTrigger edited = screenshot ? stored_.screenshotTrigger : stored_.drawingTrigger;
    CreationTrigger& other = screenshot ? stored_.drawingTrigger : stored_.screenshotTrigger;
    // Swapped rather than refused: a dropdown that grays out the very choice
    // wanted, with the reason in another row, is a puzzle.
    if (edited == other && edited != CreationTrigger::Off) {
        other = old;
    }
}

void Settings::RepairEdit(const GlobalSetting<HotkeyRule>& row, const platform::KeyCombo& /*old*/) {
    const platform::KeyCombo& combo = *row.at(stored_);
    if (!combo.IsValid()) {
        return;  // unbound registers nothing, so it collides with nothing
    }
    for (const GlobalSetting<HotkeyRule>* hotkey : {&setting::kHotkeyEditMode, &setting::kHotkeyViewMode,
                                                    &setting::kHotkeyQuickCapture, &setting::kHotkeySilentCapture}) {
        if (hotkey != &row && *hotkey->at(stored_) == combo) {
            *hotkey->at(stored_) = platform::KeyCombo{};
        }
    }
}

void Settings::SetUnderlyingApplication(platform::ForegroundApp app) {
    underlyingApp_ = std::move(app);
    Resolve();
}

ProfileableSettings Settings::ResolvedFor(std::optional<size_t> profile) const {
    return IsProfile(profile) ? ResolveProfile(Base(), stored_.profiles, *profile) : Base();
}

template <typename Field>
bool Settings::IsOverriddenImpl(std::optional<size_t> profile, const Field& field) const {
    return IsProfile(profile) && (stored_.profiles[*profile].overrides.*field.override).has_value();
}

bool Settings::IsOverridden(std::optional<size_t> profile, const ProfileableField& field) const {
    return IsOverriddenImpl(profile, field);
}

bool Settings::IsOverridden(std::optional<size_t> profile, const ProfileableIntField& field) const {
    return IsOverriddenImpl(profile, field);
}

bool Settings::IsShortcutOverridden(ShortcutAction action, std::optional<size_t> profile) const {
    return IsProfile(profile) &&
           stored_.profiles[*profile].overrides.shortcuts[ShortcutActionIndex(action)].has_value();
}

template <typename Field, typename Value>
void Settings::SetProfileableImpl(std::optional<size_t> target, const Field& field, Value value) {
    if (IsProfile(target)) {
        stored_.profiles[*target].overrides.*field.override = value;
    } else {
        stored_.profileable.*field.value = value;
    }
    Commit();
}

void Settings::SetProfileable(std::optional<size_t> target, const ProfileableField& field, bool value) {
    SetProfileableImpl(target, field, value);
}

void Settings::SetProfileable(std::optional<size_t> target, const ProfileableIntField& field, int value) {
    SetProfileableImpl(target, field, value);
}

template <typename Field>
void Settings::ClearOverrideImpl(std::optional<size_t> profile, const Field& field) {
    if (!IsProfile(profile)) {
        return;  // the defaults have nothing to clear
    }
    (stored_.profiles[*profile].overrides.*field.override).reset();
    Commit();
}

void Settings::ClearOverride(std::optional<size_t> profile, const ProfileableField& field) {
    ClearOverrideImpl(profile, field);
}

void Settings::ClearOverride(std::optional<size_t> profile, const ProfileableIntField& field) {
    ClearOverrideImpl(profile, field);
}

void Settings::SetShortcut(ShortcutAction action, platform::KeyCombo combo, std::optional<size_t> target) {
    const std::optional<platform::KeyCombo> held = Hold(setting::kShortcuts.rule, combo);
    if (!held) {
        return;
    }
    const auto bind = [this, target](ShortcutAction bound, platform::KeyCombo to) {
        if (IsProfile(target)) {
            stored_.profiles[*target].overrides.shortcuts[ShortcutActionIndex(bound)] = to;
        } else {
            stored_.profileable.shortcuts[ShortcutActionIndex(bound)] = to;
        }
    };
    if (held->key != 0) {
        const ProfileableSettings resolved = ResolvedFor(target);
        for (const ShortcutAction other : kAllShortcutActions) {
            if (other != action && resolved.shortcuts[ShortcutActionIndex(other)] == *held) {
                bind(other, platform::KeyCombo{});
            }
        }
    }
    bind(action, *held);
    CommitNow();
}

void Settings::ClearShortcutOverride(ShortcutAction action, std::optional<size_t> profile) {
    if (!IsProfile(profile)) {
        return;
    }
    stored_.profiles[*profile].overrides.shortcuts[ShortcutActionIndex(action)].reset();
    CommitNow();
}

void Settings::SetProfiles(std::vector<Profile> profiles) {
    stored_.profiles = std::move(profiles);
    Commit();
}

}  // namespace sz::core
