#pragma once

#include <tuple>
#include <type_traits>

#include "core/canvas/item.h"  // kNoteTextSizeMin/Max
#include "core/config/app_config.h"
#include "core/config/setting.h"

namespace sz::core {

// Every setting, one row each, in the order config.json has them - which
// is the order the writer writes them in. docs/SETTINGS.md, section 3, is
// the same list with what each row means; a setting is added there and
// here, and nowhere else in the reading and writing of the file.
//
// The profiles list is not a row: it is a list of objects, each holding
// the Profile rows below again, sparse (see ParseConfig).
namespace setting {

inline constexpr Choice<StrokeRenderMode> kStrokeRenderModes[] = {
    {StrokeRenderMode::Tessellated, "tessellated"},
    {StrokeRenderMode::Polyline, "polyline"},
    {StrokeRenderMode::Rasterized, "rasterized"},
};
inline constexpr Choice<platform::ImageFilter> kImageFilters[] = {
    {platform::ImageFilter::Bilinear, "bilinear"},
    {platform::ImageFilter::Nearest, "nearest"},
    {platform::ImageFilter::Bicubic, "bicubic"},
    {platform::ImageFilter::Lanczos, "lanczos"},
};
inline constexpr Choice<CreationTrigger> kCreationTriggers[] = {
    {CreationTrigger::Plain, "plain"},
    {CreationTrigger::Ctrl, "ctrl"},
    {CreationTrigger::Alt, "alt"},
    {CreationTrigger::Off, "off"},
};

using E = SettingEffect;

// ----- hotkeys -----
inline constexpr GlobalSetting<HotkeyRule> kHotkeyEditMode{
    {"hotkeys", "", "editMode"}, {}, E::Registration, [](AppConfig& c) { return &c.hotkeyEditMode; }};
inline constexpr GlobalSetting<HotkeyRule> kHotkeyViewMode{
    {"hotkeys", "", "viewMode"}, {}, E::Registration, [](AppConfig& c) { return &c.hotkeyViewMode; }};
inline constexpr GlobalSetting<HotkeyRule> kHotkeyQuickCapture{
    {"hotkeys", "", "quickCapture"}, {}, E::Registration, [](AppConfig& c) { return &c.hotkeyQuickCapture; }};
inline constexpr GlobalSetting<HotkeyRule> kHotkeySilentCapture{
    {"hotkeys", "", "silentCapture"}, {}, E::Registration, [](AppConfig& c) { return &c.hotkeySilentCapture; }};

// ----- drawing -----
// The pen's color and width are what it starts with: the pen holds its own
// from then on, and writes them back here (see OverlayApp::ToolSized).
inline constexpr GlobalSetting<ColorRule> kStrokeColor{
    {"drawing", "", "strokeColor"}, {}, E::Start, [](AppConfig& c) { return &c.strokeColorRGBA; }};
inline constexpr GlobalSetting<PositiveFloatRule> kStrokeWidth{
    {"drawing", "", "strokeWidth"}, {kMaxStrokeWidthPx}, E::Start, [](AppConfig& c) { return &c.strokeWidth; }};
inline constexpr GlobalSetting<ChoiceRule<StrokeRenderMode>> kStrokeRenderMode{
    {"drawing", "", "renderMode"}, {kStrokeRenderModes}, E::Frame, [](AppConfig& c) { return &c.strokeRenderMode; }};
inline constexpr GlobalSetting<BoolRule> kRaiseSelected{
    {"drawing", "", "raiseSelected"}, {}, E::Use, [](AppConfig& c) { return &c.raiseSelectedSnippet; }};
inline constexpr GlobalSetting<ChoiceRule<CreationTrigger>> kScreenshotTrigger{
    {"drawing", "", "screenshotTrigger"}, {kCreationTriggers}, E::Use,
    [](AppConfig& c) { return &c.screenshotTrigger; }};
inline constexpr GlobalSetting<ChoiceRule<CreationTrigger>> kDrawingTrigger{
    {"drawing", "", "drawingTrigger"}, {kCreationTriggers}, E::Use, [](AppConfig& c) { return &c.drawingTrigger; }};

// ----- appearance -----
inline constexpr GlobalSetting<BoolRule> kShowItemBorders{
    {"appearance", "", "showItemBorders"}, {}, E::Frame, [](AppConfig& c) { return &c.showItemBorders; }};
inline constexpr GlobalSetting<BoolRule> kShowToastsWhileHidden{
    {"appearance", "", "showToastsWhileHidden"}, {}, E::Use, [](AppConfig& c) { return &c.showToastsWhileHidden; }};
inline constexpr GlobalSetting<ChoiceRule<platform::ImageFilter>> kImageFilter{
    {"appearance", "", "imageFilter"}, {kImageFilters}, E::Frame, [](AppConfig& c) { return &c.imageFilter; }};
inline constexpr GlobalSetting<ColorRule> kAccentColor{
    {"appearance", "", "accentColor"}, {}, E::Frame, [](AppConfig& c) { return &c.accentColorRGBA; }};
inline constexpr GlobalSetting<AutoOrPercentRule> kUiScale{
    {"appearance", "", "uiScale"}, {kUiScalePercentMin, kUiScalePercentMax}, E::Frame,
    [](AppConfig& c) { return &c.uiScalePercent; }};
inline constexpr GlobalSetting<ColorRule> kBorderFront{
    {"appearance", "snippetColors", "borderFront"}, {}, E::Frame,
    [](AppConfig& c) { return &c.itemBorderColorFrontRGBA; }};
inline constexpr GlobalSetting<ColorRule> kBorderOther{
    {"appearance", "snippetColors", "borderOther"}, {}, E::Frame,
    [](AppConfig& c) { return &c.itemBorderColorOtherRGBA; }};
inline constexpr GlobalSetting<ColorRule> kBorderPinned{
    {"appearance", "snippetColors", "borderPinned"}, {}, E::Frame,
    [](AppConfig& c) { return &c.itemBorderColorPinnedRGBA; }};
inline constexpr GlobalSetting<BoolRule> kShowCanvasBar{
    {"appearance", "canvasBar", "show"}, {}, E::Frame, [](AppConfig& c) { return &c.showCanvasBar; }};
inline constexpr GlobalSetting<BoolRule> kShowEditModeBorder{
    {"appearance", "editModeBorder", "show"}, {}, E::Frame, [](AppConfig& c) { return &c.showEditModeBorder; }};
inline constexpr GlobalSetting<ColorRule> kEditModeBorderColor{
    {"appearance", "editModeBorder", "color"}, {}, E::Frame,
    [](AppConfig& c) { return &c.editModeBorderColorRGBA; }};
inline constexpr GlobalSetting<FloatRule> kEditModeBorderOpacity{
    {"appearance", "editModeBorder", "opacity"}, {0.0f, 1.0f}, E::Frame,
    [](AppConfig& c) { return &c.editModeBorderOpacity; }};
inline constexpr GlobalSetting<PositiveBandRule> kEditModeBorderWidth{
    {"appearance", "editModeBorder", "width"}, {kEditModeBorderWidthMin, kEditModeBorderWidthMax}, E::Frame,
    [](AppConfig& c) { return &c.editModeBorderWidthPx; }};
inline constexpr GlobalSetting<BoolRule> kEditModeBorderOnlyWhenEmpty{
    {"appearance", "editModeBorder", "onlyWhenEmpty"}, {}, E::Frame,
    [](AppConfig& c) { return &c.editModeBorderOnlyWhenEmpty; }};

// ----- bars -----
inline constexpr GlobalSetting<BarRule> kSnippetBar{
    {"bars", "", "snippet"}, {NormalizeSnippetBar}, E::Frame, [](AppConfig& c) { return &c.snippetBar; }};
inline constexpr GlobalSetting<BarRule> kDrawingBar{
    {"bars", "", "drawing"}, {NormalizeDrawingBar}, E::Frame, [](AppConfig& c) { return &c.drawingBar; }};

// ----- overview -----
inline constexpr GlobalSetting<BoolRule> kOverviewShowsStrokes{
    {"overview", "", "showStrokes"}, {}, E::Frame, [](AppConfig& c) { return &c.overviewShowsStrokes; }};
inline constexpr GlobalSetting<BoolRule> kOverviewShowsBitmaps{
    {"overview", "", "showBitmaps"}, {}, E::Frame, [](AppConfig& c) { return &c.overviewShowsBitmaps; }};

// ----- defaults -----
// The same words and ranges as a snippet's own popover, so a default reads
// as the setting it is a default for.
inline constexpr GlobalSetting<BoolRule> kScreenshotKeepAspect{
    {"defaults", "screenshot", "keepAspect"}, {}, E::Use,
    [](AppConfig& c) { return &c.screenshotDefaults.keepAspect; }};
inline constexpr GlobalSetting<FloatRule> kScreenshotForegroundOpacity{
    {"defaults", "screenshot", "foregroundOpacity"}, {0.1f, 1.0f}, E::Use,
    [](AppConfig& c) { return &c.screenshotDefaults.foregroundOpacity; }};
inline constexpr GlobalSetting<FloatRule> kScreenshotBackgroundOpacity{
    {"defaults", "screenshot", "backgroundOpacity"}, {0.0f, 1.0f}, E::Use,
    [](AppConfig& c) { return &c.screenshotDefaults.backgroundOpacity; }};
inline constexpr GlobalSetting<BoolRule> kDrawingKeepAspect{
    {"defaults", "drawing", "keepAspect"}, {}, E::Use, [](AppConfig& c) { return &c.drawingDefaults.keepAspect; }};
inline constexpr GlobalSetting<FloatRule> kDrawingForegroundOpacity{
    {"defaults", "drawing", "foregroundOpacity"}, {0.1f, 1.0f}, E::Use,
    [](AppConfig& c) { return &c.drawingDefaults.foregroundOpacity; }};
inline constexpr GlobalSetting<FloatRule> kDrawingBackgroundOpacity{
    {"defaults", "drawing", "backgroundOpacity"}, {0.0f, 1.0f}, E::Use,
    [](AppConfig& c) { return &c.drawingDefaults.backgroundOpacity; }};
inline constexpr GlobalSetting<ColorRule> kDrawingBackgroundColor{
    {"defaults", "drawing", "backgroundColor"}, {}, E::Use,
    [](AppConfig& c) { return &c.drawingBackgroundColorRGBA; }};
inline constexpr GlobalSetting<ColorRule> kNoteTextColor{
    {"defaults", "text", "color"}, {}, E::Use, [](AppConfig& c) { return &c.noteTextColorRGBA; }};
inline constexpr GlobalSetting<PositiveBandRule> kNoteTextSize{
    {"defaults", "text", "size"}, {kNoteTextSizeMin, kNoteTextSizeMax, /*zeroIsUndecided=*/true}, E::Use,
    [](AppConfig& c) { return &c.noteTextSizePx; }};

// ----- deleted -----
inline constexpr GlobalSetting<BoolRule> kConfirmDelete{
    {"deleted", "", "confirmDelete"}, {}, E::Use, [](AppConfig& c) { return &c.confirmDelete; }};
inline constexpr GlobalSetting<BoolRule> kConfirmDeleteForGood{
    {"deleted", "", "confirmDeleteForGood"}, {}, E::Use, [](AppConfig& c) { return &c.confirmDeleteForGood; }};
inline constexpr GlobalSetting<BoolRule> kPurgeDeleted{
    {"deleted", "", "deleteForGoodAutomatically"}, {}, E::Start, [](AppConfig& c) { return &c.purgeDeleted; }};
inline constexpr GlobalSetting<IntRule> kPurgeDeletedAfterDays{
    {"deleted", "", "afterDays"}, {kPurgeDeletedAfterDaysMin, kPurgeDeletedAfterDaysMax}, E::Start,
    [](AppConfig& c) { return &c.purgeDeletedAfterDays; }};

// ----- display -----
inline constexpr GlobalSetting<TextRule> kDisplayId{
    {"display", "", "id"}, {}, E::Display, [](AppConfig& c) { return &c.overlayDisplayId; }};
inline constexpr GlobalSetting<TextRule> kDisplayName{
    {"display", "", "name"}, {}, E::Display, [](AppConfig& c) { return &c.overlayDisplayName; }};

// ----- behavior: what a profile may override -----
inline constexpr ProfileSetting<BoolRule> kDontStealFocus{
    {"behavior", "", "dontStealFocus"}, {}, E::Window, &ProfileableSettings::dontStealFocus,
    &ProfileOverrides::dontStealFocus};
inline constexpr ProfileSetting<BoolRule> kTakeFocusOverElevated{
    {"behavior", "", "takeFocusOverElevated"}, {}, E::Window, &ProfileableSettings::takeFocusOverElevated,
    &ProfileOverrides::takeFocusOverElevated};
inline constexpr ProfileSetting<BoolRule> kSoftwarePointer{
    {"behavior", "", "softwarePointer"}, {}, E::Window, &ProfileableSettings::softwarePointer,
    &ProfileOverrides::softwarePointer};
inline constexpr ProfileSetting<BoolRule> kRawMouseInput{
    {"behavior", "", "rawMouseInput"}, {}, E::Window, &ProfileableSettings::rawMouseInput,
    &ProfileOverrides::rawMouseInput};
inline constexpr ProfileSetting<BoolRule> kDontForwardKeystrokes{
    {"behavior", "", "dontForwardKeystrokes"}, {}, E::Window, &ProfileableSettings::dontForwardKeystrokes,
    &ProfileOverrides::dontForwardKeystrokes};
inline constexpr ProfileSetting<BoolRule> kCounterRawMouseInput{
    {"behavior", "", "counterRawMouseInput"}, {}, E::Window, &ProfileableSettings::counterRawMouseInput,
    &ProfileOverrides::counterRawMouseInput};
inline constexpr ProfileSetting<IntRule> kCounterThreshold{
    {"behavior", "", "counterThreshold"},
    {platform::EditModeInputOptions::kCounterThresholdMin, platform::EditModeInputOptions::kCounterThresholdMax},
    E::Window, &ProfileableSettings::counterThreshold, &ProfileOverrides::counterThreshold};
inline constexpr ProfileSetting<BoolRule> kFreezeScreen{
    {"behavior", "", "freezeScreen"}, {}, E::Freeze, &ProfileableSettings::freezeScreen,
    &ProfileOverrides::freezeScreen};

// ----- shortcuts: overridable too, one per action -----
inline constexpr ShortcutSettings kShortcuts{"shortcuts", {}, E::Use};

// ----- tutorial: kept by the tutorial as it goes (docs/TUTORIAL.md, 13.7) -----
inline constexpr GlobalSetting<TextMapRule> kTutorialProgress{
    {"tutorial", "", "progress"}, {}, E::Use, [](AppConfig& c) { return &c.tutorialProgress; }};
inline constexpr GlobalSetting<TextRule> kTutorialCurrent{
    {"tutorial", "", "current"}, {}, E::Use, [](AppConfig& c) { return &c.tutorialCurrent; }};
inline constexpr GlobalSetting<IdRule> kTutorialFolder{
    {"tutorial", "", "folder"}, {}, E::Use, [](AppConfig& c) { return &c.tutorialFolder; }};

// ----- diagnostics -----
inline constexpr GlobalSetting<BoolRule> kShowDebugOverlay{
    {"diagnostics", "", "showDebugOverlay"}, {}, E::Frame, [](AppConfig& c) { return &c.showDebugOverlay; }};
inline constexpr GlobalSetting<BoolRule> kShowInputOptionsHud{
    {"diagnostics", "", "showInputOptionsHud"}, {}, E::Frame, [](AppConfig& c) { return &c.showInputOptionsHud; }};

// All of them, in file order.
inline constexpr auto kAll = std::tuple{
    &kHotkeyEditMode, &kHotkeyViewMode, &kHotkeyQuickCapture, &kHotkeySilentCapture,
    &kStrokeColor, &kStrokeWidth, &kStrokeRenderMode, &kRaiseSelected, &kScreenshotTrigger, &kDrawingTrigger,
    &kShowItemBorders, &kShowToastsWhileHidden, &kImageFilter, &kAccentColor, &kUiScale,
    &kBorderFront, &kBorderOther, &kBorderPinned, &kShowCanvasBar,
    &kShowEditModeBorder, &kEditModeBorderColor, &kEditModeBorderOpacity, &kEditModeBorderWidth,
    &kEditModeBorderOnlyWhenEmpty,
    &kSnippetBar, &kDrawingBar,
    &kOverviewShowsStrokes, &kOverviewShowsBitmaps,
    &kScreenshotKeepAspect, &kScreenshotForegroundOpacity, &kScreenshotBackgroundOpacity,
    &kDrawingKeepAspect, &kDrawingForegroundOpacity, &kDrawingBackgroundOpacity, &kDrawingBackgroundColor,
    &kNoteTextColor, &kNoteTextSize,
    &kConfirmDelete, &kConfirmDeleteForGood, &kPurgeDeleted, &kPurgeDeletedAfterDays,
    &kDisplayId, &kDisplayName,
    &kDontStealFocus, &kTakeFocusOverElevated, &kSoftwarePointer, &kRawMouseInput, &kDontForwardKeystrokes,
    &kCounterRawMouseInput, &kCounterThreshold, &kFreezeScreen,
    &kShortcuts,
    &kTutorialProgress, &kTutorialCurrent, &kTutorialFolder,
    &kShowDebugOverlay, &kShowInputOptionsHud,
};

}  // namespace setting

template <typename T>
inline constexpr bool kIsGlobalSetting = false;
template <typename Rule>
inline constexpr bool kIsGlobalSetting<GlobalSetting<Rule>> = true;
template <typename T>
inline constexpr bool kIsProfileSetting = false;
template <typename Rule>
inline constexpr bool kIsProfileSetting<ProfileSetting<Rule>> = true;

// Calls `visit` with every row, in file order.
template <typename Visit>
void ForEachSetting(Visit&& visit) {
    std::apply([&visit](const auto*... row) { (visit(*row), ...); }, setting::kAll);
}

// Calls `visit` with every ProfileSetting row - the overridable settings
// but the shortcuts, which are kShortcuts.
template <typename Visit>
void ForEachProfileSetting(Visit&& visit) {
    ForEachSetting([&visit](const auto& row) {
        if constexpr (kIsProfileSetting<std::decay_t<decltype(row)>>) {
            visit(row);
        }
    });
}

// A row's value in the stored settings: for a Profile row, the defaults
// layer's.
template <typename Rule>
typename Rule::Value& ValueIn(const GlobalSetting<Rule>& row, AppConfig& config) {
    return *row.at(config);
}
template <typename Rule>
const typename Rule::Value& ValueIn(const GlobalSetting<Rule>& row, const AppConfig& config) {
    // `at` only finds the field; nothing is written through it here.
    return *row.at(const_cast<AppConfig&>(config));
}
template <typename Rule>
typename Rule::Value& ValueIn(const ProfileSetting<Rule>& row, AppConfig& config) {
    return config.profileable.*row.value;
}
template <typename Rule>
const typename Rule::Value& ValueIn(const ProfileSetting<Rule>& row, const AppConfig& config) {
    return config.profileable.*row.value;
}

}  // namespace sz::core
