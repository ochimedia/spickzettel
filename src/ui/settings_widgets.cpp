#include "ui/settings_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "ui/icons_generated.h"
#include "generated/ui_strings.h"

// For ImGui::GetCurrentWindow, which is how a marker asks how tall the
// row it is joining already is - see CenterOnRow.
#include <imgui_internal.h>

namespace sz::ui {

namespace {

// Moves the cursor down so that an item `size` tall, next, sits in the
// middle of the row it joins, rather than on its top edge - this is always
// called after a SameLine, so the cursor is at the top of a line something
// else set the height of. Which one it is matters: a checkbox makes the
// row a frame tall, a plain heading only a line of text tall, and
// centering on the frame either way dropped every marker beside a heading
// visibly below its own words. DC.CurrLineSize.y is the tallest thing on
// the row so far, which is exactly the question; it is zero on a row with
// nothing on it yet, and then there is nothing to line up with.
//
// Never negative: an item taller than the row sits on its top edge and
// overhangs below rather than being centered. Overhanging downward is
// free; upward is not - the first row of a settings tab starts at the top
// of a scrolling child, and a marker reaching a pixel above that is a
// pixel outside the clip rect, which shaved the top off the "?" circles
// in Input, Hotkeys and Profiles.
void CenterOnRow(float size) {
    const float rowHeight = ImGui::GetCurrentWindow()->DC.CurrLineSize.y;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (rowHeight - size) * 0.5f));
}

}  // namespace

// The revert arrow that marks a settings row as set here rather than
// inherited. Small and quiet enough to sit inside a checkbox row without
// making it taller, and accent-colored because being marked is the point:
// it is both the indicator and the button that undoes it. Centered on its
// row, as the "?" before it is: on the row's top edge, it sat half a frame
// above the words and the "?".
bool RevertButton(const char* strId) {
    constexpr float kSize = 16.0f;
    CenterOnRow(Px(kSize));
    ImGui::InvisibleButton(strId, ImVec2(Px(kSize), Px(kSize)));
    const bool pressed = ImGui::IsItemClicked();
    const ImVec2 pMin = ImGui::GetItemRectMin();
    const ImU32 color = ImGui::GetColorU32(ImGui::IsItemHovered() ? theme::AccentHover() : theme::Accent());
    DrawIcon(ImGui::GetWindowDrawList(), icons::kUndo, pMin, Px(kSize), color);
    return pressed;
}

// Laid out as text is, so it goes on any line a word would - beside a tree
// node's label as well as beside a frame - and its box reaches a little
// above and below the words without making the line taller. The padding
// on each side is its own, not taken from the item spacing, so two tags in
// a row are as far apart as a tag and a word.
bool Tag(const char* text, const ImVec4& ink, const ImVec4& fill) {
    const float padX = Px(6.0f);
    const float padY = Px(1.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 at(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 boxMin(at.x - padX, at.y - padY);
    const ImVec2 boxMax(at.x + size.x + padX, at.y + size.y + padY);
    ImGui::GetWindowDrawList()->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(fill), (boxMax.y - boxMin.y) * 0.5f);
    ImGui::TextColored(ink, "%s", text);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(padX, 0.0f));
    return hovered;
}

// The "?" that carries a setting's explanation, so the panel can read as a
// list of settings rather than as an essay with checkboxes in it. Clicking
// opens the text in a popover beside the row; until then it takes one
// glyph of space and says nothing.
//
// A click rather than a hover, because these are paragraphs: text that
// appears because the pointer crossed it is text you can't read while
// reaching for the box it describes, and it covers the rows below exactly
// when you are trying to compare them.
void HelpMarker(const char* id, const char* title, const char* text) {
    // Wide enough for a paragraph to have a shape, narrow enough not to
    // cover the panel it is explaining.
    constexpr float kHelpWrapWidth = 380.0f;

    // The ids come from `id`, never from the words: two of these in one
    // window must not collide, a test needs a stable name to reach for,
    // and neither may change because someone reworded the explanation.
    char buttonId[192];
    char popupId[192];
    std::snprintf(buttonId, sizeof(buttonId), "##help_%s", id);
    std::snprintf(popupId, sizeof(popupId), "##helppop_%s", id);

    // Two pixels taller than a line of text, so that beside a heading it
    // sits on the row's top edge and overhangs below - see CenterOnRow.
    const float size = std::floor(ImGui::GetFontSize() + Px(2.0f));
    CenterOnRow(size);
    ImGui::InvisibleButton(buttonId, ImVec2(size, size));
    const bool clicked = ImGui::IsItemClicked();
    // Lit while its own popover is up, so a reader can see which row the
    // text on screen belongs to.
    const bool open = ImGui::IsPopupOpen(popupId);
    const ImU32 color =
        ImGui::GetColorU32(open || ImGui::IsItemHovered() ? theme::Accent() : theme::kGraphite300);
    const ImVec2 minPt = ImGui::GetItemRectMin();
    const ImVec2 center(minPt.x + size * 0.5f, minPt.y + size * 0.5f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddCircle(center, size * 0.5f, color, 0, Px(1.2f));
    // Centered on the glyph's own ink, not on the line box CalcTextSize
    // reports. A line box is the same height for every character in the
    // font - it has to leave room for accents above and descenders below -
    // and "?" uses neither, so centering the box left the question mark
    // sitting low enough in its circle for the dot to touch the ring.
    // ImFontGlyph's X0/Y0/X1/Y1 are the ink's own corners, in pixels,
    // relative to where AddText would put the glyph; putting the middle of
    // that where the middle of the circle is takes both axes at once.
    ImFontGlyph* glyph = ImGui::GetFontBaked()->FindGlyph(static_cast<ImWchar>('?'));
    drawList->AddText(ImVec2(center.x - (glyph->X0 + glyph->X1) * 0.5f,
                              center.y - (glyph->Y0 + glyph->Y1) * 0.5f),
                       color, "?");

    if (clicked) {
        ImGui::OpenPopup(popupId);
    }
    if (ImGui::BeginPopup(popupId)) {
        ImGui::PushTextWrapPos(Px(kHelpWrapWidth));
        // The title repeated inside, because a popover can land over the
        // row that opened it.
        ImGui::TextColored(theme::Accent(), "%s", title);
        ImGui::Spacing();
        // The names of other rows and buttons in the accent, as the title
        // names this one: "this is a name", without quotes.
        WrappedSpans(ImGui::GetStyleColorVec4(ImGuiCol_Text), theme::Accent(), MarkedSpans(text));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void SettingCheckbox(Settings& settings, const GlobalSetting<BoolRule>& row, const char* id, const char* label,
                     const char* help) {
    bool value = settings.Get(row);
    if (ImGui::Checkbox(Labeled(label, id), &value)) {
        settings.Set(row, value);
    }
    if (help != nullptr) {
        ImGui::SameLine();
        HelpMarker(id, label, help);
    }
}

// The revert arrow, shown only on a row this target states for itself -
// borrowed wholesale from property editors that do the same (the row is
// marked, and the mark is the button that undoes it).
void SettingCheckbox(Settings& settings, std::optional<size_t> target, const ProfileSetting<BoolRule>& row,
                     const char* id, const char* label, const char* help, bool disabled) {
    const bool overridden = settings.IsOverridden(row, target);
    const bool value = settings.Get(row, target);

    ImGui::PushID(id);
    ImGui::BeginDisabled(disabled);
    if (overridden) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    }
    // A switched-on row whose preconditions aren't met draws a dash rather
    // than a check mark. It keeps its stored value - that is the point of not
    // clearing it - but a check mark would claim the option is doing something,
    // and "the stored value of an option that cannot take effect is not
    // evidence of anything" (see EditModeInputOptions).
    //
    // Drawn here rather than through ImGuiItemFlags_MixedValue, which
    // renders its third state as a filled inner rect - and with this
    // theme's frame rounding that comes out as a blob that reads as a
    // heavier check mark rather than as a lesser one.
    const bool storedButNotInEffect = disabled && value;
    bool shown = value && !storedButNotInEffect;
    if (ImGui::Checkbox(Labeled(label, id), &shown)) {
        settings.Set(row, shown, target);
    }
    if (storedButNotInEffect) {
        const ImVec2 boxMin = ImGui::GetItemRectMin();
        const float box = ImGui::GetFrameHeight();
        const float inset = std::floor(box * 0.3f);
        const float centerY = boxMin.y + box * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(boxMin.x + inset, centerY),
                                             ImVec2(boxMin.x + box - inset, centerY),
                                             ImGui::GetColorU32(ImGuiCol_CheckMark), PxWhole(2.0f));
    }
    if (overridden) {
        ImGui::PopStyleColor();
    }
    ImGui::EndDisabled();

    // Outside the disabled scope, and before the revert arrow so it keeps
    // the same place whether or not the row is marked: a row you can't
    // switch on yet is exactly one whose explanation you want to read.
    if (help != nullptr) {
        ImGui::SameLine();
        HelpMarker(id, label, help);
    }

    if (overridden) {
        ImGui::SameLine();
        // Not disabled with the row: a setting whose precondition has
        // since been switched off is exactly one you want to be able to
        // hand back to the defaults.
        if (RevertButton("##revert")) {
            settings.ClearOverride(row, target);
        }
        if (ImGui::IsItemHovered()) {
            InfoTooltip(strings::kHotkeysComboSetHere,
                               (settings.Base().*row.value) ? strings::kHotkeysOn : strings::kHotkeysOff);
        }
    }
    ImGui::PopID();
}

void SettingNumber(Settings& settings, const GlobalSetting<IntRule>& row, const char* id, int step, int fastStep) {
    int value = settings.Get(row);
    if (ImGui::InputInt(id, &value, step, fastStep)) {
        settings.Set(row, value);
    }
}

void SettingNumber(Settings& settings, std::optional<size_t> target, const ProfileSetting<IntRule>& row,
                   const char* id, const char* label, const char* unit, int step, const char* help, bool disabled) {
    const bool overridden = settings.IsOverridden(row, target);
    int value = settings.Get(row, target);

    ImGui::PushID(id);
    ImGui::BeginDisabled(disabled);
    ImGui::AlignTextToFramePadding();
    if (overridden) {
        ImGui::TextColored(theme::Accent(), "%s", label);
    } else {
        ImGui::TextUnformatted(label);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(110.0f));
    // Written on every edit, as the checkbox is on every click: a profile
    // that has been typed into states the value for itself.
    if (ImGui::InputInt("##value", &value, step, step * 5)) {
        settings.Set(row, value, target);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(unit);
    ImGui::EndDisabled();

    if (help != nullptr) {
        ImGui::SameLine();
        HelpMarker(id, label, help);
    }

    if (overridden) {
        ImGui::SameLine();
        if (RevertButton("##revert")) {
            settings.ClearOverride(row, target);
        }
        if (ImGui::IsItemHovered()) {
            InfoTooltip(strings::kHotkeysComboSetHere, std::to_string(settings.Base().*row.value).c_str());
        }
    }
    ImGui::PopID();
}

// The sliders and swatches below write a preview on every frame the value
// moves, which everything drawn shows at once, and commit - write the file
// - only when the drag lets go, not once a frame.

void SettingPercent(Settings& settings, const GlobalSetting<FloatRule>& row, const char* id, const char* label) {
    int percent = static_cast<int>(std::round(settings.Get(row) * 100.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    if (ImGui::SliderInt(Labeled(label, id), &percent, static_cast<int>(std::round(row.rule.min * 100.0f)),
                         static_cast<int>(std::round(row.rule.max * 100.0f)), strings::kFormatPercent,
                         ImGuiSliderFlags_AlwaysClamp)) {
        settings.Preview(row, static_cast<float>(percent) / 100.0f);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.CommitPreviews();
    }
}

void SettingPixels(Settings& settings, const GlobalSetting<PositiveBandRule>& row, const char* id,
                   const char* label) {
    float value = settings.Get(row);
    ImGui::SetNextItemWidth(Px(160.0f));
    if (ImGui::SliderFloat(Labeled(label, id), &value, row.rule.min, row.rule.max, strings::kFormatPixels,
                           ImGuiSliderFlags_AlwaysClamp)) {
        settings.Preview(row, value);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.CommitPreviews();
    }
}

void SettingColor(Settings& settings, const GlobalSetting<ColorRule>& row, const char* id, const char* caption,
                  SwatchAlpha alpha) {
    uint32_t value = settings.Get(row);
    bool edited = false;
    if (alpha == SwatchAlpha::None) {
        float rgb[3];
        ColorRGBAToFloats(value, rgb);
        edited = ImGui::ColorEdit3(id, rgb, ImGuiColorEditFlags_NoInputs);
        if (edited) {
            value = FloatsToColorRGBA(rgb, static_cast<uint8_t>(0xFF));
        }
    } else {
        float rgba[4];
        ColorRGBAToFloats4(value, rgba);
        edited = ImGui::ColorEdit4(id, rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        if (edited) {
            value = FloatsToColorRGBA4(rgba);
        }
    }
    if (edited) {
        settings.Preview(row, value);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.CommitPreviews();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(caption);
}

}  // namespace sz::ui
