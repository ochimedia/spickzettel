#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "core/config/display_choice.h"
#include "ui/icons_generated.h"

#include <imgui.h>
// For ImGui::GetCurrentWindow, which is how HelpMarker asks how tall the
// row it is joining already is - see its own comment.
#include <imgui_internal.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Overview: canvas switcher / manager / move-copy picker =================

namespace overlay_detail {

// An icon+text button in the given colors - the .btn equivalent (icon
// and text sizes/gap match .btn svg / .btn's own gap). The two colorings
// below are the only ones in use.
bool IconTextButton(const char* strId, const Icon& icon, const char* text, const ImVec4& fill,
                    const ImVec4& hover, const ImVec4& ink) {
    constexpr float kIconSize = 15.0f;
    constexpr float kGap = 7.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 size(style.FramePadding.x * 2.0f + Px(kIconSize) + Px(kGap) + textSize.x,
                       style.FramePadding.y * 2.0f + std::max(Px(kIconSize), textSize.y));
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hover);
    const bool pressed = ImGui::Button(strId, size);
    ImGui::PopStyleColor(3);
    const ImVec2 minPt = ImGui::GetItemRectMin();
    const ImVec2 maxPt = ImGui::GetItemRectMax();
    const float contentH = maxPt.y - minPt.y;
    const ImU32 inkColor = ImGui::GetColorU32(ink);
    const ImVec2 iconPos(minPt.x + style.FramePadding.x, minPt.y + (contentH - Px(kIconSize)) * 0.5f);
    DrawIcon(ImGui::GetWindowDrawList(), icon, iconPos, Px(kIconSize), inkColor);
    const ImVec2 textPos(iconPos.x + Px(kIconSize) + Px(kGap), minPt.y + (contentH - textSize.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(textPos, inkColor, text);
    return pressed;
}

// Always accent-colored: the primary action of a panel (New canvas, New
// folder, Restore).
bool PrimaryButton(const char* strId, const Icon& icon, const char* text) {
    return IconTextButton(strId, icon, text, theme::Accent(), theme::AccentHover(), theme::AccentInk());
}

}  // namespace overlay_detail

namespace {

// The same in DangerIconButton's red - for a destructive action that
// deserves visible text rather than a bare icon (the confirm-delete
// popup's own "Delete" button - see RenderConfirmDeletePopover).
bool DangerButton(const char* strId, const Icon& icon, const char* text) {
    return IconTextButton(strId, icon, text, theme::kDangerSoft, theme::kDanger, theme::kWhite);
}

// A plain text tab, active tab in accent, inactive tabs a quiet neutral -
// the Overview's own Canvases/Settings switcher (see RenderOverview).
// `text` doubles as both the visible label and the ImGui ID (safe here -
// the two tab labels are the only buttons with that exact text anywhere
// in the Overview's ID scope), so ordinary ImGui::Button already centers
// it correctly with no extra layout math needed, unlike PrimaryButton/
// DangerButton's bespoke icon+text placement above.
bool TabButton(const char* id, const char* text, bool active) {
    const char* label = Labeled(text, id);
    const ImVec2 size(Px(84.0f), 0.0f);
    active = active || PressLandsThisFrame(label, size);
    ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::Accent() : theme::kFieldBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? theme::AccentHover() : theme::kHoverWash);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::AccentHover());
    ImGui::PushStyleColor(ImGuiCol_Text, active ? theme::AccentInk() : theme::kGraphite200);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(theme::kRadiusSm));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return pressed;
}

// The revert arrow that marks a settings row as set here rather than
// inherited. Small and quiet enough to sit inside a checkbox row without
// making it taller, and accent-colored because being marked is the point:
// it is both the indicator and the button that undoes it.
bool RevertButton(const char* strId) {
    constexpr float kSize = 16.0f;
    ImGui::InvisibleButton(strId, ImVec2(Px(kSize), Px(kSize)));
    const bool pressed = ImGui::IsItemClicked();
    const ImVec2 pMin = ImGui::GetItemRectMin();
    const ImU32 color = ImGui::GetColorU32(ImGui::IsItemHovered() ? theme::AccentHover() : theme::Accent());
    DrawIcon(ImGui::GetWindowDrawList(), icons::kUndo, pMin, Px(kSize), color);
    return pressed;
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

    const float size = std::floor(ImGui::GetFontSize() + Px(2.0f));
    // Centered on whatever else is already on this row, rather than on its
    // top edge - this is always called after a SameLine, so the cursor is
    // at the top of a line something else set the height of. Which one it
    // is matters: a checkbox makes the row a frame tall, a plain heading
    // only a line of text tall, and centering on the frame either way
    // dropped every marker beside a heading visibly below its own words.
    // DC.CurrLineSize.y is the tallest thing on the row so far, which is
    // exactly the question; it is zero on a row with nothing on it yet,
    // and then there is nothing to line up with.
    //
    // Never negative, and the marker is deliberately two pixels taller
    // than a line of text, so beside a heading it sits on the row's top
    // edge and overhangs below rather than being centered. Overhanging
    // downward is free; upward is not - the first row of a settings tab
    // starts at the top of a scrolling child, and a marker reaching a
    // pixel above that is a pixel outside the clip rect, which shaved the
    // top off the circles in Input, Hotkeys and Profiles.
    const float rowHeight = ImGui::GetCurrentWindow()->DC.CurrLineSize.y;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (rowHeight - size) * 0.5f));
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
        // Same reason as every other popup nested in this panel: the
        // Overview re-asserts itself to the front every frame, so anything
        // opened inside it has to as well or it opens behind. See
        // KeepPopoverInFront.
        KeepPopoverInFront();
        ImGui::PushTextWrapPos(Px(kHelpWrapWidth));
        // The title repeated inside, because a popover can land over the
        // row that opened it.
        ImGui::TextColored(theme::Accent(), "%s", title);
        ImGui::Spacing();
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

// A checkbox and its "?", for the settings that are not per-application
// (OverlayApp::ProfileableCheckbox is the same row for the ones that are).
bool CheckboxWithHelp(const char* id, const char* label, bool* value, const char* help) {
    const bool changed = ImGui::Checkbox(Labeled(label, id), value);
    ImGui::SameLine();
    HelpMarker(id, label, help);
    return changed;
}

// The label over a group of settings, with the explanation that covers the
// group as a whole behind its own "?".
void SettingsHeading(const char* id, const char* title, const char* help = nullptr) {
    ImGui::TextColored(theme::kGraphite200, "%s", title);
    if (help != nullptr) {
        ImGui::SameLine();
        HelpMarker(id, title, help);
    }
}

// What separates one group of settings from the next. One call rather than
// the three it takes, because they were being written out by hand and had
// started to differ.
void SettingsGroupBreak() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

// The two halves of a section that has both - Behavior, Hotkeys - each in
// a box of its own with a badge saying which half it is. A heading alone
// read as one more group of the list; the box says where the half ends,
// and the per-profile one carries the accent down its edge, the color the
// picker's "running now" is in, so the part a profile can change is the
// part that looks different.
//
// Drawn behind its contents after they are laid out - its height is not
// known before - on a draw list split in two, contents on top. The
// contents sit in a group so that the rows' own SameLine offsets are
// measured from the box's inside rather than from the window's edge.
enum class SettingsScope { Global, Profile };
struct SettingsScopeBox {
    ImDrawListSplitter splitter;
    ImVec2 start;
    float width = 0.0f;
    SettingsScope scope = SettingsScope::Global;
};
constexpr float kScopeBoxPadding = 12.0f;

void BeginSettingsScope(SettingsScopeBox& box, SettingsScope scope) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    box.scope = scope;
    box.start = ImGui::GetCursorScreenPos();
    box.width = ImGui::GetContentRegionAvail().x;
    box.splitter.Split(drawList, 2);
    box.splitter.SetCurrentChannel(drawList, 1);
    // A group's separators run to the window's edge; this stops them, and
    // anything else, the box's padding short of its border.
    ImGui::PushClipRect(box.start, ImVec2(box.start.x + box.width - Px(kScopeBoxPadding), FLT_MAX), true);

    ImGui::Dummy(ImVec2(0.0f, Px(kScopeBoxPadding) - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Indent(Px(kScopeBoxPadding));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + box.width - 2.0f * Px(kScopeBoxPadding));

    // The badge: a pill with the half's name, and a line of what it means.
    const bool global = scope == SettingsScope::Global;
    const char* name = global ? strings::kSettingsScopeGlobal : strings::kSettingsScopeProfile;
    const ImVec2 textSize = ImGui::CalcTextSize(name);
    const ImVec2 pad = Px(8.0f, 2.0f);
    const ImVec2 pillMin = ImGui::GetCursorScreenPos();
    const ImVec2 pillMax(pillMin.x + textSize.x + 2.0f * pad.x, pillMin.y + textSize.y + 2.0f * pad.y);
    drawList->AddRectFilled(pillMin, pillMax,
                            global ? ImGui::GetColorU32(theme::kGraphite600) : theme::AccentU32(),
                            (pillMax.y - pillMin.y) * 0.5f);
    drawList->AddText(ImVec2(pillMin.x + pad.x, pillMin.y + pad.y),
                      ImGui::GetColorU32(global ? theme::kGraphite100 : theme::AccentInk()), name);
    ImGui::Dummy(ImVec2(pillMax.x - pillMin.x, pillMax.y - pillMin.y));
    ImGui::SameLine(0.0f, Px(10.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad.y);
    ImGui::TextColored(theme::kGraphite300, "%s",
                       global ? strings::kSettingsScopeGlobalNote : strings::kSettingsScopeProfileNote);
    ImGui::Spacing();
    ImGui::Spacing();
}

void EndSettingsScope(SettingsScopeBox& box) {
    ImGui::PopClipRect();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::Unindent(Px(kScopeBoxPadding));
    ImGui::Dummy(ImVec2(0.0f, Px(kScopeBoxPadding) - ImGui::GetStyle().ItemSpacing.y));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    box.splitter.SetCurrentChannel(drawList, 0);
    const ImVec2 max(box.start.x + box.width, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y);
    constexpr float kRounding = 8.0f;
    drawList->AddRectFilled(box.start, max, ImGui::GetColorU32(theme::kFieldBg), Px(kRounding));
    drawList->AddRect(box.start, max, ImGui::GetColorU32(theme::kPanelBorderStrong), Px(kRounding));
    if (box.scope == SettingsScope::Profile) {
        drawList->AddRectFilled(box.start, ImVec2(box.start.x + Px(4.0f), max.y), theme::AccentU32(), Px(kRounding),
                                ImDrawFlags_RoundCornersLeft);
    }
    box.splitter.Merge(drawList);
    ImGui::Spacing();
    ImGui::Spacing();
}

// One row of what a left press on empty canvas makes: the kind, and which
// press makes it. Choosing the press the other kind has swaps the two
// rather than refusing - one press cannot make both, and a dropdown that
// grays out the very choice wanted, with the reason in another row, is a
// puzzle. See AppConfig::screenshotTrigger.
bool CreationTriggerRow(const char* id, const char* label, CreationTrigger& trigger, CreationTrigger& other) {
    struct Choice {
        CreationTrigger trigger;
        const char* label;
    };
    const Choice choices[] = {
        {CreationTrigger::Plain, strings::kCreationTriggerPlain},
        {CreationTrigger::Ctrl, strings::kCreationTriggerCtrl},
        {CreationTrigger::Alt, strings::kCreationTriggerAlt},
        {CreationTrigger::Off, strings::kCreationTriggerOff},
    };
    const char* preview = strings::kCreationTriggerPlain;
    for (const Choice& choice : choices) {
        if (choice.trigger == trigger) {
            preview = choice.label;
        }
    }
    constexpr float kLabelColumn = 110.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(Px(kLabelColumn));
    ImGui::SetNextItemWidth(Px(220.0f));
    bool changed = false;
    if (ImGui::BeginCombo(Labeled("", id), preview)) {
        // Same reason as every other popup in this panel - see
        // KeepPopoverInFront.
        KeepPopoverInFront();
        for (const Choice& choice : choices) {
            if (ImGui::Selectable(choice.label, choice.trigger == trigger) && choice.trigger != trigger) {
                if (choice.trigger == other && other != CreationTrigger::Off) {
                    other = trigger;
                }
                trigger = choice.trigger;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// One branch of the dependency tree the input options are laid out as:
// the piece of trunk from whatever came before down to this row's middle,
// plus the stub across to its checkbox.
//
// Each row draws its own piece and then leaves `trunkY` at its own middle,
// so consecutive rows at the same depth join into one continuous line
// without anyone having to know how many siblings there are. `rowPos` is
// the cursor position from *before* the row was drawn, since by the time
// it is drawn the last item is the row's "?" rather than its checkbox.
void TreeBranch(float& trunkY, const ImVec2& rowPos, float indent) {
    const float centerY = rowPos.y + ImGui::GetFrameHeight() * 0.5f;
    // Half an indent back from the checkbox, which puts the trunk under
    // the middle of the parent's own box rather than under its label.
    const float x = rowPos.x - indent * 0.5f;
    const ImU32 color = ImGui::GetColorU32(theme::kGraphite400);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddLine(ImVec2(x, trunkY), ImVec2(x, centerY), color, 1.0f);
    drawList->AddLine(ImVec2(x, centerY), ImVec2(rowPos.x - Px(3.0f), centerY), color, 1.0f);
    trunkY = centerY;
}

// Where the branches of the rows nested under this one start from: the
// bottom edge of its checkbox, so the line comes down out of the box
// rather than through it. (Through it is what starting at the middle did,
// since the trunk of the next level down runs at half an indent - which
// lands inside the parent's own box, and drew a bar over the check mark or dash
// in it.)
float TrunkFrom(const ImVec2& rowPos) { return rowPos.y + ImGui::GetFrameHeight(); }

// One row of the Settings tab's own section list. Same colors as
// TabButton, laid out down the left edge instead of across the top: full
// width of its column, text left-aligned, so a list of five reads as a list
// rather than as five buttons that happen to be stacked. The folder
// sidebar next door has the same shape for the same reason.
bool SettingsSectionButton(const char* id, const char* text, bool active) {
    const char* label = Labeled(text, id);
    const ImVec2 size(-1.0f, Px(32.0f));
    active = active || PressLandsThisFrame(label, size);
    ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::Accent() : theme::kFieldBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? theme::AccentHover() : theme::kHoverWash);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::AccentHover());
    ImGui::PushStyleColor(ImGuiCol_Text, active ? theme::AccentInk() : theme::kGraphite200);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(theme::kRadiusSm));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::Spacing();
    return pressed;
}

}  // namespace

namespace overlay_detail {

// Renders a scaled-down snapshot of `canvas`'s items into `thumbMin..thumbMax`
// - uniform scale (never stretched), letterboxed/centered, computed from how
// the real overlay's own dimensions compare to the thumbnail box.
//
// `meshCache` must be a different one from the canvas's own: a tile draws
// the same items the canvas behind it does, at a wholly different scale, in
// the same frame - see OverlayApp::previewMeshCache_.
void DrawCanvasPreview(ImDrawList* drawList, const Canvas& canvas, ImVec2 thumbMin, ImVec2 thumbMax, float displayW,
                        float displayH, StrokeRenderMode rendering, bool showStrokes,
                        const std::function<std::optional<uint64_t>(const Item&)>& previewTexture,
                        StrokeMeshSlot meshCache, ImageSampling sampling) {
    drawList->PushClipRect(thumbMin, thumbMax, true);
    drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(14, 16, 20, 255));

    if (displayW > 0.0f && displayH > 0.0f) {
        const float scale = std::min((thumbMax.x - thumbMin.x) / displayW, (thumbMax.y - thumbMin.y) / displayH);
        const float offsetX = thumbMin.x + ((thumbMax.x - thumbMin.x) - displayW * scale) * 0.5f;
        const float offsetY = thumbMin.y + ((thumbMax.y - thumbMin.y) - displayH * scale) * 0.5f;

        for (const Item& item : canvas.items) {
            // Deleted on its own: not part of what the canvas shows, and not
            // what restoring a deleted canvas brings back either.
            if (item.deletedAt != 0) {
                continue;
            }
            const ImVec2 pMin(offsetX + item.rect.x * scale, offsetY + item.rect.y * scale);
            const ImVec2 pMax(offsetX + (item.rect.x + item.rect.w) * scale, offsetY + (item.rect.y + item.rect.h) * scale);
            DrawItemPreview(drawList, item, pMin, pMax, rendering, showStrokes, previewTexture, meshCache, sampling);
        }
    }

    drawList->PopClipRect();
}

void DrawItemPreview(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, StrokeRenderMode rendering,
                     bool showStrokes,
                     const std::function<std::optional<uint64_t>(const Item&)>& previewTexture,
                     StrokeMeshSlot meshCache, ImageSampling sampling) {
    // The picture with whichever texture it can have here: the real one for
    // the current canvas (already loaded), a thumbnail-sized copy for the
    // rest if previews are on, and none at all otherwise - in which case
    // DrawSnippetPicture falls back to the same placeholder gradient or
    // plain fill it uses anywhere else.
    bool drewAnything = false;
    if (item.picture.opacity > 0.0f) {
        // Nothing at all for a picture whose pixels are still being read
        // (see PicturePreviewTexture, which says so by returning nothing
        // rather than 0). The placeholder gradient means "there is no image
        // here", and a few frames of it in front of an image that *is*
        // there and is on its way reads as the thumbnails being wrong and
        // then correcting themselves. An outlined empty box - what the
        // fall-through below draws - says the same thing quietly.
        const std::optional<uint64_t> texture =
            previewTexture ? previewTexture(item) : std::optional<uint64_t>(0);
        if (texture.has_value()) {
            DrawSnippetPicture(drawList, item.picture, pMin, pMax, *texture, sampling);
            drewAnything = true;
        }
    }
    if (!drewAnything) {
        drawList->AddRect(pMin, pMax, IM_COL32(90, 96, 110, 180));
    }
    if (!showStrokes) {
        return;
    }

    // Native -> preview is one scale factor per axis: the box over the
    // item's native size. pMin is already the item's origin in the preview,
    // so it doubles as DrawStroke's offset.
    const float boxW = pMax.x - pMin.x;
    const float boxH = pMax.y - pMin.y;
    const float strokeScaleX = item.nativeW != 0.0f ? boxW / item.nativeW : (item.rect.w != 0.0f ? boxW / item.rect.w : 1.0f);
    const float strokeScaleY = item.nativeH != 0.0f ? boxH / item.nativeH : (item.rect.h != 0.0f ? boxH / item.rect.h : 1.0f);
    // Rasterized has no bitmap to draw here - a preview keeps no cache of its
    // own, and building one for a thumbnail would cost more than the
    // difference could possibly show at this size.
    const StrokeRenderMode previewMode =
        rendering == StrokeRenderMode::Rasterized ? StrokeRenderMode::Tessellated : rendering;
    for (size_t index = 0; index < item.strokes.size(); ++index) {
        DrawStroke(drawList, item.strokes[index], previewMode, pMin.x, pMin.y, strokeScaleX, strokeScaleY,
                   item.foregroundOpacity, meshCache.For(item.id, index));
    }
}

}  // namespace overlay_detail

void OverlayApp::RenderOverview(float displayW, float displayH) {
    if (!overviewOpen_) {
        return;
    }
    if (HandleOverviewEscape()) {
        return;
    }
    if (RenderPanelBackdrop("##overview_backdrop", displayW, displayH)) {
        CloseOverview();
        return;
    }

    constexpr float kPanelMarginFrac = 0.08f;
    const ImVec2 panelMin(displayW * kPanelMarginFrac, displayH * kPanelMarginFrac);
    const ImVec2 panelSize(displayW * (1.0f - 2.0f * kPanelMarginFrac), displayH * (1.0f - 2.0f * kPanelMarginFrac));
    ImGui::SetNextWindowPos(panelMin);
    ImGui::SetNextWindowSize(panelSize);
    ImGui::Begin("##overview_panel", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                      ImGuiWindowFlags_NoMove);
    // Reasserted here, right after Begin rather than after End - still
    // after the items' own per-frame reassert (see the backdrop's own
    // BringToFront for why that ordering matters), but *before* this
    // window's own body renders. That order matters for a reason the
    // backdrop never runs into: the
    // Settings tab's hotkey editor opens a real ImGui popup window (see
    // RenderHotkeyEditor's own KeepPopoverInFront call) nested inside this
    // one. BringWindowToDisplayFront just moves its target to the end of
    // ImGui's window list - whichever such call runs *last* in the frame
    // wins the front spot for the frame *after*. Called after End() - after
    // the body, including that popup's own KeepPopoverInFront - it would
    // always run last and bury the popup one frame after it opened: a
    // dropdown that appears to open but cannot be clicked. Called here, the
    // popup's own later KeepPopoverInFront wins.
    BringToFront("##overview_panel");

    RenderOverviewHeader();
    // Closed from the header - the picker's Cancel: nothing more of the
    // panel this frame. Drawn on, the body fell back to whichever tab was
    // last open, a frame of Settings, and the grid read back the
    // thumbnails CloseOverview had just let go of, to hold them unseen.
    if (!overviewOpen_) {
        ImGui::End();
        return;
    }
    ImGui::Separator();
    BeginOverviewPreviewFrame();

    // Both the Canvases body below and RenderOverviewSettingsPanel render
    // fine even while picking (pickerItemId_ set) - the tab strip is only
    // hidden there, not the tab logic - but picking a move/copy
    // destination is exactly the "browse canvases" task, so force the
    // Canvases body regardless of whatever overviewTab_ happens to still
    // hold from a previous, non-picker visit.
    const bool showCanvasesBody = pickerItemId_.has_value() || overviewTab_ == OverviewTab::Canvases;

    OverviewActions actions;
    ImGui::BeginChild("##overview_body", ImVec2(0.0f, -Px(40.0f)), ImGuiChildFlags_None);
    // A new page starts at its beginning. All three tabs and both About
    // pages share this one scrolling child, so without this, opening the
    // licenses from halfway down the About text drops you halfway down the
    // licenses - and the buttons that ask for the switch are in the footer,
    // outside this child, where SetScrollY would move the wrong window.
    if (overviewBodyScrollToTop_) {
        ImGui::SetScrollY(0.0f);
        overviewBodyScrollToTop_ = false;
    }
    if (!showCanvasesBody && overviewTab_ == OverviewTab::About) {
        RenderOverviewAboutPanel();
    } else if (!showCanvasesBody) {
        RenderOverviewSettingsPanel();
    } else {
        SettleDeletedFolderShown();
        RenderFolderSidebar(actions);
        ImGui::SameLine();
        RenderCanvasGrid(displayW, displayH, actions);
    }
    ImGui::EndChild();  // ##overview_body

    RenderOverviewFooter(showCanvasesBody);

    ImGui::End();
    // Anything opened from inside this panel that has no Begin/End pair of
    // ours to reassert itself - ImGui's own color picker, opened by a
    // ColorEdit swatch, is the whole list of them - goes back in front of
    // the panel here. The panel brings itself to the front on every frame
    // (see the BringToFront above), and BringWindowToDisplayFront is just
    // a move to the end of ImGui's window list, so whoever calls it last
    // wins: without this the picker was in front on the frame it opened
    // and behind the panel on every frame after, which is exactly what it
    // looked like - a picker that flashed up and vanished, with the clicks
    // meant for it landing on the panel. The properties popover carries the
    // same call for the same reason.
    KeepChildPopupsInFront();

    ApplyOverviewActions(actions);
}

bool OverlayApp::HandleOverviewEscape() {
    const bool isRenaming = renamingFolderId_.has_value() || renamingCanvasId_.has_value();
    if (isRenaming || !ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        return false;
    }
    // Innermost thing first: without this, Escape on an open help popover
    // or dropdown closed the whole panel out from under it - one keypress
    // undoing everything that was open rather than the thing that was in
    // the way.
    if (CloseTopmostPopover()) {
        return true;
    }
    if (shortcutCaptureAction_.has_value()) {
        // Escape on an armed Shortcuts row clears that binding rather than
        // closing the panel - "press the key you want, or Escape for none"
        // is the whole unbinding gesture, and it has to be caught here,
        // above the close, because this check runs first every frame.
        const ShortcutAction action = *shortcutCaptureAction_;
        shortcutCaptureAction_.reset();
        SetToolShortcut(action, platform::KeyCombo{});
        return true;
    }
    CloseOverview();
    return true;
}

bool OverlayApp::RenderPanelBackdrop(const char* windowId, float displayW, float displayH) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(displayW, displayH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.039f, 0.051f, 0.071f, 0.72f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    // Rounded corners on a full-viewport window would just cut two dark
    // triangles out of the screen's own corners - zero it out here only
    // (the overview panel keeps the global radius-lg rounding).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    // No padding, or the button below starts that far in from the corner
    // and a click in the strip along the screen's edges closes nothing.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(windowId, nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove);
    const bool clicked = ImGui::InvisibleButton("##backdrop_btn", ImVec2(displayW, displayH));
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    // Ordinary windows only get pushed to the front of ImGui's own window
    // stack automatically once, the first frame they're created - items
    // and the canvas bar each reassert themselves to the front every frame
    // (see BringToFront's other call sites), and since RenderItems/
    // RenderCanvasBar both run before the Overview every frame, that left
    // them ending up in front of (and clickable over) an Overview that's
    // been open more than one frame, without this doing the same.
    BringToFront(windowId);
    return clicked;
}

void OverlayApp::RenderOverviewHeader() {
    // Null with an empty library (see CanvasManager's class comment) -
    // the Overview is exactly the screen that has to keep working then,
    // since it's where a new canvas or folder comes from.
    const Canvas* currentCanvas = Manager().CurrentOrNull();
    if (pickerItemId_.has_value() && currentCanvas) {
        const auto it = std::find_if(currentCanvas->items.begin(), currentCanvas->items.end(),
                                      [&](const Item& i) { return i.id == *pickerItemId_; });
        const std::string itemName = it != currentCanvas->items.end() ? it->name : strings::kMoveCopyItemWord;
        ImGui::TextColored(theme::Accent(), strings::kMoveCopyPrompt, pickerIsCopy_ ? strings::kMoveCopyCopy : strings::kMoveCopyMove, itemName.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(Labeled(strings::kMoveCopyCancel, "pickercancel"))) {
            CloseOverview();
        }
        return;
    }

    // Picker mode (above) never shows these - it has its own
    // single-purpose header, and Settings has no business being
    // reachable mid-pick (see overviewTab_'s own doc comment).
    if (TabButton("overviewtabcanvases", strings::kOverviewTabCanvases, overviewTab_ == OverviewTab::Canvases)) {
        SwitchOverviewTab(OverviewTab::Canvases);
    }
    ImGui::SameLine();
    if (TabButton("overviewtabsettings", strings::kOverviewTabSettings, overviewTab_ == OverviewTab::Settings)) {
        SwitchOverviewTab(OverviewTab::Settings);
    }
    ImGui::SameLine();
    if (TabButton("overviewtababout", strings::kOverviewTabAbout, overviewTab_ == OverviewTab::About)) {
        SwitchOverviewTab(OverviewTab::About);
    }
    if (overviewTab_ != OverviewTab::Canvases) {
        return;
    }

    // What the canvas thumbnails may draw, right-aligned on the tab row -
    // the reason to reach for either is being unable to tell two canvases
    // apart, so they belong next to the canvases rather than in Settings.
    // Only while a canvas grid is showing: a preview toggle over the About
    // text is a control with nothing to act on.
    //
    // What is deleted is shown in the same folders and grid, where it was,
    // rather than on a page of its own - so this is a way of looking at
    // them, beside the other two, and says how much there is to see.
    char deletedLabel[96];
    const size_t deletedCount = Manager().DeletedFolderAndCanvasCount();
    if (deletedCount > 0) {
        std::snprintf(deletedLabel, sizeof(deletedLabel), strings::kOverviewShowDeletedCount, deletedCount);
    } else {
        std::snprintf(deletedLabel, sizeof(deletedLabel), "%s", strings::kOverviewShowDeleted);
    }
    const float checkboxWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float gapBeforePreviews = ImGui::GetStyle().ItemSpacing.x * 4.0f;
    const float controlsWidth = checkboxWidth + ImGui::CalcTextSize(deletedLabel).x +
                                gapBeforePreviews + ImGui::CalcTextSize(strings::kOverviewPreviewsLabel).x +
                                ImGui::GetStyle().ItemSpacing.x + checkboxWidth +
                                ImGui::CalcTextSize(strings::kOverviewPreviewsVector).x +
                                ImGui::GetStyle().ItemSpacing.x + checkboxWidth +
                                ImGui::CalcTextSize(strings::kOverviewPreviewsBitmap).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                          std::max(0.0f, ImGui::GetContentRegionAvail().x - controlsWidth));
    // Vertically centered against the tab buttons, which are taller
    // than a checkbox's own frame.
    const float rowCenterOffset = (ImGui::GetItemRectSize().y - ImGui::GetFrameHeight()) * 0.5f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, rowCenterOffset));
    if (ImGui::Checkbox(Labeled(deletedLabel, "showdeleted"), &showDeleted_)) {
        SettleDeletedFolderShown();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kOverviewShowDeletedHelp);
    }
    ImGui::SameLine(0.0f, gapBeforePreviews);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGraphite300, "%s", strings::kOverviewPreviewsLabel);
    ImGui::SameLine();
    // "Vector", not "Strokes": in bitmap mode a stroke *is* pixels, so a
    // box labeled Strokes that leaves them showing when it is unchecked
    // reads as a bug rather than as the two halves of the drawing model.
    bool changed = ImGui::Checkbox(Labeled(strings::kOverviewPreviewsVector, "prevvector"), &Cfg().overviewShowsStrokes);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kOverviewPreviewsVectorHelp);
    }
    ImGui::SameLine();
    changed |= ImGui::Checkbox(Labeled(strings::kOverviewPreviewsBitmap, "prevbitmap"), &Cfg().overviewShowsBitmaps);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kOverviewPreviewsBitmapHelp);
    }
    if (changed) {
        if (!Cfg().overviewShowsBitmaps) {
            ReleasePicturePreviews();
        }
        settings_.Commit();
    }
}

void OverlayApp::RenderFolderSidebar(OverviewActions& actions) {
    constexpr float kFolderRowHeight = 34.0f;
    constexpr float kFolderRowGap = 4.0f;
    const std::vector<Folder>& folders = Manager().Folders();
    const FolderId currentFolderId = OverviewFolderId();
    const bool showingDeleted = ShowingDeleted();
    const std::time_t now = std::time(nullptr);

    ImGui::BeginChild("##folder_sidebar", ImVec2(OverviewSidebarWidth(), 0.0f), ImGuiChildFlags_None);
    if (!showingDeleted &&
        std::all_of(folders.begin(), folders.end(), [&](const Folder& f) { return Manager().IsDeleted(f); })) {
        // Every folder has been deleted - legal now (see CanvasManager's
        // class comment), and reachable in one step from a library with a
        // single folder in it. "New folder" in the footer is still right
        // there, and so is "New canvas", which mints a folder to put it in.
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kOverviewNoFolders);
        ImGui::PopTextWrapPos();
    }
    for (size_t fi = 0; fi < folders.size(); ++fi) {
        const Folder& f = folders[fi];
        const bool deleted = Manager().IsDeleted(f);
        // Skipped here rather than filtered out beforehand, so that `fi`
        // stays the folder's place in Folders(), which ReorderFolder takes.
        if (deleted && !showingDeleted) {
            continue;
        }
        // With Show deleted on, a folder that is deleted or holds a deleted
        // canvas is marked out, with the two buttons that act on what is
        // deleted in it, and every other folder is dimmed.
        const bool marked = showingDeleted && Manager().HoldsDeleted(f);
        const bool dimmed = showingDeleted && !marked;
        const bool isCurrentFolder = f.id == currentFolderId;
        const bool isRenamingThis = renamingFolderId_ == f.id;
        ImGui::PushID(static_cast<int>(f.id));
        if (dimmed) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * kDimmedAlpha);
        }

        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        // Room for the buttons is always reserved: the last folder is
        // deletable like any other.
        const float buttonsWidth = marked ? Px(kPillButtonSize) * 2.0f + Px(kDeletedButtonGap) : Px(kPillButtonSize);
        const float selectWidth = rowWidth - buttonsWidth - Px(6.0f);
        const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + Px(kFolderRowHeight));
        ImDrawList* sidebarDrawList = ImGui::GetWindowDrawList();
        // Through GetColorU32, which a dimmed row's alpha applies to.
        if (isCurrentFolder) {
            const ImVec4 fill = marked ? ImVec4(theme::kDanger.x, theme::kDanger.y, theme::kDanger.z, 0.32f)
                                       : ImVec4(theme::Accent().x, theme::Accent().y, theme::Accent().z, 0.2f);
            sidebarDrawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(fill), Px(theme::kRadiusSm));
        } else if (marked) {
            sidebarDrawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(theme::kDangerSoft),
                                           Px(theme::kRadiusSm));
        }

        if (isRenamingThis) {
            // No InvisibleButton/drag-drop this frame: it would sit right
            // under the InputText drawn below at the same screen rect, and
            // (per the "overlapping widgets resolve first-submitted-wins"
            // gotcha - see docs/ARCHITECTURE.md) would win every click,
            // leaving the input field unclickable.
            ImGui::Dummy(ImVec2(selectWidth, Px(kFolderRowHeight)));
        } else {
            if (ImGui::InvisibleButton("##folderrow", ImVec2(selectWidth, Px(kFolderRowHeight)))) {
                // A deleted folder is looked into, not browsed - see
                // deletedFolderShown_.
                if (deleted) {
                    actions.showDeletedFolder = f.id;
                } else {
                    actions.switchToFolder = f.id;
                }
            }
            const bool rowHovered = ImGui::IsItemHovered();
            if (rowHovered && !deleted && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                renamingFolderId_ = f.id;
                renamingCanvasId_.reset();
                std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s", f.name.c_str());
                renameJustFocused_ = true;
                window_->RequestTextInput();
            }
            if (rowHovered && !isCurrentFolder) {
                sidebarDrawList->AddRectFilled(rowMin, ImVec2(rowMin.x + selectWidth, rowMax.y),
                                                ImGui::GetColorU32(theme::kHoverWash), Px(theme::kRadiusSm));
            }
            if (rowHovered && marked) {
                if (deleted && Cfg().purgeDeleted) {
                    ImGui::SetTooltip("%s\n%s", DeletedWhen(f.deletedAt, now).c_str(),
                                      GoesOn(f.deletedAt, Cfg().purgeDeletedAfterDays).c_str());
                } else if (deleted) {
                    ImGui::SetTooltip("%s", DeletedWhen(f.deletedAt, now).c_str());
                } else {
                    const size_t count = Manager().MarkedCanvasesIn(f.id).size();
                    ImGui::SetTooltip(count == 1 ? strings::kDeletedHoldsOne : strings::kDeletedHoldsMany, count);
                }
            }
            // Nothing is dragged out of or into what is deleted: it stays
            // where it was until it is restored.
            if (!deleted) {
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("HB_FOLDER_REORDER", &f.id, sizeof(FolderId));
                    ImGui::TextUnformatted(f.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HB_FOLDER_REORDER")) {
                        const FolderId draggedId = *static_cast<const FolderId*>(payload->Data);
                        if (draggedId != f.id) {
                            actions.folderReorder = {draggedId, fi};
                        }
                    }
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HB_CANVAS_REORDER")) {
                        const CanvasId draggedCanvasId = *static_cast<const CanvasId*>(payload->Data);
                        actions.canvasToFolder = {draggedCanvasId, f.id};
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        }

        if (isRenamingThis) {
            ImGui::SetCursorScreenPos(
                ImVec2(rowMin.x + Px(6.0f), rowMin.y + (Px(kFolderRowHeight) - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::SetNextItemWidth(selectWidth - Px(12.0f));
            if (renameJustFocused_) {
                ImGui::SetKeyboardFocusHere();
                renameJustFocused_ = false;
            }
            ImGui::InputText("##renamefolder", renameBuffer_, sizeof(renameBuffer_));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                session_.RenameFolder(f.id, std::string(renameBuffer_));
                renamingFolderId_.reset();
                window_->ReleaseTextInput();
            } else if (ImGui::IsItemDeactivated()) {
                renamingFolderId_.reset();
                window_->ReleaseTextInput();
            }
        } else {
            const ImVec4& ink = marked && !isCurrentFolder ? theme::kDeletedInk
                                : isCurrentFolder         ? theme::kWhite
                                                          : theme::kGraphite200;
            const ImVec2 textPos(rowMin.x + Px(10.0f),
                                 rowMin.y + (Px(kFolderRowHeight) - ImGui::GetTextLineHeight()) * 0.5f);
            sidebarDrawList->PushClipRect(rowMin, ImVec2(rowMin.x + selectWidth - Px(4.0f), rowMax.y), true);
            sidebarDrawList->AddText(textPos, ImGui::GetColorU32(ink), f.name.c_str());
            sidebarDrawList->PopClipRect();
        }

        if (!isRenamingThis) {
            const float buttonY = rowMin.y + (Px(kFolderRowHeight) - Px(kPillButtonSize)) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + selectWidth + Px(4.0f), buttonY));
            if (marked) {
                // Both act on what is deleted in the folder: all of it back,
                // or all of it gone for good - the folder with it only if
                // the folder is what was deleted.
                switch (DeletedButtons(strings::kDeletedRestoreFolderTip,
                                       deleted ? strings::kDeletedDeleteForGoodTip
                                               : strings::kDeletedDeleteDeletedInFolderTip)) {
                    case DeletedButton::Restore:
                        actions.restore = f.id;
                        break;
                    case DeletedButton::DeleteForGood:
                        confirmDeleteTarget_ = ConfirmDeleteTarget{
                            deleted ? ConfirmDeleteTarget::Kind::Folder : ConfirmDeleteTarget::Kind::DeletedCanvasesIn,
                            f.id, f.name, /*forGood=*/true};
                        confirmDeletePopoverRequested_ = true;
                        break;
                    case DeletedButton::None:
                        break;
                }
            } else {
                // Not while picking: see SendPickedItemTo.
                ImGui::BeginDisabled(pickerItemId_.has_value());
                const bool deletePressed = DangerIconButton("##delfolder", icons::kTrash);
                ImGui::EndDisabled();
                if (deletePressed) {
                    confirmDeleteTarget_ = ConfirmDeleteTarget{ConfirmDeleteTarget::Kind::Folder, f.id, f.name};
                    confirmDeletePopoverRequested_ = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", strings::kOverviewDeleteFolder);
                }
            }
        }

        // A just-made folder is the last row, which may be below the fold -
        // bring it into view once (see overviewScrollToFolderId_). By
        // position rather than ImGui::SetScrollHereY, which measures the
        // last item submitted: that is this row's delete button, not the
        // row, and the two are different heights.
        if (overviewScrollToFolderId_ == f.id) {
            ImGui::SetScrollFromPosY(rowMin.y - ImGui::GetWindowPos().y, 0.5f);
        }
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y + Px(kFolderRowGap)));
        if (dimmed) {
            ImGui::PopStyleVar();
        }
        ImGui::PopID();
    }
    // Whether or not it was found, so a stale id can't keep pulling the
    // sidebar around on later frames - same as the canvas grid's own.
    overviewScrollToFolderId_.reset();
    // Closes out the last row's SetCursorScreenPos boundary extension above -
    // ImGui asserts if a window/child ends right after a manual cursor move
    // with no item submitted afterward to confirm growing to that position.
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndChild();
}

void OverlayApp::RenderCanvasGrid(float displayW, float displayH, OverviewActions& actions) {
    const ImVec2 tileSize = Px(200.0f, 130.0f);
    constexpr float kSpacing = 14.0f;
    const std::vector<Canvas>& canvases = Manager().Canvases();
    const CanvasId currentCanvasId = Manager().CurrentCanvasId();  // 0 when there is none
    const FolderId currentFolderId = OverviewFolderId();
    const bool showingDeleted = ShowingDeleted();
    const std::time_t now = std::time(nullptr);

    // Canvases belonging to the shown folder that aren't deleted (or all of
    // them, with Show deleted on), in Canvases() order, and beside each its
    // place among *all* of that folder's canvases - which is what
    // ReorderCanvas expects for `newIndex` (it's scoped within the moved
    // canvas's own folder, hidden ones and all).
    std::vector<size_t> folderCanvasIndices;
    std::vector<size_t> folderCanvasPlaces;
    size_t placeInFolder = 0;
    for (size_t i = 0; i < canvases.size(); ++i) {
        if (canvases[i].folderId != currentFolderId) {
            continue;
        }
        if (showingDeleted || !Manager().IsDeleted(canvases[i])) {
            folderCanvasIndices.push_back(i);
            folderCanvasPlaces.push_back(placeInFolder);
        }
        ++placeInFolder;
    }

    ImGui::BeginChild("##overview_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    const float availW = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((availW + Px(kSpacing)) / (tileSize.x + Px(kSpacing))));

    // Built once for the whole grid rather than per tile.
    const PreviewTextureFn previewTexture = PreviewTextureLookup();

    if (folderCanvasIndices.empty()) {
        // A folder holding nothing has always been possible (a freshly
        // created one starts out that way); what's new is that the
        // *library* can be in the same state, with no canvas current at
        // all - so the two get separate wording, since only the second
        // one means there's nothing being edited behind this panel.
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(theme::kGraphite200, "%s",
                           Manager().HasCurrentCanvas() ? strings::kOverviewFolderEmpty : strings::kOverviewLibraryEmpty);
        ImGui::PopTextWrapPos();
    }
    for (size_t idxInFolder = 0; idxInFolder < folderCanvasIndices.size(); ++idxInFolder) {
        const Canvas& c = canvases[folderCanvasIndices[idxInFolder]];
        if (idxInFolder % static_cast<size_t>(columns) != 0) {
            ImGui::SameLine(0.0f, Px(kSpacing));
        }
        ImGui::PushID(static_cast<int>(c.id));
        // Only ever with Show deleted on: deleted on its own or with its
        // folder, and marked out either way. Everything else is dimmed.
        const bool deleted = Manager().IsDeleted(c);
        const bool dimmed = showingDeleted && !deleted;
        if (dimmed) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * kDimmedAlpha);
        }
        ImGui::BeginGroup();

        const ImVec2 thumbMin = ImGui::GetCursorScreenPos();
        const ImVec2 thumbMax(thumbMin.x + tileSize.x, thumbMin.y + tileSize.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        DrawCanvasPreview(drawList, c, thumbMin, thumbMax, displayW, displayH, Cfg().strokeRenderMode,
                           Cfg().overviewShowsStrokes, previewTexture, PreviewMeshSlot(), PictureSampling());
        const bool isActive = c.id == currentCanvasId;
        if (deleted) {
            drawList->AddRectFilled(thumbMin, thumbMax, ImGui::GetColorU32(theme::kDangerSoft), Px(4.0f));
            drawList->AddRect(thumbMin, thumbMax, ImGui::GetColorU32(theme::kDanger), Px(4.0f), ImDrawFlags_None,
                              PxWhole(2.0f));
        } else {
            // The preview's own pictures are drawn at full strength whatever
            // the style's alpha, so a dimmed tile is dimmed by a veil.
            if (dimmed) {
                drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(14, 16, 20, 150), Px(4.0f));
            }
            drawList->AddRect(thumbMin, thumbMax,
                               ImGui::GetColorU32(isActive ? theme::Accent() : ImVec4(0.275f, 0.298f, 0.345f, 1.0f)),
                               Px(4.0f), ImDrawFlags_None, isActive ? PxWhole(2.0f) : 1.0f);
        }

        if (ImGui::InvisibleButton("##tile", tileSize) && !deleted) {
            actions.clickedCanvas = c.id;
        }
        if (deleted && ImGui::IsItemHovered()) {
            // Its own stamp, or its folder's when it went with the folder.
            const Folder* folder = Manager().FindFolder(c.folderId);
            const int64_t stamp = c.deletedAt != 0 ? c.deletedAt : folder != nullptr ? folder->deletedAt : 0;
            if (Cfg().purgeDeleted) {
                ImGui::SetTooltip("%s\n%s\n%s", DeletedWhen(stamp, now).c_str(),
                                  GoesOn(stamp, Cfg().purgeDeletedAfterDays).c_str(), strings::kDeletedRestoreToOpen);
            } else {
                ImGui::SetTooltip("%s\n%s", DeletedWhen(stamp, now).c_str(), strings::kDeletedRestoreToOpen);
            }
        }
        if (!deleted && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("HB_CANVAS_REORDER", &c.id, sizeof(CanvasId));
            ImGui::TextUnformatted(c.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (!deleted && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HB_CANVAS_REORDER")) {
                const CanvasId draggedId = *static_cast<const CanvasId*>(payload->Data);
                if (draggedId != c.id) {
                    actions.canvasReorder = {draggedId, folderCanvasPlaces[idxInFolder]};
                }
            }
            ImGui::EndDragDropTarget();
        }

        const bool isRenamingThisCanvas = renamingCanvasId_ == c.id;
        if (isRenamingThisCanvas) {
            ImGui::SetNextItemWidth(tileSize.x);
            if (renameJustFocused_) {
                ImGui::SetKeyboardFocusHere();
                renameJustFocused_ = false;
            }
            ImGui::InputText("##renamecanvas", renameBuffer_, sizeof(renameBuffer_));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                session_.RenameCanvas(c.id, std::string(renameBuffer_));
                renamingCanvasId_.reset();
                window_->ReleaseTextInput();
            } else if (ImGui::IsItemDeactivated()) {
                renamingCanvasId_.reset();
                window_->ReleaseTextInput();
            }
        } else {
            ImGui::TextColored(deleted ? theme::kDeletedInk : theme::kWhite, "%s", c.name.c_str());
            if (!deleted && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                renamingCanvasId_ = c.id;
                renamingFolderId_.reset();
                std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s", c.name.c_str());
                renameJustFocused_ = true;
                window_->RequestTextInput();
            }
        }
        // No "...and this folder holds more than one" condition: the last
        // canvas in a folder is deletable, and a button that is simply not
        // drawn reads as a bug. See CanvasManager's class comment for the
        // invariant behind it, now gone.
        if (deleted) {
            ImGui::SameLine(tileSize.x - Px(kPillButtonSize) * 2.0f - Px(kDeletedButtonGap));
            // Out of a deleted folder the folder comes back to hold it, and
            // the rest of what went with the folder stays deleted - see
            // CanvasManager::Restore.
            const Folder* folder = Manager().FindFolder(c.folderId);
            const bool folderDeleted = folder != nullptr && Manager().IsDeleted(*folder);
            switch (DeletedButtons(folderDeleted ? strings::kDeletedRestoreCanvasAndFolderTip
                                                 : strings::kDeletedRestoreCanvasTip,
                                   strings::kDeletedDeleteForGoodTip)) {
                case DeletedButton::Restore:
                    actions.restore = c.id;
                    break;
                case DeletedButton::DeleteForGood:
                    confirmDeleteTarget_ =
                        ConfirmDeleteTarget{ConfirmDeleteTarget::Kind::Canvas, c.id, c.name, /*forGood=*/true};
                    confirmDeletePopoverRequested_ = true;
                    break;
                case DeletedButton::None:
                    break;
            }
        } else if (!isRenamingThisCanvas) {
            ImGui::SameLine(tileSize.x - Px(kPillButtonSize));
            // Not while picking: see SendPickedItemTo.
            ImGui::BeginDisabled(pickerItemId_.has_value());
            const bool deletePressed = DangerIconButton("##delcanvas", icons::kTrash);
            ImGui::EndDisabled();
            if (deletePressed) {
                confirmDeleteTarget_ = ConfirmDeleteTarget{ConfirmDeleteTarget::Kind::Canvas, c.id, c.name};
                confirmDeletePopoverRequested_ = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", strings::kOverviewDeleteCanvas);
            }
        }

        ImGui::EndGroup();
        if (dimmed) {
            ImGui::PopStyleVar();
        }
        // A just-created canvas is at the end of its folder, which may be
        // past the bottom of this list - scroll it into view once (see
        // overviewScrollToCanvasId_). Centered rather than merely made
        // visible: the tile that just appeared should be the one you are
        // looking at.
        if (overviewScrollToCanvasId_ == c.id) {
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::PopID();
    }
    // Whether or not it was found - the target may be in another folder,
    // or already gone - so a stale id can't keep yanking the list around
    // on later frames.
    overviewScrollToCanvasId_.reset();
    ImGui::EndChild();
}

void OverlayApp::RenderOverviewFooter(bool showCanvasesBody) {
    if (!showCanvasesBody && overviewTab_ == OverviewTab::About) {
        // In the footer rather than at the end of the text it belongs to:
        // the licenses run to a couple of hundred lines, and a way out
        // that has to be scrolled back to is a way out you stop using.
        // The footer is the panel's own row of verbs - the Canvases tab
        // keeps New folder / New canvas here - so this is where a reader
        // already looks for one.
        if (aboutShowsNotices_) {
            if (ImGui::Button(Labeled(strings::kAboutBackToAbout, "noticesback"))) {
                aboutShowsNotices_ = false;
                overviewBodyScrollToTop_ = true;
            }
        } else if (ImGui::Button(Labeled(strings::kAboutThirdPartyLicenses, "noticesopen"))) {
            aboutShowsNotices_ = true;
            overviewBodyScrollToTop_ = true;
        }
        return;
    }
    if (!showCanvasesBody) {
        return;
    }

    // Not while picking, since it switches to the canvas it makes: see
    // SendPickedItemTo. New canvas stays, as a destination to send to.
    ImGui::BeginDisabled(pickerItemId_.has_value());
    const bool newFolderPressed = PrimaryButton("##newfolder", icons::kPlus, strings::kOverviewNewFolder);
    ImGui::EndDisabled();
    if (newFolderPressed) {
        // Named for when it was made (see TimestampName), and at the end
        // of the sidebar - which is where the eye goes after pressing a
        // button at the bottom of it, and matches where a new canvas
        // lands in its own list.
        overviewScrollToFolderId_ = session_.AddFolder(TimestampName());
        deletedFolderShown_.reset();
        // With a canvas already in it. A folder is where canvases live, so
        // an empty one is a step rather than a result, and an empty folder
        // reads as a dead end: no tile to click, nothing to drop an item
        // onto. AddFolder has already made the new folder
        // current, so this lands inside it - and, like the button next to
        // it, the canvas it makes is switched to.
        session_.SwitchToCanvas(CreateCanvasInCurrentFolder());
    }
    // Lined up with the canvas grid above it, which starts past the
    // window's padding, the sidebar and the gap after it. SameLine counts
    // from the window's edge rather than from inside its padding, so the
    // padding is added here - without it the button sat that far left of
    // the tiles.
    ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + OverviewSidebarWidth() + ImGui::GetStyle().ItemSpacing.x);
    // Not into a deleted folder, which is the one on show: a new canvas goes
    // to the folder being browsed, and that would be somewhere else.
    const bool showsDeletedFolder = ShowingDeleted() && deletedFolderShown_.has_value();
    ImGui::BeginDisabled(showsDeletedFolder);
    const bool newCanvasPressed = PrimaryButton("##newcanvas", icons::kPlus, strings::kOverviewNewCanvas);
    ImGui::EndDisabled();
    if (newCanvasPressed) {
        const CanvasId id = CreateCanvasInCurrentFolder();
        // Made at the end of the folder, so the grid may have to scroll for
        // it to be seen at all - see overviewScrollToCanvasId_, which is set
        // on both paths below (picker or not): either way the tile is what
        // the click was about.
        if (pickerItemId_.has_value()) {
            // The new canvas is a destination for the item being sent
            // away, and is not switched to: following it would take the
            // user off the canvas they were working on.
            SendPickedItemTo(id);
        } else {
            // Switch to it, and stay open. Making a canvas is asking for
            // somewhere new to draw, so leaving the app on the old one
            // meant the button did half the job and the other half was a
            // click on a tile that had just appeared. The Overview stays
            // up: a panel that vanishes the instant a button is pressed is
            // disorienting, and keeping it up leaves the grid there to
            // carry on with.
            session_.SwitchToCanvas(id);
        }
    }
}

void OverlayApp::ApplyOverviewActions(const OverviewActions& actions) {
    if (actions.folderReorder.has_value()) {
        session_.ReorderFolder(actions.folderReorder->first, actions.folderReorder->second);
    }
    if (actions.canvasToFolder.has_value()) {
        session_.MoveCanvasToFolder(actions.canvasToFolder->first, actions.canvasToFolder->second);
    }
    if (actions.canvasReorder.has_value()) {
        session_.ReorderCanvas(actions.canvasReorder->first, actions.canvasReorder->second);
    }
    if (actions.switchToFolder.has_value()) {
        session_.SwitchToFolder(*actions.switchToFolder);
        deletedFolderShown_.reset();
    }
    if (actions.showDeletedFolder.has_value()) {
        deletedFolderShown_ = *actions.showDeletedFolder;
    }
    if (actions.restore.has_value() && session_.Restore(*actions.restore)) {
        ShowActionToast(strings::kToastRestored);
        SettleDeletedFolderShown();
    }
    if (actions.clickedCanvas.has_value()) {
        if (pickerItemId_.has_value()) {
            SendPickedItemTo(*actions.clickedCanvas);
        } else {
            session_.SwitchToCanvas(*actions.clickedCanvas);
            CloseOverview();
        }
    }
}

void OverlayApp::SendPickedItemTo(CanvasId target) {
    if (!pickerItemId_.has_value()) {
        return;
    }
    const ItemId item = *pickerItemId_;
    const bool isCopy = pickerIsCopy_;
    const CanvasId source = Manager().CurrentCanvasId();
    // The item is sent from the current canvas, so nothing in the picker
    // may change which canvas that is: New folder and the delete buttons
    // are off while it is open. And what happened is checked rather than
    // assumed: a move that did nothing said "Moved to" all the same, and
    // threw away the history of a snippet that had not gone anywhere.
    if (target != source) {
        const Canvas* targetCanvas = Manager().FindCanvas(target);
        const std::string targetName = targetCanvas ? targetCanvas->name : strings::kDeleteConfirmCanvasWord;
        const Session::Placed sent = session_.SendItemsTo({item}, target, isCopy);
        if (sent.items.empty()) {
            pickerItemId_.reset();
            return;
        }
        ShowActionToast(sent.pictureLost ? std::string(strings::kToastCopiedWithoutPicture)
                                         : std::string(isCopy ? strings::kToastCopiedToPrefix
                                                              : strings::kToastMovedToPrefix) +
                                               targetName);
    }
    // Out of picker mode (the move/copy is already done), with the Overview
    // itself left open - the user can click the target canvas to switch to
    // it, or close the Overview themselves; closing it here would make the
    // whole panel vanish the instant a canvas was picked, with nothing but
    // the toast to explain what had just happened.
    pickerItemId_.reset();
}

// Which body the Settings tab shows, picked from a list down its left
// side. One long scroll was how this started, and it grew into eight
// unrelated headings between which the only navigation was the wheel -
// appearance, hotkeys, input capture and debug flags interleaved by the
// order they happened to be added.
//
// The split is not only tidiness: "Appearance", "Interaction" and "Debug"
// are about you and are always global, while "Behavior" and "Hotkeys" are about
// whatever is underneath and are what a per-application profile may
// override. Making that the *section* boundary means the rule is one
// sentence per section rather than a marker per row.
//
// "Hotkeys" is the one section that is both, which is why the three global
// ones sit above its profile picker rather than below it - see
// RenderSettingsHotkeys.
void OverlayApp::RenderOverviewSettingsPanel() {
    struct SectionRow {
        SettingsSection section;
        const char* id;
        const char* label;
    };
    static constexpr SectionRow kSections[] = {
        {SettingsSection::Appearance, "sectionappearance", strings::kSettingsTabAppearance},
        {SettingsSection::Interaction, "sectioninteraction", strings::kSettingsTabInteraction},
        {SettingsSection::Behavior, "sectionbehavior", strings::kSettingsTabBehavior},
        {SettingsSection::Defaults, "sectiondefaults", strings::kSettingsTabDefaults},
        {SettingsSection::Hotkeys, "sectionhotkeys", strings::kSettingsTabHotkeys},
        {SettingsSection::Profiles, "sectionprofiles", strings::kSettingsTabProfiles},
        {SettingsSection::Debug, "sectiondebug", strings::kSettingsTabDebug},
    };

    constexpr float kSectionListWidth = 150.0f;
    ImGui::BeginChild("##settings_sections", ImVec2(Px(kSectionListWidth), 0.0f), ImGuiChildFlags_None);
    for (const SectionRow& row : kSections) {
        if (SettingsSectionButton(row.id, row.label, settingsSection_ == row.section)) {
            settingsSection_ = row.section;
        }
    }
    ImGui::EndChild();

    // The same air on both sides of that column. A borderless child has no
    // padding of its own, so what sits left of the buttons is the panel's
    // window padding, while the gap to the body would default to one item
    // spacing - visibly less, and enough to make the list look pushed
    // against the content rather than beside it.
    ImGui::SameLine(0.0f, ImGui::GetStyle().WindowPadding.x);
    ImGui::BeginChild("##settings_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);

    // `anyChanged` batches every edit made in a single call into at most
    // one Settings::Commit - simpler than threading a
    // dozen individual "did this one control just change" call sites
    // through to the same one-line callback, and harmless: nothing here
    // reads a *stale* value in between (every widget below is bound
    // straight to the member it edits, so the live/in-session behavior is
    // already correct the instant ImGui writes to it - only the disk
    // write waits for this function to finish). The hotkey editors are the
    // one exception - see RenderHotkeyEditor/TryChangeHotkey's own doc
    // comments for why those go through a separate request/response
    // callback instead - as are the shortcut rows, which persist through
    // SetToolShortcut.
    bool anyChanged = false;
    switch (settingsSection_) {
        case SettingsSection::Appearance:
            RenderSettingsAppearance(anyChanged);
            break;
        case SettingsSection::Interaction:
            RenderSettingsInteraction(anyChanged);
            break;
        case SettingsSection::Behavior:
            RenderSettingsBehavior(anyChanged);
            break;
        case SettingsSection::Defaults:
            RenderSettingsDefaults(anyChanged);
            break;
        case SettingsSection::Hotkeys:
            RenderSettingsHotkeys(anyChanged);
            break;
        case SettingsSection::Profiles:
            RenderSettingsProfiles();
            break;
        case SettingsSection::Debug:
            RenderSettingsDebug(anyChanged);
            break;
    }

    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    if (anyChanged) {
        settings_.Commit();
    }
}

void OverlayApp::RenderSettingsAppearance(bool& anyChanged) {
    SettingsHeading("appearancemonitorheading", strings::kAppearanceMonitorHeading, strings::kAppearanceMonitorHelp);
    {
        const auto describe = [](const platform::DisplayInfo& display) {
            char text[256];
            if (display.refreshHz > 0) {
                std::snprintf(text, sizeof(text), strings::kAppearanceMonitorEntry, display.name.c_str(),
                              display.width, display.height, display.refreshHz);
            } else {
                std::snprintf(text, sizeof(text), strings::kAppearanceMonitorEntryNoRate, display.name.c_str(),
                              display.width, display.height);
            }
            return std::string(text);
        };
        const std::string chosenId = Cfg().overlayDisplayId;
        const std::string chosenName = Cfg().overlayDisplayName;
        const auto primary =
            std::find_if(displays_.begin(), displays_.end(), [](const auto& display) { return display.primary; });
        char primaryText[256];
        std::snprintf(primaryText, sizeof(primaryText), strings::kAppearanceMonitorPrimary,
                      primary != displays_.end() ? primary->name.c_str() : "");
        // Found the way the tray controller finds it, so the list says the
        // overlay is on a monitor exactly when it is - including one that
        // came back on another port and is only recognized by its name.
        const platform::DisplayInfo inUse = ChooseDisplay(displays_, chosenId, chosenName);
        const bool chosenAttached =
            !chosenId.empty() && (inUse.id == chosenId || (!chosenName.empty() && inUse.name == chosenName));
        char missingText[256];
        std::snprintf(missingText, sizeof(missingText), strings::kAppearanceMonitorMissing, chosenName.c_str());
        const std::string preview = chosenId.empty() ? std::string(primaryText)
                                    : chosenAttached ? describe(inUse)
                                                     : std::string(missingText);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(strings::kAppearanceMonitorLabel);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(300.0f));
        if (ImGui::BeginCombo("##overlaydisplay", preview.c_str())) {
            // Same reason as the Profiles dropdowns: a popup inside a window
            // that re-asserts itself to the front every frame has to as well.
            KeepPopoverInFront();
            if (ImGui::IsWindowAppearing() && displayListCallback_) {
                displays_ = displayListCallback_();
            }
            if (ImGui::Selectable(Labeled(primaryText, "displayprimary"), chosenId.empty())) {
                Cfg().overlayDisplayId.clear();
                Cfg().overlayDisplayName.clear();
                displayChoiceCommitPending_ = true;
            }
            for (size_t i = 0; i < displays_.size(); ++i) {
                const std::string id = "display" + std::to_string(i);
                const bool selected = chosenAttached && displays_[i].id == inUse.id;
                if (ImGui::Selectable(Labeled(describe(displays_[i]).c_str(), id.c_str()), selected)) {
                    Cfg().overlayDisplayId = displays_[i].id;
                    Cfg().overlayDisplayName = displays_[i].name;
                    displayChoiceCommitPending_ = true;
                }
            }
            // Kept in the list while it is away, so the choice can be seen
            // and left as it is - picking anything else is what forgets it.
            if (!chosenId.empty() && !chosenAttached) {
                ImGui::Selectable(Labeled(missingText, "displaymissing"), true);
            }
            ImGui::EndCombo();
        }
    }

    SettingsGroupBreak();

    SettingsHeading("appearanceuiscaleheading", strings::kAppearanceUiScaleHeading, strings::kAppearanceUiScaleHelp);
    {
        // Windows' own steps up to 200%, and two past it for a large
        // display seen from across the room. A value typed into the config
        // file that is none of these still shows as itself.
        constexpr int kPresets[] = {75, 100, 125, 150, 175, 200, 250, 300};
        const int windowsPercent = window_ != nullptr ? window_->ScalePercent() : 100;
        char autoText[64];
        std::snprintf(autoText, sizeof(autoText), strings::kAppearanceUiScaleAuto, windowsPercent);
        char preview[64];
        if (Cfg().uiScalePercent == 0) {
            std::snprintf(preview, sizeof(preview), "%s", autoText);
        } else {
            std::snprintf(preview, sizeof(preview), strings::kFormatPercent, Cfg().uiScalePercent);
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(strings::kAppearanceUiScaleLabel);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(200.0f));
        // Applied from the next frame on, by OnFrame - the whole of one
        // frame is drawn at one scale.
        if (ImGui::BeginCombo("##uiscale", preview)) {
            KeepPopoverInFront();
            if (ImGui::Selectable(Labeled(autoText, "uiscaleauto"), Cfg().uiScalePercent == 0)) {
                Cfg().uiScalePercent = 0;
                anyChanged = true;
            }
            for (const int percent : kPresets) {
                char text[16];
                std::snprintf(text, sizeof(text), strings::kFormatPercent, percent);
                char id[24];
                std::snprintf(id, sizeof(id), "uiscale%d", percent);
                if (ImGui::Selectable(Labeled(text, id), Cfg().uiScalePercent == percent)) {
                    Cfg().uiScalePercent = percent;
                    anyChanged = true;
                }
            }
            ImGui::EndCombo();
        }
    }

    SettingsGroupBreak();

    SettingsHeading("appearanceaccentheading", strings::kAppearanceAccentHeading, strings::kAppearanceAccentHelp);
    {
        float rgb[3];
        ColorRGBAToFloats(Cfg().accentColorRGBA, rgb);
        // Written into the setting as it is dragged, which OnFrame turns into
        // the theme on the next frame - so the panel this sits in, the tabs
        // and this very swatch's own highlights all recolor live. Saved when
        // the edit finishes, the same as every other color here.
        if (ImGui::ColorEdit3("##accentcolor", rgb, ImGuiColorEditFlags_NoInputs)) {
            Cfg().accentColorRGBA = FloatsToColorRGBA(rgb, static_cast<uint8_t>(0xFF));
        }
        anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::TextUnformatted(strings::kAppearanceAccentColor);
        const uint32_t defaultAccent = DefaultConfig().accentColorRGBA;
        if (Cfg().accentColorRGBA != defaultAccent) {
            ImGui::SameLine();
            if (ImGui::SmallButton(Labeled(strings::kAppearanceAccentReset, "accentreset"))) {
                Cfg().accentColorRGBA = defaultAccent;
                anyChanged = true;
            }
        }
    }

    SettingsGroupBreak();

    SettingsHeading("appearancedisplayheading", strings::kAppearanceDisplayHeading);
    anyChanged |= CheckboxWithHelp("appearanceshowitemborders", strings::kAppearanceShowItemBorders, &Cfg().showItemBorders,
                                    strings::kAppearanceShowItemBordersHelp);

    SettingsGroupBreak();

    SettingsHeading("appearanceimagefilterheading", strings::kAppearanceImageFilterHeading,
                     strings::kAppearanceImageFilterHelp);
    {
        int filter = static_cast<int>(Cfg().imageFilter);
        anyChanged |= ImGui::RadioButton(Labeled(strings::kAppearanceImageFilterBilinear, "imagefilterbilinear"),
                                         &filter, static_cast<int>(platform::ImageFilter::Bilinear));
        ImGui::SameLine();
        anyChanged |= ImGui::RadioButton(Labeled(strings::kAppearanceImageFilterNearest, "imagefilternearest"),
                                         &filter, static_cast<int>(platform::ImageFilter::Nearest));
        ImGui::SameLine();
        anyChanged |= ImGui::RadioButton(Labeled(strings::kAppearanceImageFilterBicubic, "imagefilterbicubic"),
                                         &filter, static_cast<int>(platform::ImageFilter::Bicubic));
        ImGui::SameLine();
        anyChanged |= ImGui::RadioButton(Labeled(strings::kAppearanceImageFilterLanczos, "imagefilterlanczos"),
                                         &filter, static_cast<int>(platform::ImageFilter::Lanczos));
        Cfg().imageFilter = static_cast<platform::ImageFilter>(filter);
    }

    SettingsGroupBreak();

    SettingsHeading("appearancesnippetcolorsheading", strings::kAppearanceSnippetColorsHeading,
                     strings::kAppearanceSnippetColorsHelp);
    {
        // ColorEdit4 rather than the ColorEdit3-plus-opacity-slider pair
        // the edit-mode border uses: here the alpha *is* the setting half
        // the time, and two widgets per color would make four rows into
        // eight.
        constexpr ImGuiColorEditFlags kSwatchFlags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;
        // The swatch takes an id-only label and the caption is written
        // beside it, rather than handing ColorEdit4 "caption##id": that
        // widget pushes its own label as an ID scope, so with the caption
        // in there the scope - and every id under it - would move whenever
        // the words changed. Which is the whole thing this is avoiding.
        auto ColorRow = [&](const char* id, const char* caption, uint32_t& colorRGBA) {
            float rgba[4];
            ColorRGBAToFloats4(colorRGBA, rgba);
            if (ImGui::ColorEdit4(id, rgba, kSwatchFlags)) {
                colorRGBA = FloatsToColorRGBA4(rgba);
            }
            // Only when the edit finishes, not on every frame the value
            // changes - a color picker reports a change per frame while it
            // is being dragged, and `anyChanged` is what writes config.json.
            // Same treatment as the sliders below; the live color is
            // already correct the instant ImGui writes to it either way.
            anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            ImGui::TextUnformatted(caption);
        };
        ColorRow("##snipcolfrontborder", strings::kAppearanceFrontmostBorder, Cfg().itemBorderColorFrontRGBA);
        ColorRow("##snipcolotherborder", strings::kAppearanceOtherBorders, Cfg().itemBorderColorOtherRGBA);
        ColorRow("##snipcolpinnedborder", strings::kAppearancePinnedBorder, Cfg().itemBorderColorPinnedRGBA);
    }

    SettingsGroupBreak();

    SettingsHeading("appearanceeditborderheading", strings::kAppearanceEditBorderHeading);
    anyChanged |= CheckboxWithHelp("appearanceshoweditborder", strings::kAppearanceShowEditBorder, &Cfg().showEditModeBorder,
        strings::kAppearanceShowEditBorderHelp);
    ImGui::BeginDisabled(!Cfg().showEditModeBorder);
    {
        float rgb[3];
        ColorRGBAToFloats(Cfg().editModeBorderColorRGBA, rgb);
        // Id-only label, caption beside it - see ColorRow above for why.
        if (ImGui::ColorEdit3("##editbordercolor", rgb, ImGuiColorEditFlags_NoInputs)) {
            Cfg().editModeBorderColorRGBA = FloatsToColorRGBA(rgb, static_cast<uint8_t>(0xFF));
        }
        // On the edit finishing, not per frame of the drag - see ColorRow.
        anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::TextUnformatted(strings::kAppearanceEditBorderColor);
    }
    ImGui::SetNextItemWidth(Px(160.0f));
    int borderPct = static_cast<int>(std::round(Cfg().editModeBorderOpacity * 100.0f));
    if (ImGui::SliderInt(Labeled(strings::kAppearanceEditBorderOpacity, "editborderopacity"), &borderPct, 0, 100, strings::kFormatPercent, ImGuiSliderFlags_AlwaysClamp)) {
        Cfg().editModeBorderOpacity = static_cast<float>(borderPct) / 100.0f;
    }
    anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetNextItemWidth(Px(160.0f));
    ImGui::SliderFloat(Labeled(strings::kAppearanceEditBorderWidth, "editborderwidth"), &Cfg().editModeBorderWidthPx, kEditModeBorderWidthMin,
                        kEditModeBorderWidthMax, strings::kFormatPixels, ImGuiSliderFlags_AlwaysClamp);
    anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
    anyChanged |= CheckboxWithHelp("appearanceeditborderemptyonly", strings::kAppearanceEditBorderEmptyOnly, &Cfg().editModeBorderOnlyWhenEmpty,
        strings::kAppearanceEditBorderEmptyOnlyHelp);
    ImGui::EndDisabled();

    SettingsGroupBreak();

    SettingsHeading("appearancecanvasbarheading", strings::kAppearanceCanvasBarHeading);
    if (CheckboxWithHelp("appearanceshowcanvasbar", strings::kAppearanceShowCanvasBar, &Cfg().showCanvasBar,
                          strings::kAppearanceShowCanvasBarHelp)) {
        anyChanged = true;
    }
}

namespace {

// The label a bar's button wears in the row that arranges them, and the
// icon that stands for it there.
//
// A fixed icon, unlike the bar's own, where the pen and the eraser show
// the shape they are cycled to: this row is about which buttons are there,
// not about what a drag would make right now. The color button has no
// icon on either - it is the color itself - and is drawn as a swatch here
// too.
const char* BarButtonName(ChromeButton button) {
    switch (button) {
        case ChromeButton::Close:
            return strings::kBarButtonClose;
        case ChromeButton::Maximize:
            return strings::kBarButtonMaximize;
        case ChromeButton::Minimize:
            return strings::kBarButtonMinimize;
        case ChromeButton::More:
            return strings::kBarButtonMore;
        case ChromeButton::Pin:
            return strings::kBarButtonPin;
        case ChromeButton::Pen:
            return strings::kBarButtonPen;
        case ChromeButton::Eraser:
            return strings::kBarButtonEraser;
        case ChromeButton::Text:
            return strings::kBarButtonText;
        case ChromeButton::Color:
            return strings::kBarButtonColor;
    }
    return "";
}

const Icon& BarButtonIcon(ChromeButton button) {
    switch (button) {
        case ChromeButton::Close:
            return icons::kX;
        case ChromeButton::Maximize:
            return icons::kMaximize;
        case ChromeButton::Minimize:
            return icons::kMinimize;
        case ChromeButton::More:
            return icons::kMoreVertical;
        case ChromeButton::Pin:
            return icons::kPin;
        case ChromeButton::Pen:
            return icons::kPen;
        case ChromeButton::Eraser:
            return icons::kEraser;
        case ChromeButton::Text:
            return icons::kType;
        case ChromeButton::Color:
            break;  // a swatch, not an icon - see RenderBarButtonRow
    }
    return icons::kPen;
}

// Where a row's tiles start, so the two rows line up under each other
// however long their labels are - the same reasoning as the profile
// fields' own column.
constexpr float kBarRowTilesX = 90.0f;

}  // namespace

// One bar as a row of its buttons, in the order the bar draws them: a tile
// per button, lit when it is shown and dimmed when it is not, clicked to
// switch it, dragged onto another tile to move it there.
//
// The row is the bar, in other words - which is why a hidden button keeps
// its place in it instead of being moved off to a list of spares: the
// question being asked of this row is "what does the bar look like", and
// the answer reads better with the missing ones still in view, grayed.
bool OverlayApp::RenderBarButtonRow(const char* id, const char* label, BarButtonList& buttons) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGraphite200, "%s", label);

    // Both deferred past the walk: either one edits the very list being
    // drawn from.
    std::optional<size_t> toggle;
    std::optional<std::pair<size_t, size_t>> move;
    // A payload type of the row's own: the payload is an index into this
    // row's list, and a tile dropped on the other row moved whatever sat
    // at that index there.
    char payloadType[32] = {};
    std::snprintf(payloadType, sizeof(payloadType), "HB_BAR_%s", id);
    for (size_t at = 0; at < buttons.size(); ++at) {
        const BarButtonSetting& entry = buttons[at];
        // Named for the button rather than for the place it currently
        // sits, so ImGui's own idea of which tile is hovered, held or
        // being dragged follows the button as it moves rather than staying
        // with the slot it left.
        const std::string_view key = BarButtonKey(entry.button);
        char tileId[32] = {};
        std::snprintf(tileId, sizeof(tileId), "##tile%.*s", static_cast<int>(key.size()), key.data());
        if (at == 0) {
            ImGui::SameLine(Px(kBarRowTilesX));
        } else {
            ImGui::SameLine();
        }
        // Hidden reads as dimmed rather than as a different shape, so the
        // row still looks like the bar it is setting.
        if (!entry.shown) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
        }
        // Both kinds of tile wear the same pill, lit or not - the color
        // one through PillSwatchButton rather than the chooser's own
        // PillColorButton, which has no pill to light.
        const bool pressed = entry.button == ChromeButton::Color
                                  ? PillSwatchButton(tileId, drawColorRGBA_, entry.shown)
                                  : PillIconButton(tileId, BarButtonIcon(entry.button), entry.shown);
        if (!entry.shown) {
            ImGui::PopStyleVar();
        }
        if (pressed) {
            toggle = at;
        }
        if (ImGui::BeginDragDropSource()) {
            const int from = static_cast<int>(at);
            ImGui::SetDragDropPayload(payloadType, &from, sizeof(int));
            ImGui::TextUnformatted(BarButtonName(entry.button));
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(payloadType)) {
                const auto from = static_cast<size_t>(*static_cast<const int*>(payload->Data));
                if (from != at && from < buttons.size()) {
                    move = std::make_pair(from, at);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n%s", BarButtonName(entry.button),
                               entry.shown ? strings::kBarsShownTip : strings::kBarsHiddenTip);
        }
    }

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
    if (ImGui::SmallButton(Labeled(strings::kBarsReset, "resetbar"))) {
        // The way out of having switched everything off, which is allowed
        // - a bar with no buttons is drawn as no bar at all.
        buttons = id == std::string("drawingbar") ? DefaultDrawingBar() : DefaultSnippetBar();
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kBarsResetTip);
    }

    if (toggle.has_value()) {
        buttons[*toggle].shown = !buttons[*toggle].shown;
        changed = true;
    }
    if (move.has_value()) {
        const BarButtonSetting dragged = buttons[move->first];
        buttons.erase(buttons.begin() + static_cast<long>(move->first));
        buttons.insert(buttons.begin() + static_cast<long>(move->second), dragged);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

void OverlayApp::RenderSettingsInteraction(bool& anyChanged) {
    // How strokes are drawn.
    SettingsHeading("drawingpenheading", strings::kDrawingPenHeading);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(strings::kDrawingStrokeRenderingHeading);
    ImGui::SameLine();
    HelpMarker("drawingstrokerenderingheading", strings::kDrawingStrokeRenderingHeading,
               strings::kDrawingStrokeRenderingHelp);
    ImGui::SameLine();
    int renderMode = static_cast<int>(Cfg().strokeRenderMode);
    anyChanged |= ImGui::RadioButton(Labeled(strings::kDrawingTessellated, "strokemodetess"), &renderMode, static_cast<int>(StrokeRenderMode::Tessellated));
    ImGui::SameLine();
    anyChanged |= ImGui::RadioButton(Labeled(strings::kDrawingPolyline, "strokemodepoly"), &renderMode, static_cast<int>(StrokeRenderMode::Polyline));
    ImGui::SameLine();
    anyChanged |= ImGui::RadioButton(Labeled(strings::kDrawingRasterized, "strokemoderaster"), &renderMode, static_cast<int>(StrokeRenderMode::Rasterized));
    Cfg().strokeRenderMode = static_cast<StrokeRenderMode>(renderMode);

    SettingsGroupBreak();

    SettingsHeading("drawingsnippetsheading", strings::kDrawingSnippetsHeading);
    anyChanged |= CheckboxWithHelp("drawingraiseselected", strings::kDrawingRaiseSelected, &Cfg().raiseSelectedSnippet,
                                    strings::kDrawingRaiseSelectedHelp);

    SettingsGroupBreak();

    SettingsHeading("creationheading", strings::kCreationHeading, strings::kCreationHelp);
    anyChanged |= CreationTriggerRow("screenshottrigger", strings::kCreationScreenshot, Cfg().screenshotTrigger,
                                     Cfg().drawingTrigger);
    anyChanged |= CreationTriggerRow("drawingtrigger", strings::kCreationDrawing, Cfg().drawingTrigger,
                                     Cfg().screenshotTrigger);

    SettingsGroupBreak();

    SettingsHeading("barsheading", strings::kBarsHeading, strings::kBarsHelp);
    anyChanged |= RenderBarButtonRow("snippetbar", strings::kBarsSnippetRow, Cfg().snippetBar);
    anyChanged |= RenderBarButtonRow("drawingbar", strings::kBarsDrawingRow, Cfg().drawingBar);
}

// Two halves, like Hotkeys. On top what is global - what happens to
// deleted folders and canvases, which reports through `anyChanged` like any
// plain setting. Below the profile picker the rows a profile may state for
// itself, every one a ProfileableCheckbox, which writes and persists
// through the profile path on its own (see TrayController::
// OnSettingsChanged, which deliberately copies no behavior setting).
void OverlayApp::RenderSettingsDefaults(bool& anyChanged) {
    // One kind's rows: its shape, and its two opacities as the popover
    // shows them - the same words and the same ranges, so a default reads
    // as the setting it is a default for.
    const auto kindRows = [&anyChanged](const char* id, SnippetDefaults& kind) {
        ImGui::PushID(id);
        anyChanged |= CheckboxWithHelp("keepaspect", strings::kDefaultsKeepAspect, &kind.keepAspect,
                                       strings::kDefaultsKeepAspectHelp);
        int foregroundPct = static_cast<int>(std::round(kind.foregroundOpacity * 100.0f));
        ImGui::SetNextItemWidth(Px(160.0f));
        if (ImGui::SliderInt(Labeled(strings::kDefaultsForeground, "foreground"), &foregroundPct, 10, 100,
                             strings::kFormatPercent, ImGuiSliderFlags_AlwaysClamp)) {
            kind.foregroundOpacity = static_cast<float>(foregroundPct) / 100.0f;
        }
        anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
        int backgroundPct = static_cast<int>(std::round(kind.backgroundOpacity * 100.0f));
        ImGui::SetNextItemWidth(Px(160.0f));
        if (ImGui::SliderInt(Labeled(strings::kDefaultsBackground, "background"), &backgroundPct, 0, 100,
                             strings::kFormatPercent, ImGuiSliderFlags_AlwaysClamp)) {
            kind.backgroundOpacity = static_cast<float>(backgroundPct) / 100.0f;
        }
        anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopID();
    };

    SettingsHeading("defaultsscreenshotheading", strings::kDefaultsScreenshotHeading,
                    strings::kDefaultsScreenshotHelp);
    kindRows("screenshot", Cfg().screenshotDefaults);

    SettingsGroupBreak();

    SettingsHeading("defaultsdrawingheading", strings::kDefaultsDrawingHeading, strings::kDefaultsDrawingHelp);
    kindRows("drawing", Cfg().drawingDefaults);
    {
        float rgb[3];
        ColorRGBAToFloats(Cfg().drawingBackgroundColorRGBA, rgb);
        if (ImGui::ColorEdit3("##drawingbackgroundcolor", rgb, ImGuiColorEditFlags_NoInputs)) {
            Cfg().drawingBackgroundColorRGBA = FloatsToColorRGBA(rgb, static_cast<uint8_t>(0xFF));
        }
        // On the edit finishing, not per frame of the drag - see ColorRow.
        anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::TextUnformatted(strings::kDefaultsBackgroundColor);
    }

    SettingsGroupBreak();

    SettingsHeading("defaultstextheading", strings::kDefaultsTextHeading, strings::kDefaultsTextHelp);
    {
        // Decided on the first frame (see AppConfig::noteTextSizePx), so
        // never still 0 by the time a panel can show it.
        ImGui::SetNextItemWidth(Px(160.0f));
        ImGui::SliderFloat(Labeled(strings::kDefaultsTextSize, "defaulttextsize"), &Cfg().noteTextSizePx,
                           kNoteTextSizeMin, kNoteTextSizeMax, strings::kFormatPixels, ImGuiSliderFlags_AlwaysClamp);
        anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
        float rgba[4];
        ColorRGBAToFloats4(Cfg().noteTextColorRGBA, rgba);
        if (ImGui::ColorEdit4("##defaulttextcolor", rgba,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
                                  ImGuiColorEditFlags_AlphaPreview)) {
            Cfg().noteTextColorRGBA = FloatsToColorRGBA4(rgba);
        }
        anyChanged |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::TextUnformatted(strings::kDefaultsTextColor);
    }
}

void OverlayApp::RenderSettingsBehavior(bool& anyChanged) {
    SettingsScopeBox globalBox;
    BeginSettingsScope(globalBox, SettingsScope::Global);
    // One row: the switch, the number of days, the unit. The days are
    // disabled while the switch is off but keep their value, so turning it
    // back on brings back the period chosen before.
    SettingsHeading("deletedheading", strings::kSettingsDeletedHeading, strings::kSettingsPurgeDeletedHelp);
    anyChanged |= CheckboxWithHelp("confirmdelete", strings::kSettingsConfirmDelete, &Cfg().confirmDelete,
                                   strings::kSettingsConfirmDeleteHelp);
    anyChanged |= CheckboxWithHelp("confirmdeleteforgood", strings::kSettingsConfirmDeleteForGood,
                                   &Cfg().confirmDeleteForGood, strings::kSettingsConfirmDeleteForGoodHelp);
    anyChanged |= ImGui::Checkbox(Labeled(strings::kSettingsPurgeDeleted, "purgedeleted"), &Cfg().purgeDeleted);
    ImGui::SameLine();
    ImGui::BeginDisabled(!Cfg().purgeDeleted);
    ImGui::SetNextItemWidth(Px(110.0f));
    int days = Cfg().purgeDeletedAfterDays;
    if (ImGui::InputInt("##purgedeleteddays", &days, 1, 7)) {
        Cfg().purgeDeletedAfterDays = std::clamp(days, kPurgeDeletedAfterDaysMin, kPurgeDeletedAfterDaysMax);
        anyChanged = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(strings::kSettingsPurgeDeletedDays);
    ImGui::EndDisabled();
    EndSettingsScope(globalBox);

    SettingsScopeBox profileBox;
    BeginSettingsScope(profileBox, SettingsScope::Profile);
    // What the overlay is up over. Read-only for now, and the reason it is
    // here at all: every setting in this section is an answer to a question
    // about *that* application, and until now there was nothing on screen
    // that said which one it is. It is also the identity a per-application
    // profile will be matched on, so seeing it - including seeing it come
    // up empty for a process whose image path can't be read - is worth
    // having before anything depends on it.
    if (window_ != nullptr) {
        const platform::ForegroundApp app = window_->UnderlyingApplication();
        SettingsHeading("inputunderneathheading", strings::kInputUnderneathHeading,
                         strings::kInputUnderneathHelp);
        ImGui::AlignTextToFramePadding();
        if (!app.Known()) {
            ImGui::TextColored(theme::kGraphite300, "%s", strings::kInputNothingIdentifiable);
        } else {
            ImGui::TextUnformatted(app.executable.empty() ? strings::kInputUnreadableExecutable
                                                           : app.executable.c_str());
            if (!app.title.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(theme::kGraphite300, strings::kInputWindowTitleLine, app.title.c_str());
            }
            if (app.executable.empty()) {
                ImGui::SameLine();
                HelpMarker("inputunreadableexecutable", strings::kInputUnreadableExecutable,
                            strings::kInputUnreadableExecutableHelp);
            }
        }
        SettingsGroupBreak();
    }

    RenderEditTargetPicker(ProfileGroup::Behavior);

    SettingsHeading("inputeditmodeheading", strings::kInputEditModeHeading,
                     strings::kInputEditModeHelp);

    // The rows that need another row are laid out as the tree they are -
    // nested under what they need, tied to it by a line - so the shape of
    // the group says what a "needs X" note on every row would have to say
    // in words. The rows that need nothing sit at the bottom, unindented,
    // which is the same statement made by leaving them out of the tree.
    constexpr float kTreeIndent = 24.0f;

    // Each checkbox is disabled exactly when its own precondition fails, read
    // from EditModeInputOptions rather than restated here - the HUD grays the
    // same rows on the same answers, and two copies of these rules would
    // eventually disagree.
    const ProfileableSettings edited = EditedSettings();
    const bool keystrokesAvailable =
        platform::EditModeInputOptions::KeystrokesCanBeHeld(edited.dontStealFocus);
    const bool rawAvailable = edited.InputOptions().RawMouseInputCanBeUsed(edited.dontStealFocus);
    const bool counterAvailable =
        edited.InputOptions().CounterRawMouseInputCanBeUsed(edited.dontStealFocus);

    ImVec2 rowPos = ImGui::GetCursorScreenPos();
    ProfileableCheckbox(
        "dontstealfocus", strings::kHudDontStealFocus,
        {&ProfileableSettings::dontStealFocus, &ProfileOverrides::dontStealFocus},
        strings::kInputDontStealFocusHelp);
    float focusTrunk = TrunkFrom(rowPos);

    ImGui::Indent(Px(kTreeIndent));
    // First under the focus row because it is the exception to it, and
    // grayed out when that row is off for the same reason the others are:
    // with focus taken already there is nothing left for it to do.
    rowPos = ImGui::GetCursorScreenPos();
    ProfileableCheckbox(
        "takefocusoverelevated", strings::kInputTakeFocusOverElevatedLabel,
        {&ProfileableSettings::takeFocusOverElevated, &ProfileOverrides::takeFocusOverElevated},
        strings::kInputTakeFocusOverElevatedHelp,
        !edited.dontStealFocus);
    TreeBranch(focusTrunk, rowPos, Px(kTreeIndent));

    rowPos = ImGui::GetCursorScreenPos();
    ProfileableCheckbox(
        "dontforwardkeys", strings::kHudDontForwardKeystrokes,
        {&ProfileableSettings::dontForwardKeystrokes, &ProfileOverrides::dontForwardKeystrokes},
        strings::kInputDontForwardKeystrokesHelp,
        !keystrokesAvailable);
    TreeBranch(focusTrunk, rowPos, Px(kTreeIndent));

    rowPos = ImGui::GetCursorScreenPos();
    ProfileableCheckbox(
        "rawmouse", strings::kHudUseRawMouseInput,
        {&ProfileableSettings::rawMouseInput, &ProfileOverrides::rawMouseInput},
        strings::kInputRawMouseHelp,
        !rawAvailable);
    TreeBranch(focusTrunk, rowPos, Px(kTreeIndent));
    float rawTrunk = TrunkFrom(rowPos);

    ImGui::Indent(Px(kTreeIndent));
    rowPos = ImGui::GetCursorScreenPos();
    ProfileableCheckbox(
        "counterrawmouse", strings::kInputCounterRawMouseLabel,
        {&ProfileableSettings::counterRawMouseInput, &ProfileOverrides::counterRawMouseInput},
        strings::kInputCounterRawMouseHelp,
        !counterAvailable);
    TreeBranch(rawTrunk, rowPos, Px(kTreeIndent));
    float counterTrunk = TrunkFrom(rowPos);

    // Nested under countering, which it tunes, and grayed with it: a
    // threshold for corrections that are not being made does nothing.
    ImGui::Indent(Px(kTreeIndent));
    rowPos = ImGui::GetCursorScreenPos();
    ProfileableInt("counterthreshold", strings::kInputCounterThresholdLabel, strings::kInputCounterThresholdUnit,
                   {&ProfileableSettings::counterThreshold, &ProfileOverrides::counterThreshold},
                   platform::EditModeInputOptions::kCounterThresholdMin,
                   platform::EditModeInputOptions::kCounterThresholdMax, 10, strings::kInputCounterThresholdHelp,
                   !counterAvailable || !edited.counterRawMouseInput);
    TreeBranch(counterTrunk, rowPos, Px(kTreeIndent));

    ImGui::Unindent(Px(kTreeIndent) * 3.0f);

    // Out of the tree: nothing above it is needed for it and nothing below
    // needs it. The overlay keeps the pointer position from the mouse
    // itself and that position exists with or without a grab, so this row
    // only picks which pointer is drawn from it - see
    // EditModeInputOptions::useSoftwarePointer.
    ProfileableCheckbox(
        "softwarepointer", strings::kHudUseSoftwarePointer,
        {&ProfileableSettings::softwarePointer, &ProfileOverrides::softwarePointer},
        strings::kInputSoftwarePointerHelp);

    ProfileableCheckbox(
        "freezescreen", strings::kHudFreezeScreenWhileEditing,
        {&ProfileableSettings::freezeScreen, &ProfileOverrides::freezeScreen},
        strings::kInputFreezeScreenHelp);
    // Switching it off can take effect immediately - there is nothing to
    // capture, only something to drop. Switching it on can't: capturing
    // means hiding this window and waiting for a composition pass, which is
    // not something to do halfway through drawing a frame. That direction
    // waits for the next entry into edit mode, which the help text says out
    // loud. Read from the *resolved* value rather than from what the
    // checkbox just wrote, since the row may be editing a profile that
    // isn't the one running.
    if (!settings_.Live().freezeScreen) {
        session_.ReleaseFrozenScreen();
    }
    EndSettingsScope(profileBox);
}

void OverlayApp::RenderSettingsDebug(bool& anyChanged) {
    // The section was called "Diagnostics", which reads like something the
    // application collects about you rather than something it draws for
    // you. Everything in here is the second thing, and every row already
    // says so: they all start with "Show". Which is also why the heading
    // carries no reassurance about where any of it goes - a denial invites
    // the question it answers.
    SettingsHeading("debugheading", strings::kDebugHeading);
    anyChanged |= CheckboxWithHelp("debugshowdebugoverlay", strings::kDebugShowDebugOverlay, &Cfg().showDebugOverlay,
                                    strings::kDebugShowDebugOverlayHelp);
    anyChanged |= CheckboxWithHelp("debugshowinputhud", strings::kDebugShowInputHud, &Cfg().showInputOptionsHud,
        strings::kDebugShowInputHudHelp);
}


void OverlayApp::RenderShortcutEditor(ShortcutAction action, const Icon& icon, const char* label, float buttonX) {
    const size_t index = ShortcutActionIndex(action);
    // What the target being edited says, which is not necessarily what is
    // running - see RenderEditTargetPicker.
    const platform::KeyCombo current = EditedSettings().shortcuts[index];
    const bool overridden = IsShortcutOverriddenHere(action);
    const bool capturing = shortcutCaptureAction_ == action;

    ImGui::PushID(static_cast<int>(index));
    // The icon the same action wears on the drawing bar, so a row is
    // recognized rather than read - see kGalleryTools/kCreateActions.
    const ImVec2 iconPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(Px(18.0f), ImGui::GetFrameHeight()));
    DrawIcon(ImGui::GetWindowDrawList(), icon,
              ImVec2(iconPos.x, iconPos.y + (ImGui::GetFrameHeight() - Px(16.0f)) * 0.5f), Px(16.0f),
              ImGui::GetColorU32(theme::kGraphite100));
    ImGui::SameLine(Px(30.0f));
    ImGui::AlignTextToFramePadding();
    if (overridden) {
        ImGui::TextColored(theme::Accent(), "%s", label);
    } else {
        ImGui::TextUnformatted(label);
    }
    ImGui::SameLine(buttonX);

    if (ImGui::Button(capturing ? Labeled(strings::kHotkeysShortcutPrompt, "shortcut_btn")
                                 : (FormatKeyComboLabel(current) + "##shortcut_btn").c_str(),
                       ImVec2(Px(200.0f), 0.0f))) {
        if (capturing) {
            shortcutCaptureAction_.reset();
        } else {
            ArmShortcutCapture(action);
        }
    }
    if (ImGui::IsItemHovered() && !capturing) {
        ImGui::SetTooltip("%s", strings::kHotkeysShortcutTooltip);
    }
    if (overridden) {
        ImGui::SameLine();
        if (RevertButton("##revert")) {
            ClearShortcutOverride(action);
        }
        if (ImGui::IsItemHovered()) {
            // The one place where "(none)" was genuinely ambiguous before:
            // a row reading it could mean "inherited, nothing bound" or
            // "unbound here on purpose", and those are different things.
            ImGui::SetTooltip(strings::kHotkeysShortcutSetHere,
                               FormatKeyComboLabel(settings_.Base().shortcuts[index]).c_str());
        }
    }

    if (capturing) {
        const ImGuiIO& io = ImGui::GetIO();
        // Escape is handled in RenderOverview, before its own close check -
        // see there. Backspace and Delete mean the same thing here, since
        // they are what a hand reaches for to empty a field.
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            shortcutCaptureAction_.reset();
            SetToolShortcut(action, platform::KeyCombo{});
        } else {
            for (int k = ImGuiKey_0; k <= ImGuiKey_F24; ++k) {
                const auto imguiKey = static_cast<ImGuiKey>(k);
                if (!ImGui::IsKeyPressed(imguiKey, false)) {
                    continue;
                }
                const std::optional<platform::KeyCombo> edited =
                    ComboForImGuiKey(imguiKey, io.KeyCtrl, io.KeyAlt, io.KeyShift);
                if (!edited.has_value()) {
                    continue;
                }
                shortcutCaptureAction_.reset();
                SetToolShortcut(action, *edited);
                break;
            }
        }
    }
    ImGui::PopID();
}

void OverlayApp::RenderEditTargetPicker(ProfileGroup group) {
    const auto nameOf = [this](std::optional<size_t> target) -> std::string {
        if (!target || *target >= settings_.Profiles().size()) {
            return strings::kProfilesDefaults;
        }
        std::string label = settings_.Profiles()[*target].name;
        if (settings_.ActiveProfile() && *settings_.ActiveProfile() == *target) {
            label += strings::kProfilesCurrent;
        }
        return label;
    };

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kProfilesShowing);
    ImGui::SameLine(Px(90.0f));
    ImGui::SetNextItemWidth(Px(280.0f));
    // A profile's name is drawn as text, never passed as a label: ImGui
    // reads "##" in a label as the start of an id, so a name holding one
    // was cut short where it showed and, with "###", shared its id with any
    // other name ending the same way. Rows are told apart by index, which
    // two profiles of one name do not share either.
    if (ImGui::BeginCombo("##edittarget", nullptr, ImGuiComboFlags_CustomPreview)) {
        // Same reason as the Profiles section's own dropdown: a popup
        // nested inside a window that re-asserts itself to the front every
        // frame has to do the same, or it opens behind the panel.
        KeepPopoverInFront();
        if (ImGui::Selectable(Labeled(strings::kProfilesDefaults, "targetdefaults"), !editProfile_.has_value())) {
            editProfile_.reset();
        }
        for (size_t i = 0; i < settings_.Profiles().size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 at = ImGui::GetCursorPos();
            if (ImGui::Selectable("##target", editProfile_ && *editProfile_ == i)) {
                editProfile_ = i;
            }
            ImGui::SetCursorPos(at);
            ImGui::TextUnformatted(nameOf(i).c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // After the combo, open or not: asked between BeginCombo and the list,
    // it found the popup's item rather than the combo's while the list was
    // open, and the box showed nothing.
    if (ImGui::BeginComboPreview()) {
        ImGui::TextUnformatted(nameOf(editProfile_).c_str());
        ImGui::EndComboPreview();
    }

    // One line: whether what is on screen is also what is running, and how
    // much of it this target
    // states for itself. The rest - what a profile leaves to the defaults,
    // and what the marks below mean - is behind the "?" next to the picker.
    const char* noun = group == ProfileGroup::Behavior ? "setting" : "shortcut";
    ImGui::SameLine();
    HelpMarker("profilesshowing", strings::kProfilesShowing,
                group == ProfileGroup::Behavior
                    ? strings::kProfilesShowingSettingsHelp
                    : strings::kProfilesShowingShortcutsHelp);

    if (editProfile_ && *editProfile_ < settings_.Profiles().size()) {
        const Profile& profile = settings_.Profiles()[*editProfile_];
        const bool isActive = settings_.ActiveProfile() && *settings_.ActiveProfile() == *editProfile_;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(isActive ? theme::Accent() : theme::kGraphite300,
                            isActive ? strings::kProfilesRunningNow : strings::kProfilesNotRunning);
        // Counted for *this* section only. The total would read as a claim
        // about what is on screen, and a profile that only rebinds keys
        // would announce settings on the Behavior section with nothing marked
        // anywhere below.
        const size_t count = profile.overrides.OverriddenCount(group);
        ImGui::SameLine();
        if (count == 0) {
            ImGui::TextColored(theme::kGraphite300, "%s", strings::kProfilesSetsNothing);
        } else {
            ImGui::TextColored(theme::kGraphite300, strings::kProfilesSetsSummary, count, noun,
                                count == 1 ? "" : strings::kProfilesPluralSuffix);
        }
    } else if (settings_.ActiveProfile() && *settings_.ActiveProfile() < settings_.Profiles().size()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::kGraphite300, strings::kProfilesIsWhatsRunning,
                            settings_.Profiles()[*settings_.ActiveProfile()].name.c_str());
    } else {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::Accent(), "%s", strings::kProfilesRunningNow);
    }
    SettingsGroupBreak();
}

// The revert arrow, shown only on a row this target states for itself -
// borrowed wholesale from property editors that do the same (the row is
// marked, and the mark is the button that undoes it).
void OverlayApp::ProfileableCheckbox(const char* id, const char* label, const ProfileableField& field, const char* help,
                                      bool disabled) {
    const bool overridden = IsOverriddenHere(field);
    bool value = EditedSettings().*field.value;

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
        SetProfileableValue(field, shown);
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
            ClearProfileableOverride(field);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(strings::kHotkeysComboSetHere,
                               (settings_.Base().*field.value) ? strings::kHotkeysOn : strings::kHotkeysOff);
        }
    }
    ImGui::PopID();
}

void OverlayApp::ProfileableInt(const char* id, const char* label, const char* unit,
                                 const ProfileableIntField& field, int min, int max, int step, const char* help,
                                 bool disabled) {
    const bool overridden = IsOverriddenHere(field);
    int value = EditedSettings().*field.value;

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
        SetProfileableValue(field, std::clamp(value, min, max));
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
            ClearProfileableOverride(field);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(strings::kHotkeysComboSetHere, std::to_string(settings_.Base().*field.value).c_str());
        }
    }
    ImGui::PopID();
}

namespace {

// Labels lead their fields inside an opened profile row rather than
// trailing them, ImGui's default: those rows carry a button on the right,
// and a trailing label lands between the field and the button.
constexpr float kProfileFieldX = 120.0f;
constexpr float kProfileFieldWidth = 260.0f;

// What a closed row says about its profile, in the order the questions
// get asked: what does it catch, how much does it change.
std::string ProfileSummary(const Profile& profile) {
    std::string summary;
    if (profile.match.Empty()) {
        summary = strings::kProfilesMatchesNothing;
    } else {
        const std::vector<std::string>& first = profile.match.executables.empty()
                                                     ? profile.match.titleContains
                                                     : profile.match.executables;
        const size_t rules = profile.match.executables.size() + profile.match.titleContains.size();
        summary = first.front().empty() ? std::string(strings::kProfilesMatchesNothing) : first.front();
        if (rules > 1) {
            summary += strings::kProfilesSummaryAnd + std::to_string(rules - 1);
        }
    }
    const size_t inputCount = profile.overrides.OverriddenCount(ProfileGroup::Behavior);
    const size_t shortcutCount = profile.overrides.OverriddenCount(ProfileGroup::Shortcuts);
    // Two numbers rather than one: Input and Shortcuts are two different
    // places to go and change them.
    if (inputCount == 0 && shortcutCount == 0) {
        summary += strings::kProfilesChangesNothing;
        return summary;
    }
    summary += strings::kProfilesSummarySeparator;
    if (inputCount > 0) {
        summary += std::to_string(inputCount) + (inputCount == 1 ? strings::kProfilesSettingWord : strings::kProfilesSettingsWord);
    }
    if (inputCount > 0 && shortcutCount > 0) {
        summary += strings::kProfilesSummaryJoin;
    }
    if (shortcutCount > 0) {
        summary += std::to_string(shortcutCount) + (shortcutCount == 1 ? strings::kProfilesShortcutWord : strings::kProfilesShortcutsWord);
    }
    return summary;
}

// A labeled list of text fields, one per entry, each with a remove
// button, and an add button after the last. Returns whether the list
// changed.
bool EditStringList(const char* label, const char* addLabel, const char* id, std::vector<std::string>& list) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGraphite200, "%s", label);
    for (size_t entry = 0; entry < list.size(); ++entry) {
        ImGui::PushID(static_cast<int>(entry));
        char text[260] = {};
        std::snprintf(text, sizeof(text), "%s", list[entry].c_str());
        // The first field shares the label's line; every one after it
        // starts its own, lined up under the first.
        if (entry == 0) {
            ImGui::SameLine(Px(kProfileFieldX));
        } else {
            ImGui::SetCursorPosX(Px(kProfileFieldX));
        }
        ImGui::SetNextItemWidth(Px(kProfileFieldWidth));
        if (ImGui::InputText("##entry", text, sizeof(text))) {
            list[entry] = text;
            changed = true;
        }
        ImGui::SameLine();
        const bool removed = ImGui::SmallButton("x");
        ImGui::PopID();
        if (removed) {
            list.erase(list.begin() + static_cast<long>(entry));
            changed = true;
            break;
        }
    }
    if (list.empty()) {
        ImGui::SameLine(Px(kProfileFieldX));
    } else {
        ImGui::SetCursorPosX(Px(kProfileFieldX));
    }
    if (ImGui::SmallButton(addLabel)) {
        list.emplace_back();
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

}  // namespace

void OverlayApp::RenderSettingsProfiles() {
    // Every edit goes into this copy and is handed back once, at the end -
    // so nothing here iterates a list the callback may have replaced.
    std::vector<Profile> edited = settings_.Profiles();
    bool changed = false;

    SettingsHeading("settingstabprofiles", strings::kSettingsTabProfiles,
        strings::kProfilesHelp);
    changed |= RenderProfileMakers(edited);

    ImGui::Spacing();
    ImGui::Separator();

    if (edited.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(theme::kGraphite300, "%s", strings::kProfilesNone);
    }

    // One row per profile, closed: the name, what it matches and what it
    // sets, which is the whole of what this list is for - "which profiles
    // are there" answered by reading down a column rather than by scrolling
    // past four fields apiece. Everything editable is inside the row and
    // costs one click to reach.
    std::optional<size_t> removeIndex;
    for (size_t i = 0; i < edited.size(); ++i) {
        bool remove = false;
        changed |= RenderProfileRow(i, edited[i], remove);
        if (remove) {
            removeIndex = i;
        }
        ImGui::Separator();
    }

    if (removeIndex.has_value()) {
        edited.erase(edited.begin() + static_cast<long>(*removeIndex));
        changed = true;
        // The target is an index, so it follows its profile down the list
        // when one above it goes, and is let go of when its own does.
        // Checked only for running off the end, it stayed where it was and
        // landed on the next profile, which every later edit then went to.
        if (editProfile_ && *editProfile_ == *removeIndex) {
            editProfile_.reset();
        } else if (editProfile_ && *editProfile_ > *removeIndex) {
            --*editProfile_;
        }
    }

    if (changed) {
        // Which profile matches may have changed; the settings work that
        // out as they commit.
        settings_.SetProfiles(std::move(edited));
    }
}

bool OverlayApp::RenderProfileMakers(std::vector<Profile>& edited) {
    bool changed = false;
    ImGui::AlignTextToFramePadding();
    if (!settings_.UnderlyingApplication().Known()) {
        ImGui::TextColored(theme::kGraphite300, "%s", strings::kProfilesNothingToMake);
    } else {
        const platform::ForegroundApp& app = settings_.UnderlyingApplication();
        const std::string label = app.executable.empty() ? app.title : app.executable;
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        if (ImGui::Button(Labeled(strings::kProfilesMakeForThis, "makeprofile"))) {
            Profile profile;
            // Named after the window's title where there is one - "Elden
            // Ring" reads better in this list than "eldenring.exe" - but
            // matched on the executable, which is the stable half.
            profile.name = UniqueProfileName(edited, app.title.empty() ? app.executable : app.title);
            if (!app.executable.empty()) {
                profile.match.executables.push_back(app.executable);
            } else {
                profile.match.titleContains.push_back(app.title);
            }
            edited.push_back(std::move(profile));
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", strings::kProfilesMakeForThisTooltip);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Labeled(strings::kProfilesNewProfile, "newprofile"))) {
        Profile profile;
        profile.name = UniqueProfileName(edited, strings::kProfilesNamePrefix);
        edited.push_back(std::move(profile));
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kProfilesNewProfileTooltip);
    }
    return changed;
}

bool OverlayApp::RenderProfileRow(size_t index, Profile& profile, bool& remove) {
    // Expanded in place rather than in a modal, because more than one
    // question gets asked of this list at a time ("does anything else
    // already match this executable?") and a dialog answers exactly one.
    // ImGui keeps the open/closed state per node ID, so it is not a member
    // here; the ID is the row's index, which means deleting a row can leave
    // the one that takes its place open. That is a wrong answer for one
    // frame and self-correcting, where an ID from the name would collapse
    // the row on every keystroke into the name field inside it.
    //
    // That index goes into the row's own id string rather than into a
    // PushID around the whole body, which leaves the fields inside an open
    // row scoped by the node itself. They are then addressable relative to
    // the row - which is how a test reaches one row's name field rather
    // than another's, the row having been found by the name it shows.
    bool changed = false;
    char rowId[32] = {};
    std::snprintf(rowId, sizeof(rowId), "##row%zu", index);

    const bool isActive = settings_.ActiveProfile().has_value() && *settings_.ActiveProfile() == index;
    // The whole row is the hit target for opening it, and the delete
    // button sits on top of that - which needs saying, or the row
    // underneath swallows the click.
    ImGui::SetNextItemAllowOverlap();
    if (isActive) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    }
    const bool open = ImGui::TreeNodeEx(rowId, ImGuiTreeNodeFlags_SpanAvailWidth, "%s",
                                         profile.name.empty() ? strings::kProfilesUnnamed : profile.name.c_str());
    if (isActive) {
        ImGui::PopStyleColor();
    }

    // What the row says about itself: is this the one running, and then
    // what it catches and how much it changes. Dim, because the name is
    // what you are scanning for.
    if (isActive) {
        ImGui::SameLine();
        ImGui::TextColored(theme::Accent(), "%s", strings::kProfilesRunningNow);
    }
    const std::string summary = ProfileSummary(profile);
    ImGui::SameLine();
    ImGui::TextColored(theme::kGraphite300, "%s", summary.c_str());

    // Right-aligned, so the buttons line up down the list however long
    // the names and summaries are.
    const float buttonWidth = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth);
    char deleteId[32] = {};
    std::snprintf(deleteId, sizeof(deleteId), "##delprofile%zu", index);
    if (DangerIconButton(deleteId, icons::kTrash)) {
        remove = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kProfilesDeleteThis);
    }

    if (!open) {
        return changed;
    }
    ImGui::Spacing();
    char name[128] = {};
    std::snprintf(name, sizeof(name), "%s", profile.name.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kProfilesName);
    ImGui::SameLine(Px(kProfileFieldX));
    ImGui::SetNextItemWidth(Px(kProfileFieldWidth));
    // Never stored empty: while the field is cleared to type a new name the
    // profile keeps its old one, and a field left empty shows it again.
    if (ImGui::InputText("##name", name, sizeof(name)) && name[0] != '\0') {
        profile.name = name;
        changed = true;
    }
    changed |= EditStringList(strings::kProfilesApplications, strings::kProfilesAddApplication, "exes",
                              profile.match.executables);
    changed |= EditStringList(strings::kProfilesTitleContains, strings::kProfilesAddTitle, "titles",
                              profile.match.titleContains);
    if (profile.match.Empty()) {
        ImGui::TextColored(theme::kGraphite300, "%s", strings::kProfilesNothingToMatchOn);
    }
    ImGui::Spacing();
    ImGui::TreePop();
    return changed;
}

// Where every key button in the Hotkeys section starts: past the longest
// label there, measured rather than fixed, since a fixed column is what
// "New canvas with the selection" ran into. One column for the summon
// hotkeys and the shortcuts alike, so the two lists line up.
float OverlayApp::KeyButtonColumn() const {
    constexpr float kShortcutLabelX = 30.0f;  // past the row's icon - see RenderShortcutEditor
    constexpr float kGap = 16.0f;
    float widest = 0.0f;
    for (const char* label : {strings::kHotkeysEditMode, strings::kHotkeysViewMode, strings::kHotkeysQuickCapture,
                              strings::kHotkeysSilentCapture}) {
        widest = std::max(widest, ImGui::CalcTextSize(label).x);
    }
    const auto shortcutLabel = [&widest](const char* label) {
        widest = std::max(widest, Px(kShortcutLabelX) + ImGui::CalcTextSize(label).x);
    };
    for (const GalleryTool& tool : kGalleryTools) {
        shortcutLabel(tool.name);
    }
    for (const CreateActionInfo& info : kCreateActions) {
        shortcutLabel(info.name);
    }
    for (const ClipboardActionInfo& info : kClipboardActions) {
        shortcutLabel(info.name);
    }
    shortcutLabel(strings::kMenuCheatSheet);
    return widest + Px(kGap);
}

void OverlayApp::RenderSettingsHotkeys(bool& anyChanged) {
    // Above the picker, and that position is the whole point of moving them
    // here: these are global, and while they sat under a picker that said
    // "Showing: Defaults / some profile" they read as more rows that
    // profile could change. A control that governs what is below it has to
    // have nothing above it that it doesn't govern.
    const float buttonX = KeyButtonColumn();
    SettingsScopeBox globalBox;
    BeginSettingsScope(globalBox, SettingsScope::Global);
    SettingsHeading("hotkeyssummoningheading", strings::kHotkeysSummoningHeading,
                     strings::kHotkeysSummoningHelp);
    RenderHotkeyEditor("hkedit", strings::kHotkeysEditMode, HotkeySlot::EditMode, Cfg().hotkeyEditMode, buttonX);
    RenderHotkeyEditor("hkview", strings::kHotkeysViewMode, HotkeySlot::ViewMode, Cfg().hotkeyViewMode, buttonX);
    RenderHotkeyEditor("hkquick", strings::kHotkeysQuickCapture, HotkeySlot::QuickCapture, Cfg().hotkeyQuickCapture,
                       buttonX);
    RenderHotkeyEditor("hksilent", strings::kHotkeysSilentCapture, HotkeySlot::SilentCapture,
                       Cfg().hotkeySilentCapture, buttonX);
    anyChanged |= CheckboxWithHelp("hotkeyssaywhenhidden", strings::kHotkeysSayWhenHidden, &Cfg().showToastsWhileHidden,
        strings::kHotkeysSayWhenHiddenHelp);
    EndSettingsScope(globalBox);

    SettingsScopeBox profileBox;
    BeginSettingsScope(profileBox, SettingsScope::Profile);
    RenderEditTargetPicker(ProfileGroup::Shortcuts);

    SettingsHeading("hotkeysshortcuttoolsheading", strings::kHotkeysShortcutToolsHeading,
                     strings::kHotkeysShortcutsHelp);
    ImGui::Spacing();
    for (const GalleryTool& tool : kGalleryTools) {
        RenderShortcutEditor(ShortcutForTool(tool.tool), *tool.icon, tool.name, buttonX);
    }

    SettingsGroupBreak();
    SettingsHeading("hotkeysshortcutcreateheading", strings::kHotkeysShortcutCreateHeading);
    ImGui::Spacing();
    for (const CreateActionInfo& info : kCreateActions) {
        RenderShortcutEditor(ShortcutForCreateAction(info.action), *info.icon, info.name, buttonX);
    }

    SettingsGroupBreak();
    SettingsHeading("hotkeysshortcutclipboardheading", strings::kHotkeysShortcutClipboardHeading);
    ImGui::Spacing();
    for (const ClipboardActionInfo& info : kClipboardActions) {
        RenderShortcutEditor(ShortcutForClipboardAction(info.action), *info.icon, info.name, buttonX);
    }

    SettingsGroupBreak();
    SettingsHeading("hotkeysshortcuthelpheading", strings::kHotkeysShortcutHelpHeading);
    ImGui::Spacing();
    RenderShortcutEditor(ShortcutAction::CheatSheet, icons::kKeyboard, strings::kMenuCheatSheet, buttonX);
    EndSettingsScope(profileBox);
}

namespace {
// What this tab shows: the component and the license it is under, in the
// order they matter to someone glancing at it. The full texts are in
// THIRD-PARTY-NOTICES.md, which is where they have to be complete - this
// list is the summary, and is the reason most people never open the other.
struct BuiltWithRow {
    const char* component;
    const char* license;
};
const BuiltWithRow kBuiltWith[] = {
    {strings::kAboutComponentImgui, strings::kAboutLicenseMit},
    {strings::kAboutComponentJson, strings::kAboutLicenseMit},
    {strings::kAboutComponentStb, strings::kAboutLicenseMitOrPublicDomain},
    {strings::kAboutComponentQoi, strings::kAboutLicenseMit},
    {strings::kAboutComponentManrope, strings::kAboutLicenseOfl},
    {strings::kAboutComponentIcons, strings::kAboutLicenseIscMit},
};

// ABOUT.md and THIRD-PARTY-NOTICES.md, rendered with just enough Markdown
// awareness to look like prose rather than like a file someone forgot to
// format: headings and bullets, nothing else. A real Markdown renderer
// would be a project of its own (ImGui has none built in), and the
// alternative - dumping the raw text with its '#' and '-' prefixes intact -
// looks like a bug. Whoever edits either file gets a live preview by
// opening this tab, so the limits are self-evident rather than needing to
// be documented somewhere they'd be missed.
//
// Rendered by *paragraph*, not by line. Both files are hard-wrapped at
// around 76 columns like any readable source file, so drawing a line at a
// time would reproduce those breaks verbatim - a narrow column stranded in
// a wide panel, re-wrapped at the wrong width whenever the panel is a
// different size than the author's editor was. Consecutive lines are
// joined back into one string and handed to ImGui to wrap, which is also
// what makes a bullet's second line line up under its first: it's one
// wrapped item rather than two unrelated ones.
void RenderMarkdownSubset(std::string_view document) {
    constexpr float kBulletIndent = 16.0f;
    std::string paragraph;
    bool paragraphIsBullet = false;
    const auto flush = [&]() {
        if (paragraph.empty()) {
            return;
        }
        if (paragraphIsBullet) {
            ImGui::Indent(Px(kBulletIndent));
            ImGui::Bullet();
            // Bullet() already ends flush against whatever follows it, so
            // this gap is the whole separation between the dot and the
            // word - 4px read as the two touching.
            ImGui::SameLine(0.0f, Px(10.0f));
            ImGui::TextUnformatted(paragraph.data(), paragraph.data() + paragraph.size());
            ImGui::Unindent(Px(kBulletIndent));
        } else {
            ImGui::TextUnformatted(paragraph.data(), paragraph.data() + paragraph.size());
        }
        paragraph.clear();
        paragraphIsBullet = false;
    };

    size_t pos = 0;
    while (pos <= document.size()) {
        const size_t eol = document.find('\n', pos);
        std::string_view line = document.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? document.size() + 1 : eol + 1;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        std::string_view trimmed = line;
        while (!trimmed.empty() && trimmed.front() == ' ') {
            trimmed.remove_prefix(1);
        }

        if (trimmed.empty()) {
            // A blank line is Markdown's paragraph break, and the only
            // thing that ends one here.
            flush();
            ImGui::Spacing();
            continue;
        }
        size_t hashes = 0;
        while (hashes < trimmed.size() && trimmed[hashes] == '#') {
            ++hashes;
        }
        if (hashes > 0 && hashes < trimmed.size() && trimmed[hashes] == ' ') {
            flush();
            const std::string heading(trimmed.substr(hashes + 1));
            ImGui::Spacing();
            // Only the top level gets larger type; deeper ones stay at body
            // size and lean on color alone, so a document with four levels
            // doesn't turn into four competing sizes.
            if (hashes == 1) {
                ImGui::PushFont(nullptr, 22.0f);
            }
            ImGui::TextColored(theme::Accent(), "%s", heading.c_str());
            if (hashes == 1) {
                ImGui::PopFont();
            }
            ImGui::Spacing();
            continue;
        }
        if (trimmed.size() > 2 && trimmed[0] == '-' && trimmed[1] == ' ') {
            flush();
            paragraphIsBullet = true;
            paragraph.assign(trimmed.substr(2));
            continue;
        }
        // Anything else continues whatever is being built - a bullet's
        // indented second line, or the next line of a paragraph.
        if (!paragraph.empty()) {
            paragraph += ' ';
        }
        paragraph.append(trimmed);
    }
    flush();
}
}  // namespace

void OverlayApp::SwitchOverviewTab(OverviewTab tab) {
    if (overviewTab_ == tab) {
        return;
    }
    overviewTab_ = tab;
    overviewBodyScrollToTop_ = true;
    // Leaving About also leaves its license page: coming back to a tab
    // that is still showing somebody else's MIT text, several tabs later,
    // is not a place anyone meant to return to.
    aboutShowsNotices_ = false;
}

void OverlayApp::RenderOverviewAboutPanel() {
    // 0.0f means "wrap at the window's own right edge", which is what this
    // wants. PushTextWrapPos takes a window-local X *position*, not a
    // width, so passing GetContentRegionAvail().x - the obvious-looking
    // thing, and what the Settings panel happens to get away with - wraps
    // at roughly half the width once the panel is wide.
    ImGui::PushTextWrapPos(0.0f);

    if (aboutShowsNotices_) {
        // The licenses, in the same panel and the same scroll region as
        // the About text rather than in a popup: this is a page you read,
        // not a thing you act on, and the Overview has enough windows
        // stacked over it already (see KeepPopoverInFront). The way back
        // is in the panel's footer, which does not scroll.
        RenderMarkdownSubset(build::NoticesText());
        ImGui::PopTextWrapPos();
        return;
    }

    // Which build this is, first and plainly - it's the one thing someone
    // opens this tab to find, and the reason the tab is worth having at all
    // when a tester needs to say which binary they were using.
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kAboutVersion);
    ImGui::SameLine(Px(90.0f));
    const std::string version = build::VersionLine();
    ImGui::TextUnformatted(version.c_str());
    // Whose it is, right under what it is. This one stays in plain sight
    // rather than behind the button below: it is the app saying who owns
    // it, which is a different job from reproducing other people''s terms.
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kAboutCopyrightLabel);
    ImGui::SameLine(Px(90.0f));
    ImGui::TextUnformatted(strings::kAboutCopyright);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    RenderMarkdownSubset(build::AboutText());

    // What is inside this binary that somebody else wrote. The list is
    // short enough to read at a glance and is the part most people want;
    // the licenses themselves are long enough that they would bury the
    // rest of this tab, so they are one click away.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushFont(nullptr, 22.0f);
    ImGui::TextColored(theme::Accent(), "%s", strings::kAboutBuiltWith);
    ImGui::PopFont();
    ImGui::Spacing();
    for (const BuiltWithRow& row : kBuiltWith) {
        ImGui::TextUnformatted(row.component);
        ImGui::SameLine(Px(260.0f));
        ImGui::TextColored(theme::kGraphite300, "%s", row.license);
    }

    ImGui::PopTextWrapPos();
}

// `current` is passed by value, fresh every call, rather than read off
// `this` directly - so a rejected edit (see TryChangeHotkey) has nothing
// to undo: the next frame's `current` is still whatever was last actually
// committed, and the widgets below just render that again, snapping the
// UI back to it on their own.
void OverlayApp::RenderHotkeyEditor(const char* id, const char* label, HotkeySlot slot, platform::KeyCombo current,
                                    float buttonX) {
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(buttonX);

    const bool capturing = hotkeyCaptureSlot_ == slot;
    if (ImGui::Button(capturing ? Labeled(strings::kHotkeysComboPrompt, "combo_btn")
                                 : (FormatKeyComboLabel(current) + "##combo_btn").c_str(),
                       ImVec2(Px(200.0f), 0.0f))) {
        // Clicking the armed row's own button cancels capture instead of
        // re-arming it. Escape does too, but only as a side effect of
        // closing the whole Overview - RenderOverview's own Escape-to-close
        // check runs before this function ever does, every frame, so a
        // dedicated Escape-cancels-just-the-capture check here would never
        // actually run (see CloseOverview, which resets hotkeyCaptureSlot_
        // for exactly this reason).
        if (capturing) {
            hotkeyCaptureSlot_.reset();
        } else {
            ArmHotkeyCapture(slot);
        }
    }
    if (ImGui::IsItemHovered() && !capturing) {
        ImGui::SetTooltip("%s", strings::kHotkeysComboTooltip);
    }

    if (capturing) {
        const ImGuiIO& io = ImGui::GetIO();
        for (int k = ImGuiKey_0; k <= ImGuiKey_F24; ++k) {
            const auto imguiKey = static_cast<ImGuiKey>(k);
            if (!ImGui::IsKeyPressed(imguiKey, false)) {
                continue;
            }
            const std::optional<platform::KeyCombo> edited =
                ComboForImGuiKey(imguiKey, io.KeyCtrl, io.KeyAlt, io.KeyShift);
            if (!edited.has_value()) {
                continue;
            }
            CompleteHotkeyCapture(*edited);
            break;
        }
    }
    ImGui::PopID();
}

void OverlayApp::ArmHotkeyCapture(HotkeySlot slot) {
    hotkeyCaptureSlot_ = slot;
    shortcutCaptureAction_.reset();
}

void OverlayApp::ArmShortcutCapture(ShortcutAction action) {
    shortcutCaptureAction_ = action;
    hotkeyCaptureSlot_.reset();
}

void OverlayApp::CompleteHotkeyCapture(platform::KeyCombo combo) {
    if (!hotkeyCaptureSlot_.has_value()) {
        return;
    }
    const HotkeySlot slot = *hotkeyCaptureSlot_;
    hotkeyCaptureSlot_.reset();
    if (!TryChangeHotkey(slot, combo)) {
        ShowActionToast(strings::kHotkeysComboRejected);
    }
}

// See SetHotkeyChangeCallback's own doc comment for why this is "ask
// first, commit after" rather than the anyChanged/Settings::Commit shape
// every other setting in this file uses: a hotkey has a real way to
// fail (already taken) that only the OS - reached only through
// TrayController, which OverlayApp has no direct access to - can tell you
// about.
bool OverlayApp::TryChangeHotkey(HotkeySlot slot, platform::KeyCombo combo) {
    platform::KeyCombo* field = nullptr;
    switch (slot) {
        case HotkeySlot::EditMode:
            field = &Cfg().hotkeyEditMode;
            break;
        case HotkeySlot::ViewMode:
            field = &Cfg().hotkeyViewMode;
            break;
        case HotkeySlot::QuickCapture:
            field = &Cfg().hotkeyQuickCapture;
            break;
        case HotkeySlot::SilentCapture:
            field = &Cfg().hotkeySilentCapture;
            break;
    }
    // Offered even when unchanged: the combo it already has may be one
    // that never registered, and picking it again is how to try again.
    if (hotkeyChangeCallback_ && !hotkeyChangeCallback_(slot, combo)) {
        return false;
    }
    *field = combo;
    return true;
}

void OverlayApp::RenderConfirmDeletePopover() {
    // Consume the deferred-open request first - see
    // confirmDeletePopoverRequested_'s own doc comment. Not strictly
    // required here the way it is for the properties popover (a Delete
    // button click always happens from inside a valid ImGui frame, never
    // the raw platform callback), but keeping the same request-flag shape
    // as RenderItemPropertiesPopover means this
    // popup's own OpenPopup/BeginPopup pair never has to worry about the
    // Overview's own PushID nesting around the button that requested it.
    if (confirmDeletePopoverRequested_) {
        confirmDeletePopoverRequested_ = false;
        // Not asked at all where Settings > Behavior says not to: done here
        // rather than at each button, where the Overview is still being
        // drawn from what the delete changes - the reason the request is
        // deferred in the first place.
        if (confirmDeleteTarget_.has_value()) {
            const ConfirmDeleteTarget& target = *confirmDeleteTarget_;
            const bool forGood = target.forGood || target.kind == ConfirmDeleteTarget::Kind::DeletedCanvasesIn;
            if (!(forGood ? Cfg().confirmDeleteForGood : Cfg().confirmDelete)) {
                const ConfirmDeleteTarget unasked = target;
                confirmDeleteTarget_.reset();
                PerformDelete(unasked);
                return;
            }
        }
        ImGui::OpenPopup("##confirm_delete_popover");
    }

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopup("##confirm_delete_popover")) {
        return;
    }
    KeepPopoverInFront();
    if (!confirmDeleteTarget_.has_value()) {
        ImGui::EndPopup();
        return;
    }
    const ConfirmDeleteTarget target = *confirmDeleteTarget_;
    const bool isFolder = target.kind == ConfirmDeleteTarget::Kind::Folder;
    const bool deletedIn = target.kind == ConfirmDeleteTarget::Kind::DeletedCanvasesIn;
    const char* word = isFolder ? strings::kDeleteConfirmFolderWord : strings::kDeleteConfirmCanvasWord;
    // A delete marks the thing, which can be restored, and says so; a delete
    // of something deleted already is for good, and says that.
    const bool forGood = target.forGood || deletedIn;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(220.0f));
    if (deletedIn) {
        ImGui::Text(strings::kDeleteConfirmPromptDeletedIn, target.name.c_str());
    } else {
        ImGui::Text(forGood ? strings::kDeleteConfirmPromptForGood : strings::kDeleteConfirmPrompt, word,
                    target.name.c_str());
    }
    if (isFolder) {
        ImGui::TextColored(theme::kDanger, "%s", strings::kDeleteConfirmAlsoCanvases);
    }
    // With the retention period on, "can be restored" has an end, and says
    // when: the dialog is where a person decides how much that matters.
    if (forGood) {
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kDeleteConfirmCannotUndo);
    } else if (Cfg().purgeDeleted) {
        ImGui::TextColored(theme::kGraphite200, strings::kDeleteConfirmRestorableFor, Cfg().purgeDeletedAfterDays);
    } else {
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kDeleteConfirmRestorable);
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const bool cancelPressed = ImGui::Button(Labeled(strings::kDeleteConfirmCancel, strings::kMoveCopyCancel));
    ImGui::SameLine();
    const bool deletePressed = DangerButton("##confirmdelete", icons::kTrash,
                                            forGood ? strings::kDeleteConfirmDeleteForGood : strings::kDeleteConfirmDelete);
    // CloseCurrentPopup must be called while this popup is still current -
    // i.e. before EndPopup, not after (it operates on the popup ID stack,
    // which EndPopup pops) - the actual state mutation below happens after
    // EndPopup instead, matching this file's established pattern of not
    // touching canvas/folder data while a window built from it is still
    // mid-render.
    if (cancelPressed || deletePressed) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();

    if (deletePressed) {
        PerformDelete(target);
    }
    if (cancelPressed || deletePressed) {
        confirmDeleteTarget_.reset();
    }
}

void OverlayApp::PerformDelete(const ConfirmDeleteTarget& target) {
    const bool forGood = target.forGood || target.kind == ConfirmDeleteTarget::Kind::DeletedCanvasesIn;
    const bool deletedIn = target.kind == ConfirmDeleteTarget::Kind::DeletedCanvasesIn;
    // Its textures go as it leaves the screen, either way - see
    // Session::Delete and DeletePermanently - and its history only with the
    // thing itself, for good.
    if (forGood) {
        if (deletedIn ? session_.DeleteMarkedCanvasesPermanently(target.id) : session_.DeletePermanently(target.id)) {
            ShowActionToast(strings::kToastDeletedForGood);
        }
    } else if (session_.Delete(target.id)) {
        ShowActionToast(strings::kToastDeleted);
    }
}

void OverlayApp::RenderActionToast() {
    if (actionToastText_.empty() || ImGui::GetTime() >= actionToastExpireAtSeconds_) {
        return;
    }
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(actionToastText_.c_str());
    constexpr float kPaddingX = 16.0f;
    constexpr float kPaddingY = 9.0f;
    const ImVec2 boxSize(textSize.x + Px(kPaddingX) * 2.0f, textSize.y + Px(kPaddingY) * 2.0f);
    const ImVec2 boxMin((ImGui::GetIO().DisplaySize.x - boxSize.x) * 0.5f, Px(22.0f));
    const ImVec2 boxMax(boxMin.x + boxSize.x, boxMin.y + boxSize.y);
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(18, 20, 26, 235), 999.0f);
    drawList->AddText(ImVec2(boxMin.x + Px(kPaddingX), boxMin.y + Px(kPaddingY)), IM_COL32(240, 242, 245, 255),
                       actionToastText_.c_str());
}

std::string OverlayApp::PersistenceWarning() const {
    std::string warning;
    char line[1024];
    if (session_.LastWriteFailed() && session_.Store() != nullptr) {
        std::snprintf(line, sizeof(line), strings::kStatusWriteFailed, session_.Store()->File().string().c_str());
        warning = line;
    }
    if (configWriteFailedPath_.has_value()) {
        std::snprintf(line, sizeof(line), strings::kStatusConfigWriteFailed, configWriteFailedPath_->c_str());
        if (!warning.empty()) {
            warning += "\n";
        }
        warning += line;
    }
    return warning;
}

void OverlayApp::RenderPersistenceWarning() {
    const std::string warning = PersistenceWarning();
    if (warning.empty()) {
        return;
    }
    // Along the bottom, out from under the canvas bar's own reveal zone and
    // away from the toast at the top, in the toast's own colors but with a
    // warning tint behind the text: this one does not go away by itself.
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(warning.c_str());
    constexpr float kPaddingX = 16.0f;
    constexpr float kPaddingY = 9.0f;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 boxSize(textSize.x + Px(kPaddingX) * 2.0f, textSize.y + Px(kPaddingY) * 2.0f);
    const ImVec2 boxMin((display.x - boxSize.x) * 0.5f, display.y - boxSize.y - Px(64.0f));
    const ImVec2 boxMax(boxMin.x + boxSize.x, boxMin.y + boxSize.y);
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(92, 40, 20, 235), Px(8.0f));
    drawList->AddText(ImVec2(boxMin.x + Px(kPaddingX), boxMin.y + Px(kPaddingY)), IM_COL32(255, 232, 210, 255),
                       warning.c_str());
}

void OverlayApp::OpenOverview() {
    pickerItemId_.reset();
    overviewOpen_ = true;
    overviewTab_ = OverviewTab::Canvases;
    showDeleted_ = false;
    deletedFolderShown_.reset();
    if (displayListCallback_) {
        displays_ = displayListCallback_();
    }
}

void OverlayApp::OpenPicker(ItemId itemId, bool isCopy) {
    pickerItemId_ = itemId;
    pickerIsCopy_ = isCopy;
    overviewOpen_ = true;
}

void OverlayApp::CloseOverview() {
    overviewOpen_ = false;
    pickerItemId_.reset();
    // A rename field gives the keyboard back when ImGui deactivates it (see
    // the InputText sites in RenderOverview). A mode switch closes the
    // Overview between frames, so the field is never rendered again and
    // that frame never comes - without this the keyboard stays borrowed
    // from the game for the rest of the session. ReleaseTextInput is
    // idempotent, so the ordinary route running as well costs nothing.
    const bool wasRenaming = renamingFolderId_.has_value() || renamingCanvasId_.has_value();
    renamingFolderId_.reset();
    renamingCanvasId_.reset();
    if (wasRenaming && window_) {
        window_->ReleaseTextInput();
    }
    // RenderOverview's own Escape-to-close check (above in this file) runs
    // before RenderOverviewSettingsPanel/RenderHotkeyEditor ever get a
    // chance to see that same keypress, so a hotkey row armed for capture
    // has no way to notice Escape closed the whole panel out from under it
    // - reset it here instead, or it would sit silently armed, capturing
    // whatever key is pressed next time Settings reopens.
    hotkeyCaptureSlot_.reset();
    // Same for an armed Shortcuts row - though that one has to survive
    // Escape *itself* (see RenderOverview), so what this covers is the
    // panel being closed some other way with a row still waiting.
    shortcutCaptureAction_.reset();
    // The thumbnails' own textures go with the panel. They exist to be
    // looked at, and a library's worth of them held for a panel nobody has
    // open is exactly the memory this app spent stage A learning not to
    // hold - see PicturePreview.
    ReleasePicturePreviews();
}

void OverlayApp::ShowActionToast(std::string text) {
    // Every other call site runs from inside RenderOverview, always mid-
    // frame with a live context - but QuickCapture can now call this
    // before the overlay has ever been shown at all this session (e.g.
    // the quick-capture hotkey pressed first, while still hidden), and a
    // backend's EnsureCreated() isn't contractually guaranteed to have
    // created one yet either. ImGui::GetTime() dereferences the current
    // context unconditionally, so calling it with none set is a crash, not
    // a graceful no-op - the toast just wouldn't be visible yet anyway.
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    actionToastText_ = std::move(text);
    actionToastExpireAtSeconds_ = ImGui::GetTime() + 2.2;
}

void OverlayApp::DeleteItemsWithToast(const std::vector<ItemId>& itemIds) {
    // Safe to call directly from the selection bar's Close button, which
    // fires from the raw mouse pipeline (unlike an OpenPopup, which the
    // bar defers via a request flag - see colorChooserRequested_'s own
    // doc comment): nothing here touches
    // ImGui's current-window/ID-stack state, only plain data
    // (CanvasManager, undoStack_) and ImGui::GetTime() (a flat context
    // field read, not window-stack-dependent - see ShowActionToast's own
    // comment on why it's safe with no frame in progress).
    // Marked, and onto the history as one step - see Session::DeleteItems.
    if (session_.DeleteItems(itemIds) == 0) {
        return;
    }
    ShowActionToast(strings::kToastDeleted);
}

void OverlayApp::ClearItemDrawing(ItemId itemId) {
    // Every stroke, as one undoable step - see Session::ClearDrawing.
    if (session_.ClearDrawing(itemId)) {
        ShowActionToast(strings::kToastClearedDrawing);
    }
}


}  // namespace sz::ui
