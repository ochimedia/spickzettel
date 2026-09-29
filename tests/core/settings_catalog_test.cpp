#include "core/config/settings_catalog.h"

#include <cmath>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// Every row of the catalog, checked the same way - docs/SETTINGS.md,
// section 12, phase 2. Each test walks the rows rather than naming
// settings, so a row added is a row tested.
namespace sz::core {
namespace {

using json = nlohmann::ordered_json;

// ===== A value other than the one a setting has =====
//
// Valid for its rule, and chosen so that changing one setting alone breaks
// no invariant: a creation trigger goes to the last choice, off, which
// never clashes with the other.

bool Other(const BoolRule&, bool value) { return !value; }
int Other(const IntRule& rule, int value) { return value != rule.min ? rule.min : rule.max; }
float Other(const FloatRule& rule, float value) { return value != rule.max ? rule.max : rule.min; }
float Other(const PositiveFloatRule& rule, float value) { return value * 2.0f <= rule.max ? value * 2.0f : value / 2.0f; }
float Other(const PositiveBandRule& rule, float value) { return value != rule.max ? rule.max : rule.min; }
uint32_t Other(const ColorRule&, uint32_t value) { return value ^ 0x12345600u; }
template <typename E>
E Other(const ChoiceRule<E>& rule, E value) {
    return value != rule.choices.back().value ? rule.choices.back().value : rule.choices.front().value;
}
int Other(const AutoOrPercentRule& rule, int value) { return value == 0 ? (rule.min + rule.max) / 2 : 0; }
platform::KeyCombo Other(const HotkeyRule&, const platform::KeyCombo& value) {
    const platform::KeyCombo other{true, true, true, 'Z'};
    return value != other ? other : platform::KeyCombo{};
}
platform::KeyCombo Other(const ShortcutRule&, const platform::KeyCombo& value) {
    const platform::KeyCombo other{true, false, true, platform::KeyCombo::kX2Button};
    return value != other ? other : platform::KeyCombo{};
}
std::string Other(const TextRule&, const std::string& value) { return value + "x"; }
uint64_t Other(const IdRule&, uint64_t value) { return value + 0x1234567890ABCDEFull; }
std::map<std::string, std::string> Other(const TextMapRule&, std::map<std::string, std::string> value) {
    value["x"] += "x";
    return value;
}
BarButtonList Other(const BarRule&, BarButtonList value) {
    value.front().shown = !value.front().shown;
    return value;
}

// ===== Rows, generically =====

// What the file holds at a path, or nullptr.
const json* At(const json& root, const SettingPath& path) {
    const auto group = root.find(std::string(path.group));
    if (group == root.end()) {
        return nullptr;
    }
    const json* parent = &*group;
    if (!path.subgroup.empty()) {
        const auto sub = parent->find(std::string(path.subgroup));
        if (sub == parent->end()) {
            return nullptr;
        }
        parent = &*sub;
    }
    const auto it = parent->find(std::string(path.key));
    return it != parent->end() ? &*it : nullptr;
}

// A file holding `value` at `path` and nothing else.
std::string FileWith(const SettingPath& path, json value) {
    json doc;
    if (path.subgroup.empty()) {
        doc[std::string(path.group)][std::string(path.key)] = std::move(value);
    } else {
        doc[std::string(path.group)][std::string(path.subgroup)][std::string(path.key)] = std::move(value);
    }
    return doc.dump();
}

SettingPath ShortcutPath(const ShortcutSettings& row, ShortcutAction action) {
    return {row.group, "", ShortcutActionKey(action)};
}

// Each row's own name, for the failure messages.
std::string Name(const SettingPath& path) {
    std::string name(path.group);
    if (!path.subgroup.empty()) {
        name += "." + std::string(path.subgroup);
    }
    return name + "." + std::string(path.key);
}

// Sets one row in `config` to Other() than it is there.
template <typename Row>
void Change(const Row& row, AppConfig& config) {
    if constexpr (std::is_same_v<Row, ShortcutSettings>) {
        for (platform::KeyCombo& combo : config.profileable.shortcuts) {
            combo = Other(row.rule, combo);
        }
    } else {
        ValueIn(row, config) = Other(row.rule, ValueIn(row, config));
    }
}

// Every setting at Other(): what the stored settings become when every row
// is changed, one after the other. Two rows on one field change it twice,
// and a color changed twice is its old self again - which the fields test
// below then sees.
AppConfig EveryRowChanged() {
    AppConfig config = DefaultConfig();
    ForEachSetting([&config](const auto& row) { Change(row, config); });
    return config;
}

// ===== The fields, one by one =====
//
// Structured bindings take an aggregate apart field by field, which is as
// close to listing a struct's fields as C++ comes. Declaration order; a
// field added fails to compile here until it is bound, as it fails the
// count in app_config.cpp.

auto Fields(const AppConfig& c) {
    const auto& [hotkeyEditMode, hotkeyViewMode, hotkeyQuickCapture, hotkeySilentCapture, profileable, profiles,
                 strokeColorRGBA, strokeWidth, showDebugOverlay, showInputOptionsHud, showItemBorders,
                 showToastsWhileHidden, accentColorRGBA, uiScalePercent, itemBorderColorFrontRGBA,
                 itemBorderColorOtherRGBA, itemBorderColorPinnedRGBA, imageFilter,
                 raiseSelectedSnippet, screenshotTrigger, drawingTrigger, snippetBar, drawingBar, overviewShowsStrokes,
                 overviewShowsBitmaps, showCanvasBar, showEditModeBorder, editModeBorderColorRGBA,
                 editModeBorderWidthPx, editModeBorderOnlyWhenEmpty, purgeDeleted,
                 purgeDeletedAfterDays, confirmDelete, confirmDeleteForGood, screenshotDefaults, drawingDefaults,
                 drawingBackgroundColorRGBA, noteTextSizePx, noteTextColorRGBA, overlayDisplayId, overlayDisplayName,
                 tutorialProgress, tutorialCurrent, tutorialFolder] = c;
    return std::tie(hotkeyEditMode, hotkeyViewMode, hotkeyQuickCapture, hotkeySilentCapture, profileable, profiles,
                    strokeColorRGBA, strokeWidth, showDebugOverlay, showInputOptionsHud, showItemBorders,
                    showToastsWhileHidden, accentColorRGBA, uiScalePercent, itemBorderColorFrontRGBA,
                    itemBorderColorOtherRGBA, itemBorderColorPinnedRGBA, imageFilter,
                    raiseSelectedSnippet, screenshotTrigger, drawingTrigger, snippetBar, drawingBar,
                    overviewShowsStrokes, overviewShowsBitmaps, showCanvasBar, showEditModeBorder,
                    editModeBorderColorRGBA, editModeBorderWidthPx, editModeBorderOnlyWhenEmpty,
                    purgeDeleted, purgeDeletedAfterDays, confirmDelete, confirmDeleteForGood, screenshotDefaults,
                    drawingDefaults, drawingBackgroundColorRGBA, noteTextSizePx, noteTextColorRGBA, overlayDisplayId,
                    overlayDisplayName, tutorialProgress, tutorialCurrent, tutorialFolder);
}

auto Fields(const ProfileableSettings& s) {
    const auto& [dontStealFocus, takeFocusOverElevated, softwarePointer, rawMouseInput, dontForwardKeystrokes,
                 counterRawMouseInput, freezeScreen, counterThreshold, shortcuts] = s;
    return std::tie(dontStealFocus, takeFocusOverElevated, softwarePointer, rawMouseInput, dontForwardKeystrokes,
                    counterRawMouseInput, freezeScreen, counterThreshold, shortcuts);
}

auto Fields(const SnippetDefaults& d) {
    const auto& [keepAspect, foregroundOpacity, backgroundOpacity] = d;
    return std::tie(keepAspect, foregroundOpacity, backgroundOpacity);
}

// The positions, in declaration order, at which `a` and `b` are equal.
template <typename T>
std::vector<size_t> EqualFields(const T& a, const T& b) {
    const auto left = Fields(a);
    const auto right = Fields(b);
    std::vector<size_t> equal;
    [&]<size_t... I>(std::index_sequence<I...>) {
        ((std::get<I>(left) == std::get<I>(right) ? equal.push_back(I) : void()), ...);
    }(std::make_index_sequence<std::tuple_size_v<decltype(left)>>{});
    return equal;
}

// ===== The tests =====

// Whether a row's value is one the file leaves out: a text size not
// decided yet, which the file says by saying nothing.
template <typename Row>
bool Undecided(const Row& row, const AppConfig& config) {
    if constexpr (std::is_same_v<std::decay_t<decltype(row.rule)>, PositiveBandRule>) {
        return row.rule.zeroIsUndecided && ValueIn(row, config) <= 0.0f;
    } else {
        return false;
    }
}

TEST(SettingsCatalogTest, EveryRowIsWrittenWhereItSays) {
    const AppConfig defaults = DefaultConfig();
    const json doc = json::parse(SerializeConfig(defaults));
    ForEachSetting([&](const auto& row) {
        using Row = std::decay_t<decltype(row)>;
        if constexpr (std::is_same_v<Row, ShortcutSettings>) {
            for (const ShortcutAction action : kAllShortcutActions) {
                EXPECT_NE(At(doc, ShortcutPath(row, action)), nullptr) << Name(ShortcutPath(row, action));
            }
        } else if (Undecided(row, defaults)) {
            EXPECT_EQ(At(doc, row.path), nullptr) << Name(row.path);
        } else {
            EXPECT_NE(At(doc, row.path), nullptr) << Name(row.path);
        }
    });
}

// A changed value survives the file and changes its own setting and no
// other: a row that points at another row's field fails here.
TEST(SettingsCatalogTest, EveryRowReadsBackItsOwnFieldAndNoOther) {
    const AppConfig defaults = DefaultConfig();
    ForEachSetting([&](const auto& row) {
        using Row = std::decay_t<decltype(row)>;
        AppConfig config = defaults;
        Change(row, config);
        std::string name;
        if constexpr (std::is_same_v<Row, ShortcutSettings>) {
            name = row.group;
        } else {
            name = Name(row.path);
        }
        const AppConfig parsed = ParseConfig(SerializeConfig(config));
        EXPECT_EQ(parsed, config) << name << " did not survive the file";
        // Everything but this row is as it was: compared row by row, so the
        // row that moved is named.
        ForEachSetting([&](const auto& other) {
            using OtherRow = std::decay_t<decltype(other)>;
            if constexpr (std::is_same_v<OtherRow, Row>) {
                if (&other == &row) {
                    return;
                }
            }
            if constexpr (std::is_same_v<OtherRow, ShortcutSettings>) {
                EXPECT_EQ(parsed.profileable.shortcuts, defaults.profileable.shortcuts) << "changing " << name;
            } else {
                EXPECT_EQ(ValueIn(other, parsed), ValueIn(other, defaults))
                    << "changing " << name << " changed " << Name(other.path);
            }
        });
    });
}

// Every field of the stored settings is some row's: with every row changed,
// every field is. A field no row reads or writes stays as it was - the one
// thing the per-row tests cannot see.
TEST(SettingsCatalogTest, EveryFieldIsSomeRows) {
    const AppConfig defaults = DefaultConfig();
    const AppConfig everyRow = EveryRowChanged();
    // Position 5 is `profiles`, which is not a row: the profiles are read
    // and written as a list, their overrides by the rows (below).
    EXPECT_EQ(EqualFields(everyRow, defaults), std::vector<size_t>{5}) << "AppConfig fields, by position";
    EXPECT_EQ(EqualFields(everyRow.profileable, defaults.profileable), std::vector<size_t>{})
        << "ProfileableSettings fields, by position";
    EXPECT_EQ(EqualFields(everyRow.screenshotDefaults, defaults.screenshotDefaults), std::vector<size_t>{});
    EXPECT_EQ(EqualFields(everyRow.drawingDefaults, defaults.drawingDefaults), std::vector<size_t>{});
}

// The rules whose values are numbers, and so have a band to be outside.
template <typename Rule>
inline constexpr bool kNumberRule =
    std::is_same_v<Rule, IntRule> || std::is_same_v<Rule, AutoOrPercentRule> || std::is_same_v<Rule, FloatRule> ||
    std::is_same_v<Rule, PositiveFloatRule> || std::is_same_v<Rule, PositiveBandRule>;

// A number outside a row's rule, written into the file, reads as the
// rule's Hold says: held, or rejected and the setting left at its default.
template <typename Row>
void ExpectHeldAsTheRuleSays(const Row& row, const AppConfig& defaults) {
    using Rule = std::decay_t<decltype(row.rule)>;
    using Value = typename Rule::Value;
    std::vector<double> probes;
    if constexpr (std::is_same_v<Rule, IntRule>) {
        probes = {row.rule.min - 1.0, row.rule.max + 1.0, 0.0, 1e12};
    } else if constexpr (std::is_same_v<Rule, AutoOrPercentRule>) {
        // Not 0: the file spells "follow Windows" as "auto", and a 0 there
        // is a percentage below the band. It is 0 only as the setting holds
        // it, which is what an edit sets.
        probes = {row.rule.min - 1.0, row.rule.max + 1.0, 1e12};
    } else if constexpr (std::is_same_v<Rule, PositiveFloatRule>) {
        probes = {0.0, -1.0, row.rule.max * 2.0, 1e100};
    } else {
        probes = {row.rule.min - 1.0, row.rule.max + 1.0, 0.0, -1.0, 1e100};
    }
    for (const double probe : probes) {
        const AppConfig parsed = ParseConfig(FileWith(row.path, probe));
        // The file's number as the setting's type: rounded for a whole
        // number, and a float's infinity past its range.
        Value asValue{};
        if constexpr (std::is_same_v<Value, int>) {
            asValue = static_cast<int>(std::lround(std::clamp(probe, -1e9, 1e9)));
        } else {
            asValue = static_cast<float>(probe);
        }
        const std::optional<Value> held = Hold(row.rule, asValue);
        EXPECT_EQ(ValueIn(row, parsed), held.value_or(ValueIn(row, defaults))) << Name(row.path) << " = " << probe;
    }
}

// A value the rule does not allow is held or rejected by the parser exactly
// as the rule's Hold says - the one Hold an edit will be held by too.
TEST(SettingsCatalogTest, EveryNumberOutsideItsRuleIsHeldAsTheRuleSays) {
    const AppConfig defaults = DefaultConfig();
    ForEachSetting([&](const auto& row) {
        using Row = std::decay_t<decltype(row)>;
        if constexpr (!std::is_same_v<Row, ShortcutSettings>) {
            if constexpr (kNumberRule<std::decay_t<decltype(row.rule)>>) {
                ExpectHeldAsTheRuleSays(row, defaults);
            }
        }
    });
}

// A number past a float's range is not a very large number to hold to the
// top of the band but no number the setting can hold - for every float,
// where the border's width and the text size used to take their maximum.
TEST(SettingsCatalogTest, ANumberPastAFloatsRangeIsRejectedByEveryFloat) {
    const AppConfig defaults = DefaultConfig();
    EXPECT_EQ(ParseConfig(FileWith(setting::kEditModeBorderWidth.path, 1e100)).editModeBorderWidthPx,
              defaults.editModeBorderWidthPx);
    EXPECT_EQ(ParseConfig(FileWith(setting::kNoteTextSize.path, 1e100)).noteTextSizePx, defaults.noteTextSizePx);
}

// JSON that is not the kind of value a setting holds - an object, where no
// setting holds one - leaves the setting as it was.
TEST(SettingsCatalogTest, EveryRowKeepsItsDefaultForTheWrongKindOfValue) {
    const AppConfig defaults = DefaultConfig();
    ForEachSetting([&](const auto& row) {
        using Row = std::decay_t<decltype(row)>;
        if constexpr (std::is_same_v<Row, ShortcutSettings>) {
            for (const ShortcutAction action : kAllShortcutActions) {
                EXPECT_EQ(ParseConfig(FileWith(ShortcutPath(row, action), json::object())), defaults)
                    << Name(ShortcutPath(row, action));
            }
        } else {
            EXPECT_EQ(ParseConfig(FileWith(row.path, json::object())), defaults) << Name(row.path);
        }
    });
}

// A profile states a setting for itself or says nothing, and the file
// keeps which: each overridable row, stated alone, survives inside a
// profile, and every other stays unstated.
TEST(SettingsCatalogTest, EveryOverridableRowSurvivesInsideAProfile) {
    ForEachSetting([&](const auto& row) {
        using Row = std::decay_t<decltype(row)>;
        if constexpr (kIsProfileSetting<Row> || std::is_same_v<Row, ShortcutSettings>) {
            AppConfig config = DefaultConfig();
            AppConfig changed = config;
            Change(row, changed);
            Profile profile;
            profile.name = "Game";
            profile.match.executables = {"game.exe"};
            std::string name;
            if constexpr (std::is_same_v<Row, ShortcutSettings>) {
                for (const ShortcutAction action : kAllShortcutActions) {
                    profile.overrides.shortcuts[ShortcutActionIndex(action)] =
                        changed.profileable.shortcuts[ShortcutActionIndex(action)];
                }
                name = row.group;
            } else {
                profile.overrides.*row.override = changed.profileable.*row.value;
                name = Name(row.path);
            }
            config.profiles.push_back(profile);
            const AppConfig parsed = ParseConfig(SerializeConfig(config));
            ASSERT_EQ(parsed.profiles.size(), 1u);
            EXPECT_EQ(parsed.profiles[0].overrides, profile.overrides) << name;
            EXPECT_EQ(parsed.profileable, config.profileable) << name << " leaked into the defaults";
        }
    });
    // And a profile that states nothing reads back stating nothing.
    AppConfig config = DefaultConfig();
    config.profiles.push_back(Profile{"Empty", {{"x.exe"}, {}}, {}});
    EXPECT_TRUE(ParseConfig(SerializeConfig(config)).profiles.at(0).overrides.Empty());
}

}  // namespace
}  // namespace sz::core
