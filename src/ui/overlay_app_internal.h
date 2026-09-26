#pragma once

// Helpers shared across the overlay_app*.cpp translation units that
// together implement OverlayApp (see overlay_app.h for the class itself,
// and docs/ARCHITECTURE.md for how the pieces fit together). Nothing here
// is part of OverlayApp's own public interface - it's split out purely
// because a handful of small drawing/widget helpers are genuinely used by
// more than one of those .cpp files, and an anonymous-namespace function
// (the usual place for a file-local helper in this codebase) has internal
// linkage, invisible outside the one .cpp it's defined in. Everything
// declared here is defined exactly once, in overlay_app.cpp, so every
// other file that includes this sees the same one definition rather than
// a drifting copy of its own.
//
// Anything used by only a single overlay_app_*.cpp file stays exactly
// where it always was: a plain anonymous-namespace helper local to that
// one file, not listed here.

#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "ui/icon_draw.h"
#include "ui/overlay_app.h"
#include "ui/ui_scale.h"
#include "core/canvas/item.h"
#include "core/drawing/stroke.h"
#include "core/drawing/stroke_mesh_cache.h"
// Every word the overlay shows, as sz::strings::k... - generated at
// configure time from assets/ui_strings.json (see cmake/UiStrings.cmake).
// Included here rather than in each overlay_app_*.cpp because all of them
// display something.
#include "core/util/timestamp_name.h"
#include "generated/ui_strings.h"

namespace sz::ui {
// Px for a size - see ui_scale.h, which stays free of ImGui for the
// editor's sake.
inline ImVec2 Px(float x, float y) { return ImVec2(Px(x), Px(y)); }
}  // namespace sz::ui

namespace sz::ui::overlay_detail {

// The popups OverlayApp::ApplyEffects opens, by the ids their render
// functions begin them with.
inline constexpr const char* kItemPropertiesPopupId = "##item_properties_popover";
inline constexpr const char* kColorChooserPopupId = "##color_chooser";
inline constexpr const char* kConfirmDeletePopupId = "##confirm_delete_popover";
// A popup's ImGui id, as its render function begins it.
const char* PopupId(PopupKind kind);

// Color palette + corner-radius scale - see docs/ARCHITECTURE.md's
// "Visual theme" section. Every themed draw call across the split files
// pulls its color from here instead of hand-rolling one, so the whole app
// moves together if the palette ever changes. `inline constexpr` (not a
// plain `constexpr`, which would give each including TU its own
// internally-linked copy) so every file that includes this header shares
// the exact same one definition, same as any other `inline` entity.
namespace theme {
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
// palette, because the accent is a setting: OverlayApp::OnFrame hands it to
// SetAccent whenever it changes, and every draw call reads the current one.
const ImVec4& Accent();
const ImVec4& AccentHover();
const ImVec4& AccentInk();
// The accent as a draw-list color at `alpha`, for the calls that take an
// ImU32 straight rather than going through the style.
ImU32 AccentU32(uint8_t alpha = 255);
void SetAccent(uint32_t rgba);

inline constexpr ImVec4 kDanger(0.898f, 0.282f, 0.302f, 1.00f);  // #e5484d

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
// RenderNoteEditor's window - both need to land text at the
// *exact* same pixel so nothing visibly shifts when editing starts or
// ends. The editor achieves this by using it as WindowPadding and zeroing
// InputTextMultiline's own FramePadding for that one widget, so neither
// side adds any padding this constant doesn't already account for.
inline constexpr float kNoteTextPad = 6.0f;
}  // namespace theme

// The backing a text note gets by default (ApplyCreationDefaults, and the
// first-run welcome note). Half-transparent black: note text defaults to
// white, and white on a light backing is poor contrast wherever the overlay
// sits over something pale. Black behind white reads on anything.
inline constexpr uint32_t kNoteBackgroundColorRGBA = 0x000000FFu;
inline constexpr float kNoteBackgroundOpacity = 0.5f;

// Every tool, with the icon, name and tooltip it is offered under - the
// marking tools and Select first, then the two creation tools, which is
// the order the Overview's Shortcuts tab lists them in. One table, so a
// tool is drawn and named the same way wherever it is shown.
struct GalleryTool {
    Tool tool;
    const Icon* icon;
    const char* name;
    const char* tooltip;
};
// Every create action - things that happen at once rather than being a tool.
struct CreateActionInfo {
    CreateAction action;
    const Icon* icon;
    const char* tooltip;
    const char* name;
};
// Defined once, in overlay_app.cpp - see this header's own opening note.
// The icons they point at are per-translation-unit constants, so a table
// defined in the header would hand each file a different set of addresses.
// Copy, Cut, Paste and Duplicate, for the Shortcuts tab to list them by.
struct ClipboardActionInfo {
    ClipboardAction action;
    const Icon* icon;
    const char* name;
};
extern const GalleryTool kGalleryTools[6];
extern const CreateActionInfo kCreateActions[2];
extern const ClipboardActionInfo kClipboardActions[4];

// The key a Settings row binds, for a tool, a create action and a
// clipboard action - the rows are listed by those, and bind the
// ShortcutAction config stores the key under. What the key then runs is
// the command table's (see CommandForShortcut).
ShortcutAction ShortcutForTool(Tool tool);
ShortcutAction ShortcutForCreateAction(CreateAction action);
ShortcutAction ShortcutForClipboardAction(ClipboardAction action);

// The two directions between ImGui's key enum and platform::KeyCombo,
// which is what both key editors (the Settings tab's hotkeys, the
// Shortcuts tab's tool bindings) and the shortcut handler need.
//
// ImGuiKey_0..ImGuiKey_F24 is one contiguous run in imgui.h - digits, then
// letters, then function keys, nothing else mixed in - which is exactly
// the set KeyCombo can represent, so the range check doubles as the
// "is this a key we support" filter.
std::optional<platform::KeyCombo> ComboForImGuiKey(ImGuiKey key, bool ctrl, bool alt, bool shift);
// ImGuiKey_None for a combo holding no key, or one outside that set.
ImGuiKey ImGuiKeyForCombo(const platform::KeyCombo& combo);
// The same for the mouse buttons a shortcut may be - the middle button and
// the two side buttons, never the left or the right, which are what
// gestures are made with.
std::optional<platform::KeyCombo> ComboForImGuiMouseButton(ImGuiMouseButton button, bool ctrl, bool alt,
                                                          bool shift);
std::optional<ImGuiMouseButton> ImGuiMouseButtonForCombo(const platform::KeyCombo& combo);
// "Ctrl+Alt+O" / "F9" / "(none)" - what a key editor's button reads while
// it isn't capturing.
std::string FormatKeyComboLabel(const platform::KeyCombo& combo);

// The cheat sheet's content (see OverlayApp::RenderCheatSheet), apart from
// drawing it so the tests can read it: groups of rows, each what to press
// and what it does. A row with no keys is a line of context for the rows
// under it ("On empty canvas"). Built from the bindings as they are - the
// global hotkeys, the tool shortcuts `shortcuts` resolves to, the creation
// triggers - so a rebound key reads as bound, and an unbound one, or a
// trigger set to Off, drops its row rather than promising nothing.
struct CheatSheetRow {
    std::string keys;
    std::string what;
};
struct CheatSheetSection {
    const char* title;
    std::vector<CheatSheetRow> rows;
};
std::vector<CheatSheetSection> BuildCheatSheet(const AppConfig& config, const ShortcutBindings& shortcuts);

// An ImGui label whose words and whose identity are separate things:
// returns "<text>###<id>", and ImGui hashes only what follows the "###".
//
// Three hashes, not two - this is the whole point and easy to get wrong.
// "label##id" hides the id from the display but hashes the *entire*
// string, so rewording the label still renames the widget; only "###id"
// starts the hash there and leaves the words free.
//
// Without it, a widget's identity *is* its label, and rewording a checkbox
// silently renames the widget and breaks every test that reaches for it by
// name. The words come from assets/ui_strings.json and the id is written
// in the code, so text can be edited freely and an id changes on purpose.
//
// The result lives in a small rotating set of buffers, so several can be
// alive in one expression (a label and its neighbor, say); it is valid
// until this has been called a handful more times, which for the "build a
// label, pass it straight to ImGui" use here is always. Nothing keeps one.
const char* Labeled(const char* text, const char* id);

// Whether the button about to be drawn as `strId`, at `size` (as passed to
// ImGui::Button), is pressed this frame: held since an earlier frame and
// released over it now. For a button that turns accent once it has been
// pressed - a selected tab, a switched-on tile - to be drawn that way on
// the release frame too. Its colors are pushed before ImGui::Button says
// it was pressed, so without this the frame in between showed it neither
// held nor selected: the plain hover color, between two accent ones.
bool PressLandsThisFrame(const char* strId, const ImVec2& size);

// Pill-sized icon button (28x28, true circle, 13px icon) - the size the
// selection bar's buttons and the dock's chips are.
bool PillIconButton(const char* strId, const Icon& icon, bool active);
// The same, for a slot that holds a color rather than an action - see its
// definition.
bool PillColorButton(const char* strId, uint32_t colorRGBA, bool highlighted);

// The color button as a tile in a row of buttons: the same pill an icon
// tile wears - accent while it is on, plain while it is off - with the
// color as a swatch where the icon would be. See the definition for why
// this is not PillColorButton with a flag.
bool PillSwatchButton(const char* strId, uint32_t colorRGBA, bool active);

// Same size as PillIconButton but danger-red instead of accent-on-active -
// idle stays neutral, only hover/press go red.
bool DangerIconButton(const char* strId, const Icon& icon);

// A full-screen, input-transparent window that exists only to own a draw
// list at a particular height in the window stack. Nothing in one is
// clickable (`NoInputs`), so where a layer sits changes what covers what
// and nothing else - which is the whole reason these are windows rather
// than the background/foreground draw lists, whose height is fixed at the
// very bottom and the very top.
//
// Where a layer sits is the stack's to say - see OverlayApp::StackSurfaces.
ImDrawList* BeginScreenLayer(const char* id, float displayW, float displayH);
void EndScreenLayer();

// Which stroke renderer to use - StrokeRenderMode, from
// core/drawing/stroke_render_mode.h, so the config setting and the drawing
// code name the same thing. An enum rather than a bool from the start,
// because it travels next to one (DrawItemContent's skipNoteText) and two
// adjacent bools at a call site is a bug waiting for a careless edit -
// which is also why adding a third mode cost nothing here.

// Draws one stroke, transformed from its own native/stroke space into
// screen space via (offsetX/Y, scaleX/Y). The scale reaches the tessellator;
// the offset is applied to the finished vertices, which is what lets a
// cached mesh outlive its item being moved - see StrokeMeshSlot.
void DrawStroke(ImDrawList* drawList, const Stroke& stroke, StrokeRenderMode rendering, float offsetX, float offsetY,
                 float scaleX, float scaleY, float opacity = 1.0f, StrokeMeshSlot meshSlot = {});

// Packs a stroke's stored 0xRRGGBBAA color plus a separate opacity
// multiplier into the ImU32 (0xAABBGGRR) ImDrawList wants.
ImU32 ToImColor(uint32_t colorRGBA, float opacity = 1.0f);

// The two halves of the bridge between this app's own packed 0xRRGGBBAA
// colors and the float[3] every ImGui color widget (ColorEdit3/
// ColorPicker3) reads and writes. The alpha byte round-trips separately -
// none of those widgets touch it, and every color that goes through here
// carries its opacity in a field of its own anyway (Item::
// backgroundOpacity, AppConfig::editModeBorderOpacity).
void ColorRGBAToFloats(uint32_t colorRGBA, float out[3]);
uint32_t FloatsToColorRGBA(const float in[3], uint8_t alpha);
// The same, for the colors whose alpha the user edits along with the hue
// (ImGui::ColorEdit4) rather than through a separate opacity slider - see
// AppConfig::itemBorderColorFrontRGBA.
void ColorRGBAToFloats4(uint32_t colorRGBA, float out[4]);
uint32_t FloatsToColorRGBA4(const float in[4]);

// `texture` stretched to fill `pMin..pMax`, tinted by `tint`, resampled
// the way `sampling` says - every snippet picture is drawn through here.
// Bilinear is ImGui's own sampler and adds nothing to the draw list; any
// other filter is the picture between two callbacks, the renderer's and
// ImGui's reset.
void DrawPicture(ImDrawList* drawList, uint64_t texture, ImVec2 pMin, ImVec2 pMax, ImU32 tint,
                  ImageSampling sampling);

// A snippet's picture stretched to fill `pMin..pMax`, at its own opacity:
// its pixels from `texture`, or, for 0, its placeholder gradient or its
// plain fill (see picture.h). Nothing is drawn at zero opacity. `texture`
// is the full-size picture's or, in the Overview's previews, a
// thumbnail-sized copy of it. `sampling` is how the pixels, if there are
// some, are resampled - see ImageSampling.
void DrawSnippetPicture(ImDrawList* drawList, const Picture& picture, ImVec2 pMin, ImVec2 pMax, uint64_t texture,
                        ImageSampling sampling = {});

// An item's picture, from `pictureTexture` (see DrawSnippetPicture), plus
// its baked strokes, into
// `pMin..pMax` - shared by
// RenderItems' per-item interactive window, RenderViewOnly's flat
// read-only pass, and the dock's own thumbnail chips. Caller owns clipping
// (PushClipRect/PopClipRect) around this. `skipNoteText`, true only from
// RenderItems' own per-item loop for whichever Note is currently being
// live-edited, skips just the read-only AddText call (the panel
// background still draws) so it doesn't double up with the actual
// InputTextMultiline's own text rendering on top of it - see
// RenderNoteEditor's window.
// `strokeRasterTexture` is only consulted in StrokeRenderMode::Rasterized:
// it's the item's strokes already drawn into a bitmap (see
// OverlayApp::RefreshStrokeRasters), composited in one AddImage instead of
// stroke by stroke. 0 means there isn't one - a preview that keeps no
// cache, or an item whose raster hasn't been built yet - and the strokes
// are drawn tessellated instead, which is the right thing to fall back to
// rather than nothing.
// `meshCache` is passed straight down to each stroke - see StrokeMeshSlot.
void DrawItemContent(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, StrokeRenderMode rendering,
                      uint64_t pictureTexture, uint64_t strokeRasterTexture = 0, bool skipNoteText = false,
                      StrokeMeshSlot meshCache = {}, ImageSampling sampling = {});

// A scaled-down snapshot of `canvas`'s items in `thumbMin..thumbMax`, the
// way the display shows them - uniform scale, letterboxed. What the
// Overview's tiles and the canvas bar both draw; defined in
// overlay_app_overview.cpp. `previewTexture` supplies each picture's
// thumbnail-sized pixels (empty for none), and `meshCache` must not be the
// canvas's own - see OverlayApp::previewMeshCache_. A snippet deleted on
// its own is left out: a preview is of what the canvas holds, which for a
// deleted canvas is also what restoring it brings back.
void DrawCanvasPreview(ImDrawList* drawList, const Canvas& canvas, ImVec2 thumbMin, ImVec2 thumbMax, float displayW,
                        float displayH, StrokeRenderMode rendering, bool showStrokes,
                        const std::function<std::optional<uint64_t>(const Item&)>& previewTexture,
                        StrokeMeshSlot meshCache, ImageSampling sampling);
// One item as a preview draws it into `pMin..pMax`: its picture with the
// texture `previewTexture` has for it, then its strokes scaled from the
// item's native size to the box. What DrawCanvasPreview draws per item.
void DrawItemPreview(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, StrokeRenderMode rendering,
                     bool showStrokes,
                     const std::function<std::optional<uint64_t>(const Item&)>& previewTexture,
                     StrokeMeshSlot meshCache, ImageSampling sampling);
// An icon+text button in the accent color - the Overview's primary actions
// (New folder, New canvas). Defined in overlay_app_overview.cpp.
bool PrimaryButton(const char* strId, const Icon& icon, const char* text);

// PillIconButton's and DangerIconButton's size.
inline constexpr float kPillButtonSize = 28.0f;

// The folder list down the Overview's left side - the sidebar's width, and
// where the footer's "New canvas" lines up (see
// OverlayApp::OverviewSidebarWidth, which adds to it with Show deleted on).
// Wide enough for a folder's default name, which is a full timestamp
// ("2026-09-07 22:53:26" - see TimestampName). Narrower clips the last digit
// of the seconds, which reads as a rendering bug rather than as a name that
// is simply long. The row reserves 34 for the delete button and insets the
// text by 10, so this is the name's width plus room to breathe.
inline constexpr float kOverviewSidebarWidth = 200.0f;

// How much of itself every snippet keeps while a new one is being made -
// see OverlayApp::ItemsFadedForCreation. Enough to tell where things are,
// little enough that what is being framed is what is seen.
inline constexpr float kCreationFadeAlpha = 0.2f;

// ----- Show deleted (see overlay_app_deleted.cpp) -----

// Between a deleted thing's Restore and its Delete permanently.
inline constexpr float kDeletedButtonGap = 4.0f;
// How much of itself a folder or canvas with nothing deleted about it keeps
// while Show deleted is on: there, still usable, and plainly not what the
// view is about.
inline constexpr float kDimmedAlpha = 0.4f;

// "Deleted today, 14:05 - 32 min ago": the day in words while that is
// shorter than a date, and how long ago while that is the quicker thing to
// read - which is what "I deleted something half an hour ago" is looking
// for. `now` is passed in, so a test can say when that is.
std::string DeletedWhen(int64_t deletedAt, std::time_t now);
// "Deleted permanently from <date> on": when the retention period, `days`
// long, takes something deleted at `deletedAt` - the first start from that
// day on (see TrayController::Initialize).
std::string GoesOn(int64_t deletedAt, int days);

// A deleted folder's or canvas's two buttons, side by side at the cursor:
// Restore, and Delete permanently. Ids "##restore" and "##deleteforgood",
// under whatever the caller has pushed.
enum class DeletedButton { None, Restore, DeleteForGood };
DeletedButton DeletedButtons(const char* restoreTip, const char* deleteTip);

// What a folder or canvas is called until someone renames it is
// TimestampName() - in core/util now, since CanvasManager names the ones
// it has to mint itself the same way.

}  // namespace sz::ui::overlay_detail
