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
#include "core/canvas/item.h"
#include "core/drawing/stroke.h"
#include "core/drawing/stroke_mesh_cache.h"
// Every word the overlay shows, as sz::strings::k... - generated at
// configure time from assets/ui_strings.json (see cmake/UiStrings.cmake).
// Included here rather than in each overlay_app_*.cpp because all of them
// display something.
#include "core/util/timestamp_name.h"
#include "generated/ui_strings.h"

namespace sz::ui::overlay_detail {

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

// The accent - AppConfig::accentColorRGBA, #ff6a3d unless changed - and the
// two colours derived from it: a lighter one for hover and press, and the
// ink for text and icons drawn on top of it, dark or light by how bright
// the accent is. Functions rather than constants like the rest of the
// palette, because the accent is a setting: OverlayApp::OnFrame hands it to
// SetAccent whenever it changes, and every draw call reads the current one.
const ImVec4& Accent();
const ImVec4& AccentHover();
const ImVec4& AccentInk();
// The accent as a draw-list colour at `alpha`, for the calls that take an
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

// px, discard smaller region captures/erases/creations as a stray click
// rather than a deliberate drag - shared between the item-creation
// gesture (a region-capture drag too small to keep) and the raw
// pen/eraser/tool mouse pipeline (RectEraser's own drag threshold).
inline constexpr float kRegionMinSize = 24.0f;

// Preset colors, offered as a snippet's background tints (see
// RenderItemPropertiesPopover), in this order.
struct PresetColor {
    uint8_t r, g, b;
};
inline constexpr PresetColor kPresetPenColors[] = {
    {0xff, 0x6a, 0x3d}, {0xff, 0x4d, 0x6d}, {0xff, 0xd2, 0x3f},
    {0x4d, 0xd6, 0xb8}, {0x5a, 0xa9, 0xff}, {0xf5, 0xf7, 0xf9},
    // Black: every one of the six above is a bright ink meant for a dark or
    // busy background, and something has to draw on a *light* one.
    {0x00, 0x00, 0x00},
};

// A preset as the packed 0xRRGGBBAA every colour-carrying field in the app
// uses, opaque - one function for the four shifts, rather than each
// swatch row writing them out by hand.
inline constexpr uint32_t ColorFromPreset(const PresetColor& c) {
    return (static_cast<uint32_t>(c.r) << 24) | (static_cast<uint32_t>(c.g) << 16) |
            (static_cast<uint32_t>(c.b) << 8) | 0xFFu;
}

// True for a preset close enough to pure white to be visually
// indistinguishable from it. Only the item-background swatch row cares:
// that row already offers literal white as its own dedicated "no tint"
// entry (see RenderItemPropertiesPopover for why it can't just use the
// preset - a tint multiply by 0xF5F7F9 isn't a no-op, only 0xFFFFFF is),
// so drawing the near-white preset alongside it put two swatches on screen
// that look identical and do subtly different things.
inline constexpr bool IsNearWhitePreset(const PresetColor& c) {
    return c.r >= 0xF0 && c.g >= 0xF0 && c.b >= 0xF0;
}

// The backing a text note gets by default (ApplyCreationDefaults, and the
// first-run welcome note). Half-transparent black: note text defaults to
// white, and white on a light backing is poor contrast wherever the overlay
// sits over something pale. Black behind white reads on anything.
inline constexpr uint32_t kNoteBackgroundColorRGBA = 0x000000FFu;
inline constexpr float kNoteBackgroundOpacity = 0.5f;

// What makes two presses a double-click on the raw mouse pipeline (see
// OverlayApp::NoteDoubleClick): the second within this long and this far of
// the first. ImGui's own defaults are 0.30 s and 6 px; a little more time,
// since a double-click is how a fullscreen snippet is made and how drawing
// mode is entered, and a near miss there is a click that did nothing.
inline constexpr double kDoubleClickSeconds = 0.35;
inline constexpr float kDoubleClickPx = 6.0f;
// How long a press has to be held still to stand in for a double-click
// (see OverlayApp::MatureHeldPress) - for a finger or a pen, which cannot
// double-click reliably. Judged still by kDoubleClickPx, as a double-click
// is: past that it is a drag, and a drag is never a hold.
inline constexpr double kHoldSeconds = 0.5;

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

// What a ShortcutAction means in the app's own terms - exactly one of the
// three is set. This is the single place the config layer's flat list of
// bindable actions (which cannot see Tool or CreateAction, being below
// them) is tied to the enums the app acts on, and it is one table rather
// than a pair of switches so the two directions cannot drift: the
// Shortcuts tab walks tools and asks which action each one is, the key
// handler has an action and asks what to run.
struct ShortcutTarget {
    ShortcutAction action;
    std::optional<Tool> tool;
    std::optional<CreateAction> create;
    std::optional<ClipboardAction> clipboard;
};
extern const ShortcutTarget kShortcutTargets[kShortcutActionCount];
const ShortcutTarget& TargetForShortcut(ShortcutAction action);
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
// "Ctrl+Alt+O" / "F9" / "(none)" - what a key editor's button reads while
// it isn't capturing.
std::string FormatKeyComboLabel(const platform::KeyCombo& combo);

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
// alive in one expression (a label and its neighbour, say); it is valid
// until this has been called a handful more times, which for the "build a
// label, pass it straight to ImGui" use here is always. Nothing keeps one.
const char* Labeled(const char* text, const char* id);

// Pill-sized icon button (28x28, true circle, 13px icon) - the size the
// selection bar's buttons and the dock's chips are.
bool PillIconButton(const char* strId, const Icon& icon, bool active);
// The same, for a slot that holds a colour rather than an action - see its
// definition.
bool PillColorButton(const char* strId, uint32_t colorRGBA, bool highlighted);

// The colour button as a tile in a row of buttons: the same pill an icon
// tile wears - accent while it is on, plain while it is off - with the
// colour as a swatch where the icon would be. See the definition for why
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
// Call BringToFront on the same id, once per frame, in the order the
// layers should stack - see RenderScreenChrome.
ImDrawList* BeginScreenLayer(const char* id, float displayW, float displayH);
void EndScreenLayer();

// Re-asserts `name`'s window as the frontmost, undoing whatever position
// ImGui's own insertion/focus history left it at. Call once per frame, in
// back-to-front order, for every window whose stacking needs to track the
// data model rather than ImGui's default focus-driven ordering.
void BringToFront(const char* name);

// Reasserts the *currently open* popup (call from inside its own
// BeginPopup/EndPopup scope) to the front of the display order - needed
// every single frame it's open, not just the frame it was created on.
// Takes the current window directly rather than going through the
// name-based BringToFront above: an anonymous popup's actual ImGuiWindow
// name isn't the id string passed to OpenPopup/BeginPopup (that's only
// the ID seed), so a name lookup for it wouldn't find anything.
void KeepPopoverInFront();

// Reasserts any popup nested *underneath* the currently open one (call
// from the same place as KeepPopoverInFront, after the nested popup's own
// owning widget - e.g. after an ImGui::ColorEdit3 swatch). A widget like
// ColorEdit3 opens and closes its own internal popup with no caller-visible
// Begin/End pair to hook a KeepPopoverInFront() call into, and - unlike our
// own popups - never reasserts that popup's position in the display order
// on frames after the one it opened on. Left alone, this popup's own
// KeepPopoverInFront() call above would then win every frame after that
// one, since it runs unconditionally and a plain Begin() call for an
// already-open window doesn't re-front itself. See the definition
// (overlay_app.cpp) for how it finds such a popup without knowing its name.
void KeepChildPopupsInFront();

// Closes the innermost open popup - a help popover, a dropdown - and says
// whether there was one. For Escape, which otherwise reaches whatever is
// underneath and closes that instead: ImGui only closes its own popups on
// Escape when keyboard nav is enabled, which this app doesn't turn on.
// ImGui::CloseCurrentPopup is the equivalent from *inside* a popup's own
// Begin/End scope and does nothing outside it, which is exactly where this
// has to be called from.
bool CloseTopmostPopover();

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
// The same, for the colours whose alpha the user edits along with the hue
// (ImGui::ColorEdit4) rather than through a separate opacity slider - see
// AppConfig::itemBorderColorFrontRGBA.
void ColorRGBAToFloats4(uint32_t colorRGBA, float out[4]);
uint32_t FloatsToColorRGBA4(const float in[4]);

// One layer's pixels (its image, its placeholder gradient, or its plain
// fill - see layer.h) stretched to fill `pMin..pMax`, at the layer's own
// opacity. Nothing is drawn for a layer at zero opacity.
//
// `textureHandle` stands in for Layer::textureHandle when given, including
// when it is 0 ("no pixels, draw the fallback"). That is what the Overview's
// canvas previews need: the same layer drawn with a thumbnail-sized copy of
// its pixels, or with none, without touching the layer or copying it.
void DrawLayer(ImDrawList* drawList, const Layer& layer, ImVec2 pMin, ImVec2 pMax,
                std::optional<uint64_t> textureHandle = std::nullopt);

// An item's layers, bottom-first, plus its baked strokes, into
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
                      uint64_t strokeRasterTexture = 0, bool skipNoteText = false, StrokeMeshSlot meshCache = {});

// A scaled-down snapshot of `canvas`'s items in `thumbMin..thumbMax`, the
// way the display shows them - uniform scale, letterboxed. What the
// Overview's tiles and the canvas bar both draw; defined in
// overlay_app_overview.cpp. `previewTexture` supplies each layer's
// thumbnail-sized pixels (empty for none), and `meshCache` must not be the
// canvas's own - see OverlayApp::previewMeshCache_. A snippet deleted on
// its own is left out: a preview is of what the canvas holds, which for a
// deleted canvas is also what restoring it brings back.
void DrawCanvasPreview(ImDrawList* drawList, const Canvas& canvas, ImVec2 thumbMin, ImVec2 thumbMax, float displayW,
                        float displayH, StrokeRenderMode rendering, bool showStrokes,
                        const std::function<std::optional<uint64_t>(const Item&, size_t)>& previewTexture,
                        StrokeMeshSlot meshCache);
// One item as a preview draws it into `pMin..pMax`: each layer with the
// texture `previewTexture` has for it, then its strokes scaled from the
// item's native size to the box. What DrawCanvasPreview draws per item.
void DrawItemPreview(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, StrokeRenderMode rendering,
                     bool showStrokes,
                     const std::function<std::optional<uint64_t>(const Item&, size_t)>& previewTexture,
                     StrokeMeshSlot meshCache);
// An icon+text button in the accent colour - the Overview's primary actions
// (New folder, New canvas). Defined in overlay_app_overview.cpp.
bool PrimaryButton(const char* strId, const Icon& icon, const char* text);

// Smallest positive integer N such that `prefix + std::to_string(N)` isn't
// already exactly one of `existingNames` - see the definition's own doc
// comment (overlay_app.cpp) for why this beats a plain "count existing +
// 1". What an *item* is named after: "Drawing 3", "Note 2", "Screenshot 5"
// - a kind and a number, which is as much as an item's name is ever asked
// to carry (a tooltip in the dock, a line in a toast).
int NextAvailableNumber(const std::string& prefix, const std::vector<std::string>& existingNames);

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

// What a notch of the wheel does to the selection - see
// OverlayApp::ScaleSelectionByWheel and StepSelectionOpacity.
inline constexpr float kWheelScaleStep = 1.1f;
inline constexpr float kWheelOpacityStep = 0.05f;

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

// A deleted folder's or canvas's two buttons, side by side at the cursor:
// Restore, and Delete permanently. Ids "##restore" and "##deleteforgood",
// under whatever the caller has pushed.
enum class DeletedButton { None, Restore, DeleteForGood };
DeletedButton DeletedButtons(const char* restoreTip, const char* deleteTip);

// What a folder or canvas is called until someone renames it is
// TimestampName() - in core/util now, since CanvasManager names the ones
// it has to mint itself the same way.

}  // namespace sz::ui::overlay_detail
