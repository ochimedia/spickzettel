#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/config/bar_layout.h"
#include "core/config/profile.h"
#include "core/config/shortcut_action.h"
#include "core/drawing/stroke_render_mode.h"
#include "platform/platform_types.h"

namespace sz::core {

// Which left press on empty canvas makes a snippet of one kind - see
// AppConfig::screenshotTrigger. Shift is not one of them: a Shift-drag on
// empty canvas is the box that selects.
enum class CreationTrigger { Plain, Ctrl, Alt, Off };

// User-editable settings, persisted as config.json (see ParseConfig/
// SerializeConfig below). No dependency on any OS API — file I/O and path
// resolution happen outside this type.
//
// The fields here are flat; the file groups them (`hotkeys`, `drawing`,
// `appearance`, `bars`, `overview`, `deleted`, `behavior`, `shortcuts`,
// `diagnostics`) and the mapping lives in one place, the serializer. The
// grouping is not cosmetic: `behavior` and `shortcuts` are exactly the settings a
// per-application profile may override - the ones that are about the
// machine in front of you rather than about you - so a profile is the same
// two objects again, sparse.
struct AppConfig {
    // Shows (or, pressed again in the same mode, hides) the overlay in
    // edit mode: the full interactive canvas. This
    // and the other two hotkeys below are editable at runtime, from the
    // Overview's own Settings tab - see OverlayApp::SetHotkeyChangeCallback
    // and TrayController::ChangeHotkey for how an in-app edit reaches the
    // OS and gets persisted back here.
    platform::KeyCombo hotkeyEditMode{/*ctrl=*/true, /*alt=*/true, /*shift=*/false, /*key=*/'S'};
    // Shows (or, pressed again in the same mode, hides) the overlay in
    // view-only mode: the current canvas is displayed read-only, with no
    // chrome and no input captured at all - clicks and keyboard input
    // pass straight through to whatever's underneath. Pressing the other
    // mode's hotkey switches between the two without hiding first - see
    // TrayController's own doc comment for the full state table.
    platform::KeyCombo hotkeyViewMode{/*ctrl=*/true, /*alt=*/true, /*shift=*/false, /*key=*/'V'};
    // Captures a fullscreen screenshot onto a new canvas at the end of the
    // folder the current one lives in, and goes to it (see
    // OverlayApp::QuickCapture for why a canvas each) -
    // a fallback for grabbing a shot of the game before edit mode's own
    // hotkey might cost it a frame or two (some games react to losing
    // focus by pausing/throttling) - then switches the overlay to edit
    // mode (showing it if hidden, switching in place if it was in
    // view-only mode) so there's something to actually notice: the
    // capture itself happens first, while the window is still in
    // whatever state it was in, so the screenshot is never of the
    // overlay's own edit-mode UI. See OverlayApp::QuickCapture and
    // TrayController::OnQuickCaptureHotkey.
    platform::KeyCombo hotkeyQuickCapture{/*ctrl=*/true, /*alt=*/true, /*shift=*/false, /*key=*/'C'};
    // The same capture, onto the same kind of canvas of its own, without
    // the overlay coming up: press it, keep working, and the shot is
    // waiting for the next time you open the overlay. All the hotkey above
    // does differently is show you the result.
    //
    // Not silent to the point of saying nothing, though - see
    // showToastsWhileHidden, and TrayController::OnSilentCaptureHotkey for
    // how a message reaches the screen with no overlay behind it.
    //
    // The one hotkey whose registration is allowed to fail: the other
    // three are how the overlay is reached at all, so the app refuses to
    // start without them, while this one is an extra, and a machine where
    // another application already owns Ctrl+Alt+X should still get an app
    // that runs. Rebind it in Settings if that happens.
    platform::KeyCombo hotkeySilentCapture{/*ctrl=*/true, /*alt=*/true, /*shift=*/false, /*key=*/'X'};
    // What each tool and create action is bound to while the overlay is up
    // in edit mode - see ShortcutAction, and the Overview's own Shortcuts
    // tab, which is where these are edited.
    //
    // Not OS-level hotkeys, and deliberately not stored alongside the three
    // above: these are plain keys the overlay reads from its own frame,
    // they only do anything while it is showing and taking input, and
    // nothing about them can fail the way registering a global hotkey can.
    // That is also why they're allowed to be bare letters - "P" costs
    // nothing outside edit mode.
    ShortcutBindings toolShortcuts = DefaultShortcuts();
    // Per-application overrides for the two groups above (`behavior` and
    // `toolShortcuts`) - see Profile. In list order, first match wins, and
    // an empty list is the ordinary case: the settings here are then simply
    // what runs, everywhere.
    //
    // Deliberately part of AppConfig rather than a file of its own: they
    // are settings, they are edited in the same panel, and one file means
    // one thing to back up and one thing to hand-edit.
    std::vector<Profile> profiles;
    uint32_t strokeColorRGBA = 0xFF0000FF;  // opaque red (0xAABBGGRR)
    float strokeWidth = 3.0f;
    // Draws a border around the screen plus a status line (stroke count,
    // active-stroke state, tracked mouse position) whenever the overlay is
    // shown — useful for confirming the overlay is rendering/receiving
    // input at all when bringing Spickzettel up on a new machine. Off by
    // default since it's a diagnostic aid, not part of the drawing surface.
    bool showDebugOverlay = false;
    // Diagnostic aid for the `editModeInput` options below, off by default:
    // a panel listing every one of them with its current state, plus a
    // frame rate and the pointer's own step statistics, and a number key per
    // row to flip that option without leaving the game. Meant for standing
    // in front of a misbehaving game and finding the combination that works;
    // the Settings tab is where the same options live for ordinary use.
    //
    // The number keys need the keyboard hook to reach a deliberately
    // focus-less overlay, so turning this on installs one even when
    // `editModeInput.dontForwardKeystrokes` is off - which means digits go
    // to the overlay instead of the game for as long as it's on. That is
    // the whole reason it isn't on all the time.
    bool showInputOptionsHud = false;
    // A read-only listing of the library directory as it is on disk, drawn
    // top-right in edit mode and re-read after every write the store makes
    // - so what was just saved can be checked against what the app
    // believes, without leaving the overlay. Costs a directory walk per
    // write while it is on, and nothing while it is off.
    bool showLibraryTreeHud = false;
    // true (default): the overlay window never steals OS input focus just
    // from being shown or clicked in edit mode (WS_EX_NOACTIVATE on
    // Windows) - drawing/item interaction all work purely via
    // mouse routing while the game underneath keeps keyboard focus and
    // doesn't see a focus-loss event, so it won't pause/throttle the way it
    // does on view-only mode's own hotkey otherwise. The one exception is
    // typing into a rename field (see OverlayApp's folder/canvas rename),
    // which briefly requests real focus for as long as that field is open,
    // then hands it back.
    //
    // This is the fundamental decision the whole `editModeInput` group
    // below exists to make good on. On its own it has a real, measured
    // cost: since the game stays the OS foreground window, and most games
    // gate raw/relative mouse input (the kind used for camera-look) on
    // being foreground rather than on being covered or z-order, the game
    // goes on receiving full mouse input at the same time as the overlay -
    // a dragged stroke can simultaneously spin the camera. That is exactly
    // what `editModeInput` takes back, which is why the two default on
    // together; turning this off makes every option in that group a no-op,
    // since a game that has lost focus has already stopped receiving input.
    //
    // Editable at runtime from the Settings tab, same as the hotkeys above
    // - see IOverlayWindow::SetEditModeNoActivate for how the change
    // reaches an already-created window live.
    bool editModeNoActivate = true;
    // true (default): `editModeNoActivate` is overruled, for one showing,
    // when the application in front is at a higher integrity level than
    // this process - something started as administrator, which on an
    // account with admin rights includes Task Manager. Windows hands such
    // an application's input to no lower-integrity process at all, so
    // leaving it focused costs not just the grab but every shortcut the
    // overlay has; taking focus is the only thing that restores either.
    // Only a *positive* reading acts: a process that refuses the question
    // is left alone, because a game behind an anti-cheat driver refuses it
    // the same way and is the one thing that must keep focus. See
    // docs/ARCHITECTURE.md.
    bool takeFocusOverElevated = true;
    // What edit mode does with physical input while `editModeNoActivate` is
    // leaving the game focused - see platform::EditModeInputOptions, which
    // documents each part and what it costs, and docs/ARCHITECTURE.md for
    // the measurements. All default to on, for the reason given above; each
    // stays individually switchable because which combination is right
    // still depends on the game.
    platform::EditModeInputOptions editModeInput;
    // true (default): every item on the current canvas draws a subtle
    // border around its bounds at all times in edit mode, not just the one
    // currently hovered/dragged/resized (that one always gets it regardless
    // of this setting - see OverlayApp::RenderItems). Mainly for a
    // still-empty Drawing item, which otherwise renders nothing at all
    // until it's hovered or has ink on it - easy to lose track of on a
    // busy canvas. Set to false to only show it on hover.
    bool showItemBorders = true;
    // Whether a hotkey that acts while the overlay is hidden may put a
    // message on screen to say what it did - which means showing the
    // overlay for as long as that message lasts, click-through and with
    // nothing else drawn (see TrayController::ShowNotice). On: a silent
    // capture is silent about interrupting you, not about having happened.
    // Off: it leaves no trace at all until you next open the overlay.
    //
    // Only about the *hidden* case. With the overlay already up, in either
    // mode, a message costs nothing - it is drawn in a frame that was
    // being drawn anyway, over a window that is already there - so it is
    // always shown and this setting has no say.
    bool showToastsWhileHidden = true;
    // The color of whatever is selected, active or current - tabs, the tool
    // in hand, checkboxes and sliders, the current canvas - packed 0xRRGGBBAA
    // with the alpha unused. Text drawn on it goes dark or light to stay
    // readable. The pinned-snippet border is a color of its own
    // (itemBorderColorPinnedRGBA).
    uint32_t accentColorRGBA = 0x2C6C7CFFu;  // teal
    // What tells the snippet in front from the ones behind it: its border
    // is drawn in its own color, both settable (packed 0xRRGGBBAA - the
    // alpha is part of the color here, since a more transparent border is
    // exactly how you make one recede). "Frontmost" is the last
    // non-minimized item in Canvas::items, the one painted over all the
    // others. Depth is shown by color and hover by a thicker border, so
    // the two cues don't compete for one channel. A selected snippet's
    // outline and handles are drawn in the accent, over these.
    uint32_t itemBorderColorFrontRGBA = 0xF5F7F96E;   // white, ~43%
    uint32_t itemBorderColorOtherRGBA = 0xF5F7F93C;   // white, ~24%
    // A pinned snippet's border (Item::pinned), in place of whichever of the
    // two above it would otherwise wear, and drawn whether or not
    // showItemBorders is on: a pinned snippet stays on screen when the
    // overlay is put away, and which ones will has to be seen at a glance.
    uint32_t itemBorderColorPinnedRGBA = 0xFF6A3D99;  // the accent, ~60%
    // Which of the three stroke renderers draws vector strokes - see
    // StrokeRenderMode for what each one is and what it costs. Purely a
    // rendering choice: the strokes are the same data either way, and
    // switching applies to what is already drawn as well as to new marks,
    // so the same drawing can be looked at three ways without redrawing it.
    // "Does this look better" is not a question any test answers.
    StrokeRenderMode strokeRenderMode = StrokeRenderMode::Tessellated;
    // How every picture in a snippet - a screenshot, a painted layer, the
    // Rasterized strokes - is resampled when shown at a size other than its
    // own. The same kind of choice: nothing stored changes, and switching
    // redraws what is already there. See platform::ImageFilter.
    platform::ImageFilter imageFilter = platform::ImageFilter::Bilinear;
    // Whether the drawing tools produce vector strokes or paint pixels.
    // One set of tools either way - a pen is a pen, and this decides where
    // its marks land, not which tools exist.
    //
    // Global for now, and deliberately so: it could as well be per snippet
    // or per tool, and which of those is right is not yet knowable. What
    // makes moving it later cheap is that it only ever picks the *target*
    // of a new mark - each layer records its own kind, so nothing already
    // drawn is reinterpreted when this changes, and a per-snippet version
    // would be the same decision read from the item instead of from here.
    //
    // Two tools ignore it. The eraser acts on whatever is under it, vector
    // and painted alike, because a snippet can hold both and "why won't
    // this erase" with no visible cause is the worst kind of bug. Text
    // stays live in both modes: rasterizing a caption would throw away the
    // one thing that makes it worth having.
    bool paintPixelsInsteadOfStrokes = false;
    // Whether selecting a snippet - a click on it, or the press that starts
    // dragging it - brings it in front of the others, the way a window
    // manager raises a window you take hold of. Off, the stacking order is
    // the context menu's alone to change (Send backward, Bring forward), as
    // in a drawing program.
    // See OverlayApp::HandleItemGesture.
    bool raiseSelectedSnippet = true;
    // What a left press on empty canvas makes, by the modifier held as it
    // starts: a plain press, one with Ctrl, one with Alt - or neither, and
    // that kind is then made from the canvas's context menu or its tool's
    // key alone. The two never share a trigger: ParseConfig puts back the
    // defaults if a file says they do, and Settings swaps them rather than
    // letting both have one. See OverlayApp::HandleCreationGesture.
    CreationTrigger screenshotTrigger = CreationTrigger::Plain;
    CreationTrigger drawingTrigger = CreationTrigger::Ctrl;
    // The two bars that float over the selection: which buttons each one
    // carries, in what order, and which of them are shown - see
    // BarButtonList, and Settings > Interaction, which is a row of the
    // same buttons to drag about and switch off. Global rather than
    // per-application: which buttons a bar has is about how you work, not
    // about what the overlay happens to be up over.
    BarButtonList snippetBar = DefaultSnippetBar();
    BarButtonList drawingBar = DefaultDrawingBar();
    // What the canvas overview's thumbnails show. Strokes are nearly free
    // to draw at tile size and are on by default, as they always were.
    // Bitmaps - screenshots and painted layers - are not: showing one means
    // reading and decoding its file, for canvases that aren't current and
    // whose pixels are therefore deliberately not in memory. On by default
    // all the same: a canvas that is mostly screenshots is unrecognizable
    // without them, and they are read a few a frame and let go when the
    // panel closes.
    bool overviewShowsStrokes = true;
    bool overviewShowsBitmaps = true;
    // The canvas bar along the bottom edge: the canvases of the folder being
    // worked in, to switch between them or start a new one. It hides against
    // the edge and slides out when the pointer reaches it, and for a moment
    // whenever the canvas changes. See OverlayApp::RenderCanvasBar.
    bool showCanvasBar = true;
    // true (default): while the overlay is up in *edit* mode, a border is
    // drawn around the whole screen - the "you are in edit mode, your
    // clicks land here and not in the game" cue. Deliberately not shown in
    // view-only mode, where clicks pass straight through and there's
    // nothing to warn about. Distinct from showDebugOverlay's own border
    // (a diagnostic, always-on-in-both-modes, with a status line attached);
    // this one is part of the normal look.
    bool showEditModeBorder = true;
    // 0xRRGGBBAA-packed like Item::backgroundColorRGBA - the alpha byte is
    // unused here too, editModeBorderOpacity is the alpha actually drawn.
    uint32_t editModeBorderColorRGBA = 0xFFFFFFFFu;  // white
    float editModeBorderOpacity = 0.22f;             // 0..1, translucent by default
    // Thickness in px, drawn fully inside the screen (inset by half its
    // own width, so all of it is visible rather than half of it hanging
    // off the edge). Clamped to kEditModeBorderWidthMin..Max by
    // ParseConfig.
    float editModeBorderWidthPx = 10.0f;
    // false (default): the border is shown for as long as edit mode is,
    // whatever's on the canvas. true: it's shown only while the current
    // canvas is still completely empty - no items, no ink - on the
    // reasoning that once there *is* content on screen, that content is
    // itself the proof the overlay is up, and the border has nothing left
    // to tell you. Turns the border from a permanent frame into a hint
    // that gets out of the way the moment it's redundant.
    bool editModeBorderOnlyWhenEmpty = false;
    // Freeze the screen while editing: on entering edit mode, grab what is
    // on screen and draw that instead of letting the live application show
    // through. For annotating over a game, this is the one thing that
    // reliably works. The camera underneath still turns while you draw -
    // nothing outside the game's process can stop that - but you no longer
    // have to watch it happen, which was most of the problem. Pairs with
    // editModeInput.counterRawMouseInput, whose job then becomes leaving
    // the view roughly where you found it rather than holding it still on
    // screen.
    //
    // Off by default: it changes what the overlay fundamentally is, from a
    // sheet of glass into an opaque page, and most of what the overlay is
    // up over is not a game that turns under the mouse. Worth switching on
    // in a game's profile. A region capture taken while the
    // screen is frozen crops the frozen image rather than re-capturing the
    // live screen, so a snippet matches what you were looking at when you
    // dragged it out - see Session::CaptureShotItem.
    bool freezeScreenInEditMode = false;

    // Whether a deleted folder or canvas is deleted for good once it has
    // been deleted for longer than purgeDeletedAfterDays - checked when the
    // library is opened, which is at startup (see Session::
    // EraseDeletedBefore for what counts, and how long, for what). On by
    // default, after two weeks, so a library does not keep everything ever
    // deleted; the number of days is kept while it is off so switching it
    // on again is one click. Deleted snippets are not a question here: they are
    // erased on every open (see Session::ImportLibrary). Chosen in
    // Settings > Behavior.
    bool purgeDeleted = true;
    int purgeDeletedAfterDays = 14;

    // Which display the overlay comes up on, remembered by the id and the
    // name it was listed under (see platform::DisplayInfo for what each is).
    // Empty: the primary display, whichever that is at the time. A chosen
    // display that is not attached is not forgotten - core::ChooseDisplay
    // puts the overlay on the primary meanwhile, and back on the chosen one
    // once it returns. Chosen in Settings > Appearance.
    std::string overlayDisplayId;
    std::string overlayDisplayName;

    bool operator==(const AppConfig&) const = default;
};

// The band editModeBorderWidthPx is held to. The low end is 1px (thinner
// than that and AddRect has nothing to draw); the high end is generous
// enough for a deliberately heavy frame while still leaving the screen
// mostly usable.
inline constexpr float kEditModeBorderWidthMin = 1.0f;
inline constexpr float kEditModeBorderWidthMax = 48.0f;

// The band purgeDeletedAfterDays is held to: at least a day, since "delete
// for good at the next start" is what Delete permanently is for, and at
// most ten years, past which the setting is "never" by another name.
inline constexpr int kPurgeDeletedAfterDaysMin = 1;
inline constexpr int kPurgeDeletedAfterDaysMax = 3650;

// Returns hardcoded defaults, matching the values a freshly-written config
// file would contain.
// The bridge between the file's own shape and what a profile talks about:
// the overridable subset pulled out, and put back. Two functions rather
// than storing a ProfileableSettings inside AppConfig, because the file
// groups these settings differently from the way a profile addresses them
// (`editModeInput` is one nested type there, four flat fields here) and
// only one place should know that.
ProfileableSettings ProfileableFrom(const AppConfig& config);
void ApplyProfileable(const ProfileableSettings& settings, AppConfig& config);

AppConfig DefaultConfig();

// Parses config.json text. Every setting is optional: one that is missing,
// of the wrong type, or out of range keeps its default, and a file that
// isn't valid JSON at all reads as one that said nothing. Nothing in the
// *content* of the text can make this throw - the same contract
// persistence::LibraryStore::Load has, and for the same reason: a
// hand-edited or truncated file should be treated as absent, not crash the
// app on startup. Running out of memory can, as it can anywhere.
AppConfig ParseConfig(std::string_view text);
// The same, but nullopt for text that is not a JSON object at all - which
// a settings file that is only missing some keys never is.
std::optional<AppConfig> TryParseConfig(std::string_view text);

// The most a settings file is read past. A config.json is a few kilobytes;
// one of megabytes is not a settings file, whatever it is, and is read as
// one that said nothing rather than allocated for - the same budget-before-
// allocation rule the library's records and pictures have (see
// persistence::kMaxRecordBytes).
constexpr size_t kMaxConfigFileBytes = size_t{1} << 20;

// A hotkey as config.json spells it - "Ctrl+Alt+O", "F9" - and as a message
// names it; empty for an unbound one.
std::string HotkeyText(const platform::KeyCombo& combo);

// What LoadOrCreateConfig found at the settings file's path, and did.
enum class ConfigSource {
    Read,        // a settings file, read
    Created,     // no file: a first run, answered by writing the defaults
    SetAside,    // not a settings file - not JSON, or past kMaxConfigFileBytes -
                 // renamed out of the way (to setAsideAs, if that worked)
    Unreadable,  // a file that could not be read, left where it is
};
struct LoadedConfig {
    AppConfig config;
    ConfigSource source = ConfigSource::Read;
    std::filesystem::path setAsideAs;
};

// The settings the app starts with. Only a missing file is a first run,
// answered by writing the defaults out for the user to edit. A file that is
// there but is not settings - a hand edit that left a trailing comma - used
// to read as defaults, and the next settings change then saved those over
// it, taking every hotkey and profile with it. It is renamed out of the
// way instead, to config-unreadable-<stamp>.json beside it, and one that
// cannot be read at all is left alone; either way the app starts on the
// defaults, and the caller says so. See main_win32.cpp.
LoadedConfig LoadOrCreateConfig(const std::filesystem::path& path, std::string_view stamp);

// The widest stroke a settings file can ask for, in pixels: past this a
// stroke is a fill, and the tessellator's work per point grows with the
// width. Held to, not rejected - a hand-typed 1000 means "very wide".
constexpr float kMaxStrokeWidthPx = 256.0f;

// Renders a config back to JSON text, suitable for writing to disk on first
// run or after an in-app settings change. Writes every setting, including
// the ones still at their default: the file doubles as the documentation of
// what can be set.
std::string SerializeConfig(const AppConfig& config);

// Writes SerializeConfig(config)'s text to `path`, creating its parent
// directory first if needed - the write-half of app_main's own
// LoadOrCreateConfig (main_win32.cpp/main_devlinux.cpp), factored out here
// so TrayController::OnSettingsChanged (an in-app settings edit, not a
// first-run default) has a single, unit-testable place to call rather
// than duplicating the same ofstream dance a third time. Returns false
// (does nothing) if `path` is empty - the "nowhere to persist to"
// convention shared with persistence::LibraryStore's own empty-directory
// handling - or if the file couldn't be opened for writing.
bool WriteConfigFile(const std::filesystem::path& path, const AppConfig& config);

}  // namespace sz::core
