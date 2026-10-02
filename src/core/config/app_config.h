#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/config/bar_layout.h"
#include "core/config/profile.h"
#include "core/config/shortcut_action.h"
#include "platform/platform_types.h"

namespace sz::core {

// Which left press on empty canvas makes a snippet of one kind - see
// AppConfig::screenshotTrigger. Shift is not one of them: a Shift-drag on
// empty canvas is the box that selects.
enum class CreationTrigger { Plain, Ctrl, Alt, Off };

// What a new snippet of one kind starts with - see AppConfig::
// screenshotDefaults. Each is the snippet property of the same name, which
// the snippet's own popover changes from then on: Item::keepAspect,
// Item::foregroundOpacity, and the picture's opacity.
struct SnippetDefaults {
    bool keepAspect = true;
    float foregroundOpacity = 1.0f;  // 0.1..1, as the popover's slider
    float backgroundOpacity = 1.0f;  // 0..1

    bool operator==(const SnippetDefaults&) const = default;
};

// User-editable settings, persisted as config.json (see ParseConfig/
// SerializeConfig below). No dependency on any OS API — file I/O and path
// resolution happen outside this type.
//
// The file groups these (`hotkeys`, `drawing`, `appearance`, `bars`,
// `overview`, `defaults`, `deleted`, `display`, `behavior`, `shortcuts`,
// `diagnostics`), and where each field is in the file is said in one place,
// its row in the catalog (settings_catalog.h). The grouping is not
// cosmetic: `behavior` and `shortcuts` are exactly the settings a
// per-application profile may override - the ones that are about the
// machine in front of you rather than about you - so a profile is the same
// two objects again, sparse, and they are held here as one struct,
// `profileable`.
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
    // TrayController::QuickCaptureAndShow.
    platform::KeyCombo hotkeyQuickCapture{/*ctrl=*/true, /*alt=*/true, /*shift=*/false, /*key=*/'C'};
    // The same capture, onto the same kind of canvas of its own, without
    // the overlay coming up: press it, keep working, and the shot is
    // waiting for the next time you open the overlay. All the hotkey above
    // does differently is show you the result.
    //
    // Not silent to the point of saying nothing, though - see
    // showToastsWhileHidden, and TrayController::SilentCapture for
    // how a message reaches the screen with no overlay behind it.
    //
    // Like every hotkey, one another application already owns is left
    // unregistered and noted at start, and the app runs without it (see
    // TrayController::UnregisteredHotkeys); rebind it in Settings.
    platform::KeyCombo hotkeySilentCapture{/*ctrl=*/true, /*alt=*/true, /*shift=*/false, /*key=*/'X'};
    // The settings a per-application profile may override - the file's
    // `behavior` and `shortcuts` groups - as the defaults have them. See
    // ProfileableSettings for each.
    ProfileableSettings profileable;
    // Per-application overrides for the settings above - see Profile. In
    // list order, first match wins, and an empty list is the ordinary case:
    // the settings here are then simply what runs, everywhere.
    //
    // Deliberately part of AppConfig rather than a file of its own: they
    // are settings, they are edited in the same panel, and one file means
    // one thing to back up and one thing to hand-edit.
    std::vector<Profile> profiles;
    uint32_t strokeColorRGBA = 0xFF0000FF;  // opaque red (0xRRGGBBAA)
    float strokeWidth = 3.0f;
    // Draws a border around the screen plus a status line (stroke count,
    // active-stroke state, tracked mouse position) whenever the overlay is
    // shown — useful for confirming the overlay is rendering/receiving
    // input at all when bringing Spickzettel up on a new machine. Off by
    // default since it's a diagnostic aid, not part of the drawing surface.
    bool showDebugOverlay = false;
    // Diagnostic aid for the input options (ProfileableSettings), off by default:
    // a panel listing every one of them with its current state, plus a
    // frame rate and the pointer's own step statistics, and a number key per
    // row to flip that option without leaving the game. Meant for standing
    // in front of a misbehaving game and finding the combination that works;
    // the Settings tab is where the same options live for ordinary use.
    //
    // The number keys need the keyboard hook to reach a deliberately
    // focus-less overlay, so turning this on installs one even when
    // `dontForwardKeystrokes` is off - which means digits go
    // to the overlay instead of the game for as long as it's on. That is
    // the whole reason it isn't on all the time.
    bool showInputOptionsHud = false;
    // Diagnostic aid, off by default: a graph in the top right corner of
    // the last ten seconds' frames - how long each took to come, and how
    // long the overlay spent building it - over lanes that mark the work
    // that can make one late: library commits and checkpoints, pictures
    // encoded and read, screen captures and config writes. Drawn in every
    // state the overlay is up in, without changing view mode's pace. See
    // core::Timeline, and docs/PERF.md, "Instrument 4".
    bool showFrameGraph = false;
    // true (default): every item on the current canvas draws a subtle
    // border around its bounds at all times in edit mode, not just the one
    // currently hovered/dragged/resized (that one always gets it regardless
    // of this setting - see CanvasView::RenderItems). Mainly for a
    // still-empty Drawing item, which otherwise renders nothing at all
    // until it's hovered or has ink on it - easy to lose track of on a
    // busy canvas. Set to false to only show it on hover.
    bool showItemBorders = true;
    // Whether a hotkey that acts while the overlay is hidden may put a
    // message on screen to say what it did - which means showing the
    // overlay for as long as that message lasts, click-through and with
    // nothing else drawn (a notice - see docs/OVERLAY_STATES.md). On: a silent
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
    // How large the overlay's own interface is drawn - text, buttons,
    // panels, bars - in percent. 0 follows Windows' scale for the display
    // the overlay is on, which is where someone who reads at 150% has
    // already said so. Snippets, strokes and note text are what is on the
    // screen rather than the interface to it, and keep their pixel sizes.
    // Held to kUiScalePercentMin..Max.
    int uiScalePercent = 0;
    // What tells the snippet in front from the ones behind it: its border
    // is drawn in its own color, both settable (packed 0xRRGGBBAA - the
    // alpha is part of the color here, since a more transparent border is
    // exactly how you make one recede). "Frontmost" is the last
    // non-minimized item in Canvas::items, the one painted over all the
    // others. Depth is shown by color and hover by a thicker border, so
    // the two cues don't compete for one channel. A selected snippet's
    // border is the selection's color instead, as heavy as these are (see
    // itemBorderColorSelectedRGBA).
    uint32_t itemBorderColorFrontRGBA = 0xF5F7F96E;   // white, ~43%
    uint32_t itemBorderColorOtherRGBA = 0xF5F7F93C;   // white, ~24%
    // A pinned snippet's border (Item::pinned), in place of whichever of the
    // two above it would otherwise wear, and drawn whether or not
    // showItemBorders is on: a pinned snippet stays on screen when the
    // overlay is put away, and which ones will has to be seen at a glance.
    uint32_t itemBorderColorPinnedRGBA = 0xFF6A3D99;  // the accent, ~60%
    // A selected snippet's border, in place of all of the above, and
    // drawing mode's halo with it: the accent while
    // itemBorderSelectedFollowsAccent is on, else
    // itemBorderColorSelectedRGBA, alpha and all. Kept while it follows
    // the accent, so switching it off brings back the color picked before.
    bool itemBorderSelectedFollowsAccent = true;
    uint32_t itemBorderColorSelectedRGBA = 0x2C6C7CFFu;  // the default accent
    // How every picture in a snippet - a screenshot, a fill's own pixels -
    // is resampled when shown at a size other than its own. The same kind of choice: nothing stored changes, and switching
    // redraws what is already there. See platform::ImageFilter.
    platform::ImageFilter imageFilter = platform::ImageFilter::Bilinear;
    // Whether selecting a snippet - a click on it, or the press that starts
    // dragging it - brings it in front of the others, the way a window
    // manager raises a window you take hold of. Off, the stacking order is
    // the context menu's alone to change (Send backward, Bring forward), as
    // in a drawing program.
    // See ui/interaction/recognizer.cpp.
    bool raiseSelectedSnippet = true;
    // What a left press on empty canvas makes, by the modifier held as it
    // starts: a plain press, one with Ctrl, one with Alt - or neither, and
    // that kind is then made from the canvas's context menu or its tool's
    // key alone. The two never share a trigger: ParseConfig puts back the
    // defaults if a file says they do, and Settings swaps them rather than
    // letting both have one. See Editor::EmptyCanvasCreationKind.
    CreationTrigger screenshotTrigger = CreationTrigger::Plain;
    CreationTrigger drawingTrigger = CreationTrigger::Ctrl;
    // The two groups of the bar that floats over the selection: which
    // buttons each one carries, in what order, and which of them are shown
    // - see BarButtonList, and Settings > Interaction, which is a row of
    // the same buttons to drag about and switch off. Global rather than
    // per-application: which buttons the bar has is about how you work,
    // not about what the overlay happens to be up over.
    BarButtonList snippetBar = DefaultSnippetBar();
    BarButtonList drawingBar = DefaultDrawingBar();
    // What the canvas overview's thumbnails show. Strokes are nearly free
    // to draw at tile size and are on by default, as they always were.
    // Screenshots are not: showing one means
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
    // whenever the canvas changes. See CanvasBar.
    bool showCanvasBar = true;
    // true (default): while the overlay is up in *edit* mode, a border is
    // drawn around the whole screen - the "you are in edit mode, your
    // clicks land here and not in the game" cue. Deliberately not shown in
    // view-only mode, where clicks pass straight through and there's
    // nothing to warn about. Distinct from showDebugOverlay's own border
    // (a diagnostic, always-on-in-both-modes, with a status line attached);
    // this one is part of the normal look.
    bool showEditModeBorder = true;
    // 0xRRGGBBAA-packed, its alpha part of the color as the snippet
    // colors have it: translucent white by default. Version 1 of the file
    // kept the alpha in an opacity of its own (config_migrations.cpp).
    uint32_t editModeBorderColorRGBA = 0xFFFFFF38u;
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
    // Whether deleting a folder or canvas asks first: one that can still be
    // restored (confirmDelete), and one already deleted, which is for good
    // (confirmDeleteForGood). Both on by default. Off, the delete happens
    // on the press, the same as a snippet's always has. Chosen in
    // Settings > Behavior, for everything at once rather than per
    // application: what a delete asks is about the library, not about what
    // the overlay is up over.
    bool confirmDelete = true;
    bool confirmDeleteForGood = true;

    // What a new snippet starts with (Settings > Defaults), by how it was
    // made. Only ever a starting point: each is a property of the snippet
    // from then on, changed in its own popover, and changing a default
    // leaves every snippet already made as it is.
    //
    // A screenshot is its capture, opaque; a drawing is ink on nothing. Both
    // keep their shape when resized - a caption typed into one included,
    // which used to free it, so that the same handle stopped doing the same
    // thing the moment there was text.
    SnippetDefaults screenshotDefaults{true, 1.0f, 1.0f};
    SnippetDefaults drawingDefaults{true, 1.0f, 0.0f};
    // A drawing's background, shown once its opacity is above zero. White,
    // what it has always been; a screenshot's background is its capture,
    // which a color would only tint.
    uint32_t drawingBackgroundColorRGBA = 0xFFFFFFFFu;
    // A new snippet's text, when some is typed into it. The size is 0 until
    // the first frame, which sets it to kDefaultNoteTextSizePx at Windows'
    // scale for the display then - once, so that someone who reads at 150%
    // gets text that size from the start, and so that it is a number in
    // the settings from then on rather than something that changes when
    // the overlay moves to another display. The color's alpha is its
    // opacity, as Item::noteTextColorRGBA's is.
    float noteTextSizePx = 0.0f;
    uint32_t noteTextColorRGBA = 0xFFFFFFFFu;

    // Which display the overlay comes up on, remembered by the id and the
    // name it was listed under (see platform::DisplayInfo for what each is).
    // Empty: the primary display, whichever that is at the time. A chosen
    // display that is not attached is not forgotten - core::ChooseDisplay
    // puts the overlay on the primary meanwhile, and back on the chosen one
    // once it returns. Chosen in Settings > Appearance.
    std::string overlayDisplayId;
    std::string overlayDisplayName;

    // How far the tutorial got with each topic (docs/TUTORIAL.md, section
    // 13.7), by the topic's id: "started", "finished" or "skipped" - and no
    // entry for a topic never started. Any other text - a step's id, which
    // builds before 0.2.3 kept - reads as started. Written by the tutorial
    // as it goes.
    std::map<std::string, std::string> tutorialProgress;

    bool operator==(const AppConfig&) const = default;
};

// The band editModeBorderWidthPx is held to. The low end is 1px (thinner
// than that and AddRect has nothing to draw); the high end is generous
// enough for a deliberately heavy frame while still leaving the screen
// mostly usable.
inline constexpr float kEditModeBorderWidthMin = 1.0f;
inline constexpr float kEditModeBorderWidthMax = 48.0f;

// A new snippet's text size at 100%, before the first frame decides the
// setting (see AppConfig::noteTextSizePx) - a little larger than the
// interface's own 17, since a note is read from further away than a menu.
inline constexpr float kDefaultNoteTextSizePx = 20.0f;

// The band uiScalePercent is held to, when it is not 0 ("follow Windows").
// Windows itself offers 100 to 500; past 300 the Overview no longer fits on
// a 1080p display, and below 75 its text stops being readable.
inline constexpr int kUiScalePercentMin = 75;
inline constexpr int kUiScalePercentMax = 300;

// The band purgeDeletedAfterDays is held to: at least a day, since "delete
// for good at the next start" is what Delete permanently is for, and at
// most ten years, past which the setting is "never" by another name.
inline constexpr int kPurgeDeletedAfterDaysMin = 1;
inline constexpr int kPurgeDeletedAfterDaysMax = 3650;

// Returns hardcoded defaults, matching the values a freshly-written config
// file would contain.
AppConfig DefaultConfig();

// Parses config.json text. Every setting is optional: one that is missing,
// of the wrong type, or out of range keeps its default, and a file that
// isn't valid JSON at all reads as one that said nothing. Nothing in the
// *content* of the text can make this throw - the same contract
// persistence::LibraryStore::Load has, and for the same reason: a
// hand-edited or truncated file should be treated as absent, not crash the
// app on startup. Running out of memory can, as it can anywhere.
AppConfig ParseConfig(std::string_view text);
// What reading a settings file gave, and whether reading changed what the
// file says: it was migrated from an older version, or needed a load
// repair, such as a hotkey unbound for having another's combination
// (docs/SETTINGS.md, sections 5 and 8). Holding one value to its rule does
// not count - a value out of range, a missing key - since the file does not
// say something the app cannot run with.
struct ParsedConfig {
    AppConfig config;
    bool changed = false;
    // The version the file said it was, 1 when it said nothing usable - see
    // kConfigVersion (config_migrations.h).
    int version = 1;
};
// The same as ParseConfig, but nullopt for text that is not a JSON object
// at all - which a settings file that is only missing some keys never is.
std::optional<ParsedConfig> TryParseConfig(std::string_view text);

// The most a settings file is read past. A config.json is a few kilobytes;
// one of megabytes is not a settings file, whatever it is, and is read as
// one that said nothing rather than allocated for.
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
    Newer,       // a settings file a newer build wrote, read as well as this
                 // build can and not to be written over
};
struct LoadedConfig {
    AppConfig config;
    ConfigSource source = ConfigSource::Read;
    std::filesystem::path setAsideAs;
    // A file read, and changed by reading it (ParsedConfig::changed): to be
    // written back at start, so that it says what runs - see
    // TrayController::WriteConfigAtStart.
    bool writeBack = false;
};

// The settings the app starts with. Only a missing file is a first run,
// answered by writing the defaults out for the user to edit. A file that is
// there but is not settings - a hand edit that left a trailing comma - used
// to read as defaults, and the next settings change then saved those over
// it, taking every hotkey and profile with it. It is renamed out of the
// way instead, to config-unreadable-<stamp>.json beside it, and one that
// cannot be read at all is left alone; either way the app starts on the
// defaults, and the caller says so. See main_win32.cpp. A file set aside is
// replaced at once by the defaults with retention off (purgeDeleted), so
// that no later start erases what the unread file may have kept.
//
// A file a newer build wrote is read, as well as this build can, and left
// as it is: written over, it would lose whatever the newer build stored
// that this one does not know, and saying so is the caller's too.
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
// directory first if needed, through a temporary file renamed over it -
// what LoadOrCreateConfig writes and every settings change after it (see
// TrayController::PersistConfig). Returns false (does nothing) if `path`
// is empty - a host with nowhere to persist to - or if the file could not
// be written.
bool WriteConfigFile(const std::filesystem::path& path, const AppConfig& config);

}  // namespace sz::core
