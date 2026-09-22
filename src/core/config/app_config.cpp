#include "core/config/app_config.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

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

// Bumped when a future version needs to tell an older file's shape from its
// own. Written by every save, read by nothing yet - the point of having it
// from the start is that the first migration doesn't have to guess.
constexpr int kConfigVersion = 1;

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
// KeyCombo's own doc comment on why bare function keys are allowed. Returns
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
        } else {
            return std::nullopt;
        }
        if (plus == std::string::npos) {
            break;
        }
        pos = plus + 1;
    }
    if (!combo.IsValid()) {
        return std::nullopt;
    }
    return combo;
}

std::string FormatHotkey(const platform::KeyCombo& combo) {
    if (!combo.IsValid()) {
        return std::string();  // unbound: nothing to spell (the file says null - see HotkeyJson)
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
    } else {
        result += static_cast<char>(combo.key);
    }
    return result;
}

// "#RRGGBB" (opaque) or "#RRGGBBAA" -> packed 0xRRGGBBAA. Returns
// std::nullopt on malformed input. The eight-digit form exists for the
// settings whose alpha is part of the colour rather than a separate
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

// Eight digits only when there is an alpha worth writing, so every colour
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

std::optional<StrokeRenderMode> ParseStrokeRenderMode(std::string_view text) {
    const std::string upper = ToUpper(Trim(text));
    if (upper == "TESSELLATED") {
        return StrokeRenderMode::Tessellated;
    }
    if (upper == "POLYLINE") {
        return StrokeRenderMode::Polyline;
    }
    if (upper == "RASTERIZED") {
        return StrokeRenderMode::Rasterized;
    }
    return std::nullopt;
}

const char* StrokeRenderModeName(StrokeRenderMode mode) {
    switch (mode) {
        case StrokeRenderMode::Polyline:
            return "polyline";
        case StrokeRenderMode::Rasterized:
            return "rasterized";
        case StrokeRenderMode::Tessellated:
            break;
    }
    return "tessellated";
}

// ===== Reading =====
//
// Every setting in the file is optional and every one of these leaves its
// target alone unless the file holds a value of the right type that also
// passes whatever validation the setting has. That is the whole
// "malformed input means defaults, never throw" contract in one place -
// where the hand-rolled key=value parser this replaced had to restate it
// per key.

// The object at `key`, or an empty one - so a file missing a whole group
// reads as "said nothing about any of it" rather than needing a check per
// setting.
const json& Group(const json& parent, const char* key) {
    static const json kEmpty = json::object();
    const auto it = parent.find(key);
    return it != parent.end() && it->is_object() ? *it : kEmpty;
}

void ReadBool(const json& j, const char* key, bool& out) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) {
        out = it->get<bool>();
    }
}

// A whole number held to [min, max]; a fraction is rounded, and anything
// that is not a finite number keeps the default.
void ReadInt(const json& j, const char* key, int& out, int min, int max) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) {
        if (const double value = it->get<double>(); std::isfinite(value)) {
            out = static_cast<int>(std::lround(std::clamp(value, static_cast<double>(min), static_cast<double>(max))));
        }
    }
}

void ReadString(const json& j, const char* key, std::string& out) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
        out = it->get<std::string>();
    }
}

// `min`/`max` clamp rather than reject: for the settings that use it every
// value has a meaning and only the range is wrong, so an out-of-range one
// is pulled to the nearest end instead of silently reverting to a default
// the user never chose.
void ReadFloat(const json& j, const char* key, float& out, float min, float max) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) {
        // 1e100 is valid JSON and infinite as a float; clamping would pull
        // it to the far end of the range, which is a value the user did
        // not choose either. Not a number this setting can hold: default.
        if (const float value = it->get<float>(); std::isfinite(value)) {
            out = std::clamp(value, min, max);
        }
    }
}

// For the settings where any positive value is workable and only zero or
// negative is nonsense - rejected rather than clamped, so a typo falls back
// to the default instead of to an arbitrary bound. Positive has a ceiling
// all the same, held to rather than rejected: "workable" stops being true
// somewhere, and a value past it still says which way the user leaned.
void ReadPositiveFloat(const json& j, const char* key, float& out, float max) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) {
        if (const float value = it->get<float>(); value > 0.0f && std::isfinite(value)) {
            out = std::min(value, max);
        }
    }
}

// Both at once, and the order matters: zero is rejected outright rather
// than pulled up to `min`, because a 0px border isn't a thin border, it is
// the "show it" checkbox saying off in a worse way. Anything positive is
// then held inside the band the layout actually works in.
void ReadPositiveClampedFloat(const json& j, const char* key, float& out, float min, float max) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_number()) {
        if (const float value = it->get<float>(); value > 0.0f) {
            out = std::clamp(value, min, max);
        }
    }
}

void ReadColor(const json& j, const char* key, uint32_t& out) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
        if (const auto color = ParseHexColor(it->get<std::string>())) {
            out = *color;
        }
    }
}

// A global hotkey in the file: a combination, or null for one deliberately
// unbound - the same spelling the tool shortcuts use (see ReadShortcuts),
// and for the same reason: an unbound hotkey has to survive a round trip.
// One that took another's combination leaves that other unbound (see
// TrayController::ChangeHotkey), and reading the unbound one back as "said
// nothing" gave it its default again on the next start - where the default
// could now be the very combination the other hotkey had taken, and two
// registrations of one combination refuse to start the app.
json HotkeyJson(const platform::KeyCombo& combo) {
    if (!combo.IsValid()) {
        return nullptr;
    }
    return FormatHotkey(combo);
}

void ReadHotkey(const json& j, const char* key, platform::KeyCombo& out) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return;  // said nothing: keep the default
    }
    if (it->is_null()) {
        out = platform::KeyCombo{};
    } else if (it->is_string()) {
        if (const auto combo = ParseHotkey(it->get<std::string>())) {
            out = *combo;
        }
    }
}

void ReadShortcuts(const json& j, ShortcutBindings& out) {
    for (const ShortcutAction action : kAllShortcutActions) {
        const auto it = j.find(std::string(ShortcutActionKey(action)));
        if (it == j.end()) {
            continue;  // said nothing: keep the default
        }
        if (it->is_null()) {
            // Deliberately unbound, and it has to survive a round trip:
            // read as "said nothing", the four bound-by-default actions
            // would take their letters back on the next start.
            out[ShortcutActionIndex(action)] = platform::KeyCombo{};
        } else if (it->is_string()) {
            if (const auto combo = ParseHotkey(it->get<std::string>())) {
                out[ShortcutActionIndex(action)] = *combo;
            }
        }
    }
}

// The `input` group's key names, in one place: the base config writes all
// of them, a profile writes whichever it overrides, and both have to agree
// on what they are called.
struct InputKeys {
    static constexpr const char* kDontStealFocus = "dontStealFocus";
    static constexpr const char* kTakeFocusOverElevated = "takeFocusOverElevated";
    static constexpr const char* kSoftwarePointer = "softwarePointer";
    static constexpr const char* kRawMouseInput = "rawMouseInput";
    static constexpr const char* kDontForwardKeystrokes = "dontForwardKeystrokes";
    static constexpr const char* kCounterRawMouseInput = "counterRawMouseInput";
    static constexpr const char* kFreezeScreen = "freezeScreen";
};

// The sparse counterpart of ReadBool: absent (or the wrong type) leaves the
// override unset, which is what "this profile says nothing about it" is.
void ReadOptionalBool(const json& j, const char* key, std::optional<bool>& out) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_boolean()) {
        out = it->get<bool>();
    }
}

void WriteOptionalBool(json& j, const char* key, const std::optional<bool>& value) {
    if (value.has_value()) {
        j[key] = *value;
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

// A bar's buttons, as the file gives them: an array of names, or of
// objects naming a button and saying whether it is shown. The bare name is
// there for a file edited by hand, where ["pin", "close"] is the obvious
// way to write "just these two, in this order"; what a shown button is
// written as when this writes the file is the object, since that is the
// form that can also say no.
//
// Nothing is read into the config unless the array is actually there: a
// file that says nothing about a bar keeps the default, the same rule the
// shortcuts follow.
std::optional<BarButtonList> ReadBar(const json& group, const char* key) {
    const auto it = group.find(key);
    if (it == group.end() || !it->is_array()) {
        return std::nullopt;
    }
    BarButtonList list;
    for (const json& entry : *it) {
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

json WriteBar(const BarButtonList& buttons) {
    json out = json::array();
    for (const BarButtonSetting& entry : buttons) {
        out.push_back(json{{"button", std::string(BarButtonKey(entry.button))}, {"shown", entry.shown}});
    }
    return out;
}

json WriteShortcuts(const ShortcutBindings& bindings) {
    json out = json::object();
    for (const ShortcutAction action : kAllShortcutActions) {
        const platform::KeyCombo& combo = bindings[ShortcutActionIndex(action)];
        const std::string key(ShortcutActionKey(action));
        if (combo.key == 0) {
            out[key] = nullptr;
        } else {
            out[key] = FormatHotkey(combo);
        }
    }
    return out;
}

}  // namespace

ProfileableSettings ProfileableFrom(const AppConfig& config) {
    ProfileableSettings settings;
    settings.dontStealFocus = config.editModeNoActivate;
    settings.takeFocusOverElevated = config.takeFocusOverElevated;
    settings.SetInputOptions(config.editModeInput);
    settings.freezeScreen = config.freezeScreenInEditMode;
    settings.shortcuts = config.toolShortcuts;
    return settings;
}

void ApplyProfileable(const ProfileableSettings& settings, AppConfig& config) {
    config.editModeNoActivate = settings.dontStealFocus;
    config.takeFocusOverElevated = settings.takeFocusOverElevated;
    config.editModeInput = settings.InputOptions();
    config.freezeScreenInEditMode = settings.freezeScreen;
    config.toolShortcuts = settings.shortcuts;
}

AppConfig DefaultConfig() { return AppConfig{}; }

AppConfig ParseConfig(std::string_view text) {
    AppConfig config = DefaultConfig();

    // No exceptions, no callback: a hand-edited or truncated file is a
    // discarded value here, and every setting below then keeps its default.
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        return config;
    }

    const json& hotkeys = Group(doc, "hotkeys");
    ReadHotkey(hotkeys, "editMode", config.hotkeyEditMode);
    ReadHotkey(hotkeys, "viewMode", config.hotkeyViewMode);
    ReadHotkey(hotkeys, "quickCapture", config.hotkeyQuickCapture);
    ReadHotkey(hotkeys, "silentCapture", config.hotkeySilentCapture);

    const json& drawing = Group(doc, "drawing");
    ReadColor(drawing, "strokeColor", config.strokeColorRGBA);
    ReadPositiveFloat(drawing, "strokeWidth", config.strokeWidth, kMaxStrokeWidthPx);
    if (const auto it = drawing.find("renderMode"); it != drawing.end() && it->is_string()) {
        if (const auto mode = ParseStrokeRenderMode(it->get<std::string>())) {
            config.strokeRenderMode = *mode;
        }
    }
    ReadBool(drawing, "paintPixels", config.paintPixelsInsteadOfStrokes);
    ReadBool(drawing, "raiseSelected", config.raiseSelectedSnippet);

    const json& bars = Group(doc, "bars");
    if (std::optional<BarButtonList> snippetBar = ReadBar(bars, "snippet")) {
        config.snippetBar = std::move(*snippetBar);
    }
    if (std::optional<BarButtonList> drawingBar = ReadBar(bars, "drawing")) {
        config.drawingBar = std::move(*drawingBar);
    }
    // Whatever the file said, both lists come out holding each of their
    // bar's buttons exactly once - see NormalizeSnippetBar.
    NormalizeSnippetBar(config.snippetBar);
    NormalizeDrawingBar(config.drawingBar);

    const json& appearance = Group(doc, "appearance");
    ReadBool(appearance, "showItemBorders", config.showItemBorders);
    ReadBool(appearance, "showToastsWhileHidden", config.showToastsWhileHidden);
    ReadColor(appearance, "accentColor", config.accentColorRGBA);

    const json& snippetColors = Group(appearance, "snippetColors");
    ReadColor(snippetColors, "borderFront", config.itemBorderColorFrontRGBA);
    ReadColor(snippetColors, "borderOther", config.itemBorderColorOtherRGBA);
    ReadColor(snippetColors, "borderPinned", config.itemBorderColorPinnedRGBA);
    ReadBool(Group(appearance, "canvasBar"), "show", config.showCanvasBar);

    const json& border = Group(appearance, "editModeBorder");
    ReadBool(border, "show", config.showEditModeBorder);
    ReadColor(border, "color", config.editModeBorderColorRGBA);
    ReadFloat(border, "opacity", config.editModeBorderOpacity, 0.0f, 1.0f);
    ReadPositiveClampedFloat(border, "width", config.editModeBorderWidthPx, kEditModeBorderWidthMin,
               kEditModeBorderWidthMax);
    ReadBool(border, "onlyWhenEmpty", config.editModeBorderOnlyWhenEmpty);

    const json& overview = Group(doc, "overview");
    ReadBool(overview, "showStrokes", config.overviewShowsStrokes);
    ReadBool(overview, "showBitmaps", config.overviewShowsBitmaps);

    const json& deleted = Group(doc, "deleted");
    ReadBool(deleted, "deleteForGoodAutomatically", config.purgeDeleted);
    ReadInt(deleted, "afterDays", config.purgeDeletedAfterDays, kPurgeDeletedAfterDaysMin, kPurgeDeletedAfterDaysMax);

    const json& display = Group(doc, "display");
    ReadString(display, "id", config.overlayDisplayId);
    ReadString(display, "name", config.overlayDisplayName);

    const json& input = Group(doc, "input");
    ReadBool(input, InputKeys::kDontStealFocus, config.editModeNoActivate);
    ReadBool(input, InputKeys::kTakeFocusOverElevated, config.takeFocusOverElevated);
    ReadBool(input, InputKeys::kSoftwarePointer, config.editModeInput.useSoftwarePointer);
    ReadBool(input, InputKeys::kRawMouseInput, config.editModeInput.useRawMouseInput);
    ReadBool(input, InputKeys::kDontForwardKeystrokes, config.editModeInput.dontForwardKeystrokes);
    ReadBool(input, InputKeys::kCounterRawMouseInput, config.editModeInput.counterRawMouseInput);
    ReadBool(input, InputKeys::kFreezeScreen, config.freezeScreenInEditMode);

    ReadShortcuts(Group(doc, "shortcuts"), config.toolShortcuts);

    const json& diagnostics = Group(doc, "diagnostics");
    ReadBool(diagnostics, "showDebugOverlay", config.showDebugOverlay);
    ReadBool(diagnostics, "showInputOptionsHud", config.showInputOptionsHud);
    ReadBool(diagnostics, "showLibraryTreeHud", config.showLibraryTreeHud);

    if (const auto profiles = doc.find("profiles"); profiles != doc.end() && profiles->is_array()) {
        for (const json& entry : *profiles) {
            if (!entry.is_object()) {
                continue;
            }
            Profile profile;
            if (const auto name = entry.find("name"); name != entry.end() && name->is_string()) {
                profile.name = name->get<std::string>();
            }
            if (profile.name.empty()) {
                // Nameless is unusable: the name is what the UI lists and
                // what the picker names, so a profile without one is
                // dropped rather than given a made-up one that would then
                // look like the user's own.
                continue;
            }
            const json& match = Group(entry, "match");
            profile.match.executables = ReadStringList(match, "exe");
            profile.match.titleContains = ReadStringList(match, "titleContains");

            const json& profileInput = Group(entry, "input");
            ReadOptionalBool(profileInput, InputKeys::kDontStealFocus, profile.overrides.dontStealFocus);
            ReadOptionalBool(profileInput, InputKeys::kTakeFocusOverElevated,
                              profile.overrides.takeFocusOverElevated);
            ReadOptionalBool(profileInput, InputKeys::kSoftwarePointer, profile.overrides.softwarePointer);
            ReadOptionalBool(profileInput, InputKeys::kRawMouseInput, profile.overrides.rawMouseInput);
            ReadOptionalBool(profileInput, InputKeys::kDontForwardKeystrokes,
                              profile.overrides.dontForwardKeystrokes);
            ReadOptionalBool(profileInput, InputKeys::kCounterRawMouseInput,
                              profile.overrides.counterRawMouseInput);
            ReadOptionalBool(profileInput, InputKeys::kFreezeScreen, profile.overrides.freezeScreen);

            const json& profileShortcuts = Group(entry, "shortcuts");
            for (const ShortcutAction action : kAllShortcutActions) {
                const auto it = profileShortcuts.find(std::string(ShortcutActionKey(action)));
                if (it == profileShortcuts.end()) {
                    continue;  // says nothing: inherit
                }
                if (it->is_null()) {
                    profile.overrides.shortcuts[ShortcutActionIndex(action)] = platform::KeyCombo{};
                } else if (it->is_string()) {
                    if (const auto combo = ParseHotkey(it->get<std::string>())) {
                        profile.overrides.shortcuts[ShortcutActionIndex(action)] = *combo;
                    }
                }
            }
            config.profiles.push_back(std::move(profile));
        }
    }

    return config;
}

std::string SerializeConfig(const AppConfig& config) {
    json doc;
    doc["version"] = kConfigVersion;

    doc["hotkeys"] = {
        {"editMode", HotkeyJson(config.hotkeyEditMode)},
        {"viewMode", HotkeyJson(config.hotkeyViewMode)},
        {"quickCapture", HotkeyJson(config.hotkeyQuickCapture)},
        {"silentCapture", HotkeyJson(config.hotkeySilentCapture)},
    };

    doc["drawing"] = {
        {"strokeColor", FormatHexColor(config.strokeColorRGBA)},
        {"strokeWidth", Num(config.strokeWidth)},
        {"renderMode", StrokeRenderModeName(config.strokeRenderMode)},
        {"paintPixels", config.paintPixelsInsteadOfStrokes},
        {"raiseSelected", config.raiseSelectedSnippet},
    };

    doc["appearance"] = {
        {"showItemBorders", config.showItemBorders},
        {"showToastsWhileHidden", config.showToastsWhileHidden},
        {"accentColor", FormatHexColor(config.accentColorRGBA)},
        {"snippetColors",
         {
             {"borderFront", FormatHexColor(config.itemBorderColorFrontRGBA)},
             {"borderOther", FormatHexColor(config.itemBorderColorOtherRGBA)},
             {"borderPinned", FormatHexColor(config.itemBorderColorPinnedRGBA)},
         }},
        {"canvasBar",
         {
             {"show", config.showCanvasBar},
         }},
        {"editModeBorder",
         {
             {"show", config.showEditModeBorder},
             {"color", FormatHexColor(config.editModeBorderColorRGBA)},
             {"opacity", Num(config.editModeBorderOpacity)},
             {"width", Num(config.editModeBorderWidthPx)},
             {"onlyWhenEmpty", config.editModeBorderOnlyWhenEmpty},
         }},
    };

    doc["bars"] = {
        {"snippet", WriteBar(config.snippetBar)},
        {"drawing", WriteBar(config.drawingBar)},
    };

    doc["overview"] = {
        {"showStrokes", config.overviewShowsStrokes},
        {"showBitmaps", config.overviewShowsBitmaps},
    };

    doc["deleted"] = {
        {"deleteForGoodAutomatically", config.purgeDeleted},
        {"afterDays", config.purgeDeletedAfterDays},
    };

    doc["display"] = {
        {"id", config.overlayDisplayId},
        {"name", config.overlayDisplayName},
    };

    // The group a per-application profile overrides - see AppConfig's own
    // doc comment on which settings are about the machine in front of you
    // rather than about you.
    doc["input"] = json{
        {InputKeys::kDontStealFocus, config.editModeNoActivate},
        {InputKeys::kTakeFocusOverElevated, config.takeFocusOverElevated},
        {InputKeys::kSoftwarePointer, config.editModeInput.useSoftwarePointer},
        {InputKeys::kRawMouseInput, config.editModeInput.useRawMouseInput},
        {InputKeys::kDontForwardKeystrokes, config.editModeInput.dontForwardKeystrokes},
        {InputKeys::kCounterRawMouseInput, config.editModeInput.counterRawMouseInput},
        {InputKeys::kFreezeScreen, config.freezeScreenInEditMode},
    };

    doc["shortcuts"] = WriteShortcuts(config.toolShortcuts);

    doc["diagnostics"] = {
        {"showDebugOverlay", config.showDebugOverlay},
        {"showInputOptionsHud", config.showInputOptionsHud},
        {"showLibraryTreeHud", config.showLibraryTreeHud},
    };

    // Sparse, unlike every group above: a profile writes only what it
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

        json input = json::object();
        WriteOptionalBool(input, InputKeys::kDontStealFocus, profile.overrides.dontStealFocus);
        WriteOptionalBool(input, InputKeys::kTakeFocusOverElevated, profile.overrides.takeFocusOverElevated);
        WriteOptionalBool(input, InputKeys::kSoftwarePointer, profile.overrides.softwarePointer);
        WriteOptionalBool(input, InputKeys::kRawMouseInput, profile.overrides.rawMouseInput);
        WriteOptionalBool(input, InputKeys::kDontForwardKeystrokes, profile.overrides.dontForwardKeystrokes);
        WriteOptionalBool(input, InputKeys::kCounterRawMouseInput, profile.overrides.counterRawMouseInput);
        WriteOptionalBool(input, InputKeys::kFreezeScreen, profile.overrides.freezeScreen);
        if (!input.empty()) {
            entry["input"] = std::move(input);
        }

        json shortcuts = json::object();
        for (const ShortcutAction action : kAllShortcutActions) {
            const std::optional<platform::KeyCombo>& combo =
                profile.overrides.shortcuts[ShortcutActionIndex(action)];
            if (!combo.has_value()) {
                continue;
            }
            const std::string key(ShortcutActionKey(action));
            if (combo->key == 0) {
                shortcuts[key] = nullptr;
            } else {
                shortcuts[key] = FormatHotkey(*combo);
            }
        }
        if (!shortcuts.empty()) {
            entry["shortcuts"] = std::move(shortcuts);
        }
        profiles.push_back(std::move(entry));
    }
    doc["profiles"] = std::move(profiles);

    return doc.dump(2) + "\n";
}

std::optional<AppConfig> ReadConfigFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::nullopt;  // no file: a first run
    }
    if (size > kMaxConfigFileBytes) {
        return ParseConfig("");  // not a settings file; read as one that said nothing
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string text(static_cast<size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    return ParseConfig(text);
}

bool WriteConfigFile(const std::filesystem::path& path, const AppConfig& config) {
    if (path.empty()) {
        return false;
    }
    // Written to a temp file and renamed over the real one, the same way the
    // library's records are (see core/util/atomic_file.h): truncating the
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
    return WriteFileAtomically(path, text);
}

}  // namespace sz::core
