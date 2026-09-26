#include "ui/theme.h"

#include <algorithm>
#include <cstdint>

namespace sz::ui {

namespace theme {
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
// until OverlayApp::Prepare applies the setting.
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

void ApplyAccentToStyle(ImGuiStyle& style) {
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_ButtonActive] = Accent();
    colors[ImGuiCol_CheckMark] = Accent();
    colors[ImGuiCol_SliderGrab] = Accent();
    colors[ImGuiCol_SliderGrabActive] = AccentHover();
    colors[ImGuiCol_SeparatorHovered] = Accent();
    colors[ImGuiCol_SeparatorActive] = Accent();
    colors[ImGuiCol_TextSelectedBg] = ImVec4(AccentHover().x, AccentHover().y, AccentHover().z, 0.35f);
    colors[ImGuiCol_DragDropTarget] = Accent();
    // The chosen row of a dropdown: the accent, softened so the text on it
    // - drawn in the ordinary text color, whatever the accent - still reads.
    colors[ImGuiCol_Header] = ImVec4(Accent().x, Accent().y, Accent().z, 0.45f);
    colors[ImGuiCol_HeaderActive] = ImVec4(Accent().x, Accent().y, Accent().z, 0.6f);
    colors[ImGuiCol_NavHighlight] = Accent();
}

// ImGui::GetStyle() dereferences the current context, which the platform
// window's renderer creates (Win32Dx11Renderer::Initialize) on the first
// showing - hence a frame, and never OverlayApp::AttachTo.
void ApplyStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    // The base font size is the font's, set when it was loaded, and not a
    // matter of style - a fresh ImGuiStyle would forget it.
    const float fontSizeBase = style.FontSizeBase;
    style = ImGuiStyle();
    style.FontSizeBase = fontSizeBase;
    style.WindowRounding = kRadiusLg;
    style.PopupRounding = kRadiusLg;
    style.FrameRounding = kRadiusSm;
    style.GrabRounding = kRadiusPill;
    style.ScrollbarRounding = kRadiusPill;
    style.ChildRounding = kRadiusMd;
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
    colors[ImGuiCol_Text] = kWhite;
    colors[ImGuiCol_TextDisabled] = kGraphite400;
    colors[ImGuiCol_WindowBg] = kPanelBg;
    colors[ImGuiCol_PopupBg] = kPanelBg;
    colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Border] = kPanelBorder;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = kFieldBg;
    colors[ImGuiCol_FrameBgHovered] = kHoverWash;
    colors[ImGuiCol_FrameBgActive] = kPanelBorderStrong;
    // A checked box keeps the unchecked box's gray, so the accent check
    // mark is what says it is on. Left unset, ImGui tints it toward its own
    // blue theme, which the accent then had to stand out against.
    colors[ImGuiCol_CheckboxSelectedBg] = kFieldBg;
    colors[ImGuiCol_Button] = kFieldBg;
    colors[ImGuiCol_ButtonHovered] = kHoverWash;
    // Hovering a dropdown row, or a profile's row, is hovering like any
    // button. Left unset, these three were ImGui's own blue.
    colors[ImGuiCol_HeaderHovered] = kHoverWash;
    colors[ImGuiCol_Separator] = kPanelBorderStrong;
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = kGraphite600;
    colors[ImGuiCol_ScrollbarGrabHovered] = kGraphite500;
    colors[ImGuiCol_ScrollbarGrabActive] = kGraphite400;
    colors[ImGuiCol_TitleBg] = kPanelBg;
    colors[ImGuiCol_TitleBgActive] = kPanelBg;
    ApplyAccentToStyle(style);

    // Every size above, and the text. ScaleAllSizes rounds each one down to
    // a whole pixel, so a 1px border stays 1px until the scale reaches 200%.
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

}  // namespace theme

// Converts our 0xRRGGBBAA packing (see stroke.h) to ImGui's native ImU32.
ImU32 ToImColor(uint32_t colorRGBA, float opacity) {
    const auto r = static_cast<int>((colorRGBA >> 24) & 0xFF);
    const auto g = static_cast<int>((colorRGBA >> 16) & 0xFF);
    const auto b = static_cast<int>((colorRGBA >> 8) & 0xFF);
    const auto a = static_cast<int>(static_cast<float>(colorRGBA & 0xFF) * opacity);
    return IM_COL32(r, g, b, a);
}

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

}  // namespace sz::ui
