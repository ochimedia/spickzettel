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
#include "ui/item_painting.h"
#include "ui/overlay_app.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/widgets.h"
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

// The popups OverlayApp::ApplyEffects opens, by the ids their render
// functions begin them with.
inline constexpr const char* kItemPropertiesPopupId = "##item_properties_popover";
inline constexpr const char* kColorChooserPopupId = "##color_chooser";
inline constexpr const char* kConfirmDeletePopupId = "##confirm_delete_popover";
// A popup's ImGui id, as its render function begins it.
const char* PopupId(PopupKind kind);

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

// How much of itself every snippet keeps while a new one is being made -
// see OverlayApp::ItemsFadedForCreation. Enough to tell where things are,
// little enough that what is being framed is what is seen.
inline constexpr float kCreationFadeAlpha = 0.2f;

// What a folder or canvas is called until someone renames it is
// TimestampName() - in core/util now, since CanvasManager names the ones
// it has to mint itself the same way.

}  // namespace sz::ui::overlay_detail
