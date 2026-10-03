#include "core/config/app_config.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/config/config_migrations.h"
#include "core/config/settings_catalog.h"
#include "core/diagnostics/timeline.h"
#include "core/util/atomic_file.h"

namespace sz::core {

// Ordered, not the default: this file is meant to be opened and read, and
// nlohmann's plain `json` sorts an object's keys alphabetically, which puts
// "version" at the bottom and interleaves settings by spelling rather than
// by what they belong to. Ordered keeps them in the order they are written
// below, which is the order a person would look for them in.
using json = nlohmann::ordered_json;

namespace {

// Floats go into the file as themselves rather than as their exact double
// promotion: 0.22f written verbatim is "0.2199999988079071", which is
// correct, useless to read, and alarming in a file people hand-edit. Six
// decimals is far more than any setting here resolves and round-trips back
// to the same float.
double Num(float value) { return std::round(static_cast<double>(value) * 1e6) / 1e6; }

// ===== Every field has a row =====
//
// C++ cannot list a struct's fields, so nothing in the catalog can see a
// field that has no row - and such a field is a setting nothing reads or
// writes, which no test of the rows would notice. What can be counted is
// how many initializers a struct takes, which for these is how many
// fields they have.
struct AnyInitializer {
    template <typename T>
    operator T() const;  // never called: only asked about, below
};

template <typename T, typename... Initializers>
constexpr size_t FieldCount() {
    if constexpr (requires { T{Initializers{}..., AnyInitializer{}}; }) {
        return FieldCount<T, Initializers..., AnyInitializer>();
    } else {
        return sizeof...(Initializers);
    }
}

constexpr size_t kProfileRows = [] {
    size_t rows = 0;
    std::apply([&rows](const auto*... row) { ((rows += kIsProfileSetting<std::decay_t<decltype(*row)>> ? 1 : 0), ...); },
               setting::kAll);
    return rows + 1;  // and kShortcuts
}();
// One field per overridable setting, in both halves of a profile.
static_assert(FieldCount<ProfileableSettings>() == kProfileRows,
              "a ProfileableSettings field without a row in settings_catalog.h");
static_assert(FieldCount<ProfileOverrides>() == kProfileRows,
              "a ProfileOverrides field without a row in settings_catalog.h");
// AppConfig's fields are not one per row - `profileable` holds a group of
// them, the snippet defaults hold three each, `profiles` is no row - so
// this is a tripwire rather than a proof: a field added here fails the
// build until it has its row, and then this count is raised.
static_assert(FieldCount<AppConfig>() == 47, "an AppConfig field added: give it a row in settings_catalog.h, "
                                              "then count it here");

// The version a file says it is. 1 when it says nothing a version can be -
// missing, which no build wrote but a hand edit can leave, or not a whole
// number from 1 up - since that is what every build before the first
// migration wrote.
int FileVersion(const json& doc) {
    const auto it = doc.find("version");
    if (it == doc.end() || !it->is_number_unsigned()) {
        return 1;
    }
    return static_cast<int>(std::clamp<uint64_t>(it->get<uint64_t>(), 1, std::numeric_limits<int>::max()));
}

std::string Trim(std::string_view s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(begin, end - begin));
}

std::string ToUpper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// "Ctrl+Alt+O" -> KeyCombo{ctrl=true, alt=true, shift=false, key='O'}; "F9"
// (no modifier tokens at all) -> KeyCombo{key=kFunctionKeyBase+9} - see
// KeyCombo's own doc comment on why bare function keys are allowed.
// "Mouse3", "Mouse4" and "Mouse5" are the middle button and the two side
// buttons, which only a shortcut may be (see HotkeyRule). Returns
// std::nullopt if the text doesn't parse to a valid combo.
std::optional<platform::KeyCombo> ParseHotkey(std::string_view text) {
    platform::KeyCombo combo;
    std::string remaining(text);
    size_t pos = 0;
    while (pos <= remaining.size()) {
        size_t plus = remaining.find('+', pos);
        std::string token = ToUpper(Trim(
            plus == std::string::npos ? remaining.substr(pos) : remaining.substr(pos, plus - pos)));
        if (token == "CTRL" || token == "CONTROL") {
            combo.ctrl = true;
        } else if (token == "ALT") {
            combo.alt = true;
        } else if (token == "SHIFT") {
            combo.shift = true;
        } else if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') ||
                                          (token[0] >= '0' && token[0] <= '9'))) {
            combo.key = token[0];
        } else if (token.size() >= 2 && token.size() <= 3 && token[0] == 'F' &&
                   std::all_of(token.begin() + 1, token.end(),
                               [](char c) { return c >= '0' && c <= '9'; })) {
            int n = 0;
            const auto* begin = token.data() + 1;
            const auto* end = token.data() + token.size();
            if (std::from_chars(begin, end, n).ec != std::errc() || n < 1 || n > 24) {
                return std::nullopt;
            }
            combo.key = platform::KeyCombo::kFunctionKeyBase + n;
        } else if (token == "MOUSE3") {
            combo.key = platform::KeyCombo::kMiddleButton;
        } else if (token == "MOUSE4") {
            combo.key = platform::KeyCombo::kX1Button;
        } else if (token == "MOUSE5") {
            combo.key = platform::KeyCombo::kX2Button;
        } else {
            return std::nullopt;
        }
        if (plus == std::string::npos) {
            break;
        }
        pos = plus + 1;
    }
    if (!combo.IsValid() && !combo.IsMouseButton()) {
        return std::nullopt;
    }
    return combo;
}

std::string FormatHotkey(const platform::KeyCombo& combo) {
    if (!combo.IsValid() && !combo.IsMouseButton()) {
        return std::string();  // unbound: nothing to spell (the file says null - see Write(HotkeyRule))
    }
    std::string result;
    if (combo.ctrl) {
        result += "Ctrl+";
    }
    if (combo.alt) {
        result += "Alt+";
    }
    if (combo.shift) {
        result += "Shift+";
    }
    if (combo.IsFunctionKey()) {
        result += "F";
        result += std::to_string(combo.FunctionKeyNumber());
    } else if (combo.IsMouseButton()) {
        result += "Mouse";
        result += std::to_string(combo.key - platform::KeyCombo::kMouseButtonBase + 2);
    } else {
        result += static_cast<char>(combo.key);
    }
    return result;
}

// "#RRGGBB" (opaque) or "#RRGGBBAA" -> packed 0xRRGGBBAA. Returns
// std::nullopt on malformed input. The eight-digit form exists for the
// settings whose alpha is part of the color rather than a separate
// opacity field beside it - see AppConfig::itemBorderColorFrontRGBA.
std::optional<uint32_t> ParseHexColor(std::string_view text) {
    const bool withAlpha = text.size() == 9;
    if ((text.size() != 7 && !withAlpha) || text[0] != '#') {
        return std::nullopt;
    }
    unsigned int packed = 0;
    auto [ptr, ec] = std::from_chars(text.data() + 1, text.data() + text.size(), packed, 16);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return withAlpha ? packed : ((packed << 8) | 0xFFu);
}

// Eight digits only when there is an alpha worth writing, so every color
// that is simply opaque - which is all of them that were here before -
// keeps the six-digit spelling it has always had on disk.
std::string FormatHexColor(uint32_t colorRGBA) {
    char buf[10];
    if ((colorRGBA & 0xFFu) == 0xFFu) {
        std::snprintf(buf, sizeof(buf), "#%06X", (colorRGBA >> 8) & 0xFFFFFFu);
    } else {
        std::snprintf(buf, sizeof(buf), "#%08X", colorRGBA);
    }
    return std::string(buf);
}

// ===== Reading and writing one setting =====
//
// Parse turns what the file holds into a candidate value, or nothing for
// JSON that is not that kind of value at all; the row's rule then decides
// what the setting keeps (Hold - see setting.h). A setting the file says
// nothing about never gets this far, and keeps its default. That is the
// whole "malformed input means defaults, never throw" contract, in one
// place for every setting.

// The object at `key`, or an empty one - so a file missing a whole group
// reads as "said nothing about any of it" rather than needing a check per
// setting.
const json& Group(const json& parent, std::string_view key) {
    static const json kEmpty = json::object();
    const auto it = parent.find(std::string(key));
    return it != parent.end() && it->is_object() ? *it : kEmpty;
}

std::optional<bool> Parse(const BoolRule&, const json& j) {
    return j.is_boolean() ? std::optional(j.get<bool>()) : std::nullopt;
}

// Held to the band here already, in double: a number past int's range is
// still a very large number, not an overflow.
std::optional<int> ParseWholeNumber(const json& j, int min, int max) {
    if (!j.is_number()) {
        return std::nullopt;
    }
    const double value = j.get<double>();
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return static_cast<int>(std::lround(std::clamp(value, static_cast<double>(min), static_cast<double>(max))));
}

std::optional<int> Parse(const IntRule& rule, const json& j) { return ParseWholeNumber(j, rule.min, rule.max); }

// As a float, which is what the setting holds: 1e100 is a number here and
// infinite there, and the rule rejects it.
std::optional<float> ParseFloat(const json& j) {
    return j.is_number() ? std::optional(j.get<float>()) : std::nullopt;
}
std::optional<float> Parse(const FloatRule&, const json& j) { return ParseFloat(j); }
std::optional<float> Parse(const PositiveFloatRule&, const json& j) { return ParseFloat(j); }
std::optional<float> Parse(const PositiveBandRule&, const json& j) { return ParseFloat(j); }

std::optional<uint32_t> Parse(const ColorRule&, const json& j) {
    return j.is_string() ? ParseHexColor(j.get<std::string>()) : std::nullopt;
}

template <typename E>
std::optional<E> Parse(const ChoiceRule<E>& rule, const json& j) {
    if (!j.is_string()) {
        return std::nullopt;
    }
    const std::string upper = ToUpper(Trim(j.get<std::string>()));
    for (const Choice<E>& choice : rule.choices) {
        if (upper == ToUpper(std::string(choice.name))) {
            return choice.value;
        }
    }
    return std::nullopt;
}

// "auto", or a percentage. Anything else keeps the default, which is
// "auto" - a scale nobody chose is Windows' one.
std::optional<int> Parse(const AutoOrPercentRule& rule, const json& j) {
    if (j.is_string()) {
        return j.get<std::string>() == "auto" ? std::optional(0) : std::nullopt;
    }
    return ParseWholeNumber(j, rule.min, rule.max);
}

// A combination, or null for one deliberately unbound - and an unbound one
// has to survive a round trip. One that took another's combination leaves
// that other unbound (see TrayController::ChangeHotkey), and reading the
// unbound one back as "said nothing" gave it its default again on the next
// start - where the default could now be the very combination the other
// hotkey had taken, and two registrations of one combination refuse to
// start the app. The same for a shortcut: read as "said nothing", the ones
// bound by default would take their letters back.
std::optional<platform::KeyCombo> ParseCombo(const json& j) {
    if (j.is_null()) {
        return platform::KeyCombo{};
    }
    return j.is_string() ? ParseHotkey(j.get<std::string>()) : std::nullopt;
}
std::optional<platform::KeyCombo> Parse(const HotkeyRule&, const json& j) { return ParseCombo(j); }
std::optional<platform::KeyCombo> Parse(const ShortcutRule&, const json& j) { return ParseCombo(j); }

std::optional<std::string> Parse(const TextRule&, const json& j) {
    return j.is_string() ? std::optional(j.get<std::string>()) : std::nullopt;
}

std::optional<std::map<std::string, std::string>> Parse(const TextMapRule&, const json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    std::map<std::string, std::string> map;
    for (const auto& [key, value] : j.items()) {
        if (value.is_string()) {
            map.emplace(key, value.get<std::string>());
        }
    }
    return map;
}

// A bar's buttons, as the file gives them: an array of names, or of
// objects naming a button and saying whether it is shown. The bare name is
// there for a file edited by hand, where ["pin", "close"] is the obvious
// way to write "just these two, in this order"; what a shown button is
// written as when this writes the file is the object, since that is the
// form that can also say no. Names this build does not know are dropped;
// the rule then makes the list hold each of the bar's buttons once.
std::optional<BarButtonList> Parse(const BarRule&, const json& j) {
    if (!j.is_array()) {
        return std::nullopt;
    }
    BarButtonList list;
    for (const json& entry : j) {
        std::optional<ChromeButton> button;
        bool shown = true;
        if (entry.is_string()) {
            button = BarButtonFromKey(entry.get<std::string>());
        } else if (entry.is_object()) {
            if (const auto name = entry.find("button"); name != entry.end() && name->is_string()) {
                button = BarButtonFromKey(name->get<std::string>());
            }
            if (const auto isShown = entry.find("shown"); isShown != entry.end() && isShown->is_boolean()) {
                shown = isShown->get<bool>();
            }
        }
        if (button.has_value()) {
            list.push_back(BarButtonSetting{*button, shown});
        }
    }
    return list;
}

json Write(const BoolRule&, bool value) { return value; }
json Write(const IntRule&, int value) { return value; }
json Write(const FloatRule&, float value) { return Num(value); }
json Write(const PositiveFloatRule&, float value) { return Num(value); }
json Write(const PositiveBandRule&, float value) { return Num(value); }
json Write(const ColorRule&, uint32_t value) { return FormatHexColor(value); }
template <typename E>
json Write(const ChoiceRule<E>& rule, E value) {
    for (const Choice<E>& choice : rule.choices) {
        if (choice.value == value) {
            return std::string(choice.name);
        }
    }
    return std::string(rule.choices.front().name);
}
json Write(const AutoOrPercentRule&, int value) { return value == 0 ? json("auto") : json(value); }
json Write(const HotkeyRule&, const platform::KeyCombo& value) {
    return value.IsValid() ? json(FormatHotkey(value)) : json(nullptr);
}
json Write(const ShortcutRule&, const platform::KeyCombo& value) {
    return value.key == 0 ? json(nullptr) : json(FormatHotkey(value));
}
json Write(const TextRule&, const std::string& value) { return value; }
json Write(const TextMapRule&, const std::map<std::string, std::string>& map) {
    json out = json::object();
    for (const auto& [key, value] : map) {
        out[key] = value;
    }
    return out;
}
json Write(const BarRule&, const BarButtonList& buttons) {
    json out = json::array();
    for (const BarButtonSetting& entry : buttons) {
        out.push_back(json{{"button", std::string(BarButtonKey(entry.button))}, {"shown", entry.shown}});
    }
    return out;
}

// Whether a value is left out of the file: only a text size not decided
// yet (see AppConfig::noteTextSizePx), which the file says by saying
// nothing.
template <typename Rule>
bool Omitted(const Rule&, const typename Rule::Value&) {
    return false;
}
bool Omitted(const PositiveBandRule& rule, float value) { return rule.zeroIsUndecided && value <= 0.0f; }

// What the file holds for a setting, if it holds anything: `path` read in
// `root`, which is the document or, for a profile's own settings, the
// profile's object.
const json* Find(const json& root, const SettingPath& path) {
    const json* parent = &Group(root, path.group);
    if (!path.subgroup.empty()) {
        parent = &Group(*parent, path.subgroup);
    }
    const auto it = parent->find(std::string(path.key));
    return it != parent->end() ? &*it : nullptr;
}

// The same place, made if it is not there yet - in the order this is
// called in, which is the catalog's.
json& Slot(json& root, const SettingPath& path) {
    json& group = root[std::string(path.group)];
    json& parent = path.subgroup.empty() ? group : group[std::string(path.subgroup)];
    return parent[std::string(path.key)];
}

template <typename Rule>
std::optional<typename Rule::Value> ReadValue(const Rule& rule, const json* j) {
    if (j == nullptr) {
        return std::nullopt;  // said nothing
    }
    if (std::optional<typename Rule::Value> value = Parse(rule, *j)) {
        return Hold(rule, std::move(*value));
    }
    return std::nullopt;
}

template <typename Rule>
void WriteValue(json& root, const SettingPath& path, const Rule& rule, const typename Rule::Value& value) {
    if (!Omitted(rule, value)) {
        Slot(root, path) = Write(rule, value);
    }
}

// One row, from the file into the config and back. The shortcuts are
// fifteen settings keyed by action in their group.
template <typename Row>
void ReadRow(const json& doc, const Row& row, AppConfig& config) {
    if (auto value = ReadValue(row.rule, Find(doc, row.path))) {
        ValueIn(row, config) = std::move(*value);
    }
}
// An action the file says nothing of - one added since it was written, as
// Paste in place was in 0.2.3 - has its default, unless the file gives
// that combination to another action: it was chosen for that one, and the
// new action starts unbound rather than take it. Taken, the key would go
// to whichever of the two comes first (see Editor::CommandForKey).
void ReadRow(const json& doc, const ShortcutSettings& row, AppConfig& config) {
    ShortcutBindings& shortcuts = config.profileable.shortcuts;
    std::array<bool, kShortcutActionCount> said{};
    for (const ShortcutAction action : kAllShortcutActions) {
        if (auto value = ReadValue(row.rule, Find(doc, {row.group, "", ShortcutActionKey(action)}))) {
            shortcuts[ShortcutActionIndex(action)] = *value;
            said[ShortcutActionIndex(action)] = true;
        }
    }
    for (size_t unsaid = 0; unsaid < kShortcutActionCount; ++unsaid) {
        if (said[unsaid] || !shortcuts[unsaid].IsValid()) {
            continue;
        }
        for (size_t other = 0; other < kShortcutActionCount; ++other) {
            if (said[other] && shortcuts[other] == shortcuts[unsaid]) {
                shortcuts[unsaid] = platform::KeyCombo{};
                break;
            }
        }
    }
}

template <typename Row>
void WriteRow(json& doc, const Row& row, const AppConfig& config) {
    WriteValue(doc, row.path, row.rule, ValueIn(row, config));
}
void WriteRow(json& doc, const ShortcutSettings& row, const AppConfig& config) {
    for (const ShortcutAction action : kAllShortcutActions) {
        WriteValue(doc, {row.group, "", ShortcutActionKey(action)}, row.rule,
                   config.profileable.shortcuts[ShortcutActionIndex(action)]);
    }
}

// A profile's answer about one row, read from and written to the profile's
// object: absent is "inherit", which is why only what it overrides is
// written. Rows no profile can override have none.
template <typename Rule>
void ReadOverride(const json&, const GlobalSetting<Rule>&, ProfileOverrides&) {}
template <typename Rule>
void ReadOverride(const json& entry, const ProfileSetting<Rule>& row, ProfileOverrides& overrides) {
    if (auto value = ReadValue(row.rule, Find(entry, row.path))) {
        overrides.*row.override = std::move(*value);
    }
}
void ReadOverride(const json& entry, const ShortcutSettings& row, ProfileOverrides& overrides) {
    for (const ShortcutAction action : kAllShortcutActions) {
        if (auto value = ReadValue(row.rule, Find(entry, {row.group, "", ShortcutActionKey(action)}))) {
            overrides.shortcuts[ShortcutActionIndex(action)] = *value;
        }
    }
}

template <typename Rule>
void WriteOverride(json&, const GlobalSetting<Rule>&, const ProfileOverrides&) {}
template <typename Rule>
void WriteOverride(json& entry, const ProfileSetting<Rule>& row, const ProfileOverrides& overrides) {
    if (const auto& value = overrides.*row.override) {
        WriteValue(entry, row.path, row.rule, *value);
    }
}
void WriteOverride(json& entry, const ShortcutSettings& row, const ProfileOverrides& overrides) {
    for (const ShortcutAction action : kAllShortcutActions) {
        if (const auto& value = overrides.shortcuts[ShortcutActionIndex(action)]) {
            WriteValue(entry, {row.group, "", ShortcutActionKey(action)}, row.rule, *value);
        }
    }
}

std::vector<std::string> ReadStringList(const json& j, const char* key) {
    std::vector<std::string> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) {
        return out;
    }
    for (const json& entry : *it) {
        if (entry.is_string()) {
            if (std::string value = entry.get<std::string>(); !value.empty()) {
                out.push_back(std::move(value));
            }
        }
    }
    return out;
}

// ===== The load repairs =====
//
// What a file can say that no one setting's rule rules out, and that the
// app cannot run with as it is: the load half of each invariant in
// docs/SETTINGS.md, section 5. Each says whether it changed anything,
// since a file that needed one is written back at start (LoadedConfig::
// writeBack) and then says what runs.

// One press cannot make both: a file that gives the two the same trigger,
// by hand or by an edit gone wrong, gets the defaults back rather than one
// of them silently winning.
bool RepairCreationTriggers(AppConfig& config) {
    if (config.screenshotTrigger != config.drawingTrigger || config.screenshotTrigger == CreationTrigger::Off) {
        return false;
    }
    const AppConfig defaults;
    config.screenshotTrigger = defaults.screenshotTrigger;
    config.drawingTrigger = defaults.drawingTrigger;
    return true;
}

// One combination cannot summon two things, so a later duplicate of an
// earlier one is unbound and the earlier keeps it - what TrayController::
// ChangeHotkey does for an edit made in the app, done here for a file
// edited by hand.
bool RepairSummonHotkeys(AppConfig& config) {
    platform::KeyCombo* hotkeys[] = {&config.hotkeyEditMode, &config.hotkeyViewMode, &config.hotkeyQuickCapture,
                                     &config.hotkeySilentCapture};
    bool repaired = false;
    for (size_t later = 1; later < std::size(hotkeys); ++later) {
        for (size_t earlier = 0; earlier < later; ++earlier) {
            if (hotkeys[later]->IsValid() && *hotkeys[later] == *hotkeys[earlier]) {
                *hotkeys[later] = platform::KeyCombo{};
                repaired = true;
            }
        }
    }
    return repaired;
}

// Every repair, not only up to the first that finds something.
bool RepairOnLoad(AppConfig& config) {
    bool repaired = RepairCreationTriggers(config);
    repaired = RepairSummonHotkeys(config) || repaired;
    repaired = RepairProfileNames(config.profiles) || repaired;
    return repaired;
}

}  // namespace

AppConfig DefaultConfig() { return AppConfig{}; }

AppConfig ParseConfig(std::string_view text) {
    std::optional<ParsedConfig> parsed = TryParseConfig(text);
    return parsed ? std::move(parsed->config) : DefaultConfig();
}

std::string HotkeyText(const platform::KeyCombo& combo) { return FormatHotkey(combo); }

std::optional<ParsedConfig> TryParseConfig(std::string_view text) {
    ParsedConfig parsed;
    AppConfig& config = parsed.config;

    // No exceptions, no callback: a hand-edited or truncated file is a
    // discarded value here.
    json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        return std::nullopt;
    }

    // Brought up to this build's version before anything is read, so that
    // the reading below knows one shape only. A newer file is read as it
    // is: a key a newer build moved, or gave another meaning, is read
    // wrong or not at all, which is why it is not written over
    // (ConfigSource::Newer).
    parsed.version = FileVersion(doc);
    if (parsed.version < kConfigVersion) {
        if (!MigrateConfig(doc, parsed.version)) {
            return std::nullopt;
        }
        parsed.changed = true;
    }

    ForEachSetting([&](const auto& row) { ReadRow(doc, row, config); });

    if (const auto profiles = doc.find("profiles"); profiles != doc.end() && profiles->is_array()) {
        for (const json& entry : *profiles) {
            if (!entry.is_object()) {
                continue;
            }
            Profile profile;
            if (const auto name = entry.find("name"); name != entry.end() && name->is_string()) {
                profile.name = name->get<std::string>();
            }
            const json& match = Group(entry, "match");
            profile.match.executables = ReadStringList(match, "exe");
            profile.match.titleContains = ReadStringList(match, "titleContains");
            ForEachSetting([&](const auto& row) { ReadOverride(entry, row, profile.overrides); });
            config.profiles.push_back(std::move(profile));
        }
    }

    // Retention on, with no period the file states that can be read: off,
    // and the file made to say so. The default period in its place could
    // be far shorter than the one a hand edit broke, and what it deletes
    // does not come back.
    const auto& days = setting::kPurgeDeletedAfterDays;
    if (config.purgeDeleted && !ReadValue(days.rule, Find(doc, days.path))) {
        config.purgeDeleted = false;
        parsed.changed = true;
    }

    parsed.changed = RepairOnLoad(config) || parsed.changed;
    return parsed;
}

std::string SerializeConfig(const AppConfig& config) {
    json doc;
    doc["version"] = kConfigVersion;

    ForEachSetting([&](const auto& row) { WriteRow(doc, row, config); });

    // Sparse, unlike everything above: a profile writes only what it
    // overrides, because an absent key is how it says "inherit". Written
    // last because it is the part someone hand-editing this file is most
    // likely to be looking for, and the part that grows.
    json profiles = json::array();
    for (const Profile& profile : config.profiles) {
        json entry;
        entry["name"] = profile.name;
        json match;
        match["exe"] = profile.match.executables;
        if (!profile.match.titleContains.empty()) {
            match["titleContains"] = profile.match.titleContains;
        }
        entry["match"] = std::move(match);
        ForEachSetting([&](const auto& row) { WriteOverride(entry, row, profile.overrides); });
        profiles.push_back(std::move(entry));
    }
    doc["profiles"] = std::move(profiles);

    // A string that is not UTF-8 is written with U+FFFD where it breaks
    // rather than thrown over. nlohmann throws by default, and nothing
    // between a settings edit and here catches it: a profile name cut
    // through a character by the field that edited it ended the app. The
    // fields edit whole strings now, but a setting saved a little wrong is
    // not worth the app.
    return doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

LoadedConfig LoadOrCreateConfig(const std::filesystem::path& path, std::string_view stamp) {
    LoadedConfig loaded;
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (!ec && !exists) {
        loaded.source = ConfigSource::Created;
        WriteConfigFile(path, loaded.config);
        return loaded;
    }
    const auto size = ec ? 0 : std::filesystem::file_size(path, ec);
    if (ec) {
        loaded.source = ConfigSource::Unreadable;
        loaded.config.purgeDeleted = false;
        return loaded;
    }
    if (size <= kMaxConfigFileBytes) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            loaded.source = ConfigSource::Unreadable;
            loaded.config.purgeDeleted = false;
            return loaded;
        }
        std::string text(static_cast<size_t>(size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<size_t>(in.gcount()));
        if (std::optional<ParsedConfig> parsed = TryParseConfig(text)) {
            loaded.config = std::move(parsed->config);
            if (parsed->version > kConfigVersion) {
                loaded.source = ConfigSource::Newer;
                return loaded;
            }
            loaded.source = ConfigSource::Read;
            loaded.writeBack = parsed->changed;
            return loaded;
        }
    }
    // Not settings - not JSON, or too big to be - and set aside rather than
    // written over, so that whatever the user had in it can still be found.
    // Retention off in the stand-in, whether or not the file can be moved:
    // whether it was on is what could not be read. The defaults have it off
    // too; said here as well, so that this holds whatever they say.
    loaded.source = ConfigSource::SetAside;
    loaded.config.purgeDeleted = false;
    std::filesystem::path aside = path;
    aside.replace_filename(path.stem().string() + "-unreadable-" + std::string(stamp) + path.extension().string());
    std::filesystem::rename(path, aside, ec);
    if (!ec) {
        loaded.setAsideAs = aside;
        // The stand-in goes where the file was at once, as the settings
        // this start runs on. A write that fails here is the tray's to
        // retry - see TrayController::StartOnStandInSettings.
        WriteConfigFile(path, loaded.config);
    }
    return loaded;
}

bool WriteConfigFile(const std::filesystem::path& path, const AppConfig& config) {
    if (path.empty()) {
        return false;
    }
    // Written to a temp file and renamed over the real one (see
    // core/util/atomic_file.h): truncating the
    // real file leaves a window in which every setting the user has is a
    // half-written file, which ParseConfig quite correctly reads as
    // defaults. Nothing half-written is left beside the real file on any
    // path out of there either: a stray config.json.tmp is the kind of thing
    // a user opens by mistake and then wonders why their edits do nothing.
    std::string text = SerializeConfig(config);
#if defined(_WIN32)
    // The platform's line endings, as text mode used to give: this file is
    // hand-editable, and Notepad is what it is opened in.
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 32);
    for (const char c : text) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    text = std::move(crlf);
#endif
    const TimelineScope marked(TimelineMark::ConfigWrite);
    return WriteFileAtomically(path, text);
}

}  // namespace sz::core
