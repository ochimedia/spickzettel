#pragma once

// The overlay's look: the palette, the accent, the corner radii, and the
// ImGui style they make - see docs/ARCHITECTURE.md's "Visual theme"
// section. Every themed draw call takes its color from here instead of
// hand-rolling one, so the whole app moves together if the palette ever
// changes. Also the bridge between the app's packed 0xRRGGBBAA colors and
// ImGui's.

#include <cstdint>

#include <imgui.h>

#include "ui/ui_scale.h"

namespace sz::ui {

// Px for a size - see ui_scale.h, which stays free of ImGui for the
// editor's sake.
inline ImVec2 Px(float x, float y) { return ImVec2(Px(x), Px(y)); }

namespace theme {

// `inline constexpr` (not a plain `constexpr`, which would give each
// including TU its own internally-linked copy) so every file that includes
// this header shares the exact same one definition, same as any other
// `inline` entity.
inline constexpr ImVec4 kGraphite600(0.200f, 0.239f, 0.298f, 1.00f);  // #333d4c
inline constexpr ImVec4 kGraphite500(0.290f, 0.333f, 0.400f, 1.00f);  // #4a5566
inline constexpr ImVec4 kGraphite400(0.392f, 0.439f, 0.510f, 1.00f);  // #647082
inline constexpr ImVec4 kGraphite300(0.537f, 0.580f, 0.643f, 1.00f);  // #8994a4
inline constexpr ImVec4 kGraphite200(0.702f, 0.733f, 0.780f, 1.00f);  // #b3bbc7
inline constexpr ImVec4 kGraphite100(0.867f, 0.882f, 0.906f, 1.00f);  // #dde1e7
inline constexpr ImVec4 kWhite(0.961f, 0.969f, 0.976f, 1.00f);        // #f5f7f9

// The accent - AppConfig::accentColorRGBA, #2c6c7c unless changed - and the
// two colors derived from it: a lighter one for hover and press, and the
// ink for text and icons drawn on top of it, dark or light by how bright
// the accent is. Functions rather than constants like the rest of the
// palette, because the accent is a setting: OverlayApp::Prepare hands it to
// SetAccent whenever it changes, and every draw call reads the current one.
const ImVec4& Accent();
const ImVec4& AccentHover();
const ImVec4& AccentInk();
// The accent as a draw-list color at `alpha`, for the calls that take an
// ImU32 straight rather than going through the style.
ImU32 AccentU32(uint8_t alpha = 255);
void SetAccent(uint32_t rgba);

inline constexpr ImVec4 kDanger(0.898f, 0.282f, 0.302f, 1.00f);  // #e5484d

// The tutorial's own highlight: the spotlight ring, and what the card marks
// out - a hint, a warning's title, the check, the progress. Not the accent:
// the ring has to stand out from the selection frame, which is the accent,
// and the accent is a setting that can be dark. Bright, so it reads on the
// card and on whatever the desktop shows.
inline constexpr ImVec4 kTutorialHighlight(0.290f, 0.835f, 1.000f, 1.00f);  // #4ad5ff

// var(--panel-bg): graphite-900 at 86% - the frosted-material panels
// (item chrome, popovers, the Overview panel). ImGui has no
// backdrop-filter/blur equivalent, so translucency plus a subtle border
// is the whole "glass" effect here, not a literal blur.
inline constexpr ImVec4 kPanelBg(0.094f, 0.118f, 0.149f, 0.86f);
inline constexpr ImVec4 kPanelBorder(0.961f, 0.969f, 0.976f, 0.09f);
inline constexpr ImVec4 kPanelBorderStrong(0.961f, 0.969f, 0.976f, 0.14f);
inline constexpr ImVec4 kFieldBg(0.961f, 0.969f, 0.976f, 0.05f);
inline constexpr ImVec4 kHoverWash(0.961f, 0.969f, 0.976f, 0.09f);   // .tb-btn:hover / .btn:hover background
inline constexpr ImVec4 kDangerSoft(0.898f, 0.282f, 0.302f, 0.16f);
// The danger red lifted for text on the panel - a deleted folder's or
// canvas's name with Show deleted on. kDanger itself is a fill, and as
// small text on graphite it reads darker than it is.
inline constexpr ImVec4 kDeletedInk(0.965f, 0.525f, 0.537f, 1.00f);
// Yellow, the same three ways: a folder that is not deleted itself but
// holds a deleted canvas, with Show deleted on - partly in the trash, so
// not the red of what is in it whole.
inline constexpr ImVec4 kCaution(0.961f, 0.773f, 0.259f, 1.00f);  // #f5c542
inline constexpr ImVec4 kCautionSoft(0.961f, 0.773f, 0.259f, 0.16f);
inline constexpr ImVec4 kCautionInk(0.980f, 0.851f, 0.502f, 1.00f);
// Green, as a soft fill and the ink on it: what is in effect right now -
// a profile's Active tag. Not the accent, which marks what is set here
// and can be any color the user picks, green included.
inline constexpr ImVec4 kRunningSoft(0.275f, 0.765f, 0.482f, 0.18f);  // #46c37b
inline constexpr ImVec4 kRunningInk(0.592f, 0.890f, 0.682f, 1.00f);   // #97e3ae

// Safe for FrameRounding/GrabRounding/ScrollbarRounding: ImDrawList's own
// AddRectFilled/AddRect clamp rounding to half the shape's own size before
// tessellating (imgui_draw.cpp's PathRect), so an oversized value here just
// yields a perfect pill regardless of the actual widget height. Do not
// push kRadiusPill into ImGuiStyleVar_WindowRounding though - that one
// feeds into a window's own *minimum height*, not just its corner radius
// (see docs/ARCHITECTURE.md's ImGui gotcha list) - use kRadiusLg there.
inline constexpr float kRadiusPill = 999.0f;
inline constexpr float kRadiusLg = 16.0f;
inline constexpr float kRadiusMd = 11.0f;
inline constexpr float kRadiusSm = 7.0f;

// Inset between an item's edge and where Item::noteText's first glyph
// starts, shared verbatim by DrawItemContent's read-only caption band and
// the note editor's window - both need to land text at the *exact* same
// pixel so nothing visibly shifts when editing starts or ends. The editor
// splits it between its window's WindowPadding and InputTextMultiline's
// own FramePadding, kNoteCaretRoom, so neither side adds any padding this
// constant doesn't already account for.
inline constexpr float kNoteTextPad = 6.0f;
// The part of kNoteTextPad inside the note editor's field: the field clips
// to its frame, and a caret at the start of a line sits on the text's left
// edge - with no room there it was cut off until the text moved it right.
inline constexpr float kNoteCaretRoom = 2.0f;

// Sets up ImGui's global style/colors for the app's dark "graphite +
// accent" material look, in place of ImGui's own built-in dark theme (flat
// opaque gray panels, square corners, no accent color), at the interface
// scale `scale` (see UiScale). OverlayApp::Prepare calls it on the first
// frame and again whenever the scale changes: it starts from a fresh style
// every time, because ImGuiStyle::ScaleAllSizes multiplies what is there,
// and a style scaled twice would be scaled by the product.
//
// Needs a live ImGui context, which is why it is called from a frame and
// not from OverlayApp::AttachTo: the context isn't created until the
// platform window's EnsureCreated() runs, well after AttachTo.
void ApplyStyle(float scale);
// The style colors that are the accent, from whatever Accent() is now.
// Separate from ApplyStyle because the accent is a setting and these are
// applied again whenever it changes.
void ApplyAccentToStyle(ImGuiStyle& style);

}  // namespace theme

// Packs a stored 0xRRGGBBAA color plus a separate opacity multiplier into
// the ImU32 (0xAABBGGRR) ImDrawList wants.
ImU32 ToImColor(uint32_t colorRGBA, float opacity = 1.0f);

// The two halves of the bridge between this app's own packed 0xRRGGBBAA
// colors and the float[3] every ImGui color widget (ColorEdit3/
// ColorPicker3) reads and writes. The alpha byte round-trips separately -
// none of those widgets touch it, and every color that goes through here
// is opaque or carries its opacity in a field of its own anyway (Item::
// backgroundOpacity).
void ColorRGBAToFloats(uint32_t colorRGBA, float out[3]);
uint32_t FloatsToColorRGBA(const float in[3], uint8_t alpha);
// The same, for the colors whose alpha the user edits along with the hue
// (ImGui::ColorEdit4) rather than through a separate opacity slider - see
// AppConfig::itemBorderColorFrontRGBA.
void ColorRGBAToFloats4(uint32_t colorRGBA, float out[4]);
uint32_t FloatsToColorRGBA4(const float in[4]);

}  // namespace sz::ui
