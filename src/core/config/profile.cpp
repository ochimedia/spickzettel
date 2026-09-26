#include "core/config/profile.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "core/config/settings_catalog.h"

namespace sz::core {

namespace {

std::string Lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void Apply(const ProfileOverrides& overrides, ProfileableSettings& settings) {
    ForEachProfileSetting([&](const auto& row) {
        if ((overrides.*row.override).has_value()) {
            settings.*row.value = *(overrides.*row.override);
        }
    });
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
    return OverriddenCount(ProfileGroup::Behavior) == 0 && OverriddenCount(ProfileGroup::Shortcuts) == 0;
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
        ForEachProfileSetting([&](const auto& row) { count += (this->*row.override).has_value() ? 1 : 0; });
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

bool IsProfileNameTaken(const std::vector<Profile>& profiles, size_t index, std::string_view name) {
    for (size_t other = 0; other < profiles.size(); ++other) {
        if (other != index && profiles[other].name == name) {
            return true;
        }
    }
    return false;
}

bool RepairProfileNames(std::vector<Profile>& profiles) {
    bool repaired = false;
    // Nameless is unusable - the name is what the UI lists and what the
    // picker names - but dropping the profile took its match and every
    // override with it, at the next start, for a name cleared by hand. It
    // gets the name a new profile would (the UI's "profiles.namePrefix").
    for (Profile& profile : profiles) {
        if (profile.name.empty()) {
            profile.name = "Profile";
            repaired = true;
        }
    }
    // A name another profile already has is made unique the same way: two
    // rows named alike in the picker cannot be told apart. Against every
    // name in the file, the later ones included: made unique against the
    // earlier ones alone, [Game, Game, "Game 2"] came out Game, Game 2,
    // Game 2 2 - renaming the profile its owner had named, rather than the
    // duplicate.
    for (size_t i = 0; i < profiles.size(); ++i) {
        const std::string& name = profiles[i].name;
        const auto end = profiles.begin() + static_cast<std::ptrdiff_t>(i);
        if (std::none_of(profiles.begin(), end, [&name](const Profile& p) { return p.name == name; })) {
            continue;
        }
        std::vector<Profile> others = profiles;
        others.erase(others.begin() + static_cast<std::ptrdiff_t>(i));
        profiles[i].name = UniqueProfileName(others, name);
        repaired = true;
    }
    return repaired;
}

}  // namespace sz::core
