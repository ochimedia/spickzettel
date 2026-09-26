#pragma once

// The overlay's own widgets, drawn in the theme (see theme.h): the buttons
// every surface uses, the dimming backdrop behind a panel, the full-screen
// layers the canvas and the chrome paint into, and the names of keys. What
// is used by one surface only stays with it.

#include <optional>
#include <string>

#include <imgui.h>

#include "platform/i_overlay_window.h"
#include "ui/icon_draw.h"
#include "ui/theme.h"

namespace sz::ui {

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

// PillIconButton's and DangerIconButton's size.
inline constexpr float kPillButtonSize = 28.0f;

// Pill-sized icon button (28x28, true circle, 13px icon) - the size the
// selection bar's buttons and the dock's chips are.
bool PillIconButton(const char* strId, const Icon& icon, bool active);
// The color button as a tile in a row of buttons: the same pill an icon
// tile wears - accent while it is on, plain while it is off - with the
// color as a swatch where the icon would be. See the definition for why
// the pill, and not a ring around the swatch, says it is on.
bool PillSwatchButton(const char* strId, uint32_t colorRGBA, bool active);
// Same size as PillIconButton but danger-red instead of accent-on-active -
// idle stays neutral, only hover/press go red.
bool DangerIconButton(const char* strId, const Icon& icon);

// An icon+text button in the given colors - the .btn equivalent.
bool IconTextButton(const char* strId, const Icon& icon, const char* text, const ImVec4& fill, const ImVec4& hover,
                    const ImVec4& ink);
// An icon+text button in the accent color - a panel's primary action (New
// folder, New canvas).
bool PrimaryButton(const char* strId, const Icon& icon, const char* text);
// The same in DangerIconButton's red - for a destructive action that
// deserves visible text rather than a bare icon (the delete confirmation's
// own "Delete").
bool DangerButton(const char* strId, const Icon& icon, const char* text);
// A plain text tab, the active one in accent and the rest a quiet neutral -
// the Overview's Canvases/Settings/About switcher.
bool TabButton(const char* id, const char* text, bool active);

// Dims the whole screen behind a panel - the Overview, the cheat sheet -
// and is true for a click on it, outside the panel, which closes it.
bool PanelBackdrop(const char* windowId, float displayW, float displayH);

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

}  // namespace sz::ui
