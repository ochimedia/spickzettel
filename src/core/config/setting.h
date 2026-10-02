#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/config/bar_layout.h"
#include "core/config/profile.h"
#include "platform/platform_types.h"

namespace sz::core {

struct AppConfig;

// What one setting is - see docs/SETTINGS.md, which this is the code of.
// A row of the catalog (settings_catalog.h) says where the setting is in
// config.json, what values it may hold, when a change to it takes effect,
// and where its value lives. Reading and writing the file walk the rows,
// so a setting is named in the file in one place.

// ===== Rules =====
//
// What values a setting may hold. Each rule's Hold is the one answer to
// "what does this setting keep, given this value": the value as kept, or
// nullopt for one it rejects, which leaves the setting as it was. The
// parser holds what it reads to it, and an edit will (section 6), so the
// two cannot disagree. Held means pulled to the nearest end of a band:
// every value there has a meaning and only its size is wrong. Rejected is
// for a value that means nothing - a zero width is the "show it" checkbox
// saying off in a worse way.

struct BoolRule {
    using Value = bool;
};

// A whole number held to [min, max]. The file's fractions are rounded.
struct IntRule {
    using Value = int;
    int min;
    int max;
};

// A number held to [min, max]. One that is not finite - 1e100 is valid
// JSON and infinite as a float - is rejected: its far end of the band is
// a value nobody chose either.
struct FloatRule {
    using Value = float;
    float min;
    float max;
};

// Any positive number, held to `max`: "workable" stops somewhere, and a
// value past it still says which way it leaned. Zero, negative and not
// finite are rejected.
struct PositiveFloatRule {
    using Value = float;
    float max;
};

// A positive number held to [min, max]; zero, negative and not finite are
// rejected rather than pulled up. With `zeroIsUndecided` the setting's own
// 0 means "not decided yet" and is not written at all (see AppConfig::
// noteTextSizePx).
struct PositiveBandRule {
    using Value = float;
    float min;
    float max;
    bool zeroIsUndecided = false;
};

// A color packed 0xRRGGBBAA: "#RRGGBB" in the file when opaque,
// "#RRGGBBAA" otherwise.
struct ColorRule {
    using Value = uint32_t;
};

// One of a list of names, matched ignoring case. The names are what the
// file says, so the enum may be reordered freely.
template <typename E>
struct Choice {
    E value;
    std::string_view name;
};
template <typename E>
struct ChoiceRule {
    using Value = E;
    std::span<const Choice<E>> choices;
};

// A percentage held to [min, max], or 0, written "auto": follow Windows.
struct AutoOrPercentRule {
    using Value = int;
    int min;
    int max;
};

// A global hotkey: a key the OS can register, or unbound - `null` in the
// file, which has to survive a round trip (see AppConfig::hotkeyEditMode).
// A mouse button is no global hotkey, and one written by hand is rejected.
struct HotkeyRule {
    using Value = platform::KeyCombo;
};

// A tool shortcut: a key or one of the mouse's extra buttons, or unbound.
struct ShortcutRule {
    using Value = platform::KeyCombo;
};

struct TextRule {
    using Value = std::string;
};

// Texts by key, such as a topic's progress by its id: an object of
// strings in the file. An entry whose value is not a string reads as not
// there.
struct TextMapRule {
    using Value = std::map<std::string, std::string>;
};

// A bar's buttons, made to hold each of that bar's buttons exactly once
// by `normalize` (see NormalizeSnippetBar).
struct BarRule {
    using Value = BarButtonList;
    void (*normalize)(BarButtonList&);
};

inline std::optional<bool> Hold(const BoolRule&, bool value) { return value; }
inline std::optional<int> Hold(const IntRule& rule, int value) { return std::clamp(value, rule.min, rule.max); }
inline std::optional<float> Hold(const FloatRule& rule, float value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return std::clamp(value, rule.min, rule.max);
}
inline std::optional<float> Hold(const PositiveFloatRule& rule, float value) {
    if (!(value > 0.0f) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return std::min(value, rule.max);
}
inline std::optional<float> Hold(const PositiveBandRule& rule, float value) {
    if (!(value > 0.0f) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return std::clamp(value, rule.min, rule.max);
}
inline std::optional<uint32_t> Hold(const ColorRule&, uint32_t value) { return value; }
template <typename E>
std::optional<E> Hold(const ChoiceRule<E>& rule, E value) {
    for (const Choice<E>& choice : rule.choices) {
        if (choice.value == value) {
            return value;
        }
    }
    return std::nullopt;
}
inline std::optional<int> Hold(const AutoOrPercentRule& rule, int value) {
    return value == 0 ? 0 : std::clamp(value, rule.min, rule.max);
}
inline std::optional<platform::KeyCombo> Hold(const HotkeyRule&, platform::KeyCombo value) {
    if (value.key == 0) {
        return platform::KeyCombo{};  // unbound, whatever modifiers came with it
    }
    return value.IsValid() ? std::optional(value) : std::nullopt;
}
inline std::optional<platform::KeyCombo> Hold(const ShortcutRule&, platform::KeyCombo value) {
    if (value.key == 0) {
        return platform::KeyCombo{};
    }
    return value.IsValid() || value.IsMouseButton() ? std::optional(value) : std::nullopt;
}
inline std::optional<std::string> Hold(const TextRule&, std::string value) { return value; }
inline std::optional<std::map<std::string, std::string>> Hold(const TextMapRule&,
                                                              std::map<std::string, std::string> value) {
    return value;
}
inline std::optional<BarButtonList> Hold(const BarRule& rule, BarButtonList value) {
    rule.normalize(value);
    return value;
}

// ===== Rows =====

// When a committed change is seen - docs/SETTINGS.md, section 7.
enum class SettingEffect {
    Frame,         // the next frame drawn
    Use,           // the next time it is acted on
    Window,        // reconciled into the window after the frame
    Display,       // the overlay moves after the frame
    Freeze,        // the frozen screen: taken on entry, released when off
    Registration,  // registered with the OS as it is edited
    Session,       // the matching profile resolved again
    Start,         // the next start
};

// Where a setting is in config.json: a key in a group, or in an object
// inside the group when `subgroup` is set.
struct SettingPath {
    std::string_view group;
    std::string_view subgroup;
    std::string_view key;
};

// A setting no profile can override: one value, somewhere in AppConfig.
// `at` finds it - a function rather than a member pointer, since some live
// one object down (AppConfig::screenshotDefaults).
template <typename Rule>
struct GlobalSetting {
    using Value = typename Rule::Value;
    SettingPath path;
    Rule rule;
    SettingEffect effect;
    Value* (*at)(AppConfig&);
};

// A setting a profile may override: its value in the defaults layer (and
// in the live values) is `value`, a profile's answer about it `override`.
// The two member pointers have one Value, so the compiler keeps the two
// structs in step.
template <typename Rule>
struct ProfileSetting {
    using Value = typename Rule::Value;
    SettingPath path;
    Rule rule;
    SettingEffect effect;
    Value ProfileableSettings::*value;
    std::optional<Value> ProfileOverrides::*override;
};

// The tool shortcuts: one setting per ShortcutAction, each overridable,
// keyed in `group` by ShortcutActionKey. One row for the thirteen, since
// the actions are already a list.
struct ShortcutSettings {
    std::string_view group;
    ShortcutRule rule;
    SettingEffect effect;
};

}  // namespace sz::core
