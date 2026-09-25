#include "core/config/profile.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace sz::core {

namespace {

std::string Lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void Apply(const ProfileOverrides& overrides, ProfileableSettings& settings) {
    for (const ProfileableField& field : kProfileableFields) {
        if ((overrides.*field.override).has_value()) {
            settings.*field.value = *(overrides.*field.override);
        }
    }
    for (const ProfileableIntField& field : kProfileableIntFields) {
        if ((overrides.*field.override).has_value()) {
            settings.*field.value = *(overrides.*field.override);
        }
    }
    for (size_t i = 0; i < overrides.shortcuts.size(); ++i) {
        if (overrides.shortcuts[i].has_value()) {
            settings.shortcuts[i] = *overrides.shortcuts[i];
        }
    }
    // A key the profile binds is the profile's, over any action that
    // inherits the same key from the defaults. Settings keeps one key to
    // one action within what is being edited, but a key bound in the
    // defaults later is not checked against every profile - and with two
    // actions on one key, only the first in the list ever fired.
    for (size_t i = 0; i < overrides.shortcuts.size(); ++i) {
        if (!overrides.shortcuts[i].has_value() || overrides.shortcuts[i]->key == 0) {
            continue;
        }
        for (size_t other = 0; other < settings.shortcuts.size(); ++other) {
            if (other != i && !overrides.shortcuts[other].has_value() &&
                settings.shortcuts[other] == *overrides.shortcuts[i]) {
                settings.shortcuts[other] = platform::KeyCombo{};
            }
        }
    }
}

}  // namespace

platform::EditModeInputOptions ProfileableSettings::InputOptions() const {
    platform::EditModeInputOptions options;
    options.useSoftwarePointer = softwarePointer;
    options.useRawMouseInput = rawMouseInput;
    options.dontForwardKeystrokes = dontForwardKeystrokes;
    options.counterRawMouseInput = counterRawMouseInput;
    options.counterThreshold = counterThreshold;
    return options;
}

void ProfileableSettings::SetInputOptions(const platform::EditModeInputOptions& options) {
    softwarePointer = options.useSoftwarePointer;
    rawMouseInput = options.useRawMouseInput;
    dontForwardKeystrokes = options.dontForwardKeystrokes;
    counterRawMouseInput = options.counterRawMouseInput;
    counterThreshold = options.counterThreshold;
}

bool ProfileMatch::Matches(const platform::ForegroundApp& app) const {
    for (const std::string& executable : executables) {
        // Both sides lowercased: the platform layer already lowercases what
        // it reports, and a name typed into the UI has whatever case the
        // person used.
        if (!app.executable.empty() && Lowered(executable) == app.executable) {
            return true;
        }
    }
    if (!app.title.empty()) {
        const std::string title = Lowered(app.title);
        for (const std::string& needle : titleContains) {
            if (!needle.empty() && title.find(Lowered(needle)) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool ProfileOverrides::Empty() const {
    if (dontStealFocus || takeFocusOverElevated || softwarePointer || rawMouseInput ||
        dontForwardKeystrokes || counterRawMouseInput || freezeScreen || counterThreshold) {
        return false;
    }
    return std::none_of(shortcuts.begin(), shortcuts.end(),
                         [](const std::optional<platform::KeyCombo>& combo) { return combo.has_value(); });
}

std::optional<size_t> FindMatchingProfile(const std::vector<Profile>& profiles,
                                           const platform::ForegroundApp& app) {
    if (!app.Known()) {
        return std::nullopt;
    }
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].match.Matches(app)) {
            return i;
        }
    }
    return std::nullopt;
}

size_t ProfileOverrides::OverriddenCount(ProfileGroup group) const {
    size_t count = 0;
    if (group == ProfileGroup::Behavior) {
        for (const ProfileableField& field : kProfileableFields) {
            count += (this->*field.override).has_value() ? 1 : 0;
        }
        for (const ProfileableIntField& field : kProfileableIntFields) {
            count += (this->*field.override).has_value() ? 1 : 0;
        }
    } else {
        for (const std::optional<platform::KeyCombo>& combo : shortcuts) {
            count += combo.has_value() ? 1 : 0;
        }
    }
    return count;
}

ProfileableSettings ResolveProfile(const ProfileableSettings& base, const std::vector<Profile>& profiles,
                                    size_t index) {
    ProfileableSettings resolved = base;
    if (index >= profiles.size()) {
        return resolved;
    }
    Apply(profiles[index].overrides, resolved);
    return resolved;
}

ProfileableSettings ResolveForApplication(const ProfileableSettings& base,
                                           const std::vector<Profile>& profiles,
                                           const platform::ForegroundApp& app) {
    if (const std::optional<size_t> index = FindMatchingProfile(profiles, app)) {
        return ResolveProfile(base, profiles, *index);
    }
    return base;
}

std::string UniqueProfileName(const std::vector<Profile>& profiles, std::string_view wanted) {
    const std::string base = wanted.empty() ? std::string("Profile") : std::string(wanted);
    const auto taken = [&profiles](const std::string& candidate) {
        return std::any_of(profiles.begin(), profiles.end(),
                            [&candidate](const Profile& p) { return p.name == candidate; });
    };
    if (!taken(base)) {
        return base;
    }
    for (int suffix = 2; suffix < 1000; ++suffix) {
        std::string candidate = base + " " + std::to_string(suffix);
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return base;
}

}  // namespace sz::core
