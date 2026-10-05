#pragma once

// The Settings panel's rows, each bound to its setting's row in the catalog
// (docs/SETTINGS.md, section 9). A widget reads the value for what the
// panel shows, writes it as an edit (Settings::Set, or Preview while it is
// dragged and CommitPreviews as it lets go), and takes its band from the
// row's rule rather than restating it. On a Profile row it also shows
// whether the target states the setting for itself, with the arrow that
// hands it back to the defaults. The layout - headings, the order, the
// input options' tree - stays the panel's, written out by hand: its rows
// are as many different explanations, not a list.
//
// Only the Settings page draws these.

#include <cstddef>
#include <optional>

#include <imgui.h>

#include "core/config/setting.h"
#include "core/session/settings.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace sz::ui {

// The settings are the core's, spoken of in its own vocabulary, as the rest
// of the view does (see overlay_app.h).
using namespace ::sz::core;

// The "?" that carries a row's explanation - see the definition.
void HelpMarker(const char* id, const char* title, const char* text);
// The revert arrow that marks a row as set here rather than inherited, and
// is the button that undoes it.
bool RevertButton(const char* strId);

// A Global switch: a checkbox, and its "?" when there is `help`.
void SettingCheckbox(Settings& settings, const GlobalSetting<BoolRule>& row, const char* id, const char* label,
                     const char* help);
// A Profile switch, for `target` - the defaults for nullopt. `disabled`
// grays it without touching what is stored, for a row whose preconditions
// are not met; switched on, such a row draws a dash rather than a check
// mark, since its stored value is not in effect.
void SettingCheckbox(Settings& settings, std::optional<size_t> target, const ProfileSetting<BoolRule>& row,
                     const char* id, const char* label, const char* help, bool disabled = false);

// A Global whole number as a step field and nothing else - `id` is the
// field's own - for a row that lays out its words itself.
void SettingNumber(Settings& settings, const GlobalSetting<IntRule>& row, const char* id, int step, int fastStep);
// A Profile whole number: the label, a step field, `unit` after it.
void SettingNumber(Settings& settings, std::optional<size_t> target, const ProfileSetting<IntRule>& row,
                   const char* id, const char* label, const char* unit, int step, const char* help,
                   bool disabled = false);

// A number in its band as a slider in percent, previewed while dragged.
void SettingPercent(Settings& settings, const GlobalSetting<FloatRule>& row, const char* id, const char* label);
// A size in pixels as a slider, previewed while dragged.
void SettingPixels(Settings& settings, const GlobalSetting<PositiveBandRule>& row, const char* id,
                   const char* label);

// A color as a swatch, with its caption beside it, previewed while the
// picker is dragged. `id` is the swatch's own ("##accentcolor"): ColorEdit
// pushes its label as an ID scope, so words in it would move every id
// under it whenever they changed.
enum class SwatchAlpha {
    None,  // opaque, the alpha byte left 0xFF
    Bar,   // the alpha is part of the color, shown over a checkerboard
};
void SettingColor(Settings& settings, const GlobalSetting<ColorRule>& row, const char* id, const char* caption,
                  SwatchAlpha alpha);

// One choice of a choice row, as the panel names it.
template <typename E>
struct ChoiceLabel {
    E value;
    const char* label;
    const char* id;
};

// A choice as a dropdown, its label in a column of `labelColumn` - or
// none, for a `label` of nullptr: a row whose heading says what it is.
template <typename E, size_t N>
void SettingCombo(Settings& settings, const GlobalSetting<ChoiceRule<E>>& row, const char* id, const char* label,
                  const ChoiceLabel<E> (&choices)[N], float labelColumn, float width) {
    const E current = settings.Get(row);
    const char* preview = choices[0].label;
    for (const ChoiceLabel<E>& choice : choices) {
        if (choice.value == current) {
            preview = choice.label;
        }
    }
    if (label != nullptr) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(Px(labelColumn));
    }
    ImGui::SetNextItemWidth(Px(width));
    if (ImGui::BeginCombo(Labeled("", id), preview)) {
        for (const ChoiceLabel<E>& choice : choices) {
            if (ImGui::Selectable(Labeled(choice.label, choice.id), choice.value == current) &&
                choice.value != current) {
                settings.Set(row, choice.value);
            }
        }
        ImGui::EndCombo();
    }
}

}  // namespace sz::ui
