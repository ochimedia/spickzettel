#include "ui/view/settings_page.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "core/config/display_choice.h"
#include "ui/icons_generated.h"
#include "core/util/timestamp_name.h"
#include "generated/ui_strings.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "ui/settings_widgets.h"

#include <imgui.h>
// For the combo preview the edit-target picker draws a profile's name in -
// see RenderEditTargetPicker.
#include <imgui_internal.h>

namespace sz::ui {

using namespace ::sz::core;

const GalleryTool kGalleryTools[6] = {
    {Tool::Draw, &icons::kPen, strings::kToolDraw, strings::kToolDrawTip},
    {Tool::Erase, &icons::kEraser, strings::kToolErase, strings::kToolEraseTip},
    {Tool::Text, &icons::kType, strings::kToolText, strings::kToolTextTip},
    {Tool::Select, &icons::kSelect, strings::kToolSelect, strings::kToolSelectTip},
    {Tool::NewScreenshot, &icons::kCamera, strings::kToolNewScreenshot, strings::kToolNewScreenshotTip},
    {Tool::NewDrawing, &icons::kNote, strings::kToolNewDrawing, strings::kToolNewDrawingTip},
};

const CreateActionInfo kCreateActions[2] = {
    {CreateAction::NewCanvas, &icons::kPlus, strings::kCreateNewCanvas, strings::kCreateNewCanvas},
    {CreateAction::NewCanvasWithSelection, &icons::kPlus, strings::kCreateNewCanvasWithSelection,
     strings::kCreateNewCanvasWithSelection},
};

const ClipboardActionInfo kClipboardActions[4] = {
    {ClipboardAction::Copy, &icons::kCopy, strings::kClipboardCopy},
    {ClipboardAction::Cut, &icons::kScissors, strings::kClipboardCut},
    {ClipboardAction::Paste, &icons::kClipboard, strings::kClipboardPaste},
    {ClipboardAction::Duplicate, &icons::kCopy, strings::kClipboardDuplicate},
};

ShortcutAction ShortcutForTool(Tool tool) {
    switch (tool) {
        case Tool::Draw:
            return ShortcutAction::Draw;
        case Tool::Erase:
            return ShortcutAction::Erase;
        case Tool::Text:
            return ShortcutAction::Text;
        case Tool::Select:
            return ShortcutAction::Select;
        case Tool::NewScreenshot:
            return ShortcutAction::NewScreenshot;
        case Tool::NewDrawing:
            return ShortcutAction::NewDrawing;
    }
    return ShortcutAction::Draw;  // unreachable: the switch names every tool
}

ShortcutAction ShortcutForCreateAction(CreateAction action) {
    switch (action) {
        case CreateAction::NewCanvas:
            return ShortcutAction::NewCanvas;
        case CreateAction::NewCanvasWithSelection:
            return ShortcutAction::NewCanvasWithSelection;
    }
    return ShortcutAction::NewCanvas;  // unreachable: the switch names every action
}

ShortcutAction ShortcutForClipboardAction(ClipboardAction action) {
    switch (action) {
        case ClipboardAction::Copy:
            return ShortcutAction::Copy;
        case ClipboardAction::Cut:
            return ShortcutAction::Cut;
        case ClipboardAction::Paste:
            return ShortcutAction::Paste;
        case ClipboardAction::Duplicate:
            return ShortcutAction::Duplicate;
    }
    return ShortcutAction::Copy;  // unreachable: the switch names every action
}



SettingsPage::SettingsPage(Settings& settings, Editor& editor, ViewHost& host)
    : settings_(settings), editor_(editor), host_(host) {}

void SettingsPage::OnOverlayShown() { editProfile_ = settings_.ActiveProfile(); }

void SettingsPage::OnPanelOpened() { displays_ = host_.ListDisplays(); }

namespace {

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
// A column of short rows side by side - a swatch and its caption - as
// Appearance has them.
constexpr float kColorColumnWidth = 170.0f;

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
void SettingsPage::Draw() {
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
        const bool pressed = SettingsSectionButton(row.id, row.label, settingsSection_ == row.section);
        host_.Mark(Anchor{AnchorId::SettingsSection, static_cast<uint64_t>(row.section)}, ImGui::GetItemRectMin(),
                   ImGui::GetItemRectMax());
        if (pressed && settingsSection_ != row.section) {
            // A row waiting for its key goes out of sight with its section,
            // and stops waiting: left to wait, the next key - Escape to
            // close the Overview - went to a row no one could see.
            DisarmCapture();
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

    if (!fileKeptNotice_.empty()) {
        ImGui::TextColored(theme::kDeletedInk, "%s", fileKeptNotice_.c_str());
        ImGui::Spacing();
    }

    // Every row makes its own edit as it is changed (see settings_widgets.h),
    // so nothing is collected here to commit afterwards.
    switch (settingsSection_) {
        case SettingsSection::Appearance:
            RenderSettingsAppearance();
            break;
        case SettingsSection::Interaction:
            RenderSettingsInteraction();
            break;
        case SettingsSection::Behavior:
            RenderSettingsBehavior();
            break;
        case SettingsSection::Defaults:
            RenderSettingsDefaults();
            break;
        case SettingsSection::Hotkeys:
            RenderSettingsHotkeys();
            break;
        case SettingsSection::Profiles:
            RenderSettingsProfiles();
            break;
        case SettingsSection::Debug:
            RenderSettingsDebug();
            break;
    }

    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void SettingsPage::SetFileKept(core::ConfigSource why, const std::string& path) {
    char line[1024];
    std::snprintf(line, sizeof(line),
                  why == core::ConfigSource::Newer ? strings::kSettingsNotSavedNewer
                                                   : strings::kSettingsNotSavedUnreadable,
                  path.c_str());
    fileKeptNotice_ = line;
}

void SettingsPage::RenderSettingsAppearance() {
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
            if (ImGui::IsWindowAppearing()) {
                displays_ = host_.ListDisplays();
            }
            // The two rows are one choice, so both are previewed and then
            // committed together. The overlay moves after the frame - see
            // TrayController::ApplySettingsToWindow.
            if (ImGui::Selectable(Labeled(primaryText, "displayprimary"), chosenId.empty())) {
                settings_.Preview(setting::kDisplayId, std::string());
                settings_.Preview(setting::kDisplayName, std::string());
                settings_.CommitPreviews();
            }
            for (size_t i = 0; i < displays_.size(); ++i) {
                const std::string id = "display" + std::to_string(i);
                const bool selected = chosenAttached && displays_[i].id == inUse.id;
                if (ImGui::Selectable(Labeled(describe(displays_[i]).c_str(), id.c_str()), selected)) {
                    settings_.Preview(setting::kDisplayId, displays_[i].id);
                    settings_.Preview(setting::kDisplayName, displays_[i].name);
                    settings_.CommitPreviews();
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
        const int windowsPercent = host_.Window() != nullptr ? host_.Window()->ScalePercent() : 100;
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
            if (ImGui::Selectable(Labeled(autoText, "uiscaleauto"), Cfg().uiScalePercent == 0)) {
                settings_.Set(setting::kUiScale, 0);
            }
            for (const int percent : kPresets) {
                char text[16];
                std::snprintf(text, sizeof(text), strings::kFormatPercent, percent);
                char id[24];
                std::snprintf(id, sizeof(id), "uiscale%d", percent);
                if (ImGui::Selectable(Labeled(text, id), Cfg().uiScalePercent == percent)) {
                    settings_.Set(setting::kUiScale, percent);
                }
            }
            ImGui::EndCombo();
        }
    }

    SettingsGroupBreak();

    SettingsHeading("appearanceaccentheading", strings::kAppearanceAccentHeading, strings::kAppearanceAccentHelp);
    {
        // Previewed as it is dragged, which OnFrame turns into the theme on
        // the next frame - so the panel this sits in, the tabs and this very
        // swatch's own highlights all recolor live. Committed when the edit
        // finishes, the same as every other color here.
        SettingColor(settings_, setting::kAccentColor, "##accentcolor", strings::kAppearanceAccentColor,
                     SwatchAlpha::None);
        const uint32_t defaultAccent = DefaultConfig().accentColorRGBA;
        if (Cfg().accentColorRGBA != defaultAccent) {
            ImGui::SameLine();
            if (ImGui::SmallButton(Labeled(strings::kAppearanceAccentReset, "accentreset"))) {
                settings_.Set(setting::kAccentColor, defaultAccent);
            }
        }
    }

    SettingsGroupBreak();

    SettingsHeading("appearancedisplayheading", strings::kAppearanceDisplayHeading);
    SettingCheckbox(settings_, setting::kShowItemBorders, "appearanceshowitemborders",
                    strings::kAppearanceShowItemBorders, strings::kAppearanceShowItemBordersHelp);

    SettingsGroupBreak();

    SettingsHeading("appearanceimagefilterheading", strings::kAppearanceImageFilterHeading,
                     strings::kAppearanceImageFilterHelp);
    const ChoiceLabel<platform::ImageFilter> filters[] = {
        {platform::ImageFilter::Bilinear, strings::kAppearanceImageFilterBilinear, "imagefilterbilinear"},
        {platform::ImageFilter::Nearest, strings::kAppearanceImageFilterNearest, "imagefilternearest"},
        {platform::ImageFilter::Bicubic, strings::kAppearanceImageFilterBicubic, "imagefilterbicubic"},
        {platform::ImageFilter::Lanczos, strings::kAppearanceImageFilterLanczos, "imagefilterlanczos"},
    };
    SettingRadio(settings_, setting::kImageFilter, filters);

    SettingsGroupBreak();

    SettingsHeading("appearancesnippetcolorsheading", strings::kAppearanceSnippetColorsHeading,
                     strings::kAppearanceSnippetColorsHelp);
    // Swatches with their alpha rather than each a swatch and an opacity
    // slider: here the alpha *is* the setting half the time. Side by side,
    // in columns, since each is short and the page is long.
    const float colorsX = ImGui::GetCursorPosX();
    SettingColor(settings_, setting::kBorderFront, "##snipcolfrontborder", strings::kAppearanceFrontmostBorder,
                 SwatchAlpha::Bar);
    ImGui::SameLine(colorsX + Px(kColorColumnWidth));
    SettingColor(settings_, setting::kBorderOther, "##snipcolotherborder", strings::kAppearanceOtherBorders,
                 SwatchAlpha::Bar);
    ImGui::SameLine(colorsX + Px(2.0f * kColorColumnWidth));
    SettingColor(settings_, setting::kBorderPinned, "##snipcolpinnedborder", strings::kAppearancePinnedBorder,
                 SwatchAlpha::Bar);
    // The selection's, on a row of its own: a color of its own - kept,
    // grayed, while the accent stands in for it - and the switch that says
    // which.
    ImGui::BeginDisabled(Cfg().itemBorderSelectedFollowsAccent);
    SettingColor(settings_, setting::kBorderSelected, "##snipcolselectedborder", strings::kAppearanceSelectedBorder,
                 SwatchAlpha::Bar);
    ImGui::EndDisabled();
    ImGui::SameLine(colorsX + Px(kColorColumnWidth));
    SettingCheckbox(settings_, setting::kBorderSelectedFollowsAccent, "appearanceselectedborderaccent",
                    strings::kAppearanceSelectedBorderFollowsAccent, nullptr);

    SettingsGroupBreak();

    SettingsHeading("appearanceeditborderheading", strings::kAppearanceEditBorderHeading);
    SettingCheckbox(settings_, setting::kShowEditModeBorder, "appearanceshoweditborder",
                    strings::kAppearanceShowEditBorder, strings::kAppearanceShowEditBorderHelp);
    ImGui::BeginDisabled(!Cfg().showEditModeBorder);
    // The color with its alpha, as the snippet colors have it, and the
    // width in the next column.
    const float borderX = ImGui::GetCursorPosX();
    SettingColor(settings_, setting::kEditModeBorderColor, "##editbordercolor", strings::kAppearanceEditBorderColor,
                 SwatchAlpha::Bar);
    ImGui::SameLine(borderX + Px(kColorColumnWidth));
    SettingPixels(settings_, setting::kEditModeBorderWidth, "editborderwidth", strings::kAppearanceEditBorderWidth);
    SettingCheckbox(settings_, setting::kEditModeBorderOnlyWhenEmpty, "appearanceeditborderemptyonly",
                    strings::kAppearanceEditBorderEmptyOnly, strings::kAppearanceEditBorderEmptyOnlyHelp);
    ImGui::EndDisabled();

    SettingsGroupBreak();

    SettingsHeading("appearancecanvasbarheading", strings::kAppearanceCanvasBarHeading);
    SettingCheckbox(settings_, setting::kShowCanvasBar, "appearanceshowcanvasbar", strings::kAppearanceShowCanvasBar,
                    strings::kAppearanceShowCanvasBarHelp);
}

namespace {

// The label a bar's button wears in the row that arranges them, and the
// icon that stands for it there.
//
// A fixed icon, unlike the bar's own, where the pen and the eraser show
// the shape picked for them: this row is about which buttons are there,
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
void SettingsPage::RenderBarButtonRow(const char* id, const char* label, const GlobalSetting<BarRule>& row) {
    // Edited as a copy, set once below: the walk draws from the list.
    BarButtonList buttons = settings_.Get(row);
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
    std::snprintf(payloadType, sizeof(payloadType), "SZ_BAR_%s", id);
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
        // one through PillSwatchButton, a swatch where the icon would be.
        const bool pressed = entry.button == ChromeButton::Color
                                  ? PillSwatchButton(tileId, editor_.DrawColorRGBA(), entry.shown)
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
        buttons = ValueIn(row, AppConfig{});
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
    if (changed) {
        settings_.Set(row, std::move(buttons));
    }
}

void SettingsPage::RenderSettingsInteraction() {
    SettingsHeading("drawingsnippetsheading", strings::kDrawingSnippetsHeading);
    SettingCheckbox(settings_, setting::kRaiseSelected, "drawingraiseselected", strings::kDrawingRaiseSelected,
                    strings::kDrawingRaiseSelectedHelp);

    SettingsGroupBreak();

    // What a left press on empty canvas makes, by kind. Choosing the press
    // the other kind has swaps the two rather than refusing (the edit
    // repair - see Settings::Set): one press cannot make both, and a
    // dropdown that grays out the very choice wanted, with the reason in
    // another row, is a puzzle. See AppConfig::screenshotTrigger.
    SettingsHeading("creationheading", strings::kCreationHeading, strings::kCreationHelp);
    const ChoiceLabel<CreationTrigger> triggers[] = {
        {CreationTrigger::Plain, strings::kCreationTriggerPlain, "triggerplain"},
        {CreationTrigger::Ctrl, strings::kCreationTriggerCtrl, "triggerctrl"},
        {CreationTrigger::Alt, strings::kCreationTriggerAlt, "triggeralt"},
        {CreationTrigger::Off, strings::kCreationTriggerOff, "triggeroff"},
    };
    constexpr float kLabelColumn = 110.0f;
    constexpr float kComboWidth = 220.0f;
    SettingCombo(settings_, setting::kScreenshotTrigger, "screenshottrigger", strings::kCreationScreenshot, triggers,
                 kLabelColumn, kComboWidth);
    SettingCombo(settings_, setting::kDrawingTrigger, "drawingtrigger", strings::kCreationDrawing, triggers,
                 kLabelColumn, kComboWidth);

    SettingsGroupBreak();

    SettingsHeading("barsheading", strings::kBarsHeading, strings::kBarsHelp);
    // In the order the bar shows them.
    RenderBarButtonRow("drawingbar", strings::kBarsDrawingRow, setting::kDrawingBar);
    RenderBarButtonRow("snippetbar", strings::kBarsSnippetRow, setting::kSnippetBar);

    SettingsGroupBreak();

    // Its list of topics; the Overview, which this page is in, closes for
    // it (docs/TUTORIAL.md, section 13.4).
    SettingsHeading("tutorialheading", strings::kTutorialSettingsHeading, strings::kTutorialSettingsHelp);
    if (ImGui::Button(Labeled(strings::kTutorialSettingsOpen, "tutorial_open"))) {
        host_.Act(action::OpenTutorialList{});
    }
}

void SettingsPage::RenderSettingsDefaults() {
    // One kind's rows: its shape, and its two opacities as the popover
    // shows them - the same words and the same ranges, so a default reads
    // as the setting it is a default for.
    struct KindRows {
        const GlobalSetting<BoolRule>& keepAspect;
        const GlobalSetting<FloatRule>& foreground;
        const GlobalSetting<FloatRule>& background;
    };
    const auto kindRows = [this](const char* id, const KindRows& rows) {
        ImGui::PushID(id);
        SettingCheckbox(settings_, rows.keepAspect, "keepaspect", strings::kDefaultsKeepAspect,
                        strings::kDefaultsKeepAspectHelp);
        SettingPercent(settings_, rows.foreground, "foreground", strings::kDefaultsForeground);
        SettingPercent(settings_, rows.background, "background", strings::kDefaultsBackground);
        ImGui::PopID();
    };

    SettingsHeading("defaultsscreenshotheading", strings::kDefaultsScreenshotHeading,
                    strings::kDefaultsScreenshotHelp);
    kindRows("screenshot", {setting::kScreenshotKeepAspect, setting::kScreenshotForegroundOpacity,
                            setting::kScreenshotBackgroundOpacity});

    SettingsGroupBreak();

    SettingsHeading("defaultsdrawingheading", strings::kDefaultsDrawingHeading, strings::kDefaultsDrawingHelp);
    kindRows("drawing",
             {setting::kDrawingKeepAspect, setting::kDrawingForegroundOpacity, setting::kDrawingBackgroundOpacity});
    SettingColor(settings_, setting::kDrawingBackgroundColor, "##drawingbackgroundcolor",
                 strings::kDefaultsBackgroundColor, SwatchAlpha::None);

    SettingsGroupBreak();

    SettingsHeading("defaultstextheading", strings::kDefaultsTextHeading, strings::kDefaultsTextHelp);
    // Decided on the first frame (see AppConfig::noteTextSizePx), so never
    // still 0 by the time a panel can show it.
    SettingPixels(settings_, setting::kNoteTextSize, "defaulttextsize", strings::kDefaultsTextSize);
    SettingColor(settings_, setting::kNoteTextColor, "##defaulttextcolor", strings::kDefaultsTextColor,
                 SwatchAlpha::Bar);
}

// Two halves, like Hotkeys. On top what is global - what happens to
// deleted folders and canvases. Below the profile picker the rows a profile
// may state for itself, each written into whichever of the defaults or a
// profile the picker shows (editProfile_).
void SettingsPage::RenderSettingsBehavior() {
    SettingsScopeBox globalBox;
    BeginSettingsScope(globalBox, SettingsScope::Global);
    // One row: the switch, the number of days, the unit. The days are
    // disabled while the switch is off but keep their value, so turning it
    // back on brings back the period chosen before.
    SettingsHeading("deletedheading", strings::kSettingsDeletedHeading, strings::kSettingsPurgeDeletedHelp);
    SettingCheckbox(settings_, setting::kConfirmDelete, "confirmdelete", strings::kSettingsConfirmDelete,
                    strings::kSettingsConfirmDeleteHelp);
    SettingCheckbox(settings_, setting::kConfirmDeleteForGood, "confirmdeleteforgood",
                    strings::kSettingsConfirmDeleteForGood, strings::kSettingsConfirmDeleteForGoodHelp);
    SettingCheckbox(settings_, setting::kPurgeDeleted, "purgedeleted", strings::kSettingsPurgeDeleted, nullptr);
    ImGui::SameLine();
    ImGui::BeginDisabled(!Cfg().purgeDeleted);
    ImGui::SetNextItemWidth(Px(110.0f));
    SettingNumber(settings_, setting::kPurgeDeletedAfterDays, "##purgedeleteddays", 1, 7);
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
    if (host_.Window() != nullptr) {
        const platform::ForegroundApp app = host_.Window()->UnderlyingApplication();
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

    // Each row's revert arrow is marked for the tutorial (docs/TUTORIAL.md,
    // section 18.3): the last item a row draws, when the target states it.
    uint64_t rowIndex = 0;
    const auto markRevert = [&](bool stated) {
        if (stated) {
            host_.Mark(Anchor{AnchorId::SettingsRevert, rowIndex}, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        ++rowIndex;
    };

    ImVec2 rowPos = ImGui::GetCursorScreenPos();
    SettingCheckbox(settings_, editProfile_, setting::kDontStealFocus, "dontstealfocus",
                    strings::kHudDontStealFocus, strings::kInputDontStealFocusHelp);
    markRevert(settings_.IsOverridden(setting::kDontStealFocus, editProfile_));
    // The box and its label, which is what the tutorial points at.
    host_.Mark(Anchor{AnchorId::SettingsDontStealFocus}, rowPos,
               ImVec2(rowPos.x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                          ImGui::CalcTextSize(strings::kHudDontStealFocus).x,
                      rowPos.y + ImGui::GetFrameHeight()));
    float focusTrunk = TrunkFrom(rowPos);

    ImGui::Indent(Px(kTreeIndent));
    // First under the focus row because it is the exception to it, and
    // grayed out when that row is off for the same reason the others are:
    // with focus taken already there is nothing left for it to do.
    rowPos = ImGui::GetCursorScreenPos();
    SettingCheckbox(settings_, editProfile_, setting::kTakeFocusOverElevated, "takefocusoverelevated",
                    strings::kInputTakeFocusOverElevatedLabel, strings::kInputTakeFocusOverElevatedHelp,
                    !edited.dontStealFocus);
    markRevert(settings_.IsOverridden(setting::kTakeFocusOverElevated, editProfile_));
    TreeBranch(focusTrunk, rowPos, Px(kTreeIndent));

    rowPos = ImGui::GetCursorScreenPos();
    SettingCheckbox(settings_, editProfile_, setting::kDontForwardKeystrokes, "dontforwardkeys",
                    strings::kHudDontForwardKeystrokes, strings::kInputDontForwardKeystrokesHelp, !keystrokesAvailable);
    markRevert(settings_.IsOverridden(setting::kDontForwardKeystrokes, editProfile_));
    TreeBranch(focusTrunk, rowPos, Px(kTreeIndent));

    rowPos = ImGui::GetCursorScreenPos();
    SettingCheckbox(settings_, editProfile_, setting::kRawMouseInput, "rawmouse",
                    strings::kHudUseRawMouseInput, strings::kInputRawMouseHelp, !rawAvailable);
    markRevert(settings_.IsOverridden(setting::kRawMouseInput, editProfile_));
    TreeBranch(focusTrunk, rowPos, Px(kTreeIndent));
    float rawTrunk = TrunkFrom(rowPos);

    ImGui::Indent(Px(kTreeIndent));
    rowPos = ImGui::GetCursorScreenPos();
    SettingCheckbox(settings_, editProfile_, setting::kCounterRawMouseInput, "counterrawmouse",
                    strings::kInputCounterRawMouseLabel, strings::kInputCounterRawMouseHelp, !counterAvailable);
    markRevert(settings_.IsOverridden(setting::kCounterRawMouseInput, editProfile_));
    TreeBranch(rawTrunk, rowPos, Px(kTreeIndent));
    float counterTrunk = TrunkFrom(rowPos);

    // Nested under countering, which it tunes, and grayed with it: a
    // threshold for corrections that are not being made does nothing.
    ImGui::Indent(Px(kTreeIndent));
    rowPos = ImGui::GetCursorScreenPos();
    SettingNumber(settings_, editProfile_, setting::kCounterThreshold, "counterthreshold",
                  strings::kInputCounterThresholdLabel, strings::kInputCounterThresholdUnit, 10,
                  strings::kInputCounterThresholdHelp, !counterAvailable || !edited.counterRawMouseInput);
    markRevert(settings_.IsOverridden(setting::kCounterThreshold, editProfile_));
    TreeBranch(counterTrunk, rowPos, Px(kTreeIndent));

    ImGui::Unindent(Px(kTreeIndent) * 3.0f);

    // Out of the tree: nothing above it is needed for it and nothing below
    // needs it. The overlay keeps the pointer position from the mouse
    // itself and that position exists with or without a grab, so this row
    // only picks which pointer is drawn from it - see
    // EditModeInputOptions::useSoftwarePointer.
    SettingCheckbox(settings_, editProfile_, setting::kSoftwarePointer, "softwarepointer",
                    strings::kHudUseSoftwarePointer, strings::kInputSoftwarePointerHelp);
    markRevert(settings_.IsOverridden(setting::kSoftwarePointer, editProfile_));

    SettingCheckbox(settings_, editProfile_, setting::kFreezeScreen, "freezescreen",
                    strings::kHudFreezeScreenWhileEditing, strings::kInputFreezeScreenHelp);
    markRevert(settings_.IsOverridden(setting::kFreezeScreen, editProfile_));
    // What it does to a frozen screen already held is the tray's, after the
    // frame - see TrayController::ApplySettingsToWindow.
    EndSettingsScope(profileBox);
}

void SettingsPage::RenderSettingsDebug() {
    // The section was called "Diagnostics", which reads like something the
    // application collects about you rather than something it draws for
    // you. Everything in here is the second thing, and every row already
    // says so: they all start with "Show". Which is also why the heading
    // carries no reassurance about where any of it goes - a denial invites
    // the question it answers.
    SettingsHeading("debugheading", strings::kDebugHeading);
    SettingCheckbox(settings_, setting::kShowDebugOverlay, "debugshowdebugoverlay", strings::kDebugShowDebugOverlay,
                    strings::kDebugShowDebugOverlayHelp);
    SettingCheckbox(settings_, setting::kShowInputOptionsHud, "debugshowinputhud", strings::kDebugShowInputHud,
                    strings::kDebugShowInputHudHelp);
    SettingCheckbox(settings_, setting::kShowFrameGraph, "debugshowframegraph", strings::kDebugShowFrameGraph,
                    strings::kDebugShowFrameGraphHelp);
}


void SettingsPage::RenderShortcutEditor(ShortcutAction action, const Icon& icon, const char* label, float buttonX) {
    const size_t index = ShortcutActionIndex(action);
    // What the target being edited says, which is not necessarily what is
    // running - see RenderEditTargetPicker.
    const platform::KeyCombo current = EditedSettings().shortcuts[index];
    const bool overridden = IsShortcutOverriddenHere(action);
    const bool capturing = CapturingShortcut() == action;

    ImGui::PushID(static_cast<int>(index));
    // The icon the same action wears on the selection bar, so a row is
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
            DisarmCapture();
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

    // What a waiting row does with the next key or button is its
    // KeyCapture's - see ArmShortcutCapture.
    ImGui::PopID();
}

void SettingsPage::RenderEditTargetPicker(ProfileGroup group) {
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
    // The whole box, for the tutorial: after it, the last item is its
    // preview's text alone.
    const ImVec2 showingAt = ImGui::GetCursorScreenPos();
    host_.Mark(Anchor{AnchorId::SettingsShowing}, showingAt,
               ImVec2(showingAt.x + Px(280.0f), showingAt.y + ImGui::GetFrameHeight()));
    // A profile's name is drawn as text, never passed as a label: ImGui
    // reads "##" in a label as the start of an id, so a name holding one
    // was cut short where it showed and, with "###", shared its id with any
    // other name ending the same way. Rows are told apart by index, which
    // two profiles of one name do not share either.
    if (ImGui::BeginCombo("##edittarget", nullptr, ImGuiComboFlags_CustomPreview)) {
        // A row waiting for its key would have taken it into whatever the
        // target is when the key comes: another target is another row.
        if (ImGui::Selectable(Labeled(strings::kProfilesDefaults, "targetdefaults"), !editProfile_.has_value())) {
            DisarmCapture();
            editProfile_.reset();
        }
        host_.Mark(Anchor{AnchorId::SettingsShowingEntry, 0}, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        for (size_t i = 0; i < settings_.Profiles().size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 at = ImGui::GetCursorPos();
            if (ImGui::Selectable("##target", editProfile_ && *editProfile_ == i)) {
                DisarmCapture();
                editProfile_ = i;
            }
            host_.Mark(Anchor{AnchorId::SettingsShowingEntry, i + 1}, ImGui::GetItemRectMin(),
                       ImGui::GetItemRectMax());
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
        // The first field shares the label's line; every one after it
        // starts its own, lined up under the first.
        if (entry == 0) {
            ImGui::SameLine(Px(kProfileFieldX));
        } else {
            ImGui::SetCursorPosX(Px(kProfileFieldX));
        }
        ImGui::SetNextItemWidth(Px(kProfileFieldWidth));
        // The entry itself, whole - a window title can be longer than any
        // array sized for one. See InputString.
        if (InputString("##entry", list[entry])) {
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

void SettingsPage::RenderSettingsProfiles() {
    // Every edit goes into this copy and is handed back once, at the end -
    // so nothing here iterates a list the callback may have replaced. A
    // rename is the exception: it is an edit of its own, which can be
    // refused, and the copy is kept in step with it (see RenderProfileRow).
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

void SettingsPage::RemoveProfiles(const std::vector<ProfileId>& ids) {
    std::vector<Profile> kept;
    std::optional<size_t> showing;
    const std::vector<Profile>& profiles = settings_.Profiles();
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (std::find(ids.begin(), ids.end(), profiles[i].id) != ids.end()) {
            continue;
        }
        // Showing follows its profile down the list, and lets go of it
        // when it goes - as a row's trash button has it.
        if (editProfile_ == i) {
            showing = kept.size();
        }
        kept.push_back(profiles[i]);
    }
    if (kept.size() == profiles.size()) {
        return;
    }
    DisarmCapture();
    editProfile_ = showing;
    takenProfileName_.reset();
    settings_.SetProfiles(std::move(kept));
}

bool SettingsPage::RenderProfileMakers(std::vector<Profile>& edited) {
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
            // What is made is edited next: Showing points at it, as it
            // points at the profile that runs when the overlay comes up
            // (docs/TUTORIAL.md, question 42). Left where it was, the
            // first change after making a profile went to the defaults.
            editProfile_ = edited.size() - 1;
            changed = true;
        }
        host_.Mark(Anchor{AnchorId::SettingsMakeProfile}, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", strings::kProfilesMakeForThisTooltip);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(Labeled(strings::kProfilesNewProfile, "newprofile"))) {
        Profile profile;
        profile.name = UniqueProfileName(edited, strings::kProfilesNamePrefix);
        edited.push_back(std::move(profile));
        editProfile_ = edited.size() - 1;
        changed = true;
    }
    host_.Mark(Anchor{AnchorId::SettingsNewProfile}, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kProfilesNewProfileTooltip);
    }
    return changed;
}

bool SettingsPage::RenderProfileRow(size_t index, Profile& profile, bool& remove) {
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
    const float rightX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - ImGui::GetWindowPos().x;
    ImGui::SameLine(rightX - buttonWidth);
    char deleteId[32] = {};
    std::snprintf(deleteId, sizeof(deleteId), "##delprofile%zu", index);
    if (DangerIconButton(deleteId, icons::kTrash)) {
        remove = true;
    }
    host_.Mark(Anchor{AnchorId::SettingsDeleteProfile, index}, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kProfilesDeleteThis);
    }

    if (!open) {
        return changed;
    }
    ImGui::Spacing();
    // The whole name, not a copy cut to a fixed size: a new profile is
    // named after the window's title, and that can be longer than any
    // size chosen for a name. See InputString.
    std::string name = profile.name;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kProfilesName);
    ImGui::SameLine(Px(kProfileFieldX));
    ImGui::SetNextItemWidth(Px(kProfileFieldWidth));
    // A rename is an edit of its own (Settings::RenameProfile), made at once
    // rather than handed back with the rest of the row. Never stored empty
    // or taken: while the field is cleared, or says another profile's name,
    // the profile keeps its old one, and a field left like that shows it
    // again once it lets go. A taken name is said under the field while it
    // is typed, since the refusal is otherwise invisible.
    if (InputString("##name", name)) {
        if (settings_.RenameProfile(index, name)) {
            profile.name = name;
            takenProfileName_.reset();
        } else if (!name.empty() && IsProfileNameTaken(settings_.Profiles(), index, name)) {
            takenProfileName_ = std::make_pair(index, name);
        } else {
            takenProfileName_.reset();
        }
    }
    if (ImGui::IsItemDeactivated()) {
        takenProfileName_.reset();
    }
    if (takenProfileName_ && takenProfileName_->first == index) {
        ImGui::SetCursorPosX(Px(kProfileFieldX));
        ImGui::TextColored(theme::kDeletedInk, strings::kProfilesNameTaken, takenProfileName_->second.c_str());
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
float SettingsPage::KeyButtonColumn() const {
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

void SettingsPage::RenderSettingsHotkeys() {
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
    RenderHotkeyEditor("hkedit", strings::kHotkeysEditMode, HotkeySlot::EditMode, buttonX);
    RenderHotkeyEditor("hkview", strings::kHotkeysViewMode, HotkeySlot::ViewMode, buttonX);
    RenderHotkeyEditor("hkquick", strings::kHotkeysQuickCapture, HotkeySlot::QuickCapture, buttonX);
    RenderHotkeyEditor("hksilent", strings::kHotkeysSilentCapture, HotkeySlot::SilentCapture, buttonX);
    SettingCheckbox(settings_, setting::kShowToastsWhileHidden, "hotkeyssaywhenhidden", strings::kHotkeysSayWhenHidden,
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

// What is shown is what is stored, read fresh every frame - so a rejected
// edit (see TryChangeHotkey), which stores nothing, has nothing to undo.
void SettingsPage::RenderHotkeyEditor(const char* id, const char* label, HotkeySlot slot, float buttonX) {
    const platform::KeyCombo current = settings_.Get(HotkeySetting(slot));
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(buttonX);

    const bool capturing = CapturingHotkey() == slot;
    if (ImGui::Button(capturing ? Labeled(strings::kHotkeysComboPrompt, "combo_btn")
                                 : (FormatKeyComboLabel(current) + "##combo_btn").c_str(),
                       ImVec2(Px(200.0f), 0.0f))) {
        // Clicking the armed row's own button cancels capture instead of
        // re-arming it, as Escape does (see KeyCapture).
        if (capturing) {
            DisarmCapture();
        } else {
            ArmHotkeyCapture(slot);
        }
    }
    if (ImGui::IsItemHovered() && !capturing) {
        ImGui::SetTooltip("%s", strings::kHotkeysComboTooltip);
    }

    // What a waiting row does with the next key is its KeyCapture's - see
    // ArmHotkeyCapture.
    ImGui::PopID();
}

// A row waiting is a KeyCapture on the machine's Text level - one at a
// time, so arming one disarms the other, as the level holds one.
void SettingsPage::ArmHotkeyCapture(HotkeySlot slot) {
    editor_.Input().Push(std::make_unique<KeyCapture>(slot,
                                                      [this, slot](platform::KeyCombo combo) {
                                                          if (!TryChangeHotkey(slot, combo)) {
                                                              host_.Say(strings::kHotkeysComboRejected);
                                                          }
                                                      }),
                         Event{});
}

void SettingsPage::ArmShortcutCapture(ShortcutAction action) {
    editor_.Input().Push(std::make_unique<KeyCapture>(
                             action,
                             [this, action](platform::KeyCombo combo) {
                                 settings_.SetShortcut(action, combo, editProfile_);
                             }),
                         Event{});
}

std::optional<HotkeySlot> SettingsPage::CapturingHotkey() const {
    const KeyCapture* capture = editor_.Input().As<KeyCapture>(Level::Text);
    if (capture == nullptr || !std::holds_alternative<HotkeySlot>(capture->Waiting())) {
        return std::nullopt;
    }
    return std::get<HotkeySlot>(capture->Waiting());
}

std::optional<ShortcutAction> SettingsPage::CapturingShortcut() const {
    const KeyCapture* capture = editor_.Input().As<KeyCapture>(Level::Text);
    if (capture == nullptr || !std::holds_alternative<ShortcutAction>(capture->Waiting())) {
        return std::nullopt;
    }
    return std::get<ShortcutAction>(capture->Waiting());
}

void SettingsPage::DisarmCapture() {
    if (editor_.Input().As<KeyCapture>(Level::Text) != nullptr) {
        editor_.Input().End(Level::Text);
    }
}

void SettingsPage::CompleteHotkeyCapture(platform::KeyCombo combo) {
    const std::optional<HotkeySlot> slot = CapturingHotkey();
    if (!slot.has_value()) {
        return;
    }
    DisarmCapture();
    if (!TryChangeHotkey(*slot, combo)) {
        host_.Say(strings::kHotkeysComboRejected);
    }
}

// "Ask first, store after" rather than the plain Settings::Set every other
// setting in this file is: a hotkey has a real way to fail (already taken)
// that only the OS - reached only through the host - can tell you about.
bool SettingsPage::TryChangeHotkey(HotkeySlot slot, platform::KeyCombo combo) {
    // Offered even when unchanged: the combo it already has may be one that
    // never registered, and picking it again is how to try again.
    return host_.ChangeHotkey(slot, combo);
}

}  // namespace sz::ui
