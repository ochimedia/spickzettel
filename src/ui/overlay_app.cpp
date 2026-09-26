#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <ctime>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ui/icon_draw.h"
#include "ui/icons_generated.h"
#include "core/canvas/item_geometry.h"
#include "core/drawing/stroke_mesh.h"
#include "platform/pen_glyph.h"

#include <imgui.h>
// For BringWindowToDisplayFront/FindWindowByName - see the "Window
// stacking order" note in docs/ARCHITECTURE.md for why this is needed:
// the app's own layers and panels (the canvas layer, the items layer, the
// canvas bar, the screen chrome) all use NoBringToFrontOnFocus (so a
// click on one doesn't jump it to front, which would fight the order the
// frame draws them in), but that flag also changes where a *new* window
// is first inserted in ImGui's internal stack, and nothing else
// re-asserts the intended order afterward. Pinned to a specific ImGui
// commit (see cmake/FetchImGui.cmake), so relying on this internal header
// is a deliberate, contained choice rather than an accident.
#include <imgui_internal.h>

namespace sz::ui {

using namespace overlay_detail;

namespace overlay_detail::theme {
namespace {
struct AccentColors {
    ImVec4 accent;
    ImVec4 hover;
    ImVec4 ink;
};

AccentColors DeriveAccent(uint32_t rgba) {
    const float r = static_cast<float>((rgba >> 24) & 0xFF) / 255.0f;
    const float g = static_cast<float>((rgba >> 16) & 0xFF) / 255.0f;
    const float b = static_cast<float>((rgba >> 8) & 0xFF) / 255.0f;
    AccentColors colors;
    colors.accent = ImVec4(r, g, b, 1.0f);
    // A little toward white - which takes the design's #ff6a3d to its own
    // hover shade, #ff7c53, near enough.
    constexpr float kHoverLift = 0.12f;
    colors.hover = ImVec4(r + (1.0f - r) * kHoverLift, g + (1.0f - g) * kHoverLift, b + (1.0f - b) * kHoverLift, 1.0f);
    // Dark ink on a bright accent - its own hue at a seventh of the
    // brightness, which is the design's #241005 on #ff6a3d - and the
    // palette's white on a dark one. Brightness as Rec. 709 luma on the
    // stored values: rough, but it only has to pick a side.
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    colors.ink = luma >= 0.5f ? ImVec4(r * 0.14f, g * 0.14f, b * 0.14f, 1.0f) : kWhite;
    return colors;
}

// Derived once per change rather than per draw call; the default accent
// until OnFrame applies the setting.
AccentColors& CurrentAccent() {
    static AccentColors colors = DeriveAccent(0x2C6C7CFFu);
    return colors;
}
}  // namespace

const ImVec4& Accent() { return CurrentAccent().accent; }
const ImVec4& AccentHover() { return CurrentAccent().hover; }
const ImVec4& AccentInk() { return CurrentAccent().ink; }

ImU32 AccentU32(uint8_t alpha) {
    const ImVec4& a = CurrentAccent().accent;
    return IM_COL32(static_cast<int>(a.x * 255.0f + 0.5f), static_cast<int>(a.y * 255.0f + 0.5f),
                    static_cast<int>(a.z * 255.0f + 0.5f), alpha);
}

void SetAccent(uint32_t rgba) { CurrentAccent() = DeriveAccent(rgba); }
}  // namespace overlay_detail::theme

namespace {

// How long the mouse wheel's own size preview stays up after the last
// step, and how much of that tail it spends fading. Long enough to read
// the number and judge the dot without turning into something that sits
// on screen after you've moved on - see RenderBrushSizePreview.
constexpr double kSizePreviewHoldSeconds = 1.1;
constexpr double kSizePreviewFadeSeconds = 0.35;

// Whole wheel notches out of `remainder`, which the caller keeps across
// frames (see OverlayApp's own sizeWheelRemainder_/canvasWheelRemainder_
// for why the leftover has to persist rather than being truncated per
// frame). Returns however many complete notches `wheelDelta` brings the
// running total to - 3 on a fast spin that lands three in one frame, 0
// partway through a high-resolution wheel's own sub-notch reports - and
// leaves the sub-notch fraction behind for next time.
int TakeWheelSteps(float& remainder, float wheelDelta) {
    remainder += wheelDelta;
    // Truncation toward zero, not floor: a remainder of -0.4 has to stay
    // -0.4 rather than becoming a whole step downward it never earned.
    const float whole = std::trunc(remainder);
    remainder -= whole;
    return static_cast<int>(whole);
}

// The style colors that are the accent, from whatever theme::Accent() is
// now. Separate from ApplySpickzettelStyle because the accent is a setting
// and these are applied again whenever it changes - see OnFrame.
void ApplyAccentToStyle(ImGuiStyle& style) {
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_ButtonActive] = theme::Accent();
    colors[ImGuiCol_CheckMark] = theme::Accent();
    colors[ImGuiCol_SliderGrab] = theme::Accent();
    colors[ImGuiCol_SliderGrabActive] = theme::AccentHover();
    colors[ImGuiCol_SeparatorHovered] = theme::Accent();
    colors[ImGuiCol_SeparatorActive] = theme::Accent();
    colors[ImGuiCol_TextSelectedBg] = ImVec4(theme::AccentHover().x, theme::AccentHover().y, theme::AccentHover().z, 0.35f);
    colors[ImGuiCol_DragDropTarget] = theme::Accent();
    // The chosen row of a dropdown: the accent, softened so the text on it
    // - drawn in the ordinary text color, whatever the accent - still reads.
    colors[ImGuiCol_Header] = ImVec4(theme::Accent().x, theme::Accent().y, theme::Accent().z, 0.45f);
    colors[ImGuiCol_HeaderActive] = ImVec4(theme::Accent().x, theme::Accent().y, theme::Accent().z, 0.6f);
    colors[ImGuiCol_NavHighlight] = theme::Accent();
}

// Sets up ImGui's global style/colors for the app's dark "graphite +
// accent orange" material look, in place of ImGui's own
// built-in dark theme (flat opaque gray panels, square corners, no accent
// color), at the interface scale `scale` (see UiScale). OnFrame calls it
// on the first frame and again whenever the scale changes: it starts from
// a fresh style every time, because ImGuiStyle::ScaleAllSizes multiplies
// what is there, and a style scaled twice would be scaled by the product.
//
// Deliberately NOT called from AttachTo: ImGui::GetStyle() dereferences
// the current context, and that context isn't created until the platform
// window's EnsureCreated() runs (Win32Dx11Renderer::Initialize /
// DevLinuxGlRenderer::Initialize both call ImGui::CreateContext()) - which
// happens well after AttachTo, on first hotkey-toggle-open. OnFrame is
// only ever invoked as an ImGui frame callback, so a context is always
// live by the time it runs.
void ApplySpickzettelStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    // The base font size is the font's, set when it was loaded, and not a
    // matter of style - a fresh ImGuiStyle would forget it.
    const float fontSizeBase = style.FontSizeBase;
    style = ImGuiStyle();
    style.FontSizeBase = fontSizeBase;
    style.WindowRounding = theme::kRadiusLg;
    style.PopupRounding = theme::kRadiusLg;
    style.FrameRounding = theme::kRadiusSm;
    style.GrabRounding = theme::kRadiusPill;
    style.ScrollbarRounding = theme::kRadiusPill;
    style.ChildRounding = theme::kRadiusMd;
    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.GrabMinSize = 14.0f;
    style.ScrollbarSize = 12.0f;
    // ImGui's own default (0.60) barely dims a disabled widget - a
    // disabled pill button read as "basically the same as every other
    // button, just slightly duller" rather than "not available right
    // now". Lowered so ImGui::BeginDisabled's alpha multiplier actually
    // reads as unavailable at a glance. (0.28 was this value's first
    // attempt, back when only a disabled button's *background* actually
    // dimmed - DrawIcon drew straight to the drawlist with a hardcoded
    // opaque color, bypassing style.Alpha entirely, so the icon on top
    // stayed fully visible and made the whole button read as barely
    // dimmed no matter how low this went. Now that IconButton/
    // DangerIconButton/PrimaryButton/DangerButton all compute their
    // icon/text color via GetColorU32 - which does apply style.Alpha -
    // dimming actually affects the whole button, so this can sit at a
    // more moderate value.)
    style.DisabledAlpha = 0.45f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = theme::kWhite;
    colors[ImGuiCol_TextDisabled] = theme::kGraphite400;
    colors[ImGuiCol_WindowBg] = theme::kPanelBg;
    colors[ImGuiCol_PopupBg] = theme::kPanelBg;
    colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border] = theme::kPanelBorder;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = theme::kFieldBg;
    colors[ImGuiCol_FrameBgHovered] = theme::kHoverWash;
    colors[ImGuiCol_FrameBgActive] = theme::kPanelBorderStrong;
    // A checked box keeps the unchecked box's gray, so the accent check
    // mark is what says it is on. Left unset, ImGui tints it toward its own
    // blue theme, which the accent then had to stand out against.
    colors[ImGuiCol_CheckboxSelectedBg] = theme::kFieldBg;
    colors[ImGuiCol_Button] = theme::kFieldBg;
    colors[ImGuiCol_ButtonHovered] = theme::kHoverWash;
    // Hovering a dropdown row, or a profile's row, is hovering like any
    // button. Left unset, these three were ImGui's own blue.
    colors[ImGuiCol_HeaderHovered] = theme::kHoverWash;
    colors[ImGuiCol_Separator] = theme::kPanelBorderStrong;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = theme::kGraphite600;
    colors[ImGuiCol_ScrollbarGrabHovered] = theme::kGraphite500;
    colors[ImGuiCol_ScrollbarGrabActive] = theme::kGraphite400;
    colors[ImGuiCol_TitleBg] = theme::kPanelBg;
    colors[ImGuiCol_TitleBgActive] = theme::kPanelBg;
    ApplyAccentToStyle(style);

    // Every size above, and the text. ScaleAllSizes rounds each one down to
    // a whole pixel, so a 1px border stays 1px until the scale reaches 200%.
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

// An icon-only square/circle button - the .tb-btn / .icon-btn equivalent
// (both are the same idea at different sizes: a plain Button for its
// frame/hit-region/hover-active states, an icon centered on top instead
// of a text label). Fills with the accent color and switches to dark ink
// icon color while `active`, matching .tb-btn.is-active/.icon-btn.is-on.
// `strId` must start with "##" (or otherwise carry no visible label) and
// be unique within its window - it's the button's ImGui ID, not anything
// drawn. Only called from PillIconButton below - every other themed
// button (DangerIconButton, and Overview's PrimaryButton/DangerButton/
// TabButton) has its own bespoke styling instead of going through this.
bool IconButton(const char* strId, const Icon& icon, bool active, float buttonSize, float iconSize,
                 float rounding) {
    active = active || PressLandsThisFrame(strId, ImVec2(buttonSize, buttonSize));
    ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::Accent() : theme::kFieldBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? theme::Accent() : theme::kHoverWash);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::AccentHover());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
    const bool pressed = ImGui::Button(strId, ImVec2(buttonSize, buttonSize));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    const ImVec2 minPt = ImGui::GetItemRectMin();
    const ImVec2 maxPt = ImGui::GetItemRectMax();
    const ImVec2 iconPos((minPt.x + maxPt.x - iconSize) * 0.5f, (minPt.y + maxPt.y - iconSize) * 0.5f);
    // GetColorU32(ImVec4), not the plain ColorConvertFloat4ToU32 the rest
    // of this file mostly uses - it additionally multiplies alpha by
    // g.Style.Alpha, which is exactly what ImGui::BeginDisabled itself
    // scales down (see its own PushStyleVar(ImGuiStyleVar_Alpha, ...)).
    // DrawIcon draws straight to the drawlist rather than through any
    // ImGui-styled widget path, so without this the icon stayed fully
    // opaque while a disabled button's own *background* dimmed around
    // it - the icon was the one part of a "grayed out" button that
    // never actually looked grayed out.
    const ImU32 iconColor = ImGui::GetColorU32(active ? theme::AccentInk() : theme::kWhite);
    DrawIcon(ImGui::GetWindowDrawList(), icon, iconPos, iconSize, iconColor);
    return pressed;
}

// A border (drawn with real content, so it can't be color-keyed away like
// the background) plus a status line of live state. Useful for confirming
// the overlay is actually rendering and receiving input, e.g. after moving
// to a new machine or display setup.
void DrawDebugOverlay(ImDrawList* drawList, const ImGuiIO& io, const CanvasManager& canvases,
                       const std::string& hoveredResizeHandle) {
    drawList->AddRect(ImVec2(4, 4), ImVec2(io.DisplaySize.x - 4, io.DisplaySize.y - 4), IM_COL32(0, 255, 255, 255),
                       0.0f, 3.0f, ImDrawFlags_None);
    char debugLine[200];
    std::snprintf(debugLine, sizeof(debugLine),
                   "Spickzettel overlay active | canvas=%s | items=%zu | mouse=(%.0f,%.0f)",
                   canvases.CurrentOrNull() ? canvases.CurrentOrNull()->name.c_str() : strings::kHotkeyNone,
                   canvases.CurrentOrNull() ? canvases.CurrentOrNull()->items.size() : size_t{0},
                   io.MousePos.x, io.MousePos.y);
    drawList->AddText(Px(16.0f, 16.0f), IM_COL32(0, 255, 255, 255), debugLine);
    // A live readout of exactly which resize handle (if any) the mouse is
    // over right now - see debugHoveredResizeHandle_'s own doc comment for
    // why this exists: a screenshot alone can't tell "covered by a handle
    // that's visually identical to its neighbor" apart from "not covered
    // by anything," this can.
    char handleLine[96];
    std::snprintf(handleLine, sizeof(handleLine), "resize handle: %s",
                   hoveredResizeHandle.empty() ? "none" : hoveredResizeHandle.c_str());
    drawList->AddText(ImVec2(Px(16.0f), Px(16.0f) + ImGui::GetTextLineHeight()), IM_COL32(0, 255, 255, 255),
                       handleLine);
}

}  // namespace

namespace overlay_detail {

bool PressLandsThisFrame(const char* strId, const ImVec2& size) {
    // ImGui's own rule for a press (ButtonBehavior's default): the mouse
    // went down on the button, which made it the active item, and comes up
    // over it. The rectangle is the one ImGui::Button is about to lay out,
    // at the cursor, sized the way it sizes one.
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::GetActiveID() != ImGui::GetID(strId)) {
        return false;
    }
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 labelSize = ImGui::CalcTextSize(strId, nullptr, true);
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 sized = ImGui::CalcItemSize(size, labelSize.x + style.FramePadding.x * 2.0f,
                                             labelSize.y + style.FramePadding.y * 2.0f);
    return ImGui::IsMouseHoveringRect(min, ImVec2(min.x + sized.x, min.y + sized.y));
}

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

std::optional<platform::KeyCombo> ComboForImGuiKey(ImGuiKey key, bool ctrl, bool alt, bool shift) {
    if (key < ImGuiKey_0 || key > ImGuiKey_F24) {
        return std::nullopt;
    }
    platform::KeyCombo combo;
    combo.ctrl = ctrl;
    combo.alt = alt;
    combo.shift = shift;
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        combo.key = '0' + (key - ImGuiKey_0);
    } else if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        combo.key = 'A' + (key - ImGuiKey_A);
    } else if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
        combo.key = platform::KeyCombo::kFunctionKeyBase + 1 + (key - ImGuiKey_F1);
    } else {
        // Inside the range but not one of the three runs - imgui.h has no
        // such gap today, and this is what keeps that an assumption the
        // code states rather than one it relies on.
        return std::nullopt;
    }
    return combo;
}

ImGuiKey ImGuiKeyForCombo(const platform::KeyCombo& combo) {
    if (combo.key == 0) {
        return ImGuiKey_None;
    }
    if (combo.IsFunctionKey()) {
        const int number = combo.FunctionKeyNumber();
        if (number < 1 || number > 24) {
            return ImGuiKey_None;
        }
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (number - 1));
    }
    if (combo.key >= '0' && combo.key <= '9') {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (combo.key - '0'));
    }
    if (combo.key >= 'A' && combo.key <= 'Z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + (combo.key - 'A'));
    }
    switch (combo.key) {
        case platform::KeyCombo::kEscape:
            return ImGuiKey_Escape;
        case platform::KeyCombo::kDelete:
            return ImGuiKey_Delete;
        case platform::KeyCombo::kBackspace:
            return ImGuiKey_Backspace;
        case platform::KeyCombo::kLeftArrow:
            return ImGuiKey_LeftArrow;
        case platform::KeyCombo::kRightArrow:
            return ImGuiKey_RightArrow;
        case platform::KeyCombo::kUpArrow:
            return ImGuiKey_UpArrow;
        case platform::KeyCombo::kDownArrow:
            return ImGuiKey_DownArrow;
        default:
            return ImGuiKey_None;
    }
}

std::optional<platform::KeyCombo> ComboForImGuiMouseButton(ImGuiMouseButton button, bool ctrl, bool alt,
                                                          bool shift) {
    int key = 0;
    switch (button) {
        case ImGuiMouseButton_Middle:
            key = platform::KeyCombo::kMiddleButton;
            break;
        case 3:
            key = platform::KeyCombo::kX1Button;
            break;
        case 4:
            key = platform::KeyCombo::kX2Button;
            break;
        default:
            return std::nullopt;  // the left and the right are the gestures'
    }
    return platform::KeyCombo{ctrl, alt, shift, key};
}

std::optional<ImGuiMouseButton> ImGuiMouseButtonForCombo(const platform::KeyCombo& combo) {
    switch (combo.key) {
        case platform::KeyCombo::kMiddleButton:
            return ImGuiMouseButton_Middle;
        case platform::KeyCombo::kX1Button:
            return 3;  // ImGui names no constant for the side buttons
        case platform::KeyCombo::kX2Button:
            return 4;
        default:
            return std::nullopt;
    }
}

std::string FormatKeyComboLabel(const platform::KeyCombo& combo) {
    std::string result;
    if (combo.ctrl) {
        result += strings::kHotkeyCtrlPrefix;
    }
    if (combo.alt) {
        result += strings::kHotkeyAltPrefix;
    }
    if (combo.shift) {
        result += strings::kHotkeyShiftPrefix;
    }
    if (combo.key == 0) {
        result += strings::kHotkeyNone;
    } else if (combo.IsFunctionKey()) {
        result += "F";
        result += std::to_string(combo.FunctionKeyNumber());
    } else if (combo.IsMouseButton()) {
        result += combo.key == platform::KeyCombo::kMiddleButton ? strings::kHotkeyMiddleButton
                  : combo.key == platform::KeyCombo::kX1Button   ? strings::kHotkeyX1Button
                                                                 : strings::kHotkeyX2Button;
    } else if (combo.IsNamedKey()) {
        switch (combo.key) {
            case platform::KeyCombo::kEscape:
                result += strings::kHotkeyEscape;
                break;
            case platform::KeyCombo::kDelete:
                result += strings::kHotkeyDelete;
                break;
            case platform::KeyCombo::kBackspace:
                result += strings::kHotkeyBackspace;
                break;
            case platform::KeyCombo::kLeftArrow:
                result += strings::kHotkeyLeftArrow;
                break;
            case platform::KeyCombo::kRightArrow:
                result += strings::kHotkeyRightArrow;
                break;
            case platform::KeyCombo::kUpArrow:
                result += strings::kHotkeyUpArrow;
                break;
            default:
                result += strings::kHotkeyDownArrow;
                break;
        }
    } else {
        result += static_cast<char>(combo.key);
    }
    return result;
}

// See the declaration for why a label and an id are separate things here.
//
// Eight buffers, used round-robin: one call site can build several labels
// in a single statement, and ImGui reads a label during the call it is
// passed to and never afterwards, so a handful of slots is enough and the
// alternative - returning std::string and calling .c_str() at forty call
// sites - is noisier for no gain. thread_local because ImGui is
// single-threaded but nothing here should assume the app always will be.
const char* Labeled(const char* text, const char* id) {
    constexpr size_t kSlots = 8;
    constexpr size_t kMaxLabel = 256;
    static thread_local char buffers[kSlots][kMaxLabel];
    static thread_local size_t next = 0;
    char* buffer = buffers[next];
    next = (next + 1) % kSlots;
    std::snprintf(buffer, kMaxLabel, "%s###%s", text, id);
    return buffer;
}

// Item-pill-sized icon button (28x28, true circle, 13px icon), reused by
// the Overview's per-tile delete button too.
bool PillIconButton(const char* strId, const Icon& icon, bool active) {
    return IconButton(strId, icon, active, Px(kPillButtonSize), Px(13.0f), theme::kRadiusPill);
}

// The color button where it has to say whether it is *on*: the pill
// every icon tile beside it wears - accent while on, plain while off -
// with the color as a swatch where the icon would be. Used by the bar
// rows in Settings > Interaction, where the row is read as "these buttons
// are on the bar and these are not", and the swatch alone could not say
// which it was: PillColorButton's ring is its hover/selected cue, and
// against nine other tiles whose whole background answers the question, a
// ring on one of them does not read as an answer at all.
bool PillSwatchButton(const char* strId, uint32_t colorRGBA, bool active) {
    // The same three colors and the same Button underneath as IconButton,
    // so the two kinds of tile hover and press alike.
    active = active || PressLandsThisFrame(strId, Px(kPillButtonSize, kPillButtonSize));
    ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::Accent() : theme::kFieldBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? theme::Accent() : theme::kHoverWash);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::AccentHover());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kRadiusPill);
    const bool pressed = ImGui::Button(strId, Px(kPillButtonSize, kPillButtonSize));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    const ImVec2 minPt = ImGui::GetItemRectMin();
    const ImVec2 maxPt = ImGui::GetItemRectMax();
    const ImVec2 center((minPt.x + maxPt.x) * 0.5f, (minPt.y + maxPt.y) * 0.5f);
    // The swatch the selection bar's own color button draws, at its size.
    constexpr float kSwatchRadius = 7.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Through GetColorU32, which multiplies by the style's own alpha, so
    // the swatch fades with the tile when the row grays a switched-off
    // button out. Drawn straight to the draw list with the packed color,
    // it stayed fully bright while the pill behind it and every icon
    // beside it went dim - which left the one tile whose state could not
    // be read as the one tile that had to say it.
    dl->AddCircleFilled(center, Px(kSwatchRadius),
                        ImGui::GetColorU32(ImGui::ColorConvertU32ToFloat4(ToImColor(colorRGBA))));
    // Ringed in whichever ink the icons beside it are using, so a swatch
    // close to the color of the pill behind it still has an edge -
    // GetColorU32 so the ring dims with the tile when the row grays it
    // out, the same reasoning as IconButton's own icon color.
    dl->AddCircle(center, Px(kSwatchRadius), ImGui::GetColorU32(active ? theme::AccentInk() : theme::kWhite), 0,
                  Px(1.5f));
    return pressed;
}

// The same button standing for a color instead of an action: a filled
// circle, no glyph. Same 28x28 hit target as PillIconButton, so the two
// drop into the same layout wherever a slot might hold either.
// `highlighted`
// mirrors PillIconButton's `active`, but as a ring around the swatch
// rather than a filled background: the swatch's own fill already carries
// the color, so there is no separate "on" background the way an icon
// button has.
bool PillColorButton(const char* strId, uint32_t colorRGBA, bool highlighted) {
    const float size = Px(kPillButtonSize);
    const bool pressed = ImGui::InvisibleButton(strId, ImVec2(size, size));
    const ImVec2 minPt = ImGui::GetItemRectMin();
    const ImVec2 maxPt = ImGui::GetItemRectMax();
    const ImVec2 center((minPt.x + maxPt.x) * 0.5f, (minPt.y + maxPt.y) * 0.5f);
    const auto r = static_cast<int>((colorRGBA >> 24) & 0xFF);
    const auto g = static_cast<int>((colorRGBA >> 16) & 0xFF);
    const auto b = static_cast<int>((colorRGBA >> 8) & 0xFF);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, size * 0.5f - Px(3.0f), IM_COL32(r, g, b, 255));
    // A permanent hairline rim, not just the hover/selected ring below.
    // Without it a dark swatch on a dark backing has no edge at all and
    // reads as a hole rather than as a color - which is what black looked
    // like the moment it was added to the palette.
    dl->AddCircle(center, size * 0.5f - Px(3.0f), ImGui::ColorConvertFloat4ToU32(theme::kPanelBorderStrong), 0, 1.0f);
    if (highlighted || ImGui::IsItemHovered()) {
        dl->AddCircle(center, size * 0.5f - Px(1.0f), ImGui::ColorConvertFloat4ToU32(theme::kWhite), 0, Px(1.5f));
    }
    return pressed;
}

// The pill's Del / Overview tile's delete button - same size as
// PillIconButton but danger-red instead of accent-on-active, matching
// .icon-btn.danger:hover (idle stays neutral; only hover/press go red).
bool DangerIconButton(const char* strId, const Icon& icon) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::kDangerSoft);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kDanger);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kRadiusPill);
    const bool pressed = ImGui::Button(strId, Px(kPillButtonSize, kPillButtonSize));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    const ImVec2 minPt = ImGui::GetItemRectMin();
    const ImVec2 maxPt = ImGui::GetItemRectMax();
    const float iconSize = Px(13.0f);
    const ImVec2 iconPos((minPt.x + maxPt.x - iconSize) * 0.5f, (minPt.y + maxPt.y - iconSize) * 0.5f);
    DrawIcon(ImGui::GetWindowDrawList(), icon, iconPos, iconSize, ImGui::GetColorU32(theme::kWhite));
    return pressed;
}

ImDrawList* BeginScreenLayer(const char* id, float displayW, float displayH) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(displayW, displayH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(id, nullptr,
                  ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                      ImGuiWindowFlags_NoFocusOnAppearing);
    return ImGui::GetWindowDrawList();
}

void EndScreenLayer() {
    ImGui::End();
    ImGui::PopStyleVar();
}

// Re-asserts `name`'s window as the frontmost, undoing whatever position
// ImGui's own insertion/focus history left it at. Call once per frame, in
// back-to-front order, for every window whose stacking needs to track the
// data model rather than ImGui's default focus-driven ordering.
void BringToFront(const char* name) {
    if (ImGuiWindow* window = ImGui::FindWindowByName(name)) {
        ImGui::BringWindowToDisplayFront(window);
    }
}

// Reasserts the *currently open* popup (call from inside its own
// BeginPopup/EndPopup scope) to the front of the display order - needed
// every single frame it's open, not just the frame it was created on: see
// this function's call site in RenderItemPropertiesPopover for why an
// opener window's own per-frame
// BringToFront(name) would otherwise quietly win the front spot back on
// every later frame. Takes the window pointer directly rather than going
// through the name-based BringToFront above: an anonymous popup's actual
// ImGuiWindow name isn't the id string passed to OpenPopup/BeginPopup
// (that's only the ID seed), so a name lookup for it wouldn't find
// anything - GetCurrentWindow() is the only reliable way to get it.
void KeepPopoverInFront() { ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); }

// See this function's own doc comment (overlay_app_internal.h) for why it's
// needed at all. g.OpenPopupStack is one shared stack for every currently
// open popup app-wide, nested in open order; g.BeginPopupStack reflects how
// many levels of that stack are still inside an active Begin/End scope
// right now - since a widget like ColorEdit3 has already closed its own
// nested popup's Begin/End pair by the time it returns, anything from
// BeginPopupStack.Size onward in OpenPopupStack is exactly "still open, but
// with no Begin call left this frame to reassert it" - reach into
// imgui_internal.h and do that reassertion here instead.
// See this function's own doc comment (overlay_app_internal.h). Closing to
// "one fewer than are open" is ImGui's own way of saying "just the
// innermost"; restore_focus_to_window_under_popup hands focus back to the
// panel underneath, which is where it came from.
bool CloseTopmostPopover() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.OpenPopupStack.Size == 0) {
        return false;
    }
    ImGui::ClosePopupToLevel(g.OpenPopupStack.Size - 1, true);
    return true;
}

void KeepChildPopupsInFront() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = g.BeginPopupStack.Size; i < g.OpenPopupStack.Size; ++i) {
        if (ImGuiWindow* popupWindow = g.OpenPopupStack[i].Window) {
            ImGui::BringWindowToDisplayFront(popupWindow);
        }
    }
}

// Converts our 0xRRGGBBAA packing (see stroke.h) to ImGui's native ImU32.
ImU32 ToImColor(uint32_t colorRGBA, float opacity) {
    const auto r = static_cast<int>((colorRGBA >> 24) & 0xFF);
    const auto g = static_cast<int>((colorRGBA >> 16) & 0xFF);
    const auto b = static_cast<int>((colorRGBA >> 8) & 0xFF);
    const auto a = static_cast<int>(static_cast<float>(colorRGBA & 0xFF) * opacity);
    return IM_COL32(r, g, b, a);
}

// See the declarations in overlay_app_internal.h.
void ColorRGBAToFloats(uint32_t colorRGBA, float out[3]) {
    out[0] = static_cast<float>((colorRGBA >> 24) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((colorRGBA >> 16) & 0xFF) / 255.0f;
    out[2] = static_cast<float>((colorRGBA >> 8) & 0xFF) / 255.0f;
}

uint32_t FloatsToColorRGBA(const float in[3], uint8_t alpha) {
    auto Channel = [](float f) { return static_cast<uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (Channel(in[0]) << 24) | (Channel(in[1]) << 16) | (Channel(in[2]) << 8) | alpha;
}

void ColorRGBAToFloats4(uint32_t colorRGBA, float out[4]) {
    ColorRGBAToFloats(colorRGBA, out);
    out[3] = static_cast<float>(colorRGBA & 0xFF) / 255.0f;
}

uint32_t FloatsToColorRGBA4(const float in[4]) {
    return FloatsToColorRGBA(in, static_cast<uint8_t>(std::clamp(in[3], 0.0f, 1.0f) * 255.0f + 0.5f));
}

// Tessellated here rather than handed to ImGui's AddPolyline, which offsets
// each point along the average of its two adjacent segment normals and
// rescales by 1/cos^2 of half the turn - a miter clamped only at 100x the
// half width, so a near-reversal threw a spike out the side of a wide pen.
// It also has no cap but a flat one and no round join at all. See
// stroke_mesh.h for the geometry this builds instead, and why one connected
// mesh with no overlapping triangles is what a translucent stroke needs.
//
// The fringe is one pixel in *screen* space, so it stays a pixel wide
// whatever the item is scaled to - see kStrokeFringePx, which both this and
// the mesh cache take it from.
//
// The scale is baked into the geometry and the offset is not: the mesh comes
// out around the origin and is translated as its vertices are written. That
// is what makes a mesh worth keeping between frames - see StrokeMeshSlot.
void DrawStroke(ImDrawList* drawList, const Stroke& stroke, StrokeRenderMode rendering, float offsetX,
                 float offsetY, float scaleX, float scaleY, float opacity, StrokeMeshSlot meshSlot) {
    if (stroke.points.empty() || opacity <= 0.0f) {
        return;
    }
    const float scaleAvg = (std::abs(scaleX) + std::abs(scaleY)) / 2.0f;
    const float halfWidth = stroke.width * scaleAvg * 0.5f;
    const ImU32 color = ToImColor(stroke.colorRGBA, opacity);

    if (rendering == StrokeRenderMode::Polyline) {
        // Reused between calls rather than allocated per stroke - and,
        // unlike the tessellated path below, positioned rather than merely
        // scaled: AddPolyline takes screen coordinates and there is nothing
        // here to translate afterwards.
        static std::vector<StrokePoint> screenPoints;
        screenPoints.clear();
        screenPoints.reserve(stroke.points.size());
        for (const StrokePoint& p : stroke.points) {
            screenPoints.push_back(StrokePoint{offsetX + p.x * scaleX, offsetY + p.y * scaleY});
        }
        // Kept switchable to compare the tessellator against - see
        // StrokeRenderMode::Polyline. ImGui's AddPolyline
        // leaves flat ends, so a disc goes on each one: the caps are not
        // what is being compared, and without them the two renderers differ
        // in an obvious way that has nothing to do with the tessellation.
        //
        // Those discs do overlap the line they cap, which a translucent
        // stroke shows as a darker blob at each end - a fair part of what
        // the tessellated path exists to avoid, and the reason it can't
        // simply be done this way.
        if (screenPoints.size() >= 2) {
            static std::vector<ImVec2> polyline;
            polyline.clear();
            polyline.reserve(screenPoints.size());
            for (const StrokePoint& p : screenPoints) {
                polyline.push_back(ImVec2(p.x, p.y));
            }
            drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), color,
                                   stroke.width * scaleAvg);
        }
        drawList->AddCircleFilled(ImVec2(screenPoints.front().x, screenPoints.front().y), halfWidth, color);
        drawList->AddCircleFilled(ImVec2(screenPoints.back().x, screenPoints.back().y), halfWidth, color);
        return;
    }

    // The mesh is built around the origin and translated as it is written
    // out below, rather than built at the position it will appear. That is
    // what lets a cached mesh survive its item being dragged: the offset is
    // the only thing a move changes, and it never reaches the tessellator.
    // It also keeps the geometry math at small coordinates, which is where
    // floats behave best.
    const StrokeMesh* mesh = nullptr;
    if (meshSlot.cache != nullptr) {
        mesh = &meshSlot.cache->MeshFor(meshSlot.itemId, meshSlot.strokeIndex, stroke, meshSlot.generation, scaleX,
                                         scaleY, halfWidth);
    } else {
        // Nothing to cache it in - the stroke being drawn right now, which
        // grows every frame, or a caller that keeps no cache. Static rather
        // than local so the two buffers are reused frame to frame; this is
        // the render thread, and nothing here re-enters.
        static StrokeMesh uncached;
        static std::vector<StrokePoint> scaledPoints;
        scaledPoints.clear();
        scaledPoints.reserve(stroke.points.size());
        for (const StrokePoint& p : stroke.points) {
            scaledPoints.push_back(StrokePoint{p.x * scaleX, p.y * scaleY});
        }
        BuildStrokeMesh(scaledPoints, halfWidth, kStrokeFringePx, uncached);
        mesh = &uncached;
    }
    if (mesh->indices.empty()) {
        return;
    }

    // Coverage is only ever 0 or 1 out of the builder - solid inside, and the
    // outer edge of the anti-aliasing fringe - so two colors cover it.
    const ImU32 transparent = color & ~IM_COL32_A_MASK;
    const ImVec2 uv = drawList->_Data->TexUvWhitePixel;

    drawList->PrimReserve(static_cast<int>(mesh->indices.size()), static_cast<int>(mesh->vertices.size()));
    // After PrimReserve, which is what may start a fresh draw command (and
    // reset this) when a mesh crosses the 16-bit index ceiling.
    const unsigned int base = drawList->_VtxCurrentIdx;
    for (const StrokeVertex& v : mesh->vertices) {
        drawList->PrimWriteVtx(ImVec2(offsetX + v.x, offsetY + v.y), uv, v.coverage >= 1.0f ? color : transparent);
    }
    for (const uint32_t index : mesh->indices) {
        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
    }
}

// An item's fill (its captured image/placeholder gradient/plain color
// fill, at backgroundOpacity) plus its baked strokes (at
// foregroundOpacity), into `pMin..pMax` - the shared core of both
// RenderItems' per-item interactive window, RenderViewOnly's flat
// read-only pass, and the dock's own thumbnail chips (see RenderDock).
// Caller owns clipping (PushClipRect/PopClipRect) around this.
void DrawPicture(ImDrawList* drawList, uint64_t texture, ImVec2 pMin, ImVec2 pMax, ImU32 tint,
                  ImageSampling sampling) {
    const bool filtered = sampling.apply != nullptr && sampling.filter != platform::ImageFilter::Bilinear;
    if (filtered) {
        drawList->AddCallback(sampling.apply, reinterpret_cast<void*>(static_cast<intptr_t>(sampling.filter)));
    }
    drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(texture)), pMin, pMax, ImVec2(0.0f, 0.0f),
                        ImVec2(1.0f, 1.0f), tint);
    if (filtered) {
        // Null only where no renderer backend is set up, which is also
        // where nothing is drawn.
        if (const ImDrawCallback reset = ImGui::GetPlatformIO().DrawCallback_ResetRenderState) {
            drawList->AddCallback(reset, nullptr);
        }
    }
}

void DrawSnippetPicture(ImDrawList* drawList, const Picture& picture, ImVec2 pMin, ImVec2 pMax, uint64_t texture,
                        ImageSampling sampling) {
    if (picture.opacity <= 0.0f) {
        return;
    }
    if (texture != 0) {
        // Real pixels - AddImage stretches the whole texture to fill
        // pMin..pMax on its own, the same way the gradient/fill below fills
        // whatever rect the item currently has, so resizing the item needs
        // no extra handling here. The tint multiplies the sampled texture
        // (see Picture::tintColorRGBA) - white, the default, leaves a
        // capture unmodified; any other color mixes into it.
        DrawPicture(drawList, texture, pMin, pMax, ToImColor(picture.tintColorRGBA, picture.opacity), sampling);
    } else if (picture.showsPlaceholder) {
        // No pixels to show (the OS-level capture failed, or the picture
        // cannot be read) - a placeholder gradient, faded by the same
        // opacity a real capture would use.
        const ImU32 top = ImColor::HSV(picture.placeholderHue / 360.0f, 0.38f, 0.55f, picture.opacity);
        const ImU32 bottom = ImColor::HSV(picture.placeholderHue / 360.0f, 0.24f, 0.82f, picture.opacity);
        drawList->AddRectFilledMultiColor(pMin, pMax, top, top, bottom, bottom);
    } else {
        // A picture given a solid color instead - just the color itself, no
        // image to tint.
        drawList->AddRectFilled(pMin, pMax, ToImColor(picture.tintColorRGBA, picture.opacity));
    }
}

void DrawItemContent(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, StrokeRenderMode rendering,
                      uint64_t pictureTexture, uint64_t strokeRasterTexture, bool skipNoteText,
                      StrokeMeshSlot meshCache, ImageSampling sampling) {
    DrawSnippetPicture(drawList, item.picture, pMin, pMax, pictureTexture, sampling);

    if (rendering == StrokeRenderMode::Rasterized && strokeRasterTexture != 0) {
        // Every stroke, already drawn into one bitmap and composited here
        // as a single image - which is what makes a stroke that crosses
        // over itself one even color instead of darker at the crossing.
        // The opacity is applied once, to the finished picture, rather than
        // per stroke.
        DrawPicture(drawList, strokeRasterTexture, pMin, pMax, ToImColor(0xFFFFFFFFu, item.foregroundOpacity),
                    sampling);
    } else {
        const float scaleX = item.nativeW != 0.0f ? (pMax.x - pMin.x) / item.nativeW : 1.0f;
        const float scaleY = item.nativeH != 0.0f ? (pMax.y - pMin.y) / item.nativeH : 1.0f;
        // Rasterized with no raster to draw falls back to Tessellated
        // rather than to nothing - see this function's own declaration.
        const StrokeRenderMode perStroke =
            rendering == StrokeRenderMode::Rasterized ? StrokeRenderMode::Tessellated : rendering;
        for (size_t index = 0; index < item.strokes.size(); ++index) {
            DrawStroke(drawList, item.strokes[index], perStroke, pMin.x, pMin.y, scaleX, scaleY,
                        item.foregroundOpacity, meshCache.For(item.id, index));
        }
    }

    // Text (Item::noteText, see its own doc comment) is a caption layered
    // on top of whatever's already here - a background image, strokes, or
    // both - rather than replacing either, so a snippet's drawing/
    // screenshot content and its text can freely coexist on one item. A
    // band pinned to the content's own top edge, sized to fit the wrapped
    // text, not the whole item, so it reads as a caption
    // rather than swallowing whatever it's sitting on. Skipped for
    // whichever item skipNoteText names - the one currently open in
    // RenderNoteEditor's own window, which paints its own
    // matching backing panel over the same rect - to avoid double-drawing
    // the same text under that window's own InputTextMultiline.
    if (!item.noteText.empty() && !skipNoteText) {
        // No panel fill behind the text (kept transparent on purpose - see
        // Item::noteText's own doc comment: a user who wants a backdrop
        // already has the item's own background color/opacity controls,
        // so this doesn't need to impose one automatically). Just clip to
        // the content rect so wrapped text can't bleed past it.
        // Color and size both come off the item itself now (see
        // Item::noteTextColorRGBA/noteTextSizePx) rather than being fixed
        // at white-and-whatever-the-UI-font-is - RenderNoteEditor's live
        // editor reads the exact same two fields, so switching between
        // editing and not doesn't change how the text looks.
        const float wrapWidth = std::max(0.0f, (pMax.x - pMin.x) - 2.0f * theme::kNoteTextPad);
        drawList->PushClipRect(pMin, pMax, true);
        drawList->AddText(ImGui::GetFont(), item.noteTextSizePx,
                           ImVec2(pMin.x + theme::kNoteTextPad, pMin.y + theme::kNoteTextPad),
                           ToImColor(item.noteTextColorRGBA), item.noteText.c_str(), nullptr, wrapWidth);
        drawList->PopClipRect();
    }
}

}  // namespace overlay_detail

OverlayApp::OverlayApp(Settings& settings, Session& session)
    : editor_(settings, session), session_(session), settings_(settings) {
    editor_.SetViews(this);
}

void OverlayApp::AttachTo(platform::IOverlayWindow& window) {
    window_ = &window;
    editor_.AttachWindow(&window);
    window.SetFrameCallback([this](float dt) { OnFrame(dt); });
    window.SetInputCallback([this](const platform::InputEvent& ev) { OnInput(ev); });
}

void OverlayApp::SetViewOnly(bool viewOnly) {
    if (viewOnly == viewOnly_) {
        return;
    }
    viewOnly_ = viewOnly;
    if (!viewOnly_) {
        // A notice is only ever a kind of view-only (see SetNoticeOnly), so
        // it cannot outlive it. TrayController clears it explicitly on the
        // way into any mode as well, which is what covers entering
        // view-only *from* a notice - that case never reaches here, since
        // the mode is already what is being asked for.
        noticeOnly_ = false;
        pinnedOnly_ = false;
    }
    if (viewOnly_) {
        // Nothing should stay "in progress" while merely viewing: the hand
        // is settled first, while drawing mode still says which snippet a
        // stroke in flight belongs to - and a note being typed with it,
        // whose editor is not drawn in view-only mode and so would never
        // hear that it closed. Then the creation tool is put down and every
        // transient edit-mode surface closed, so re-entering edit mode
        // later starts clean rather than resuming whatever popover happened
        // to be up.
        SettleHand();
        editor_.PutDownCreationTool();
        editor_.ExitDrawingMode();
        editor_.SettleUntouchedDrawing();
        CloseOverview();
        itemPropertiesPopoverItemId_.reset();
        confirmDeleteTarget_.reset();
        effects_.clear();
        // Normally cleared at the top of every RenderItems call - which
        // view-only mode never runs, so without this the debug overlay's
        // "resize handle:" line would keep showing whatever handle
        // happened to be hovered on the last edit-mode frame for the
        // whole view-only session.
        debugHoveredResizeHandle_.clear();
    }
}

void OverlayApp::SettleForPersistence() {
    SettleHand();
    // Going away is moving on too, as the next showing would say (see
    // OnOverlayShown) - but exit has no next showing, and a restart loads
    // the drawing as an ordinary snippet: a fullscreen empty one, over the
    // canvas.
    editor_.SettleUntouchedDrawing();
}

void OverlayApp::SettleHand() {
    EndGesture();
    editor_.CommitNoteBeingEdited();
    // And the rest of the hand with it, whole - the press a hold or a
    // double-click would be judged on, and which buttons are down: as far
    // as this knows none is from here, and the release still to come finds
    // nothing to end. Replaced rather than cleared field by field, so
    // nothing added to Hand later can be missed.
    hand_ = Hand{};
}

// ================= Frame =================

// Brackets one frame's use of the two stroke-mesh caches. A struct rather
// than a pair of calls because OnFrame has an early return in it (view-only
// mode), and a cache left thinking its frame is still running never drops
// what that frame didn't draw.
struct OverlayApp::MeshCacheFrame {
    StrokeMeshCache& canvas;
    StrokeMeshCache& preview;
    MeshCacheFrame(StrokeMeshCache& canvasCache, StrokeMeshCache& previewCache)
        : canvas(canvasCache), preview(previewCache) {
        canvas.BeginFrame();
        preview.BeginFrame();
    }
    ~MeshCacheFrame() {
        canvas.EndFrame();
        preview.EndFrame();
    }
    MeshCacheFrame(const MeshCacheFrame&) = delete;
    MeshCacheFrame& operator=(const MeshCacheFrame&) = delete;
};

void OverlayApp::HandleMouseWheel(float notches) {
    // What the wheel does is told apart by a modifier, and without one by
    // the mode. All of it is suppressed while the Overview is up: it has
    // its own canvas navigation and its own scroll, and having the wheel
    // quietly change something underneath it would be a surprise.
    //
    // Alt held: step between the canvases of the current canvas's own
    // folder - Alt being the modifier that already means "select, whatever
    // tool is in hand" for a press (see HandleItemGesture). This one
    // deliberately does not yield to an ImGui widget, matching an Alt
    // press; everything below does.
    //
    // Ctrl or Shift held: the selection's background or foreground
    // opacity, in either mode - in drawing mode the selection is the
    // snippet being drawn on.
    //
    // Nothing held: in drawing mode, stroke/eraser size, the near-universal
    // convention in drawing tools and the one tool "option" reached for
    // *during* work rather than while configuring - which is why there is
    // no width slider anywhere. Outside it, the selection's size. The mode
    // is what decides, not whether something happens to be selected: in
    // drawing mode something always is, and the wheel must not start
    // scaling the snippet under the pen.
    //
    // None of it while a gesture is in flight: the wheel is on the mouse
    // that is holding the gesture, and input from the gesture's own device
    // waits for it to end rather than settling it, as a second button's
    // does (see Hand::ignoredButton) - a notch nudged in the middle of a
    // stroke would switch the canvas under it, and one mid-drag would be
    // filed inside the drag, undone to a size the drag then wrote over.
    if (notches == 0.0f || PanelOpen() || GestureInFlight()) {
        return;
    }
    {
        const platform::Modifiers& held = editor_.Held();
        if (held.alt) {
            // Wheel up goes back through the list, wheel down forward -
            // the direction a page scrolls, applied to canvases.
            if (const int steps = TakeWheelSteps(canvasWheelRemainder_, notches); steps != 0) {
                editor_.SwitchCanvasByOffset(-steps);
            }
        } else if (ImGui::GetIO().WantCaptureMouse) {
            // A widget under the pointer has the wheel.
        } else if (held.ctrl != held.shift) {
            if (const int steps = TakeWheelSteps(selectionWheelRemainder_, notches); steps != 0) {
                editor_.StepSelectionOpacity(steps, /*background=*/held.ctrl);
            }
        } else if (held.ctrl) {
            // Both held: neither opacity is meant more than the other.
        } else if (!editor_.DrawingItem().has_value()) {
            if (const int steps = TakeWheelSteps(selectionWheelRemainder_, notches); steps != 0) {
                editor_.ScaleSelectionByWheel(steps);
            }
        } else {
            const int steps = TakeWheelSteps(sizeWheelRemainder_, notches);
            if (steps == 0) {
                // Nothing whole came out of the accumulator yet (a
                // high-resolution wheel mid-notch) - not a size change,
                // so nothing to show either.
            } else {
                const auto step = static_cast<float>(steps);
                bool sizeChanged = true;
                switch (editor_.ActiveTool()) {
                    case Tool::Draw:
                        editor_.SetDrawWidth(std::clamp(editor_.DrawWidth() + step, 1.0f, 24.0f));
                        drawWidthDirty_ = true;
                        break;
                    case Tool::Erase:
                        editor_.SetEraserWidth(std::clamp(editor_.EraserWidth() + step * 2.0f, 8.0f, 64.0f));
                        break;
                    case Tool::Text:
                    case Tool::Select:
                    case Tool::NewDrawing:
                    case Tool::NewScreenshot:
                        sizeChanged = false;  // none of these has a size of its own - see the Tool enum
                        break;
                }
                if (sizeChanged) {
                    // Armed even when the value was already at its clamp:
                    // "you're at the maximum" is feedback too, and a wheel
                    // step that showed nothing at all would read as the
                    // wheel not working.
                    sizePreviewExpireAtSeconds_ = ImGui::GetTime() + kSizePreviewHoldSeconds;
                }
            }
        }
    }
}

void OverlayApp::OnFrame(float /*deltaSeconds*/) {
    // Function scope, so every path out of here - including view-only mode's
    // own early return - closes the frame out.
    const MeshCacheFrame meshCacheFrame(strokeMeshCache_, previewMeshCache_);
    // The display the canvas is on, for the editor - which draws nothing
    // and so has no other way to know it.
    editor_.SetDisplaySize(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);

    // The interface scale, before anything is drawn, so a whole frame is
    // drawn at one scale. The setting, or Windows' own for the display the
    // overlay is on - asked every frame, so the overlay follows a change
    // made in Windows while it is up.
    {
        const int percent = Cfg().uiScalePercent != 0 ? Cfg().uiScalePercent
                                                        : (window_ != nullptr ? window_->ScalePercent() : 100);
        if (!styleApplied_ || percent != appliedUiScalePercent_) {
            SetUiScale(static_cast<float>(percent) / 100.0f);
            ApplySpickzettelStyle(UiScale());
            appliedUiScalePercent_ = percent;
        }
    }
    // A new note's text size, decided once: the default at Windows' scale
    // for the display the overlay first came up on - see
    // AppConfig::noteTextSizePx. Windows' rather than the interface's,
    // which is the same thing unless someone has already set the other.
    if (Cfg().noteTextSizePx <= 0.0f && window_ != nullptr) {
        const float scale = static_cast<float>(window_->ScalePercent()) / 100.0f;
        Cfg().noteTextSizePx =
            std::clamp(std::round(kDefaultNoteTextSizePx * scale), kNoteTextSizeMin, kNoteTextSizeMax);
        settings_.Commit();
    }
    if (!styleApplied_) {
        // No imgui.ini. ImGui writes one next to the working directory to
        // remember window positions and sizes, and this app has nothing to
        // remember: every window it opens - the canvas layer, each item's
        // chrome, the popovers, the Overview, the dock - sets its own position
        // and size explicitly on every frame, from the data model. Nothing
        // ever reads a saved position back, so the file was purely clutter
        // dropped wherever the exe happened to be launched from. Turned off
        // here rather than pointed at the config directory, because a file
        // nobody reads isn't worth relocating.
        //
        // Set from core, on the first frame, so both platform backends get
        // it from one place - and safely, since ImGui only writes the file
        // some seconds after a settings change, never during startup. The
        // ImGuiWindowFlags_NoSavedSettings already on several windows is
        // now redundant but harmless, and still documents the intent.
        ImGui::GetIO().IniFilename = nullptr;
        styleApplied_ = true;
    }
    // A press held still sends no events, so its hold matures here, on the
    // clock - see Hand::heldPress. Not in view-only mode, where no press
    // reaches the app in the first place.
    if (!viewOnly_) {
        MatureHeldPress();
    }
    // The accent, whenever the setting differs from what was last applied -
    // on every frame while a color is being dragged in Settings, so the
    // whole overlay recolors as it moves. The theme's accessors and the
    // ImGui style both carry it.
    if (appliedAccentRGBA_ != Cfg().accentColorRGBA) {
        theme::SetAccent(Cfg().accentColorRGBA);
        ApplyAccentToStyle(ImGui::GetStyle());
        appliedAccentRGBA_ = Cfg().accentColorRGBA;
    }
    // Before anything is drawn, so that the frozen image this frame will
    // use as its backdrop is the one taken on the new monitor rather than
    // one released halfway through the frame - see
    // displayChoiceCommitPending_.
    if (displayChoiceCommitPending_) {
        displayChoiceCommitPending_ = false;
        settings_.Commit();
    }
    // Every display refresh, or only now and then - see
    // IOverlayWindow::SetFramePacing. Decided at the start of a frame from
    // state that changes between frames (a hotkey, a mode switch), so the
    // frame that follows a change already runs at the new pace.
    //
    // View-only - the pinned view included - shows a picture that doesn't
    // change by itself, so it is idle unless something on it moves: a
    // message fading, which is all a notice is. What it shows otherwise
    // changes only through something that arrives as a message, like a
    // capture hotkey, and a message always gets a frame. The debug overlay
    // is the exception, since it follows the pointer.
    {
        const bool toastShowing = !actionToastText_.empty() && ImGui::GetTime() < actionToastExpireAtSeconds_;
        const platform::FramePacing pacing = viewOnly_ && !noticeOnly_ && !toastShowing && !Cfg().showDebugOverlay
                                                 ? platform::FramePacing::Idle
                                                 : platform::FramePacing::EveryFrame;
        if (window_ != nullptr && appliedFramePacing_ != pacing) {
            window_->SetFramePacing(pacing);
            appliedFramePacing_ = pacing;
        }
    }
    // Before anything draws: what the last frame drew and this one has not
    // asked for yet is let go of (see TextureCache::BeginFrame), and the
    // current canvas's pictures - from the library, the first time - are
    // asked for, which is what keeps them.
    Textures().BeginFrame();
    KeepCurrentCanvasTextures();

    const ImGuiIO& io = ImGui::GetIO();
    const float displayW = io.DisplaySize.x;
    const float displayH = io.DisplaySize.y;

    // Live, not just at startup - see
    // CanvasManager::SyncItemsToDisplaySize's own doc comment. Cheap: a
    // no-op comparison per item whenever the display hasn't changed since
    // last frame, which is every frame but the one right after an actual
    // change.
    session_.SyncItemsToDisplaySize(displayW, displayH);

    // First run, first frame that knows how big the screen is - see
    // RequestWelcomeNote for why this waits rather than happening at
    // startup.
    if (welcomeNotePending_ && displayW > 0.0f && displayH > 0.0f) {
        welcomeNotePending_ = false;
        PlaceWelcomeNotes(displayW, displayH);
    }

    if (viewOnly_) {
        // A notice is this same click-through mode with the canvas left
        // out: nothing of the library on screen, only the message. See
        // SetNoticeOnly.
        if (!noticeOnly_) {
            RenderViewOnly(displayW, displayH);
        }
        // Drawn in view-only too, not just in edit mode where it started.
        // A hotkey that acts while the overlay is merely being looked
        // through still did something, and this is the only thing that
        // says so - it costs a frame that was being drawn anyway, and
        // click-through means it cannot get in the way of anything.
        RenderActionToast();
        RenderPersistenceWarning();
        // A notice exists only to carry that message, so it is over when
        // the message is - faded, or never set at all, which is the same
        // condition RenderActionToast draws nothing on. Reported once (see
        // noticeFinishedReported_); the window goes away on the other end.
        if (noticeOnly_ && !noticeFinishedReported_ &&
            (actionToastText_.empty() || ImGui::GetTime() >= actionToastExpireAtSeconds_)) {
            noticeFinishedReported_ = true;
            if (noticeFinishedCallback_) {
                noticeFinishedCallback_();
            }
        }
        return;
    }

    // A drawing a stray click made goes once the hand has moved on from it
    // (see Editor::UntouchedDrawing). A press elsewhere settles it as it
    // happens (OnMouse); this catches moving on without one.
    editor_.WatchUntouchedDrawing();

    // Nothing acts on a snippet that has gone - see Editor::Selection. The
    // keys and the wheel have been handled as they came (see OnInput).
    editor_.PruneSelection();

    // Over a snippet a plain drag would pick up - the selection live, and
    // not in drawing mode unless Alt is held - the four-way arrow says so.
    // Only over a snippet: on empty canvas a press clears the selection.
    // Set before the render calls below rather than after, so anything
    // more specific - a handle's own directional cursor, the dock chips'
    // hand - still wins where it applies.
    if (editor_.SelectionLive() && editor_.PressPicksUp() && !io.WantCaptureMouse) {
        const ImVec2 mouse = ImGui::GetMousePos();
        if (editor_.ResolvePointerTarget(mouse.x, mouse.y).kind == PointerTarget::Kind::Body) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }

    // In the rasterized mode, the bitmaps the strokes are drawn into. Last
    // thing before anything item-shaped is drawn: everything above this
    // line is input handling, and Alt+wheel canvas stepping lives up there.
    // A picture needs nothing of the kind - its texture is asked for as it
    // is drawn, so a canvas switched to mid-frame is drawn whole.
    RefreshStrokeRasters();

    // Where the panels docked against the screen's edges are this frame, and
    // how far out - before anything is drawn, since the minimized chips
    // RenderItems draws have to clear the ones on the bottom edge.
    UpdateEdgePanels(displayW, displayH);

    RenderCanvasLayer(displayW, displayH);
    RenderItems(displayW, displayH);
    // Over the items: the canvas bar, and the popovers - the properties
    // popover a snippet's More button opens, and the color chooser.
    RenderCanvasBar(displayW, displayH);
    // What was asked for that only a frame can do - see Effect - just
    // before the popups it opens are drawn.
    ApplyEffects();
    RenderItemPropertiesPopover();
    // And the menu a right-click on a snippet opens - beside the popover
    // rather than inside it: the two hold the same actions and are opened
    // different ways, and only one of them can be up at a time anyway,
    // since opening either closes whatever popup was there.
    RenderItemContextMenu();
    // And the canvas bar's, for the tile that was right-clicked - out here
    // rather than inside the bar's own window so that it is a popup at the
    // same level as every other, and so it survives a frame in which the
    // bar itself does not draw.
    RenderCanvasContextMenu();
    // And empty canvas's, the same way.
    RenderEmptyCanvasMenu();
    // After both things that can open it, so it opens on the frame after
    // either asked - and at the top level every frame, so the popup always
    // belongs to the same window whichever of the two it came from.
    RenderColorChooser(displayW, displayH);
    RenderRegionCaptureOverlay();
    RenderRectEraserOverlay();
    RenderBrushSizePreview();
    RenderToolModifierBadge();
    // Over everything the canvas holds, under the Overview - see its own
    // doc comment for what is in it and in which order.
    RenderScreenChrome(displayW, displayH);
    // Last, so it renders on top of everything above without needing its
    // own BringToFront - see the include comment for why that'd otherwise
    // be necessary.
    RenderOverview(displayW, displayH);
    RenderCheatSheet(displayW, displayH);
    RenderConfirmDeletePopover();
    RenderActionToast();
    RenderPersistenceWarning();
    HandleInputOptionsHudKeys();
    ApplyPointerShape();
    DrawSoftwareCursor();
}

// Which shape the OS cursor should wear, decided here at the end of the
// frame rather than at the start - because ImGui only knows what it wants
// once every widget has run, and this must not overrule it. A resize
// handle's directional arrow, the dock's hand and the note editor's I-beam
// are all ImGui's to install; asking the platform for a crosshair on top of
// them is what made them flash and vanish.
//
// Asks for Default while the software pointer is drawn rather than worn by
// the OS: the OS cursor is hidden then, so this would be shaping something
// invisible.
void OverlayApp::ApplyPointerShape() {
    if (!window_) {
        return;
    }
    const ImGuiMouseCursor imguiCursor = ImGui::GetMouseCursor();
    const bool imguiOwnsIt = imguiCursor != ImGuiMouseCursor_Arrow;
    const platform::CursorShape wanted =
        (imguiOwnsIt || settings_.Live().InputOptions().SoftwarePointerDrawn()) ? platform::CursorShape::Default
                                                               : WantedPointerShape();

    // Re-asserted when either half of the decision moves, and not otherwise.
    //
    // Pushing it every frame was the previous version, and the note here
    // said it "costs one call that does nothing on the shape it is already
    // wearing". Measured, it cost 82 us of CPU per frame - a quarter of the
    // whole frame on an idle overlay - because SetCursorShape answers "is
    // the cursor still over one of my windows" with WindowFromPoint, a
    // system-wide hit test, and then installs the shape again.
    //
    // What "moved" has to mean is the whole of the difficulty, and getting it
    // wrong is not subtle: it puts the arrow back over the canvas and leaves
    // it there.
    //
    // ImGui's backend installs its own cursor from NewFrame when ImGui's
    // wanted shape differs from the one it installed last - and it reads that
    // shape *before* NewFrame resets it, so its install lands one frame after
    // the change. Watching `ImGui::GetMouseCursor()` for a change is
    // therefore a frame too early: on the frame the backend actually takes
    // the cursor away, ImGui's own answer has already been Arrow since the
    // previous frame, and a key built on it says nothing changed. That is
    // exactly how the pen stopped coming back after a resize handle. So the
    // re-assert has to cover the frame after a change as well as the frame of
    // one - hence two frames of history rather than one.
    //
    // A moved pointer re-asserts too. It costs nothing where it matters (an
    // overlay nobody is touching is the case being optimized, and there the
    // pointer is still by definition), and it covers everything that can only
    // happen while the pointer moves - including SetCursorShape declining to
    // act because its own hit test said the cursor had left our window, which
    // would otherwise be recorded as applied and never retried.
    const ImVec2 pointer = ImGui::GetMousePos();
    const bool pointerMoved = pointer.x != lastPointerX_ || pointer.y != lastPointerY_;
    const bool imguiTouchedTheCursor =
        imguiCursor != lastImGuiCursor_ || lastImGuiCursor_ != previousImGuiCursor_;
    previousImGuiCursor_ = lastImGuiCursor_;
    lastImGuiCursor_ = imguiCursor;
    lastPointerX_ = pointer.x;
    lastPointerY_ = pointer.y;

    if (!pointerMoved && !imguiTouchedTheCursor && appliedPointerShape_ == wanted) {
        return;
    }
    appliedPointerShape_ = wanted;
    window_->SetCursorShape(wanted);
}

void OverlayApp::SayDeletedForGoodAtStart(size_t count, int days) {
    if (count == 0) {
        return;
    }
    char text[160];
    if (count == 1) {
        std::snprintf(text, sizeof(text), strings::kToastPurgedOne, days);
    } else {
        std::snprintf(text, sizeof(text), strings::kToastPurgedMany, count, days);
    }
    messageForNextShow_ = text;
}

void OverlayApp::OnOverlayShown() {
    // Whose settings the panel shows by default: the ones in effect, which
    // the host resolved for what the overlay is coming up over.
    editProfile_ = settings_.ActiveProfile();
    // Something the app did while nobody was looking - see
    // SayDeletedForGoodAtStart - said now, and for long enough to be read.
    if (!messageForNextShow_.empty() && ImGui::GetCurrentContext() != nullptr) {
        actionToastText_ = std::move(messageForNextShow_);
        messageForNextShow_.clear();
        actionToastExpireAtSeconds_ = ImGui::GetTime() + 8.0;
    }
    // Hiding is moving on too, and nothing ran while hidden to notice - see
    // Editor::UntouchedDrawing.
    editor_.SettleUntouchedDrawing();
    // Nothing is in the hand as the overlay comes up: what went down before
    // it was hidden has come up since, wherever that release went. Settled
    // already when it was put away, unless it went some other way.
    SettleHand();
    // The panels docked against the edges come out for a moment, so they
    // are seen where they are - asked for here, done on the first frame.
    edgePanelsFlashPending_ = true;
    // While the overlay was away, whatever is underneath owned the pointer
    // and will have installed its own shape. What ApplyPointerShape last
    // asked for therefore says nothing about what is on screen now, and
    // skipping the push because "it hasn't changed" would leave the other
    // application's cursor over our canvas until the next mouse move.
    appliedPointerShape_.reset();
    // A showing starts from no input at all: nothing that happened while
    // the overlay was hidden is input to it.
    //
    // Not housekeeping - a key really does survive the gap. The keyboard
    // grab hands every keystroke to this window for as long as edit mode is
    // up, the hotkey that *ends* edit mode included (see
    // Win32InputGrab::OnKeyboard, which dispatches the hotkey and passes the
    // key on deliberately). That last one is posted after the final frame
    // and nothing drains it: no frame is drawn while hidden, so ImGui's
    // event queue keeps it until the next showing reads it as a fresh
    // press. Seen with Ctrl+Alt+S bound to edit mode, while the command
    // keys were still read from ImGui: the overlay came back with the
    // screenshot tool in hand, because "S" alone is that tool's key. They
    // come through the input stream now, which a hidden window hands
    // nothing (see IOverlayWindow::SetInputCallback); ImGui's queue is
    // what the panels and their widgets still read.
    //
    // The key state as well as the queue, because it goes stale the same
    // way and in both directions: what ImGui believes is held is whatever
    // the last frame before the hiding saw, however long ago that was, and
    // modifiers left latched that way would make the exact-modifier test
    // refuse a perfectly ordinary key on the way back. Nothing true is lost
    // by clearing - a modifier still physically held is re-sent on every
    // frame by the platform (see Win32OverlayWindow::RenderFrame).
    //
    // The mouse too, which ClearInputKeys leaves alone. A button held on a
    // scrollbar or a panel as the overlay went away never has its release
    // seen - under the grab, with no activation, not even as a focus loss -
    // so it came back held: the first click was taken as the release, and
    // an ImGui drag went on with no button down.
    //
    // Guarded like ShowActionToast's: the overlay can be shown by a hotkey
    // pressed before a single frame has ever been drawn.
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
    }
}

namespace {
// The rows of the input options HUD, in the order the number keys address
// them. Kept as data so the drawing and the key handling cannot disagree
// about which key means which option.
struct InputOptionRow {
    const char* label;
    // Which live setting the row reflects, reached through the accessor
    // below rather than a pointer-to-member so the options that live inside
    // the input options can sit in the same table as the two that don't.
    enum class Which {
        NoActivate,
        SoftwarePointer,
        DontForwardKeystrokes,
        RawMouseInput,
        CounterRawMouseInput,
        FreezeScreen,
    };
    Which which;
};

// Same names and same order as the Settings tab, so that finding the right
// combination here and then finding it again there isn't a translation
// exercise. The count travels to the platform via
// IOverlayWindow::SetInputOptionsHudDigits, so the number of digits the
// keyboard hook claims follows this table rather than a constant beside it.
//
// Ordered so that every option's prerequisites are above it, which is also
// what puts the Settings tab's tree at the top of both. "Don't steal focus"
// is what leaves the game receiving input, so the things that take it back
// follow it, contiguously, each link below the one it needs (see
// EditModeInputOptions' *CanBeUsed predicates). The last two need nothing
// here, so they sit at the bottom - the same statement the Settings tab
// makes by leaving them out of its tree.
// Which rows need edit mode re-entered to take effect at all. Freezing only
// captures the screen on entry, and no-activate decides how the window is
// shown - toggling either would otherwise read ON in the HUD while nothing
// had changed on screen. Everything else is pushed into the window live by
// OnSettingsChanged, exactly as the Settings tab does it, and needs no such
// thing - which matters because a restart is not free: see
// pendingOverlayRestart_.
constexpr bool RowNeedsOverlayRestart(InputOptionRow::Which which) {
    return which == InputOptionRow::Which::NoActivate || which == InputOptionRow::Which::FreezeScreen;
}

constexpr InputOptionRow kInputOptionRows[] = {
    {strings::kHudDontStealFocus, InputOptionRow::Which::NoActivate},
    {strings::kHudDontForwardKeystrokes, InputOptionRow::Which::DontForwardKeystrokes},
    {strings::kHudUseRawMouseInput, InputOptionRow::Which::RawMouseInput},
    {strings::kHudCounterRawMouseInput, InputOptionRow::Which::CounterRawMouseInput},
    {strings::kHudUseSoftwarePointer, InputOptionRow::Which::SoftwarePointer},
    {strings::kHudFreezeScreenWhileEditing, InputOptionRow::Which::FreezeScreen},
};
}  // namespace

// Which setting a HUD row addresses, as the pair the profile machinery
// speaks in - see ProfileableField. Not a pointer into the live values:
// those are derived, and a write to one would be overwritten by the next
// resolve and stored nowhere.
ProfileableField OverlayApp::InputOptionField(int index) {
    switch (kInputOptionRows[index].which) {
        case InputOptionRow::Which::SoftwarePointer:
            return {&ProfileableSettings::softwarePointer, &ProfileOverrides::softwarePointer};
        case InputOptionRow::Which::DontForwardKeystrokes:
            return {&ProfileableSettings::dontForwardKeystrokes, &ProfileOverrides::dontForwardKeystrokes};
        case InputOptionRow::Which::RawMouseInput:
            return {&ProfileableSettings::rawMouseInput, &ProfileOverrides::rawMouseInput};
        case InputOptionRow::Which::CounterRawMouseInput:
            return {&ProfileableSettings::counterRawMouseInput, &ProfileOverrides::counterRawMouseInput};
        case InputOptionRow::Which::FreezeScreen:
            return {&ProfileableSettings::freezeScreen, &ProfileOverrides::freezeScreen};
        case InputOptionRow::Which::NoActivate:
            break;
    }
    return {&ProfileableSettings::dontStealFocus, &ProfileOverrides::dontStealFocus};
}

bool OverlayApp::InputOptionValue(int index) const {
    if (index < 0 || index >= static_cast<int>(std::size(kInputOptionRows))) {
        return false;
    }
    // The resolved value: the HUD reports what is running, which is the
    // whole reason it exists.
    switch (kInputOptionRows[index].which) {
        case InputOptionRow::Which::NoActivate:
            return settings_.Live().dontStealFocus;
        case InputOptionRow::Which::SoftwarePointer:
            return settings_.Live().InputOptions().useSoftwarePointer;
        case InputOptionRow::Which::DontForwardKeystrokes:
            return settings_.Live().InputOptions().dontForwardKeystrokes;
        case InputOptionRow::Which::RawMouseInput:
            return settings_.Live().InputOptions().useRawMouseInput;
        case InputOptionRow::Which::CounterRawMouseInput:
            return settings_.Live().InputOptions().counterRawMouseInput;
        case InputOptionRow::Which::FreezeScreen:
            return settings_.Live().freezeScreen;
    }
    return false;
}

// Whether a row's option can currently do anything - the same preconditions
// the Settings tab grays its checkboxes on, read from the one place that
// states them. An unavailable row is dimmed and its number key ignored:
// storing a change that has no effect, with nothing on screen saying so, is
// how you end up believing an option is broken.
bool OverlayApp::InputOptionAvailable(int index) const {
    if (index < 0 || index >= static_cast<int>(std::size(kInputOptionRows))) {
        return false;
    }
    switch (kInputOptionRows[index].which) {
        case InputOptionRow::Which::NoActivate:
        case InputOptionRow::Which::FreezeScreen:
            return true;  // depend on nothing else here
        case InputOptionRow::Which::SoftwarePointer:
            return true;  // a matter of appearance; works with or without focus
        case InputOptionRow::Which::DontForwardKeystrokes:
            return platform::EditModeInputOptions::KeystrokesCanBeHeld(settings_.Live().dontStealFocus);
        case InputOptionRow::Which::RawMouseInput:
            return settings_.Live().InputOptions().RawMouseInputCanBeUsed(settings_.Live().dontStealFocus);
        case InputOptionRow::Which::CounterRawMouseInput:
            return settings_.Live().InputOptions().CounterRawMouseInputCanBeUsed(settings_.Live().dontStealFocus);
    }
    return false;
}

// A debugging aid, off by default - see AppConfig::showInputOptionsHud.
// Not always on, tempting as that is for something meant to be seen while
// standing in front of a misbehaving game, because of the cost: the number
// keys need a keyboard hook to reach an overlay that deliberately has no
// focus, so an always-on HUD meant digits never reached the game even with
// "Don't forward keystrokes" off. A diagnostic that quietly eats input is
// one to switch on deliberately.
void OverlayApp::DrawInputOptionsHud(ImDrawList* drawList) const {
    if (!Cfg().showInputOptionsHud || drawList == nullptr) {
        return;
    }
    constexpr float kPad = 10.0f;
    constexpr float kLineHeight = 19.0f;
    constexpr float kOriginX = 14.0f;
    constexpr float kOriginY = 14.0f;
    const int rowCount = static_cast<int>(std::size(kInputOptionRows));

    float widest = 0.0f;
    for (const InputOptionRow& row : kInputOptionRows) {
        widest = std::max(widest, ImGui::CalcTextSize(row.label).x);
    }

    char fps[160];
    // Availability, not just the stored value: with raw input grayed out
    // there is no pointer of ours being driven, so its gain and step
    // histogram would be a readout of nothing.
    if (settings_.Live().InputOptions().useRawMouseInput && settings_.Live().InputOptions().RawMouseInputCanBeUsed(settings_.Live().dontStealFocus) &&
        window_ != nullptr) {
        // Per-report step sizes against per-frame ones. All ones in the
        // first and twos in the second means the pointer arithmetic is fine
        // and it is the once-a-frame drawing that looks coarse - a
        // different problem with a different fix.
        const platform::InputGrabDiagnostics diag = window_->GetInputGrabDiagnostics();
        std::snprintf(fps, sizeof(fps), "%.0f fps  gain %.2f %s  report %d/%d/%d/%d  frame %d/%d/%d/%d",
                       ImGui::GetIO().Framerate, diag.pointerGain, diag.ballisticsEnabled ? "curve" : "flat",
                       diag.stepCounts[0], diag.stepCounts[1], diag.stepCounts[2], diag.stepCounts[3],
                       diag.frameSteps[0], diag.frameSteps[1], diag.frameSteps[2], diag.frameSteps[3]);
    } else {
        std::snprintf(fps, sizeof(fps), "%.0f fps   %.2f ms", ImGui::GetIO().Framerate,
                       1000.0f / std::max(1.0f, ImGui::GetIO().Framerate));
    }
    // What countering is actually managing, when it is on: how long the
    // game had each movement to itself before the negation arrived, and
    // which of the two injection timings produced that. The residual the
    // camera keeps is not in here and cannot be - see
    // InputGrabDiagnostics::correctionLagMsLast.
    // What the last number key did, and how much re-deriving has happened
    // since - see hudToggleCount_.
    char lastKey[160];
    lastKey[0] = '\0';
    if (hudLastToggledRow_ != 0) {
        std::snprintf(lastKey, sizeof(lastKey),
                       "last key %d: set %s, into %s  -  now prof=%s  (toggles %d, resolves %d)",
                       hudLastToggledRow_, hudLastToggledTo_ ? strings::kHotkeysOn : strings::kHotkeysOff,
                       hudLastWentToProfile_ ? strings::kProfilesNamePrefix : strings::kProfilesDefaults,
                       settings_.ActiveProfile() && *settings_.ActiveProfile() < settings_.Profiles().size()
                           ? settings_.Profiles()[*settings_.ActiveProfile()].name.c_str()
                           : "none",
                       hudToggleCount_, settings_.ResolveCount());
    }

    char counter[192];
    counter[0] = '\0';
    if (settings_.Live().InputOptions().counterRawMouseInput &&
        settings_.Live().InputOptions().CounterRawMouseInputCanBeUsed(settings_.Live().dontStealFocus) && window_ != nullptr) {
        const platform::InputGrabDiagnostics diag = window_->GetInputGrabDiagnostics();
        std::snprintf(counter, sizeof(counter), "counter lag %.2f ms (max %.2f)  n=%d",
                       diag.correctionLagMsLast, diag.correctionLagMsMax, diag.correctionsInjected);
    }

    // What is in front and where it sits relative to us - the line that
    // answers "why is nothing in this panel moving". Above us, Windows
    // delivers that application's input to no lower-integrity process at
    // all, so every number here stays where it is however the rows are
    // set, and no shortcut of the overlay's arrives either. See
    // platform::ForegroundIntegrity.
    char foreground[224];
    foreground[0] = '\0';
    {
        const platform::ForegroundApp& app = settings_.UnderlyingApplication();
        const char* what = !app.executable.empty() ? app.executable.c_str()
                           : !app.title.empty()    ? app.title.c_str()
                                                   : "(nothing identifiable)";
        switch (app.integrity) {
            case platform::ForegroundIntegrity::Above:
                std::snprintf(foreground, sizeof(foreground),
                               "over %s  -  above us, none of its input reaches here", what);
                break;
            case platform::ForegroundIntegrity::NotAbove:
                std::snprintf(foreground, sizeof(foreground), "over %s  -  not above us", what);
                break;
            case platform::ForegroundIntegrity::Unknown:
                std::snprintf(foreground, sizeof(foreground), "over %s  -  integrity unreadable", what);
                break;
        }
    }

    // Number prefix, label, then the ON/OFF column clear of the longest label.
    const float statusX = Px(kOriginX) + Px(kPad) + Px(26.0f) + widest + Px(16.0f);
    // Wide enough for the header line too - it carries the pointer
    // diagnostics and is easily longer than the rows.
    const float panelW = std::max({statusX + Px(34.0f) + Px(kPad) - Px(kOriginX),
                                    ImGui::CalcTextSize(fps).x + Px(kPad) * 2.0f,
                                    ImGui::CalcTextSize(counter).x + Px(kPad) * 2.0f,
                                    ImGui::CalcTextSize(foreground).x + Px(kPad) * 2.0f,
                                    ImGui::CalcTextSize(lastKey).x + Px(kPad) * 2.0f});
    // One extra line for the frame rate: "the overlay feels slower with the
    // grab on" is a measurement, not an impression, and this is where it can
    // be read without leaving the situation that caused it. Then one for the
    // counter readout and one for the last key, on the frames there are any.
    const int extraLines = 1 + (counter[0] != '\0' ? 1 : 0) + (foreground[0] != '\0' ? 1 : 0) +
                          (lastKey[0] != '\0' ? 1 : 0);
    const float panelH = Px(kPad) * 2.0f + Px(kLineHeight) * static_cast<float>(rowCount + extraLines);

    const ImVec2 panelMin = Px(kOriginX, kOriginY);
    const ImVec2 panelMax(panelMin.x + panelW, panelMin.y + panelH);
    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(12, 15, 20, 205), Px(6.0f));
    drawList->AddRect(panelMin, panelMax, IM_COL32(255, 255, 255, 40), Px(6.0f));

    drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad)), IM_COL32(150, 158, 172, 255), fps);

    for (int i = 0; i < rowCount; ++i) {
        const float y = Px(kOriginY) + Px(kPad) + Px(kLineHeight) * static_cast<float>(i + 1);
        // A row whose prerequisite isn't met is dimmed and reads "--" rather
        // than ON/OFF: its stored value is still there and still what it will
        // do once the row above allows it, but saying ON about something that
        // is doing nothing is the one thing a diagnostic panel must not do.
        // Its number key is ignored to match.
        const bool available = InputOptionAvailable(i);
        char key[8];
        std::snprintf(key, sizeof(key), "%d", i + 1);
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), y),
                           available ? IM_COL32(150, 158, 172, 255) : IM_COL32(96, 102, 114, 255), key);
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad) + Px(20.0f), y),
                           available ? IM_COL32(226, 230, 238, 255) : IM_COL32(120, 126, 138, 255),
                           kInputOptionRows[i].label);

        const bool on = InputOptionValue(i);
        if (!available) {
            drawList->AddText(ImVec2(statusX, y), IM_COL32(120, 126, 138, 255), "--");
            continue;
        }
        drawList->AddText(ImVec2(statusX, y), on ? IM_COL32(90, 214, 130, 255) : IM_COL32(232, 100, 100, 255),
                           on ? strings::kHotkeysOn : strings::kHotkeysOff);
    }

    float footerLine = static_cast<float>(rowCount + 1);
    if (counter[0] != '\0') {
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad) + Px(kLineHeight) * footerLine),
                           IM_COL32(150, 158, 172, 255), counter);
        footerLine += 1.0f;
    }
    if (foreground[0] != '\0') {
        // Brighter when it is the answer: above us, nothing else in this
        // panel can be trusted to mean anything.
        const bool above = settings_.UnderlyingApplication().integrity == platform::ForegroundIntegrity::Above;
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad) + Px(kLineHeight) * footerLine),
                           above ? IM_COL32(232, 100, 100, 255) : IM_COL32(150, 158, 172, 255), foreground);
        footerLine += 1.0f;
    }
    if (lastKey[0] != '\0') {
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad) + Px(kLineHeight) * footerLine),
                           IM_COL32(150, 158, 172, 255), lastKey);
    }
}

void OverlayApp::HandleInputOptionsHudKeys() {
    // Both halves of the same decision: the digits belong to the HUD only
    // while it is visible, and the platform needs to know so it can stop
    // holding a keyboard hook open on the HUD's behalf. Pushed only on a
    // change - installing or removing a hook is not a per-frame ask.
    const bool hudActive = Cfg().showInputOptionsHud && !viewOnly_;
    const int hudDigits = hudActive ? static_cast<int>(std::size(kInputOptionRows)) : 0;
    if (window_ && hudDigits != appliedInputOptionsHudDigits_) {
        window_->SetInputOptionsHudDigits(hudDigits);
        appliedInputOptionsHudDigits_ = hudDigits;
    }
    if (!hudActive) {
        // A restart asked for by a row is the HUD's business; with the panel
        // gone there is nobody left to have asked, and firing it later would
        // hide and show the overlay for no reason anyone could see.
        pendingOverlayRestart_ = false;
        return;
    }

    // A restart tears the whole input path down and builds it again - the
    // window is hidden and shown, which takes the keyboard hook with it. Doing
    // that while the key that triggered it is still held loses the key-up, and
    // a key ImGui still believes is held makes the *next* press no press at
    // all. So the restart waits for every HUD digit to be up; without the
    // wait about one press in three went missing.
    if (pendingOverlayRestart_) {
        bool anyDown = false;
        for (int i = 0; i < static_cast<int>(std::size(kInputOptionRows)); ++i) {
            anyDown = anyDown || ImGui::IsKeyDown(static_cast<ImGuiKey>(ImGuiKey_1 + i));
        }
        if (!anyDown) {
            pendingOverlayRestart_ = false;
            if (restartOverlayCallback_) {
                restartOverlayCallback_();
            }
            return;  // the overlay is being rebuilt; nothing else this frame
        }
    }

    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    for (int i = 0; i < static_cast<int>(std::size(kInputOptionRows)); ++i) {
        const auto key = static_cast<ImGuiKey>(ImGuiKey_1 + i);
        if (!ImGui::IsKeyPressed(key, /*repeat=*/false)) {
            continue;
        }
        if (!InputOptionAvailable(i)) {
            // Dimmed in the panel, and inert here to match. Its prerequisite
            // is one of the rows above, so it is one keypress away.
            return;
        }
        // Into the profile that matched, if one did - the HUD is about the
        // configuration that is running, and the running configuration is
        // that profile's. Deliberately not the Settings panel's own edit
        // target, which may be some other profile entirely.
        const bool wanted = !InputOptionValue(i);
        settings_.SetProfileable(settings_.ActiveProfile(), InputOptionField(i), wanted);
        // Recorded for the HUD, which is the only place this can be seen
        // happening - see hudToggleCount_.
        ++hudToggleCount_;
        hudLastToggledRow_ = i + 1;
        hudLastToggledTo_ = wanted;
        hudLastWentToProfile_ = settings_.ActiveProfile().has_value();
        // Only two rows need edit mode re-entered, and the restart is now
        // deferred until the key that asked for it is back up - see
        // pendingOverlayRestart_.
        if (RowNeedsOverlayRestart(kInputOptionRows[i].which)) {
            pendingOverlayRestart_ = true;
        }
        return;  // one row per frame
    }
}

// Draws the pointer at ImGui's mouse position, whatever put it there - which
// is what makes this work in both configurations. Under a mouse grab that
// position is the grab's own accumulated one, so by construction the drawn
// pointer can't drift from what a click actually hits; with only
// useSoftwarePointer on, it's the real cursor's position and this is purely a
// change of appearance. Either way the OS cursor is hidden over this window
// (see Win32Dx11Renderer::NewFrame) so there is exactly one pointer visible.
//
// Gated on the settings rather than on the grab really running, which core
// has no way to ask: a backend without an input grab (the Linux dev harness)
// would otherwise draw a second, redundant pointer.
// The one place that decides what the pointer means, shared by both of them
// so they cannot disagree.
//
// An arrow unless something more specific applies - not a crosshair
// whenever a drawing tool is in hand, which put a crosshair over empty
// desktop where nothing would be drawn.

platform::CursorShape OverlayApp::WantedPointerShape() const {
    // Placing a snippet: the click puts a corner somewhere exact.
    if (ArmedCreation().has_value()) {
        return platform::CursorShape::Crosshair;
    }
    // Everything below is about what the pointer is *over*. Over the
    // overview, a popover or the selection's handles and bar, ImGui owns the
    // pointer and this must not argue with it - the arrow here is only
    // what's left when ImGui wants nothing more specific, which
    // ApplyPointerShape has already checked before this answer is used at
    // all.
    if (ImGui::GetIO().WantCaptureMouse || PanelOpen()) {
        return platform::CursorShape::Arrow;
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    const PointerTarget target = editor_.ResolvePointerTarget(mouse.x, mouse.y);
    if (target.kind != PointerTarget::Kind::Body) {
        // Open canvas, a handle or the bar: nothing here for the tool to
        // mark. A handle has already asked ImGui for its own shape
        // (RenderItems), which ApplyPointerShape lets win; a button gets
        // the arrow, as a button does.
        return platform::CursorShape::Arrow;
    }
    // A marking tool marks only the snippet in drawing mode, and not with
    // Alt held, when the press picks the snippet up instead.
    if (editor_.DrawingItem() != target.item || editor_.PressPicksUp()) {
        return platform::CursorShape::Arrow;
    }
    switch (editor_.ActiveTool()) {
        case Tool::Draw:
            return platform::CursorShape::Pen;
        case Tool::Erase:
            // No eraser glyph in either cursor set, and the exact point is
            // what matters - the brush-size ring already says how much will
            // come away.
            return platform::CursorShape::Crosshair;
        case Tool::Text:
        case Tool::Select:
        case Tool::NewDrawing:
        case Tool::NewScreenshot:
            break;
    }
    return platform::CursorShape::Arrow;
}

void OverlayApp::RenderToolModifierBadge() {
    if (PanelOpen() || ArmedCreation().has_value() ||
        (editor_.ActiveTool() != Tool::Draw && editor_.ActiveTool() != Tool::Erase)) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const StrokeInFlight* stroke = GestureIf<StrokeInFlight>();
    const bool dragging = stroke != nullptr;
    if (!dragging) {
        // Only where a press would make one: over the snippet in drawing
        // mode, not a panel, and not with Alt held.
        const PointerTarget target = editor_.ResolvePointerTarget(io.MousePos.x, io.MousePos.y);
        if (io.WantCaptureMouse || target.kind != PointerTarget::Kind::Body || editor_.DrawingItem() != target.item ||
            editor_.PressPicksUp()) {
            return;
        }
    }
    // Mid-drag, what the gesture is making; before one, what a press would
    // make now - the modifiers held, or the bar's cycled shape.
    const Icon* icon = nullptr;
    if (editor_.ActiveTool() == Tool::Draw) {
        const DrawShape shape = dragging ? stroke->shape : editor_.ShapeForPress();
        if (dragging && stroke->kind != StrokeInFlight::Kind::Shape) {
            return;  // freehand, which needs no saying
        }
        icon = shape == DrawShape::Rectangle ? &icons::kRectangle
               : shape == DrawShape::Line    ? &icons::kLine
                                             : nullptr;
    } else if (dragging ? stroke->kind == StrokeInFlight::Kind::EraseRect : editor_.ShapeForPress() == DrawShape::Rectangle) {
        icon = &icons::kEraserRect;
    }
    if (icon == nullptr) {
        return;
    }
    // Down and to the right of the hotspot, clear of the pen glyph and the
    // brush-size dot, on a disc of the panel color so it reads over any
    // snippet.
    const float offset = Px(18.0f);
    const float size = Px(22.0f);
    const float inset = Px(4.0f);
    const ImVec2 min(io.MousePos.x + offset, io.MousePos.y + offset);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(min, ImVec2(min.x + size, min.y + size), ImGui::ColorConvertFloat4ToU32(theme::kPanelBg),
                      Px(theme::kRadiusSm));
    DrawIcon(dl, *icon, ImVec2(min.x + inset, min.y + inset), size - inset * 2.0f,
             ImGui::ColorConvertFloat4ToU32(theme::kWhite), 1.5f);
}

void OverlayApp::DrawSoftwareCursor() const {
    if (!settings_.Live().InputOptions().SoftwarePointerDrawn()) {
        return;
    }
    // Whatever ImGui asked for, if it asked for anything: its atlas already
    // carries the resize arrows, the hand, the I-beam and NotAllowed, and
    // io.MouseDrawCursor makes it draw them at the same position this
    // pointer would be. Drawing over them instead is what made a resize
    // handle's arrow flash and turn back into a crosshair.
    if (ImGui::GetMouseCursor() != ImGuiMouseCursor_Arrow) {
        return;
    }

    // Nothing more specific, so this pointer is the one being drawn. Stops
    // ImGui drawing its own on top: the backend's io.MouseDrawCursor - the
    // flag that reliably hides the OS cursor - also asks ImGui to render a
    // pointer from its font atlas, and ImGui::EndFrame skips that only
    // while the wanted cursor is None. Set here, after every widget has had
    // its say for this frame, so nothing reinstates a shape afterwards.
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    // Whole pixels, every frame - the same integer position ImGui hit-tests
    // against, so the pointer is drawn exactly where clicks land, and crisp.
    // The grab keeps the pointer's position as a float internally (that is
    // what keeps slow movement from vanishing under a pixel) and floors it
    // once; on identical input its integer steps match the OS cursor's, so
    // there is nothing left for fractional drawing to smooth over. It was
    // tried, and it traded a crisp pointer for a soft one to hide a stepping
    // problem that turned out to be a wrong speed multiplier.
    const ImVec2 at = ImGui::GetMousePos();
    if (at.x < 0.0f || at.y < 0.0f) {
        return;
    }

    constexpr ImU32 kFill = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kEdge = IM_COL32(20, 24, 32, 235);
    constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 70);
    // At Windows' scale for the display rather than the interface scale:
    // this stands in for the system pointer, which Windows draws larger on
    // a scaled display whatever this app's own setting says.
    const float scale = window_ != nullptr ? static_cast<float>(window_->ScalePercent()) / 100.0f : 1.0f;

    const platform::CursorShape shape = WantedPointerShape();

    if (shape == platform::CursorShape::Pen) {
        // A pen, for a snippet a drawing tool would mark - the shared
        // outline (see platform::pen_glyph), placed with its nib at the
        // pointer's own position so it points at the pixel it will draw
        // on. Same two-pass outline and fill as the arrow below, and the
        // same reason: it has to stay legible over a bright game and a
        // dark one alike.
        const auto place = [&at, scale](platform::Vec2 p) { return ImVec2(at.x + p.x * scale, at.y + p.y * scale); };
        ImVec2 nibShape[std::size(platform::pen_glyph::kNib)];
        ImVec2 body[std::size(platform::pen_glyph::kBody)];
        ImVec2 shadow[std::size(platform::pen_glyph::kBody)];
        for (size_t i = 0; i < std::size(nibShape); ++i) {
            nibShape[i] = place(platform::pen_glyph::kNib[i]);
        }
        for (size_t i = 0; i < std::size(body); ++i) {
            body[i] = place(platform::pen_glyph::kBody[i]);
            shadow[i] = ImVec2(body[i].x + 1.5f * scale, body[i].y + 1.5f * scale);
        }
        const float outline = platform::pen_glyph::kOutlineWidth * scale;
        drawList->AddConvexPolyFilled(shadow, static_cast<int>(std::size(shadow)), kShadow);
        drawList->AddConvexPolyFilled(body, static_cast<int>(std::size(body)), kFill);
        drawList->AddConvexPolyFilled(nibShape, static_cast<int>(std::size(nibShape)), kFill);
        drawList->AddPolyline(body, static_cast<int>(std::size(body)), kEdge, ImDrawFlags_Closed, outline);
        drawList->AddPolyline(nibShape, static_cast<int>(std::size(nibShape)), kEdge, ImDrawFlags_Closed,
                              outline);
        return;
    }

    if (shape == platform::CursorShape::Crosshair) {
        // A crosshair for the tools where the exact point matters and an
        // arrow's body would sit on top of it. The gap in the middle leaves
        // the target pixel itself visible.
        const float arm = 11.0f * scale;
        const float gap = 3.0f * scale;
        const ImVec2 spans[4][2] = {
            {ImVec2(at.x - arm, at.y), ImVec2(at.x - gap, at.y)},
            {ImVec2(at.x + gap, at.y), ImVec2(at.x + arm, at.y)},
            {ImVec2(at.x, at.y - arm), ImVec2(at.x, at.y - gap)},
            {ImVec2(at.x, at.y + gap), ImVec2(at.x, at.y + arm)},
        };
        for (const auto& span : spans) {
            drawList->AddLine(span[0], span[1], kEdge, 3.0f * scale);
            drawList->AddLine(span[0], span[1], kFill, scale);
        }
        return;
    }

    // The ordinary arrow, hotspot at its tip so it points at what it is
    // over rather than near it. Drawn white with a dark outline (and a
    // slight shadow) so it stays legible over a bright game and a dark one
    // alike - the same reason the OS cursor is shaped this way.
    constexpr ImVec2 kArrow[7] = {
        ImVec2(0.0f, 0.0f),   ImVec2(0.0f, 17.0f),  ImVec2(4.2f, 12.8f),  ImVec2(7.0f, 18.6f),
        ImVec2(10.0f, 17.2f), ImVec2(7.2f, 11.6f),  ImVec2(12.2f, 11.6f),
    };
    ImVec2 arrow[7];
    ImVec2 shadow[7];
    for (int i = 0; i < 7; ++i) {
        arrow[i] = ImVec2(at.x + kArrow[i].x * scale, at.y + kArrow[i].y * scale);
        shadow[i] = ImVec2(arrow[i].x + 1.5f * scale, arrow[i].y + 1.5f * scale);
    }
    drawList->AddConvexPolyFilled(shadow, 7, kShadow);
    drawList->AddConvexPolyFilled(arrow, 7, kFill);
    drawList->AddPolyline(arrow, 7, kEdge, ImDrawFlags_Closed, 1.4f * scale);
}

// ================= View-only mode =================

void OverlayApp::RenderViewOnly(float displayW, float displayH) {
    ImDrawList* drawList = BeginScreenLayer("##spickzettel_view_only", displayW, displayH);
    // Nothing to show with an empty library (see CanvasManager's class
    // comment) - view-only mode has no UI of its own to offer instead, so
    // it just renders nothing, which is exactly right: a fully
    // transparent, fully click-through overlay.
    if (const Canvas* canvas = Manager().CurrentOrNull()) {
        for (const Item& item : canvas->items) {
            // A minimized snippet is drawn nowhere but its dock chip, and
            // view-only has no dock. The pinned view draws the pinned ones
            // and nothing else.
            if (Manager().IsDeleted(*canvas, item) || item.minimized || (pinnedOnly_ && !item.pinned)) {
                continue;
            }
            const ImVec2 pMin(item.rect.x, item.rect.y);
            const ImVec2 pMax(item.rect.x + item.rect.w, item.rect.y + item.rect.h);
            drawList->PushClipRect(pMin, pMax, true);
            DrawItemContent(drawList, item, pMin, pMax, Cfg().strokeRenderMode, PictureTexture(item),
                            StrokeRasterTextureFor(item.id), /*skipNoteText=*/false, CanvasMeshSlot(),
                            PictureSampling());
            drawList->PopClipRect();
        }
    }

    if (Cfg().showDebugOverlay) {
        DrawDebugOverlay(drawList, ImGui::GetIO(), Manager(), debugHoveredResizeHandle_);
    }

    // Also here, not just in edit mode - see DrawDemoWatermark. Drawn last
    // within this one layer rather than in a layer of its own: view-only
    // mode has nothing else on screen for it to be under.
    DrawDemoWatermark(drawList, displayW, displayH);

    EndScreenLayer();
}

// Feedback for the mouse wheel's size change, which otherwise altered the
// tool silently and left "how big is it now?" to be answered by drawing a
// test stroke and undoing it. Deliberately shows the *size itself* - a dot
// of exactly the diameter the tool will mark at, under the cursor - rather
// than only a number somewhere else on screen: the question is about a
// size, so the answer should be one. The number rides along for the cases
// where the dot alone is hard to judge (1px vs 2px).
void OverlayApp::RenderBrushSizePreview() {
    const double now = ImGui::GetTime();
    if (now >= sizePreviewExpireAtSeconds_) {
        // The preview gone, the width the wheel settled on is kept - once,
        // not per notch (see drawWidthDirty_).
        if (drawWidthDirty_) {
            drawWidthDirty_ = false;
            if (settings_.Stored().strokeWidth != editor_.DrawWidth()) {
                settings_.Mutable().strokeWidth = editor_.DrawWidth();
                settings_.Commit();
            }
        }
        return;
    }
    if (PanelOpen()) {
        return;
    }
    const std::optional<float> diameter = editor_.ActiveToolSizePx();
    if (!diameter.has_value()) {
        return;
    }
    // Holds at full strength, then fades over the last stretch rather than
    // blinking out - a hard disappearance at the end reads as a glitch.
    const float alpha =
        static_cast<float>(std::clamp((sizePreviewExpireAtSeconds_ - now) / kSizePreviewFadeSeconds, 0.0, 1.0));

    // Follows the live cursor rather than freezing where the wheel was
    // turned: adjusting the size and then moving to where you're about to
    // draw is one continuous motion, and a preview left behind at the old
    // spot would be answering the question in the wrong place.
    const ImVec2 center = ImGui::GetIO().MousePos;
    const float radius = *diameter * 0.5f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const auto fade = [alpha](ImU32 color) {
        const ImU32 a = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };

    if (editor_.ActiveTool() == Tool::Erase) {
        // Hollow, and in RenderRectEraserOverlay's own cool blue rather
        // than the warm placement tone: the eraser takes ink away, and a
        // filled dot would read as something about to be painted.
        dl->AddCircleFilled(center, radius, fade(IM_COL32(120, 170, 255, 40)));
        dl->AddCircle(center, radius, fade(IM_COL32(120, 170, 255, 255)), 0, 2.0f);
    } else {
        // The pen's actual color, so this previews the mark itself and not
        // just its footprint.
        dl->AddCircleFilled(center, radius, fade(ToImColor(editor_.DrawColorRGBA())));
        // A hairline at exactly `radius` (not outside it - the ring must
        // not make the dot look bigger than it is) keeps a dark color, or
        // a 1px width, findable against whatever is underneath.
        dl->AddCircle(center, radius, fade(IM_COL32(255, 255, 255, 200)), 0, 1.0f);
    }

    char label[16];
    std::snprintf(label, sizeof(label), strings::kFormatPixels, *diameter);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    constexpr float kLabelPadX = 7.0f;
    constexpr float kLabelPadY = 3.0f;
    constexpr float kLabelGap = 10.0f;
    // Clear of the dot at every size in both ranges (1..24 and 8..64), so
    // the two never overlap and the label doesn't jump sides.
    const ImVec2 boxMin(center.x + radius + Px(kLabelGap), center.y - textSize.y * 0.5f - Px(kLabelPadY));
    const ImVec2 boxMax(boxMin.x + textSize.x + Px(kLabelPadX) * 2.0f, boxMin.y + textSize.y + Px(kLabelPadY) * 2.0f);
    // Same pill as RenderActionToast - over arbitrary game content, plain
    // text has no guaranteed contrast to sit against.
    dl->AddRectFilled(boxMin, boxMax, fade(IM_COL32(18, 20, 26, 235)), theme::kRadiusPill);
    dl->AddText(ImVec2(boxMin.x + Px(kLabelPadX), boxMin.y + Px(kLabelPadY)), fade(IM_COL32(240, 242, 245, 255)),
                label);
}

// The demo build's permanent mark (see build::kDemoMode). Drawn in *both*
// edit and view-only mode - it goes wherever the overlay is visible at all,
// and a mark you could drop by pressing the other hotkey wouldn't be one.
// It needs no suppression for screen capture: the platform layer leaves the
// whole overlay window out of what it grabs (see CaptureScreen), so nothing
// this draws can reach a captured image.
//
// It moves, every kDemoWatermarkMoveSeconds. A mark that lives in one
// corner is a mark you stop seeing after a minute and can work around
// permanently - putting one snippet over it and never moving that snippet
// again. Wandering, it has to be dealt with rather than arranged around,
// which is the whole point of a nag.
//
// The screen is divided into a 3x3 grid and each move picks a *different*
// cell, plus a jitter within it: pure randomness lands in nearly the same
// spot often enough to read as the mark being stuck, and picking a new
// cell by stepping 1..8 cells on cannot repeat by construction.
void OverlayApp::DrawDemoWatermark(ImDrawList* drawList, float displayW, float displayH) {
    if constexpr (!build::kDemoMode) {
        // Compiled and type-checked in every build; folded away entirely in
        // the ones where it's false. See build_config.h.in.
        return;
    } else {
        constexpr float kTextSize = 30.0f;
        constexpr float kMargin = 26.0f;
        constexpr float kLineGap = 2.0f;
        constexpr double kMoveSeconds = 10.0;
        constexpr int kGrid = 3;  // cells per axis

        // ImGui's clock only advances while frames are being drawn, so a
        // hidden overlay doesn't burn through positions it never showed -
        // the mark moves ten seconds of *being visible* after the last one.
        const auto move = static_cast<int64_t>(ImGui::GetTime() / kMoveSeconds);
        if (move != demoWatermarkMove_) {
            // One scramble, three uses: which cell to step to, and where in
            // it to sit. Cheap enough to not be worth a real generator, and
            // being a pure function of the move number keeps this
            // reproducible when something looks wrong.
            auto scramble = static_cast<uint32_t>(move) * 2654435761u;
            scramble ^= scramble >> 15;
            scramble *= 2246822519u;
            scramble ^= scramble >> 13;
            // 1..(cells-1), so the new cell is never the current one.
            constexpr int kCells = kGrid * kGrid;
            demoWatermarkCell_ = (demoWatermarkCell_ + 1 + static_cast<int>(scramble % (kCells - 1))) % kCells;
            demoWatermarkJitter_ = ImVec2(static_cast<float>((scramble >> 8) & 0xFF) / 255.0f,
                                           static_cast<float>((scramble >> 16) & 0xFF) / 255.0f);
            demoWatermarkMove_ = move;
        }

        // Faint enough to read as a mark on the glass rather than as
        // content, but not so faint it can be missed on a bright
        // background - which is the whole job.
        const ImU32 color = ToImColor(0xFFFFFFFFu, 0.20f);
        ImFont* font = ImGui::GetFont();
        const char* lines[] = {strings::kDemoTitle, strings::kDemoSubtitle};
        // Measured at the size actually being drawn, not the UI font's -
        // GetFont()->CalcTextSizeA takes the size, ImGui::CalcTextSize
        // doesn't - so the block's own width is the wider of the two lines.
        float blockW = 0.0f;
        for (const char* line : lines) {
            blockW = std::max(blockW, font->CalcTextSizeA(Px(kTextSize), FLT_MAX, 0.0f, line).x);
        }
        const float blockH = 2.0f * Px(kTextSize) + Px(kLineGap);

        // The cell grid covers the positions the block's *top-left* may
        // take, so the whole mark stays inside the margin whichever cell it
        // lands in.
        const float spanX = std::max(0.0f, displayW - 2.0f * Px(kMargin) - blockW);
        const float spanY = std::max(0.0f, displayH - 2.0f * Px(kMargin) - blockH);
        const float cellW = spanX / kGrid;
        const float cellH = spanY / kGrid;
        const float x = Px(kMargin) + static_cast<float>(demoWatermarkCell_ % kGrid) * cellW +
                        demoWatermarkJitter_.x * cellW;
        float y = Px(kMargin) + static_cast<float>(demoWatermarkCell_ / kGrid) * cellH +
                  demoWatermarkJitter_.y * cellH;
        for (const char* line : lines) {
            drawList->AddText(font, Px(kTextSize), ImVec2(x, y), color, line);
            y += Px(kTextSize) + Px(kLineGap);
        }
    }
}

// The "your clicks land here, not in the game" frame - see
// AppConfig::showEditModeBorder. Called only from RenderScreenChrome, which
// is edit-mode-only; view-only mode passes input straight through and so
// has nothing to warn about, and deliberately draws no border of its own.
//
// Inset by half its own width rather than drawn on the screen edge:
// ImDrawList::AddRect centers thickness on the path it's given, so a rect
// at the actual edge would have half of every side clipped away off-screen
// and the border would render at half the width the user asked for.
void OverlayApp::DrawEditModeBorder(ImDrawList* drawList, float displayW, float displayH) const {
    if (!Cfg().showEditModeBorder || Cfg().editModeBorderOpacity <= 0.0f || Cfg().editModeBorderWidthPx <= 0.0f) {
        return;
    }
    if (Cfg().editModeBorderOnlyWhenEmpty) {
        // "Empty" means nothing at all on this canvas - no items (even a
        // blank one still proves the overlay is up) and no un-armed ink on
        // the live layer either. No canvas at all counts as empty too, and
        // is exactly when the cue is worth the most: the screen is
        // otherwise completely blank.
        const Canvas* canvas = Manager().CurrentOrNull();
        const bool anythingShown =
            canvas != nullptr && (!session_.LiveLayer().Strokes().empty() ||
                                  std::any_of(canvas->items.begin(), canvas->items.end(),
                                              [&](const Item& item) { return !Manager().IsDeleted(*canvas, item); }));
        if (anythingShown) {
            return;
        }
    }
    const float half = Cfg().editModeBorderWidthPx * 0.5f;
    drawList->AddRect(ImVec2(half, half), ImVec2(displayW - half, displayH - half),
                       ToImColor(Cfg().editModeBorderColorRGBA, Cfg().editModeBorderOpacity), 0.0f, ImDrawFlags_None,
                       Cfg().editModeBorderWidthPx);
}

// ================= Background: live layer + armed-item overlay + debug =================

void OverlayApp::RenderCanvasLayer(float displayW, float displayH) {
    ImDrawList* drawList = BeginScreenLayer("##spickzettel_canvas", displayW, displayH);

    // First, under everything: the frozen screen, if one is held. Stretched
    // to the display rather than drawn 1:1 so a resolution change between
    // the capture and now scales instead of leaving a gap - it will look
    // soft, but a soft backdrop beats a torn one.
    if (const uint64_t frozen = session_.FrozenScreenTexture(); frozen != 0) {
        drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(frozen)), ImVec2(0.0f, 0.0f),
                            ImVec2(displayW, displayH));
    }

    if (Cfg().showDebugOverlay) {
        DrawDebugOverlay(drawList, ImGui::GetIO(), Manager(), debugHoveredResizeHandle_);
    }

    // The armed item's own in-progress live stroke is drawn as part of
    // RenderItems instead of here - see the comment there for why (a Shot
    // item's opaque fill would otherwise hide it until the stroke
    // finishes).

    EndScreenLayer();
}

// The three things that belong over the canvas rather than in it, each in
// its own layer so their heights can be stated rather than inherited from
// where in the frame they happen to be drawn. Called after everything the
// canvas holds and before the Overview, so the whole group sits between
// them - the Overview is the one panel that covers everything, because it
// is the one you go to when something on screen is in the way.
//
// Bottom to top:
//  - The input options HUD. On the foreground draw list it would sit on
//    top of the Overview - including the Settings tab holding the switch
//    that turns it off.
//  - The edit-mode border, which is the "your clicks land here" cue: a
//    frame drawn under the snippets is a frame a fullscreen snippet hides
//    completely, which is exactly when the cue matters.
//  - The demo mark, above the border and everything below it, so nothing
//    but the Overview can cover it.
//
// Nothing here takes input (see BeginScreenLayer), so none of it changes
// what can be clicked, dragged or drawn on.
void OverlayApp::RenderScreenChrome(float displayW, float displayH) {
    DrawInputOptionsHud(BeginScreenLayer("##sz_input_hud_layer", displayW, displayH));
    EndScreenLayer();
    BringToFront("##sz_input_hud_layer");

    ImDrawList* chrome = BeginScreenLayer("##sz_chrome_layer", displayW, displayH);
    DrawEditModeBorder(chrome, displayW, displayH);
    DrawDemoWatermark(chrome, displayW, displayH);
    EndScreenLayer();
    BringToFront("##sz_chrome_layer");
}


}  // namespace sz::ui
