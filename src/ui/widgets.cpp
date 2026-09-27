#include "ui/widgets.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>

#include "generated/ui_strings.h"

namespace sz::ui {

namespace {

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

}  // namespace

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
// which it was: against nine other tiles whose whole background answers
// the question, a ring around one swatch does not read as an answer at
// all.
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

// The same in DangerIconButton's red - for a destructive action that
// deserves visible text rather than a bare icon (the confirm-delete
// popup's own "Delete" button - see RenderConfirmDeletePopover).
bool DangerButton(const char* strId, const Icon& icon, const char* text) {
    return IconTextButton(strId, icon, text, theme::kDangerSoft, theme::kDanger, theme::kWhite);
}

// A plain text tab, active tab in accent, inactive tabs a quiet neutral -
// the Overview's own Canvases/Settings switcher (see OverviewPanel::Draw).
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

bool InputString(const char* label, std::string& text) {
    return ImGui::InputText(label, text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize,
                            &ResizeStringForInputText, &text);
}

int ResizeStringForInputText(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

bool PanelBackdrop(const char* windowId, float displayW, float displayH) {
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
    return clicked;
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

}  // namespace sz::ui
