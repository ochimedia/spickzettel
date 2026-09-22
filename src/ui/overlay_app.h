#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/context_menu.h"
#include "ui/icon_draw.h"
#include "core/build_info/build_info.h"
#include "core/canvas/canvas_manager.h"
#include "core/canvas/item.h"
#include "core/config/app_config.h"
#include "core/drawing/draw_tool.h"
#include "core/drawing/stroke.h"
#include "core/drawing/stroke_mesh_cache.h"
#include "core/persistence/library_store.h"
#include "core/session/actions.h"
#include "core/session/settings.h"
#include "core/session/session.h"
#include "platform/i_overlay_window.h"

namespace sz::ui {

// The UI is a view of the core: it speaks in the core's own vocabulary
// (Item, Canvas, Session, ...) throughout, so the core namespace is open
// here rather than every type named twice.
using namespace ::sz::core;

// How far out a panel that hides against an edge of the screen is: 0 all
// the way in, 1 all the way out. It slides out while it is wanted or until
// `holdUntil` (a flash), and back in once it has gone unwanted for
// kEdgeRevealLingerSeconds - long enough that crossing a gap between the
// edge and the panel doesn't send it away. See UpdateEdgePanels.
inline constexpr float kEdgeRevealSlideSeconds = 0.14f;
inline constexpr double kEdgeRevealLingerSeconds = 0.45;
struct EdgeReveal {
    float amount = 0.0f;
    double holdUntil = 0.0;
    double lastWanted = -1.0e9;
    void Update(bool wanted, double now, float deltaSeconds);
    void Flash(double now, double seconds) { holdUntil = std::max(holdUntil, now + seconds); }
};

// One of a selected snippet's eight resize handles, by compass point.
enum class ResizeHandle { NW, NE, SE, SW, N, S, E, W };

// What is under a screen point, as far as the canvas is concerned, decided
// by one walk (see OverlayApp::ResolvePointerTarget). The selection's own
// furniture - its bar and its handles - is drawn over every snippet and so
// is asked first; then the snippets, front to back, so a fronter item's
// body beats a backer item's, which is the whole of the occlusion rule.
struct PointerTarget {
    // The frontmost thing that would take a press at the point: a selection
    // bar button, one of a selected snippet's handles, or an item's body -
    // None for open canvas. Handles and buttons exist only while the
    // selection is live (see OverlayApp::SelectionLive).
    enum class Kind { None, Body, Handle, Button };
    Kind kind = Kind::None;
    ItemId item = 0;                            // whose, for Body and Handle
    ResizeHandle handle = ResizeHandle::NW;     // which, when kind is Handle
    ChromeButton button = ChromeButton::Close;  // which, when kind is Button
    // The frontmost item whose content rect holds the point, handles and
    // bar ignored: which item a stroke started there goes into, and which
    // the hover highlight follows.
    std::optional<ItemId> body;
};

// A move or resize in progress - see OverlayApp::HandleItemGesture. A move
// carries the whole selection; a resize is one snippet's, by one handle or
// from its nearest edge with Alt+right-drag.
struct ItemGesture {
    // The snippet grabbed: the one being resized, or the one the move was
    // started on.
    ItemId item = 0;
    bool resize = false;  // else a move
    // Which button started it: the left for a move and a handle, the right
    // for a resize from the nearest edge (or, never dragged, a right
    // click). Events from the other button are not its business.
    platform::MouseButton button = platform::MouseButton::Left;
    std::optional<ResizeHandle> handle;  // the handle it was grabbed by, when by one
    // The snapshot the whole gesture is computed from - see
    // HandleItemGesture for why a snapshot plus the full delta, not an
    // incremental delta per event. A move snapshots every selected
    // snippet's rect; a resize only `item`'s, as `startRects.front()`.
    float startMouseX = 0.0f;
    float startMouseY = 0.0f;
    struct StartRect {
        ItemId item = 0;
        Rect rect;
    };
    std::vector<StartRect> startRects;
    // A resize of more than one selected snippet at once: the handle
    // drags the selection's box, and every snippet in it is scaled about
    // the point the drag leaves fixed - see
    // OverlayApp::ResizeSelectionAsAGroup. `startBounds` is that box as
    // it was at the press; unused for a move and for a one-snippet
    // resize, which is `startRects.front()`'s own rect.
    bool group = false;
    Rect startBounds;
    // Whether the pointer has travelled far enough since the press for
    // this to be a drag rather than a click: a click selects and moves
    // nothing, so nothing is written until then.
    bool moved = false;
    // Which edge(s) a resize moves - fixed for the gesture's whole lifetime
    // once decided at the press. Unused (all false) for a move.
    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;
};

// A box drawn over the canvas to select by - Shift held, dragged from
// open canvas (see OverlayApp::HandleBoxSelection). It holds the two
// corners the drag has reached, and whether it has travelled far enough
// to be a drag at all: a Shift-press that never moves is a click on open
// canvas, which with Shift held adds nothing and takes nothing away.
struct BoxSelection {
    float fromX = 0.0f;
    float fromY = 0.0f;
    float toX = 0.0f;
    float toY = 0.0f;
    bool moved = false;
    Rect Bounds() const {
        const float x0 = std::min(fromX, toX);
        const float y0 = std::min(fromY, toY);
        return Rect{x0, y0, std::max(fromX, toX) - x0, std::max(fromY, toY) - y0};
    }
};

// What a row of the snippet context menu does - the value a chosen row
// carries back out of ContextMenu::Render (see BuildItemContextMenuRows,
// which names each one, and RunItemMenuAction, which runs it).
//
// The UI's own, unlike ChromeButton next door in core/session/actions.h:
// nothing persists these and no setting names them, so they are free to
// be reordered, renamed and added to as the menu grows.
enum class ItemMenuAction {
    ToggleFullscreen,
    ResetSize,
    ClearDrawing,
    Duplicate,
    SendBackward,
    BringForward,
    MoveToCanvas,
    MoveToNewCanvas,
};

// The same, for the context menu on a canvas bar tile. One row so far;
// see BuildCanvasContextMenuRows.
enum class CanvasMenuAction {
    Delete,
};

// Owns the canvas/item UI and renders it into whatever IOverlayWindow it's
// attached to via Dear ImGui. Contains no OS-specific code: rendering is
// entirely through ImGui's platform-agnostic API.
//
// No always-visible chrome at the *canvas* level: snippets are objects
// with a selection, the way a drawing program's are (see selection_), and
// drawing on one is a mode entered by double-clicking or holding on it
// (see drawingItem_); everything that acts on a snippet is on the bar that
// floats over the selection (see PaintSelectionBar), and everything else
// is a key or the Overview. Input arrives two ways accordingly:
//  - Everything over the canvas - freehand pen/eraser strokes, the "double
//    click or hold for fullscreen, drag for a region" item-creation
//    gesture, and the selection's own gestures (a press on a snippet, on a
//    selected snippet's handle, or on the selection bar - see
//    HandleItemGesture) - is driven by the window's raw MouseCallback
//    (WM_MOUSEMOVE-driven, decoupled from render/frame rate). What a press
//    lands on is ResolvePointerTarget's answer: the app's own walk over
//    the selection's furniture and then the items, asked of the event's
//    own position. ImGui never hit-tests an item, a handle or the bar, so
//    nothing about them is a frame late (see docs/ARCHITECTURE.md,
//    "Selection").
//  - Popovers, the canvas bar, the dock and the Overview are ordinary
//    bounded ImGui windows/widgets, and every one of them sits above every
//    item. `io.WantCaptureMouse` means one of them is under the pointer
//    (or holds a press, or a popup is open), and gates all of the first
//    group: a click landing on a widget doesn't also start a stroke, a
//    gesture or a placement underneath it.
class OverlayApp {
public:
    // A view of `session`, editing it and the `settings` - both outlive it;
    // TrayController owns all three. Nothing is copied out: every frame reads
    // the models and the settings as they are.
    OverlayApp(Settings& settings, Session& session);

    // Subscribes to the window's frame/mouse callbacks. The window is not
    // required to exist yet in the OS sense; callbacks simply won't fire
    // until the platform layer creates and shows it.
    void AttachTo(platform::IOverlayWindow& window);

    // Switches between the full interactive editor and a read-only display
    // of the current canvas (no selection, no drawing, no Overview) -
    // driven by TrayController's edit/view-mode hotkeys, see
    // its own doc comment for the state table this participates in.
    // Entering view-only mode (true) disarms/closes anything mid-flight
    // (an armed item, an open popover/Overview/picker) so edit mode
    // resumes from a clean state later rather than wherever it was left
    // off. A no-op if already in the requested mode.
    void SetViewOnly(bool viewOnly);

    // The overlay has just been put back on screen. Anything this class
    // remembers about state the OS owns is stale at that moment - which today
    // is only the installed pointer shape, since while the overlay was hidden
    // the application underneath owned the cursor. Distinct from SetViewOnly,
    // which no-ops when the mode is unchanged and so never fires on a plain
    // hide-then-show.
    void OnOverlayShown();
    bool IsViewOnly() const { return viewOnly_; }
    // A notice: view-only mode with the canvas left out, so the only thing
    // on screen is whatever ShowActionToast last put there. What a hotkey
    // that acts while the overlay is hidden shows for a moment instead of
    // opening the whole overlay - see TrayController::ShowNotice, which
    // pairs it with a click-through, never-focused window.
    //
    // Set alongside view-only, never instead of it: a notice must not take
    // input either, and everything SetViewOnly stands down is equally
    // unwanted here. Cleared when any real mode is entered, so a hotkey
    // pressed while a notice is up leaves the overlay in the mode that
    // hotkey asked for rather than hiding it two seconds later.
    void SetNoticeOnly(bool noticeOnly) {
        noticeOnly_ = noticeOnly;
        noticeFinishedReported_ = false;
    }
    bool IsNoticeOnly() const { return noticeOnly_; }
    // The pinned view: view-only mode drawing only the current canvas's
    // pinned snippets (Item::pinned) - what the overlay leaves on screen
    // when it is put away with any of them there. See
    // TrayController::ShowPinnedView. Like a notice, only ever set
    // alongside view-only, and cleared when view-only is.
    void SetPinnedOnly(bool pinnedOnly) { pinnedOnly_ = pinnedOnly; }
    bool IsPinnedOnly() const { return pinnedOnly_; }
    // Called once, from the frame in which a notice's message has faded,
    // to say there is nothing left to show. TrayController hides the
    // window from it - safe mid-frame, since the renderer still ends the
    // ImGui frame afterwards.
    void SetNoticeFinishedCallback(std::function<void()> callback) {
        noticeFinishedCallback_ = std::move(callback);
    }
    // Drops the current message without showing it, for the caller that
    // asked for something and then decided nothing may appear on screen
    // (see AppConfig::showToastsWhileHidden). Worth doing rather than
    // leaving it to expire: the clock a message expires on only runs while
    // frames do, so one set while hidden would otherwise still be waiting,
    // hours later, for whenever the overlay next comes up.
    void DismissActionToast() { actionToastText_.clear(); }
    // What that message currently says, empty for none - the readable half
    // of the pair above, and how a test asks whether something was said at
    // all rather than looking at pixels.
    const std::string& ActionToastText() const { return actionToastText_; }

    // ===== When the disk says no =====
    //
    // What is on screen looks saved whether or not it is, so a save that
    // failed is said out loud, and kept on screen for as long as it stays
    // failed: a line along the bottom naming the library (see
    // RenderPersistenceWarning), rather than a toast that fades while the
    // problem does not. The same line carries a settings file that could
    // not be written, which the tray reports here.
    void SetConfigWriteFailed(std::optional<std::string> path) { configWriteFailedPath_ = std::move(path); }
    // The warning as it would be drawn this frame, or empty when there is
    // nothing wrong - for a test, and for anything else that has to know.
    std::string PersistenceWarning() const;
    // What the overlay is currently set to do, for anything that needs to
    // ask rather than watch: the tool a stroke would use, whether a create
    // action is armed and waiting for a click, and whether the Overview is
    // up. Read-only - each of these is changed through the action that owns
    // it (SetTool, the creation gesture, OpenOverview) so there is one path in.
    //
    // Added for the headless UI tests, which had no way to see any of it
    // and had to be run by hand against screenshots instead.
    Tool ActiveTool() const { return activeTool_; }
    // The colour the next stroke would use. Public for the same reason
    // ActiveTool is: it is what a test asks instead of reading pixels.
    uint32_t DrawColorRGBA() const { return drawColorRGBA_; }
    // What the next left press places, if anything: the creation gesture in
    // flight, or else what the tool in hand places (see CreationKindFor).
    std::optional<ItemCreationKind> ArmedCreation() const {
        return creation_.has_value() ? std::optional<ItemCreationKind>(creation_->kind)
                                     : CreationKindFor(activeTool_);
    }
    bool IsOverviewOpen() const { return overviewOpen_; }
    // Whether the colour chooser is up - see RenderColorChooser.
    bool IsColorChooserOpen() const { return colorChooserOpen_; }
    // Whether the snippet context menu is up, and over which snippet -
    // see RenderItemContextMenu.
    bool IsItemContextMenuOpen() const { return itemContextMenu_.IsOpen(); }
    std::optional<ItemId> ItemContextMenuItem() const { return itemContextMenuItemId_; }
    // The same for the canvas bar's tiles - see RenderCanvasContextMenu.
    bool IsCanvasContextMenuOpen() const { return canvasContextMenu_.IsOpen(); }
    std::optional<CanvasId> CanvasContextMenuCanvas() const { return canvasContextMenuCanvasId_; }
    // How far out the canvas bar is, 0 to 1 - see EdgeReveal and
    // UpdateEdgePanels.
    float CanvasBarReveal() const { return canvasBarReveal_.amount; }
    // Whether a note is being typed into, and which.
    std::optional<ItemId> EditingNote() const { return editingNoteItemId_; }
    // Which resize handle of which item thinks the cursor is on it right
    // now, empty for none - the same string the debug overlay draws (see
    // debugHoveredResizeHandle_). Exposed because "whose handle is live
    // where two items overlap" is a question about occlusion that a
    // screenshot answers badly and a test answers exactly.
    const std::string& DebugHoveredResizeHandle() const { return debugHoveredResizeHandle_; }
    // The selected snippets, in the order they were selected - see
    // selection_.
    const std::vector<ItemId>& Selection() const { return selection_; }
    bool IsSelected(ItemId id) const;
    // The snippet in drawing mode, if one is - see drawingItem_.
    std::optional<ItemId> DrawingItem() const { return drawingItem_; }
    // What the drawing bar's pen and eraser draw or erase on a plain drag -
    // see penShape_ and eraserShape_.
    DrawShape PenShape() const { return penShape_; }
    DrawShape EraserShape() const { return eraserShape_; }
    // Where the middle of a selection bar button is this frame, or nothing
    // while the bar is not showing - for a test to press it where it is
    // rather than where it computes it to be.
    std::optional<ImVec2> SelectionBarButtonCenter(ChromeButton button) const;

    // Captures a fullscreen screenshot onto a canvas made for it (see the
    // definition, and the other declaration of this below).
    // Doesn't touch the overlay's visibility or mode itself - it's purely
    // the capture step; TrayController::OnQuickCaptureHotkey switches to
    // edit mode afterward so the capture is noticed. Safe to call while
    // hidden: needs no active ImGui frame (it's plain CanvasManager/
    // IOverlayWindow calls), and `window`'s own CaptureRegionAsTexture
    // already hides/reshows itself around the OS-level capture regardless
    // of visibility.
    // A fullscreen screenshot onto a canvas of its own: a new one at the
    // end of the folder the current canvas lives in, switched to, with the
    // shot on it and a message saying so. Both capture hotkeys come here -
    // the difference between them is only whether the overlay is shown
    // afterwards (see TrayController::OnQuickCaptureHotkey and
    // OnSilentCaptureHotkey).
    //
    // A canvas each rather than all of them piling onto whatever canvas
    // was current: every capture is the same size and shape, so stacked
    // they hide each other, while one per canvas is a row of tiles in the
    // Overview that can be told apart at a glance. The user's own call.
    void QuickCapture(float displayW, float displayH);

    // Brings everything the hand is in the middle of into the model, so
    // that a save taken right after it has all of it: the gesture under a
    // held button ends where the pointer is (a stroke is committed, a drag
    // lands), and a note being typed is committed to its item. What every
    // canvas switch settles first (see SwitchToCanvasSettled), done for the
    // saves that happen with no frame to follow - hiding, restarting,
    // exiting, the OS ending the session. A flush that ran without this
    // wrote the note as it was when the editor opened, and typing that had
    // been visible for a minute was gone at the next start.
    void SettleForPersistence();
    // The settling itself: the gesture under a held button ends where the
    // pointer is, and a note being typed is committed. Every canvas switch
    // does this first - the hand's work belongs to the canvas it started
    // on, and a gesture carried across a switch would go on editing a
    // snippet nobody can see, then file its undo entry under the canvas
    // that is current when it ends. Needs a live ImGui context to ask
    // whether the button is down; with none there is nothing in flight.
    void SettleHand();

    // Asks for the first-run welcome note to be placed on the current
    // canvas. Called by TrayController when there was no library on disk to
    // load - i.e. a genuinely fresh install, not merely an empty library
    // someone deliberately cleared out (that one loads fine and says so).
    //
    // Deferred rather than done immediately: the note is centered on the
    // display, and the display size isn't known until ImGui has run a
    // frame. OnFrame places it on the first frame that has one.
    void RequestWelcomeNote() { welcomeNotePending_ = true; }

    // Asks the host to hide the overlay and show it again. Installed by
    // TrayController alongside the callbacks above, and used by the input
    // options HUD: several of the options it toggles only take effect on
    // entry to edit mode (freezing captures there; no-activate is decided
    // by how the window is shown), so a toggle that didn't re-enter would
    // appear to do nothing.
    void SetRestartOverlayCallback(std::function<void()> callback) {
        restartOverlayCallback_ = std::move(callback);
    }

    // Where the monitor list in Settings > Appearance comes from: the
    // host's displays, by way of TrayController. Left null - a test that
    // doesn't care - the list offers only the primary display.
    void SetDisplayListCallback(std::function<std::vector<platform::DisplayInfo>()> callback) {
        displayListCallback_ = std::move(callback);
    }

    // One line of the library tree HUD: a file or directory, and how deep
    // under the library root it sits. Public so a test can check what the
    // HUD would show against what it put on disk.
    struct LibraryTreeLine {
        int depth = 0;
        std::string name;
        bool isDirectory = false;
    };
    // What the HUD showed on the last frame it was drawn - the tree as it
    // was on disk after the store's most recent write.
    const std::vector<LibraryTreeLine>& LibraryTreeHudLines() const { return libraryTreeLines_; }


    // Installed once by TrayController - called from the Settings panel's
    // hotkey editor whenever the user picks a new combo for one of the
    // global hotkeys. Unlike every other setting, a hotkey has a real way to
    // fail that only the OS can tell you about (the combo's already taken
    // by another app) - so, unlike Settings::Commit's "tell the host after
    // the fact" shape, this is "ask the host first": the callback itself
    // attempts the actual RegisterGlobalHotkey/UnregisterGlobalHotkey swap,
    // stores the combo in the settings and persists them on success, and
    // returns whether it took - see TryChangeHotkey, the Settings panel's
    // only caller of this. Left null (e.g. a test), every requested change
    // is written into the settings unconditionally - there's no real OS
    // hotkey to fail against.
    void SetHotkeyChangeCallback(std::function<bool(HotkeySlot, platform::KeyCombo)> callback) {
        hotkeyChangeCallback_ = std::move(callback);
    }


private:
    void OnFrame(float deltaSeconds);
    void OnMouse(const platform::MouseEvent& event);
    // Ends whatever left-button gesture is in progress - a stroke, an
    // erase, a shape, a placement, a move - exactly as releasing the
    // button where the pointer is now would. For the moments the canvas
    // changes under a gesture; see the definition.
    void FinishLeftButtonGesture();

    void RenderCanvasLayer(float displayW, float displayH);  // live layer + armed-item overlay + debug text
    // The layers that sit over the canvas and under the Overview: the
    // input options HUD, the edit-mode border, the demo mark. See its
    // definition for the order and why each is where it is.
    void RenderScreenChrome(float displayW, float displayH);
    // See AppConfig::showEditModeBorder. Called from RenderScreenChrome -
    // edit mode is the whole point of it.
    void DrawEditModeBorder(ImDrawList* drawList, float displayW, float displayH) const;
    // The demo build's permanent "Spickzettel / Demo Version" mark (see
    // build::kDemoMode). Called from both RenderCanvasLayer and
    // RenderViewOnly - it belongs wherever the overlay is visible. Not
    // const: it moves itself around the screen on a timer (see its
    // definition), and the three members below are where it stands.
    void DrawDemoWatermark(ImDrawList* drawList, float displayW, float displayH);
    // Which pointer the canvas itself asks for, from what is under the mouse
    // and what the tools would do there. Says nothing about what ImGui wants
    // over its own windows and widgets; ApplyPointerShape settles that.
    platform::CursorShape WantedPointerShape() const;
    // Hands WantedPointerShape() to the platform window, or Default when
    // something else owns the cursor this frame. Called once per frame, after
    // the UI has been submitted (so ImGui's own cursor request for this frame
    // is known) and before DrawSoftwareCursor. Only actually pushes when the
    // answer has moved - see its definition for what "moved" has to mean.
    void ApplyPointerShape();
    // What the last push asked for. nullopt means "nothing known" - a fresh
    // start, or the overlay having just been shown (see OnOverlayShown).
    std::optional<platform::CursorShape> appliedPointerShape_;
    // Two frames of ImGui's wanted cursor, not one, and the pointer position.
    // See ApplyPointerShape for why the history has to be two deep - a
    // one-frame view misses the frame ImGui's backend actually installs on.
    ImGuiMouseCursor lastImGuiCursor_ = ImGuiMouseCursor_Arrow;
    ImGuiMouseCursor previousImGuiCursor_ = ImGuiMouseCursor_Arrow;
    float lastPointerX_ = 0.0f;
    float lastPointerY_ = 0.0f;
    // The overlay's own pointer, drawn while the mouse grab is taking input
    // away from a game - which also means the OS cursor is hidden and left
    // wherever that game is holding it. See the definition.
    void DrawSoftwareCursor() const;
    // A debugging aid: the state of every capture/input option, top-left,
    // with a number key per row that toggles it. These options interact in
    // ways that are only discoverable by trying combinations, and reaching
    // the Overview to change one is several clicks away from the situation
    // being tested. See the definition for what each row is.
    // Draws into whichever layer the caller hands it - see
    // RenderScreenChrome, which is what decides how high that layer sits.
    void DrawInputOptionsHud(ImDrawList* drawList) const;
    // The library directory as it is on disk, top-right, read-only - see
    // AppConfig::showLibraryTreeHud. Walks the real filesystem rather than
    // mirroring the model, since disagreement between the two is the whole
    // thing it exists to show, and walks it again only after the store has
    // written something (see LibraryStore::WriteGeneration).
    void DrawLibraryTreeHud(ImDrawList* drawList, float displayW, float displayH);
    void RefreshLibraryTreeIfChanged();
    // Handles the number keys the HUD advertises. Toggles the option,
    // persists it, and re-enters edit mode so options that only apply on
    // entry actually take hold.
    void HandleInputOptionsHudKeys();
    // The option a HUD row addresses, or nullptr for an out-of-range index.
    // Which setting a HUD row addresses, and what it currently reads - see
    // the definitions. The value is the resolved one: the HUD reports what
    // is running.
    static ProfileableField InputOptionField(int index);
    bool InputOptionValue(int index) const;
    bool InputOptionAvailable(int index) const;
    // Every item on the current canvas, back to front, into one layer,
    // then the selection's outline, handles and bar over all of them, plus
    // the note editor and the dock above it. Decides once, from
    // ResolvePointerTarget, what the pointer is over and so what lights up
    // and which shape it wears; see the definition.
    void RenderItems(float displayW, float displayH);
    // An item's content, its in-progress stroke (`drawing`: it is the
    // snippet in drawing mode, whose stroke in flight is on the live layer)
    // and its border, into the items layer's draw list - see the definition.
    void PaintItemBody(ImDrawList* drawList, const Item& item, const Canvas& canvas, bool drawing, bool highlighted,
                       bool isFrontmost);
    // What a selected snippet wears while the selection is live, *drawn*:
    // an accent outline and, unless it is fullscreen, its eight handles.
    // Nothing in it takes input. Which of it is under the pointer is
    // ResolvePointerTarget's answer, what the pointer looks like over it
    // is RenderItems', and a press on any of it is HandleItemGesture's -
    // so nothing over the canvas is hit-tested by ImGui at all - see
    // docs/ARCHITECTURE.md, "Selection", for why that rule is absolute.
    // `drawing`: the snippet is in drawing mode, and wears the stronger
    // outline that says so.
    void PaintSelectionOutline(ImDrawList* drawList, const Item& item, bool drawing);
    // The selection bar: its buttons on a small pill floating over
    // the selection's bounding box (see SelectionBarLayout in the
    // definition file for where exactly). `hotButton` is the button the
    // pointer is over and allowed to light up this frame, if any - see
    // RenderItems for what "allowed" means while something is held.
    void PaintSelectionBar(ImDrawList* drawList, const std::optional<ChromeButton>& hotButton);
    // What a selection bar button does, on the release that completes a
    // press on it (see HandleItemGesture).
    void ActivateBarButton(ChromeButton button);
    // Which edges a resize handle moves - a corner two, an edge one.
    static void ResizeHandleEdges(ResizeHandle handle, bool& left, bool& right, bool& top, bool& bottom);
    // ----- The selection -----
    // Whether the selection's furniture - the bar, the handles - is on
    // screen and takes presses this frame: no creation tool in hand.
    // Selecting is the hand at rest, so this is almost
    // always true; in drawing mode the bar shows the drawing buttons
    // instead, and a press on the snippet's body draws rather than moves
    // (see PressPicksUp).
    bool SelectionLive() const;
    // Whether a left press on a snippet's body picks it up - selects and
    // moves it - rather than drawing on it: not in drawing mode, or Alt
    // held, which is the way to move the snippet being drawn on without
    // leaving drawing mode.
    bool PressPicksUp() const;
    // ----- Drawing mode -----
    // Puts `id` into drawing mode: selected alone, outlined for it, its bar
    // showing Pen/Eraser/Text and the colour, and `tool` (the pen, if none
    // is given) in hand, so a left press on it draws. See drawingItem_.
    void EnterDrawingMode(ItemId id, std::optional<Tool> tool = std::nullopt);
    // Back to the hand at rest: no snippet in drawing mode, Select in hand.
    // The selection is left as it was.
    void ExitDrawingMode();
    // What picking a tool from the drawing bar or a key does - the one
    // place a tool is chosen, above SetTool, which only sets it. Select
    // leaves drawing mode; a marking tool switches the tool in drawing
    // mode, or enters it on the snippet selected last, and does nothing
    // with no snippet to draw on; a creation tool leaves drawing mode and
    // is picked up.
    void PickTool(Tool tool);
    // The buttons the bar shows this frame: the item buttons, or the
    // drawing buttons in drawing mode.
    std::vector<ChromeButton> BarButtons() const;
    void SelectOnly(ItemId id);
    void ToggleSelected(ItemId id);
    void ClearSelection();
    // Drops whatever is no longer on the current canvas, or is deleted or
    // minimized there - run at the top of every frame, so nothing acts on
    // a snippet that has gone.
    void PruneSelection();
    // The snippet selected last, which the single-snippet actions (More,
    // Maximize) take. Nothing when nothing is selected.
    std::optional<ItemId> PrimarySelection() const;
    // The selected snippets' union rect, for the bar to float over.
    std::optional<Rect> SelectionBounds() const;
    // Deletes every selected snippet, undoably, as Close does.
    void DeleteSelection();
    // What a resize started on `item` carries: that snippet alone, or -
    // when it is one of several selected - every selected snippet and
    // the box around them, which the drag then scales.
    void SnapshotResizeTargets(ItemGesture& gesture, ItemId item);
    // ----- The clipboard -----
    // Copy and Cut put the selection on the clipboard - as ids, so
    // nothing is taken away or duplicated until a paste. Paste puts what
    // is on it onto the canvas being looked at, skipping whatever has
    // gone since.
    void CopySelectionToClipboard(bool cut);
    void PasteFromClipboard();
    // The two of those in one step, without going through the clipboard -
    // see its definition.
    void DuplicateSelection();
    // Whether this snippet is waiting for the paste that will move it,
    // which is what it is drawn faded for.
    bool IsWaitingToBeCut(ItemId id) const;
    // Every snippet the box touches, added to the selection - see
    // HandleBoxSelection for why it adds rather than replaces. A snippet
    // is caught by any overlap at all, however slight.
    void AddTouchedToSelection(const Rect& box);
    // A resize carrying the whole selection: every snippet in it scaled
    // by one factor about the corner or edge the drag leaves fixed, so
    // the group keeps its shape and its spacing. `dx`/`dy` are the whole
    // drag, from the press to now, as every other resize is computed.
    void ResizeSelectionAsAGroup(float dx, float dy);
    // Moves every selected snippet by (dx, dy), clamped on screen - the
    // arrow keys.
    void NudgeSelection(float dx, float dy);
    // Delete, Escape and the arrow keys, from OnFrame: what the keyboard
    // does to the selection.
    void HandleSelectionKeys();
    // The live text editor over an item whose note is being edited - the
    // one piece of an item that is a real ImGui widget, in a window of its
    // own above the items layer. See the definition.
    void RenderNoteEditor(Item& item, ImVec2 pMin, ImVec2 pMax);
    // Small chips along the bottom of the screen, one per minimized item
    // on the current canvas (see Item::minimized) - click one to restore
    // it (unset minimized, bring to front). A no-op (renders nothing) if
    // nothing's minimized.
    void RenderDock(float displayW, float displayH);
    // view-only mode's entire render path: the current canvas's items,
    // read-only, at their real screen positions - no selection, no
    // drag-resize, no Overview. See DrawItemContent (in the .cpp)
    // for the fill/stroke drawing shared with RenderItems.
    void RenderViewOnly(float displayW, float displayH);
    // Where the canvas bar is this frame and how far out, from whether it
    // is wanted - the pointer at the bottom edge or on the bar -
    // and from what has happened (the overlay coming up, the canvas
    // changing). Once per frame, before anything is drawn; see
    // overlay_app_docks.cpp.
    void UpdateEdgePanels(float displayW, float displayH);
    // The canvas bar along the bottom edge: the canvases of the folder the
    // current canvas is in, as thumbnails, the current one outlined, and a
    // button for a new one. Drawn where UpdateEdgePanels put it.
    void RenderCanvasBar(float displayW, float displayH);
    // Those canvases, in the folder's order.
    std::vector<CanvasId> CanvasBarCanvases() const;
    // Switches canvas, first ending whatever the hand is doing and
    // committing a note being typed - what every way of switching from the
    // canvas itself has to do.
    void SwitchToCanvasSettled(CanvasId id);
    // The context menu a right-click on a canvas bar tile opens. Rendered
    // at the top level of the frame rather than inside the bar's own
    // window, like every other popup here; the bar is held out for as long
    // as the menu is up (see UpdateEdgePanels), since a menu floating over
    // the panel it belongs to having slid away would be a puzzle.
    void OpenCanvasContextMenu(CanvasId canvasId, ImVec2 at);
    void RenderCanvasContextMenu();
    void BuildCanvasContextMenuRows(const Canvas& canvas, std::vector<ContextMenuEntry>& rows) const;
    void RunCanvasMenuAction(CanvasMenuAction action, CanvasId canvasId);
    // The colour chooser: one picker, and what it is set to is the colour
    // drawn with. Opened by the drawing bar's colour button, next to
    // `from`, the point it was pressed from; OpenColorChooser only asks,
    // since the bar's buttons fire from the raw mouse pipeline outside
    // any frame, and RenderColorChooser opens it on the next one. What
    // was picked is kept as AppConfig::strokeColorRGBA when the chooser
    // closes.
    void OpenColorChooser(ImVec2 from);
    void RenderColorChooser(float displayW, float displayH);
    // Beside the pointer while Draw or Erase is in hand over a snippet and
    // a modifier changes what a press would make - a line or a rectangle -
    // a small glyph of it, so the modifiers are not a secret.
    void RenderToolModifierBadge();

    // Foreground/background opacity sliders, background color, fullscreen,
    // copy, z-order, and move-to-canvas, for the item the selection bar's
    // "More" button opened it on.
    void RenderItemPropertiesPopover();
    // Its sections, top to bottom, for the item it is open on: the two
    // opacities, the background colour, the text's size and colour, and
    // the row of actions.
    void RenderItemOpacity(Item& item);
    void RenderItemBackgroundColour(Layer& picture);
    void RenderItemTextStyle(Item& item);
    void RenderItemActions(Item& item);

    // The context menu a right-click on a snippet opens - the popover's
    // actions as a list of named rows with their shortcuts beside them,
    // plus the ones that only had a key until now. Asked for from the raw
    // mouse callback, which is why opening is a request rather than a call
    // (see ContextMenu::RequestOpenAt).
    void OpenItemContextMenu(ItemId itemId, ImVec2 at);
    void RenderItemContextMenu();
    // The rows, for the snippet the menu is open over. Rebuilt every frame
    // it is up, so "nothing to clear" and "nothing behind it" are answered
    // from the canvas as it is now rather than as it was when the menu
    // opened.
    void BuildItemContextMenuRows(Item& item, std::vector<ContextMenuEntry>& rows);
    void RunItemMenuAction(ItemMenuAction action, ItemId itemId);
    // "Ctrl+D" for a bound action, empty for an unbound one - what a menu
    // row shows on its right. FormatKeyComboLabel's "(none)" is the right
    // answer for a key editor and the wrong one here, where an action
    // without a shortcut should simply show nothing.
    std::string MenuShortcutLabel(ShortcutAction action) const;

    void RenderRegionCaptureOverlay();
    // RectEraser's own drag-preview overlay (see rectErase_) -
    // same visual language as RenderRegionCaptureOverlay (a translucent
    // fill plus an outline and live dimensions), kept as its own function
    // rather than sharing that one since the two track independent state
    // and are never active at the same time for unrelated reasons (one's
    // gated on the creation gesture, the other on the stroke gesture).
    void RenderRectEraserOverlay();
    // The mouse wheel's own feedback: a dot the exact size the active tool
    // will draw (or erase) at, under the cursor, plus the number. See
    // sizePreviewExpireAtSeconds_ for why it's transient.
    void RenderBrushSizePreview();
    // Steps `delta` canvases along the folder the *current canvas* lives
    // in (negative = toward the front of the list, which is where the
    // newest canvas sits and where the Overview draws leftmost), switches
    // to whatever lands there, and reports the new position. Deliberately
    // does not wrap: running off either end stops there rather than
    // teleporting to the far side, so holding the gesture is a safe way to
    // reach the first or last canvas. Still shows the position even when
    // it didn't move - see its definition.
    void SwitchCanvasByOffset(int delta);
    // The active tool's own size in px, or nullopt for the two tools that
    // don't have one (RectEraser drags out its own region, Text isn't
    // stroke-based at all) - the same split the wheel handler applies, in
    // one place both it and the preview read.
    std::optional<float> ActiveToolSizePx() const;
    // The Overview, in the order it is drawn: Escape first, which closes
    // the innermost open thing and ends the frame's Overview when it did;
    // the dimming backdrop, which closes the Overview on a click outside
    // the panel; the header, which is the picker's prompt while a snippet
    // is being sent somewhere and the tabs otherwise; the body - the
    // folder sidebar and the canvas grid (with what is deleted in them, see
    // ShowingDeleted), or the Settings or About panel; and the footer, whose buttons belong to
    // whichever body is showing.
    void RenderOverview(float displayW, float displayH);
    bool HandleOverviewEscape();
    void RenderOverviewBackdrop(float displayW, float displayH);
    void RenderOverviewHeader();
    // What the sidebar and the grid ask for, applied once both have
    // finished reading the folders and canvases rather than mutating the
    // manager mid-loop - see ApplyOverviewActions. Deleting is not
    // deferred this way: a Delete button opens the confirm popover (see
    // RenderConfirmDeletePopover), which does the deleting on a later
    // frame once the user confirms.
    struct OverviewActions {
        std::optional<CanvasId> clickedCanvas;
        std::optional<FolderId> switchToFolder;
        std::optional<std::pair<FolderId, size_t>> folderReorder;  // folder, new place in Folders()
        std::optional<std::pair<CanvasId, size_t>> canvasReorder;  // canvas, new place in its folder
        std::optional<std::pair<CanvasId, FolderId>> canvasToFolder;
        // A deleted folder picked in the sidebar with Show deleted on - see
        // deletedFolderShown_.
        std::optional<FolderId> showDeletedFolder;
        // A folder's or canvas's Restore - see CanvasManager::Restore.
        std::optional<uint64_t> restore;
    };
    void RenderFolderSidebar(OverviewActions& actions);
    void RenderCanvasGrid(float displayW, float displayH, OverviewActions& actions);
    void RenderOverviewFooter(bool showCanvasesBody);
    // Whether the Canvases tab shows what is deleted - showDeleted_, never
    // while picking where a snippet goes, which is a place among the live
    // ones.
    bool ShowingDeleted() const { return showDeleted_ && !pickerItemId_.has_value(); }
    // The folder the sidebar marks as open and the grid shows: the one
    // being browsed, or a deleted one picked with Show deleted on.
    FolderId OverviewFolderId() const;
    // Lets go of deletedFolderShown_ once it is no longer something to
    // show: Show deleted is off, the folder is gone, or it has been
    // restored - in which case it is browsed as any live folder is.
    void SettleDeletedFolderShown();
    // The sidebar's width: wider with Show deleted on, where a row can
    // carry two buttons rather than one.
    float OverviewSidebarWidth() const;
    void ApplyOverviewActions(const OverviewActions& actions);
    // The picker's one outcome: the snippet it was opened for goes to
    // `target` - moved or copied, as the picker was opened - and the
    // picker closes, leaving the Overview open. Nothing moves when the
    // target is the canvas the snippet is already on.
    void SendPickedItemTo(CanvasId target);
    // The Overview's "Settings" tab body (see overviewTab_'s own doc
    // comment) - checkboxes/radio/slider bound directly to the settings'
    // own fields (see Cfg), so a widget's current value and the app's own
    // live behavior can never disagree. Commits the settings once per
    // completed edit (not every frame a slider is merely being dragged),
    // which is what persists the change to disk.
    void RenderOverviewSettingsPanel();
    // The Settings tab's own bodies, one per section in its list - see
    // RenderOverviewSettingsPanel for what decides which settings live
    // where. `anyChanged` is the shared "something was edited, persist it"
    // flag; the shortcuts section has none because its rows persist
    // themselves through SetToolShortcut.
    void RenderSettingsAppearance(bool& anyChanged);
    void RenderSettingsInteraction(bool& anyChanged);
    // One bar's buttons as a row to arrange: each is a tile that switches
    // it on or off when clicked and can be dragged onto another to move it
    // there. True when the row changed anything.
    bool RenderBarButtonRow(const char* id, const char* label, BarButtonList& buttons);
    void RenderSettingsInput(bool& anyChanged);
    void RenderSettingsDebug(bool& anyChanged);
    // The list of profiles, and what the overlay is up over. Edits are
    // collected into a copy and handed to Settings::SetProfiles once, at
    // the end - the codebase's usual
    // don't-mutate-while-rendering-from-it rule, and here it also keeps the
    // callback from re-entering the loop it was called from.
    void RenderSettingsProfiles();
    // Its two parts: the buttons that make a profile - for what the
    // overlay is up over, or a blank one - and one profile's row, closed
    // or open. Both edit what they are handed in place and say whether
    // they did; a row asks to be removed through `remove` rather than
    // erasing from the list it is being drawn from.
    bool RenderProfileMakers(std::vector<Profile>& edited);
    bool RenderProfileRow(size_t index, Profile& profile, bool& remove);
    // The picker at the top of the two overridable sections: whose values
    // are on screen - the defaults, the profile that matched, or any other
    // one - plus what it inherits and how much of it is set here.
    void RenderEditTargetPicker(ProfileGroup group);
    // One overridable checkbox: the effective value, an accent label and a
    // revert arrow when this target states it for itself, and nothing but
    // the value when it inherits. `help` is the row's explanation, reached
    // through the "?" beside it rather than printed underneath (see
    // HelpMarker). `disabled` greys the control without touching the
    // override state, for the rows whose preconditions aren't met - such a
    // row draws a dash instead of a tick when it is switched on, since its
    // stored value is not in effect.
    void ProfileableCheckbox(const char* id, const char* label, const ProfileableField& field, const char* help,
                              bool disabled = false);
    // The Settings calls, aimed at whichever of the defaults or a profile
    // the panel is showing (editProfile_) - which is not necessarily the
    // profile in effect. What the section being edited currently resolves
    // to; what it would resolve to with this target's own overrides taken
    // away is Settings::Base and needs no call: a profile inherits the
    // defaults and nothing else.
    ProfileableSettings EditedSettings() const { return settings_.ResolvedFor(editProfile_); }
    // True when the target being edited states this field for itself.
    bool IsOverriddenHere(const ProfileableField& field) const { return settings_.IsOverridden(editProfile_, field); }
    // Writes one field into whatever is being edited, then re-derives the
    // live values and persists.
    void SetProfileableValue(const ProfileableField& field, bool value) {
        settings_.SetProfileable(editProfile_, field, value);
    }
    void ClearProfileableOverride(const ProfileableField& field) { settings_.ClearOverride(editProfile_, field); }
    // The same two, for a shortcut binding.
    void SetEditedShortcut(ShortcutAction action, platform::KeyCombo combo) {
        settings_.SetShortcut(editProfile_, action, combo);
    }
    void ClearShortcutOverride(ShortcutAction action) { settings_.ClearShortcutOverride(editProfile_, action); }
    bool IsShortcutOverriddenHere(ShortcutAction action) const {
        return settings_.IsShortcutOverridden(editProfile_, action);
    }
    // The Overview's third tab: which build this is (build::VersionLine)
    // plus ABOUT.md, compiled in so it travels with the binary rather than
    // living next to it as a file that can go missing - see
    // build::AboutText.
    void RenderOverviewAboutPanel();
    // One hotkey's own press-to-capture editor row, called three times (by
    // RenderOverviewSettingsPanel) for the edit/view/quick-capture hotkeys.
    // A button showing the current combo; clicking it arms hotkeyCaptureSlot_
    // for this slot (clicking the armed button again cancels), and the very
    // next real key press becomes the new combo, whatever modifiers happen
    // to be held at that moment - including none at all, and including a
    // function key
    // (see platform::KeyCombo's own doc comment). "Press the combo you
    // want" rather than checkboxes plus a letter picker, which could not
    // represent a function key at all. See TryChangeHotkey for how a
    // captured combo takes effect.
    void RenderHotkeyEditor(const char* id, const char* label, HotkeySlot slot, platform::KeyCombo current);
    // Offers `combo` to hotkeyChangeCallback_ (see its own doc comment)
    // before it is stored as the matching hotkey in the settings. Returns
    // false (and leaves that hotkey untouched) if the callback rejects it -
    // a real OS-level conflict - so RenderHotkeyEditor's widgets can show
    // the edit didn't take rather than silently keeping a value nothing
    // downstream actually agreed to. A collision with one of this app's
    // own other hotkeys is not a rejection: that one is unbound instead
    // (see TrayController::ChangeHotkey).
    bool TryChangeHotkey(HotkeySlot slot, platform::KeyCombo combo);

public:
    // Whether a hotkey row is armed, waiting for the combo the user wants
    // (see hotkeyCaptureSlot_). Asked by TrayController when a registered
    // hotkey fires: a combo one of the app's own hotkeys already has never
    // reaches the capture loop as a key press - Windows hands a registered
    // combination to the hotkey and to nothing else - so the hotkey firing
    // *is* the press, and completes the capture rather than doing what it
    // usually does.
    bool IsCapturingHotkey() const { return hotkeyCaptureSlot_.has_value(); }
    // Ends the capture with `combo` as the answer - the key the loop saw,
    // or the combo of the hotkey that fired. Nothing while none is armed.
    void CompleteHotkeyCapture(platform::KeyCombo combo);
    // What clicking a hotkey row's button does - here so a test can arm
    // one without driving the Settings panel.
    void ArmHotkeyCapture(HotkeySlot slot);

private:
    // The Settings tab's Shortcuts section: every tool and create action,
    // each with the key it answers to. A section of its own rather than
    // rows appended to another one - eleven key editors would be most of
    // whatever panel they were put in, and "what is bound to what" is a
    // question people come to answer on its own.
    // Takes `anyChanged` like every other section, though the key editors
    // in it report nothing through it: a rebind goes to the OS and to disk
    // through TryChangeHotkey rather than through the settings-changed
    // callback. The one ordinary setting here - whether a hidden overlay
    // may say what it just did - is what needs it.
    void RenderSettingsHotkeys(bool& anyChanged);
    // One row of it. `capturing` rows read the next key pressed; Escape
    // unbinds, and Backspace/Delete do too, since a row showing "(none)" is
    // exactly what someone reaches for those keys to get.
    void RenderShortcutEditor(ShortcutAction action, const Icon& icon, const char* label);
    // Binds `combo` to `action`, taking it off whatever else held it -
    // rejecting the change would leave the user to find the other holder
    // themselves, and two rows claiming one key is a state where only one
    // of them can ever fire. Pass a default-constructed combo to unbind.
    void SetToolShortcut(ShortcutAction action, platform::KeyCombo combo);
    // Fires whichever shortcut was pressed this frame, at most one. Runs
    // only while the overlay is showing edit mode and nothing is typing.
    void HandleToolShortcuts();
    // The wheel: with Alt it steps between the canvases of the current
    // canvas's folder, plain it sizes the tool in hand - see the definition.
    void HandleMouseWheel();
    // What a bound key does: select a tool, or run a create action.
    void RunShortcutAction(ShortcutAction action);
    // Cancel/Delete confirmation for a canvas or folder Delete button
    // clicked in the Overview - see confirmDeleteTarget_'s own doc
    // comment for why canvas/folder deletion gets this extra step while
    // item/stroke deletion (DeleteItemWithToast) stays instant + undoable.
    void RenderConfirmDeletePopover();
    void RenderActionToast();
    // See SetConfigWriteFailed.
    void RenderPersistenceWarning();

    void SetTool(Tool tool);
    // Sets the draw color (see drawColorRGBA_'s own doc comment) - the
    // single place that color actually changes.
    void SetDrawColor(uint32_t colorRGBA);
    // Runs a create action - New canvas, which has nothing to place and so
    // happens at once.
    void RunCreateAction(CreateAction action);
    void RunClipboardAction(ClipboardAction action);
    // The defaults a freshly created item gets for its kind - a capture
    // for Screenshot, drawing mode with the pen for Drawing. Shared by the
    // fullscreen and region-drag creation paths so the two can't drift.
    void ApplyCreationDefaults(ItemCreationKind kind, ItemId id, Item& item);
    // Forgets the creation gesture in flight, if any.
    void ClearCreationGesture();
    // Hands back the tool that was in hand before a creation tool, if one is
    // in hand now - after a screenshot is placed, and wherever nothing may
    // be made (view-only).
    void PutDownCreationTool();
    // The current canvas, creating one first if the library is empty (a
    // state CanvasManager allows - see its class comment). Never returns
    // a dangling reference: the create path always produces a canvas.
    // Shared by both item-creation paths so neither has to decide what an
    // empty library means on its own.
    Canvas& EnsureCanvasForNewItem();
    // Places the first-run welcome note, centered - see RequestWelcomeNote.
    // An ordinary item, deliberately: it can be moved, edited, or closed
    // like anything else, and it autosaves, so it stays until the user is
    // done with it and then stops existing for good. A modal dialog would
    // have to be dismissed before the app could be touched at all, and
    // would teach nothing about how the app actually works.
    void PlaceWelcomeNote(float displayW, float displayH);
    ItemId CreateFullscreenItem(ItemCreationKind kind, float displayW, float displayH);
    ItemId FinishRegionCapture();
    // The one hit test: what is under a screen point, front to back - see
    // PointerTarget. Pure in position and model (no ImGui state), so the
    // frame and the raw mouse pipeline get the same answer for the same
    // pixel.
    PointerTarget ResolvePointerTarget(float x, float y) const;
    // Enters live text editing for a Note item - copies its current
    // noteText into noteEditBuffer_, arms RenderNoteEditor's
    // editor window to grab keyboard focus the next frame it renders
    // (noteEditJustFocused_), and requests real OS focus (see
    // IOverlayWindow::RequestTextInput's own doc comment for why that's
    // needed regardless of SetEditModeNoActivate). Commits whatever was
    // being edited before, if anything and if it's a different item -
    // clicking straight from one note into another without an
    // intermediate click-away shouldn't silently drop the first edit.
    void BeginEditingNote(ItemId id);
    // Ends live text editing and commits `text` into the item's noteText.
    // Called from the note-editor window's own IsItemDeactivated() check
    // (see RenderNoteEditor) with a *pre-this-frame* snapshot of the edit
    // buffer, not noteEditBuffer_ itself - InputTextMultiline reverts its
    // own buffer internally on Escape before reporting deactivation, so by
    // the time this runs on an Escape frame noteEditBuffer_ already holds
    // the stale pre-edit-session value again. The snapshot the caller
    // passes is what the user actually typed, which is what should be
    // kept: Escape here means "stop editing," not "undo my typing" - the
    // user has the app's own Undo for that if they want it, which this
    // makes literally true: the session files the edit as one undo entry
    // whenever the text changed (see Session::EndTextEdit).
    void EndEditingNote(const std::string& text);
    // The box-selection gesture's moves and its release - started in
    // HandleItemGesture, which is also where the press that starts one is
    // decided. Always consumes the event.
    bool HandleBoxSelection(const platform::MouseEvent& event);
    // Starts, continues, or ends the selection's one gesture (a press on a
    // snippet with Select in hand or Alt held, which selects it and moves
    // the selection; or on a selected snippet's handle, which resizes it -
    // see itemGesture_'s own doc comment and the definition), and holds
    // and fires the selection bar's buttons. Returns true if this event
    // was consumed (caller returns immediately without its own normal
    // handling for that button); false lets it fall through to whatever it
    // would otherwise do.
    bool HandleItemGesture(const platform::MouseEvent& event);
    // The one creation gesture, press to release: a press on empty canvas
    // starts it with either button - the left makes a screenshot, the
    // right a drawing - and so does a left press anywhere while a creation
    // is armed. Dragged, it frames the snippet; the second press of a
    // double-click makes it fullscreen; a plain click makes nothing (with
    // a creation tool in hand, which was picked on purpose, a click is
    // fullscreen too). Returns true if the event was the gesture's.
    bool HandleCreationGesture(const platform::MouseEvent& event);
    // A stroke, an erase or a note opened, on the snippet in drawing mode
    // - the marking tools' whole gesture, Down to Up. Only ever called in
    // drawing mode: the Down arms drawingItem_, and the Move and Up that
    // follow continue whatever it started.
    void HandleStrokeEvent(const platform::MouseEvent& event);
    // The shape a Draw or Erase press would make now: the modifiers' if
    // one is held (see DrawShapeFor), else the drawing bar's cycled shape
    // for the tool. What HandleStrokeEvent fixes at the press and the
    // modifier badge shows before one, from one rule.
    DrawShape ShapeForPress() const;
    // Remembers this press for the next one to be judged a double-click
    // against, and says whether it is one: the same button, within
    // kDoubleClickSeconds and kDoubleClickPx of the last press, with no
    // modifier held for either. A double is never the first half of the
    // next one.
    bool NoteDoubleClick(const platform::MouseEvent& event);
    // Remembers the press in progress as one a hold can stand in for a
    // double-click on: held still for kHoldSeconds it enters drawing mode
    // on `item`, or makes a fullscreen snippet of `creates` - see
    // heldPress_. The second press of a double-click is never remembered:
    // it does on release what the hold would do.
    void HoldPress(const platform::MouseEvent& event, std::optional<ItemId> item,
                   std::optional<ItemCreationKind> creates);
    // Acts on heldPress_ once it has been held long enough - called every
    // frame, since a pointer held still sends no events. Whatever the press
    // started (a move, a creation) is dropped from under it unwritten, so
    // its release then does nothing.
    void MatureHeldPress();
    // Whether a press at this point, with this button, is one on empty
    // canvas that should make a snippet - nothing under it, nothing open
    // that a click outside of is meant to close.
    bool PressMakesASnippet(const platform::MouseEvent& event) const;
    // Discards untouchedDrawing_ if nothing has been put into it by now,
    // and stops watching it either way.
    void SettleUntouchedDrawing();
    // Stops watching untouchedDrawing_ if the gesture in flight moves or
    // resizes it - see its own doc comment. Also what a nudge by key does.
    void KeepPlacedDrawings();

    // A new empty canvas at the end of the browsed folder, named for when
    // it was made (see TimestampName), and *not* switched to - every
    // caller that wants that says so. Returns its id, or 0 if the library
    // couldn't take one.
    CanvasId CreateCanvasInCurrentFolder();
    // The same, in the folder the current canvas lives in - where the work
    // is - rather than the one the Overview happens to be browsing, which
    // is only the same folder until someone browses elsewhere and closes
    // the Overview without switching. What every way of making a canvas
    // from the canvas itself uses; the Overview's own button means the
    // browsed folder, which is the one on screen there.
    CanvasId CreateCanvasBesideCurrent();
    // New canvas (its shortcut): create a new canvas beside the current
    // one and switch straight to it - jumping there immediately is the
    // useful default when there's no Overview grid open to decide from.
    void CreateAndSwitchToNewCanvas();
    // That, taking the selected snippets along - see its definition.
    void MoveSelectionToNewCanvas();

    // ===== Rasterized vector strokes (StrokeRenderMode::Rasterized) =====

    // An item's vector strokes, drawn into a bitmap so they can be
    // composited in one go. Purely derived - the strokes are still the
    // truth, this is thrown away and rebuilt from them, and is never
    // persisted. That is the whole difference between this and a painted
    // layer, which looks almost identical from the outside.
    struct StrokeRaster {
        PaintedImage pixels;
        uint64_t textureHandle = 0;
        // What it was built from, kept so a stale raster is recognised by
        // comparing rather than by guessing. A count is not enough and
        // never could be: undoing back to nothing and drawing something new
        // leaves the count exactly where it started, which is how the last
        // undone stroke stayed on screen and the next one didn't appear.
        //
        // A copy of the strokes, not a summary of them. It costs a second
        // copy of the points - a couple of hundred kilobytes for a heavy
        // item - and buys an exact answer with no hash and no probability
        // attached. The comparison is only reached when something has
        // actually changed; see strokeRasterGeneration_.
        float nativeW = 0.0f;
        float nativeH = 0.0f;
        std::vector<Stroke> builtFrom;
    };
    // Brings the current canvas's rasters in line with its items, and
    // releases every other one. A no-op in the other two render modes,
    // which also frees whatever was cached the moment the mode changes.
    // Called once a frame from the same place the layer textures are
    // brought up to date.
    void RefreshStrokeRasters();
    // Brings `raster` in line with `item`'s strokes: nothing at all when it
    // already matches, just the new strokes on top of what is there when
    // only new ones have been appended since it was built, and everything
    // from scratch otherwise - all decided by one comparison of what it
    // was built from against what is there now. Appending is the common
    // case by a long way: it is what finishing a stroke does, and
    // rebuilding every stroke each time would make a busy item cost more
    // with every mark on it.
    void BuildStrokeRaster(const Item& item, StrokeRaster& raster);
    // The texture for `itemId`'s rasterized strokes, or 0 if there isn't
    // one - which DrawItemContent takes as "draw them tessellated instead".
    uint64_t StrokeRasterTextureFor(ItemId itemId) const;
    void ReleaseStrokeRasters();

    // ===== Overview bitmap previews (AppConfig::overviewShowsBitmaps) =====

    // A layer's pixels, decoded and scaled down to thumbnail size, for the
    // canvas overview - which draws canvases that aren't current and whose
    // full-size pixels are deliberately not in memory.
    //
    // Deliberately not a cache that survives the panel: these are loaded
    // while the overview is open and released when it closes, so a library
    // of screenshots costs nothing while it isn't being browsed. The
    // expensive part is the decode, not the memory - a preview is at most
    // kOverviewPreviewMaxExtent on its long edge, a couple of hundred
    // kilobytes against the eight megabytes it came from.
    // A handle of 0 means it was tried and failed - a missing or unreadable
    // file. The entry exists either way, which is what stops a failure
    // being retried on every frame the panel is open.
    struct LayerPreview {
        uint64_t textureHandle = 0;
    };
    // Identifies a layer across the whole library: which item, and where in
    // its stack.
    using LayerKey = std::pair<ItemId, size_t>;
    struct LayerKeyHash {
        size_t operator()(const LayerKey& key) const {
            return std::hash<uint64_t>{}(key.first) ^ (std::hash<size_t>{}(key.second) << 1);
        }
    };
    // Resets the per-frame decode budget - called once, at the top of the
    // overview's own rendering.
    void BeginOverviewPreviewFrame();
    // The preview texture for one layer, loading it if there is budget left
    // this frame and it hasn't already failed. Three answers, and the
    // Overview draws each differently: a handle to draw with; 0 for a layer
    // that has no pixels to show at all (or whose read failed), which gets
    // the placeholder gradient; and nothing at all for one whose turn to be
    // read hasn't come yet, which gets drawn as an empty tile rather than a
    // stand-in that will be replaced a few frames later. A slow library
    // fills in over those frames rather than stalling the panel.
    std::optional<uint64_t> LayerPreviewTexture(const Item& item, size_t layerIndex);
    // How a preview finds a layer's pixels: LayerPreviewTexture while the
    // Overview shows bitmaps (see AppConfig::overviewShowsBitmaps), else
    // nothing - an empty lookup, which DrawCanvasPreview tests for. Shared
    // by the canvas grid, the canvas bar and the recently-deleted list.
    using PreviewTextureFn = std::function<std::optional<uint64_t>(const Item&, size_t)>;
    PreviewTextureFn PreviewTextureLookup();
    void ReleaseLayerPreviews();

    // ===== Painting (see AppConfig::paintPixelsInsteadOfStrokes) =====



    // Overview: switch canvases, delete/reorder them, or (when opened via
    // the Properties popover's Move or Copy button) pick a target canvas
    // for that item.
    void OpenOverview();
    void OpenPicker(ItemId itemId, bool isCopy);
    void CloseOverview();
    void ShowActionToast(std::string text);
    // A snippet's Close button: an undoable delete (see
    // Session::DeleteItem). No-op if `itemId` isn't on the current canvas.
    void DeleteItemWithToast(ItemId itemId);
    // Removes every stroke and every painted pixel from the item (its
    // captured screenshot is untouched - this only ever clears ink drawn
    // on top) as one undoable step - see Session::ClearDrawing - and says
    // so. No-op if `itemId` doesn't exist or has nothing drawn on it.
    void ClearItemDrawing(ItemId itemId);
    // Nudges a freshly-made copy diagonally so it doesn't sit exactly on
    // top of the original it was copied from - the usual desktop "paste"
    // cascade. Clamped to the viewport the same way any other move is
    // (see ClampRectToViewport), so it never actually offsets somewhere
    // there's no room for - "if there is space" falls out of that
    // existing guarantee rather than needing its own check. Re-anchors
    // afterward (see CanvasManager::CommitItemLayout), same as any other
    // deliberate reposition. No-op if `itemId` doesn't exist.
    void OffsetCopiedItem(ItemId itemId);

    // Undo and redo, as Ctrl+Z and Ctrl+Y reach them: the session steps
    // the current canvas's history (see Session::Undo), and this says
    // what happened.
    void Undo();
    void Redo();
    void ShowUndoStep(const std::optional<Session::UndoStep>& step);
    // Whether there is anything to undo - see Session::CanUndo.
    bool CanUndo() const { return session_.CanUndo(); }

    // A Delete button was clicked that asks first - a canvas or folder, or
    // Delete permanently on one with Show deleted on - pending the user
    // actually confirming in RenderConfirmDeletePopover. A snippet on screen
    // doesn't ask: its delete is undoable instead (see Session::DeleteItem).
    // `name` is captured at click time purely for the popup's own "Delete
    // <name>?" text, so it doesn't need to re-look-up a possibly-renamed-since
    // target.
    struct ConfirmDeleteTarget {
        // DeletedCanvasesIn is a folder that is not deleted itself, and
        // what goes for good is the canvases in it that are - see
        // Session::DeleteMarkedCanvasesPermanently. Always for good.
        enum class Kind { Canvas, Folder, DeletedCanvasesIn };
        Kind kind = Kind::Canvas;
        uint64_t id = 0;  // CanvasId or FolderId depending on kind
        std::string name;
        // Deleted already, so this is Delete permanently rather than a mark.
        bool forGood = false;
    };
    std::optional<ConfirmDeleteTarget> confirmDeleteTarget_;
    // See RenderConfirmDeletePopover's own request-flag reasoning (mirrors
    // itemPropertiesPopoverRequested_, even though nothing here is
    // actually reached from the raw platform callback the way that is).
    bool confirmDeletePopoverRequested_ = false;


    // Applied once, on the first OnFrame call (ImGui's style/color tables
    // only exist once a context does, which isn't guaranteed yet at
    // construction or even AttachTo time - see ApplySpickzettelStyle's own
    // comment for why this couldn't just run from AttachTo).
    bool styleApplied_ = false;

    // See SetViewOnly's doc comment.
    bool viewOnly_ = false;
    // See SetNoticeOnly's doc comment. Only ever true alongside viewOnly_.
    bool noticeOnly_ = false;
    // See SetPinnedOnly's doc comment. Only ever true alongside viewOnly_.
    bool pinnedOnly_ = false;
    // Fired once when a notice's message has faded - see
    // SetNoticeFinishedCallback. Guarded by this, so a notice that stays up
    // (because the window could not be hidden, say) does not call it on
    // every frame afterwards.
    bool noticeFinishedReported_ = false;

    // What is being worked on, which library is showing, and everything that
    // keeps it in step with the disk and the GPU - see Session. Owned by
    // TrayController; this class is a view of it.
    Session& session_;
    // The visible library's model and store, which is what nearly
    // everything in this class means by "the library".
    CanvasManager& Manager() { return session_.Manager(); }
    const CanvasManager& Manager() const { return session_.Manager(); }
    persistence::LibraryStore* Store() const { return session_.Store(); }

    // Every setting, stored and resolved - owned by TrayController, which
    // also persists it. Plain fields are read and edited through Cfg();
    // the profileable ones through settings_.Live() and the helpers above.
    Settings& settings_;
    AppConfig& Cfg() { return settings_.Mutable(); }
    const AppConfig& Cfg() const { return settings_.Stored(); }

    DrawTool drawTool_;
    // See DrawLibraryTreeHud for the two below: the lines last read off the
    // disk, and the store's write count they were read at, so the walk
    // happens after a write and not per frame.
    std::vector<LibraryTreeLine> libraryTreeLines_;
    std::optional<uint64_t> libraryTreeSeenGeneration_;
    const persistence::LibraryStore* libraryTreeSeenStore_ = nullptr;
    // The last count handed to IOverlayWindow::SetInputOptionsHudDigits, so
    // that only an actual change reaches the platform - it can install or
    // remove a keyboard hook, which is not something to ask for every frame.
    int appliedInputOptionsHudDigits_ = 0;
    // A HUD toggle that needs edit mode re-entered waits here until the key is
    // released - see HandleInputOptionsHudKeys for why holding the restart is
    // the difference between every press counting and one in three vanishing.
    bool pendingOverlayRestart_ = false;

    // What the HUD's last number key actually did, shown in the HUD itself.
    //
    // For a report that only reproduces on someone else's machine: "pressing
    // 1 sometimes switches back" has two very different causes - the key
    // acting twice, or acting once and something else undoing it - and from
    // the outside they look identical. The toggle count tells them apart at
    // a glance, and the rest says where the value was written and what it
    // resolved to afterwards.
    int hudToggleCount_ = 0;
    int hudLastToggledRow_ = 0;       // 1-based, 0 for "nothing yet"
    bool hudLastToggledTo_ = false;   // what that press asked for
    bool hudLastWentToProfile_ = false;
    // How the stroke currently being drawn is rendered. Never Rasterized:
    // a live stroke isn't in item.strokes yet and so isn't in the raster,
    // and rebuilding one per mouse-move to include it would cost far more
    // than the difference is worth. It joins the raster the moment it is
    // finished.
    StrokeRenderMode LiveStrokeRenderMode() const {
        return Cfg().strokeRenderMode == StrokeRenderMode::Polyline ? StrokeRenderMode::Polyline
                                                                : StrokeRenderMode::Tessellated;
    }
    // Per item, and only while the mode is Rasterized - see
    // RefreshStrokeRasters, which is also what empties this again when the
    // mode changes or a canvas stops being current.
    std::unordered_map<ItemId, StrokeRaster> strokeRasters_;
    // The canvas generation these rasters were last checked against, and
    // the reason comparing whole stroke lists costs nothing in practice:
    // while it hasn't moved, nothing anywhere has changed and there is
    // nothing to compare. Only a gate on *when* to look - the comparison
    // itself is still what decides the answer, so this never has to be
    // precise about what changed, only that something did.
    //
    // Leaning on the generation counter rather than on a new
    // "remember to invalidate" rule at each mutation site is deliberate:
    // it is already load-bearing for autosave, so a mutation that forgot to
    // bump it would be losing the edit outright, which is a louder failure
    // than a stale picture. nullopt means "check regardless" - a fresh
    // start, or the mode having just been switched on.
    std::optional<uint64_t> strokeRasterGeneration_;
    // The tessellated shape of each stroke, kept between frames - see
    // StrokeMeshCache for what that saves and what invalidates an entry.
    //
    // Two of them, not one, because a cached mesh is only valid at the scale
    // it was built at, and the two places strokes are drawn use different
    // ones *in the same frame*: an item on the canvas is drawn at its own
    // size, and the same item appears again, much smaller, in its canvas's
    // Overview tile. One cache would have each rebuild the other's entry
    // every frame - strictly worse than not caching at all. Sharing one
    // between the canvas and the dock is safe by contrast, and they do:
    // RenderItems skips minimized items and the dock draws only those, so no
    // item is ever in both at once.
    StrokeMeshCache strokeMeshCache_;
    StrokeMeshCache previewMeshCache_;
    // Opens and closes a frame on both of them - see its definition in
    // overlay_app.cpp, and the one instance of it at the top of OnFrame.
    struct MeshCacheFrame;
    // Each cache aimed at the current generation, which is all a drawing
    // call needs to be handed - see StrokeMeshSlot.
    StrokeMeshSlot CanvasMeshSlot() { return StrokeMeshSlot{&strokeMeshCache_, Manager().Generation()}; }
    StrokeMeshSlot PreviewMeshSlot() { return StrokeMeshSlot{&previewMeshCache_, Manager().Generation()}; }
    // Thumbnail-sized copies of layer pixels, alive only while the overview
    // is open - see LayerPreview. Emptied by ReleaseLayerPreviews when it
    // closes, or when the setting is switched off.
    std::unordered_map<LayerKey, LayerPreview, LayerKeyHash> layerPreviews_;
    // How many previews are still allowed to be read this frame, one budget
    // per cost. Both reset each frame the overview is drawn.
    //
    // Full images are the fallback for a library with no sidecar
    // thumbnails, and a decode is milliseconds - a handful per frame, so
    // the panel doesn't stall on the frame it opens, which is exactly the
    // frame it must not. Thumbnails are the ordinary path and cost well
    // under a millisecond, so the budget is high enough that a normal
    // library appears at once, and bounded only against a folder holding
    // an unreasonable number of canvases.
    int layerPreviewLoadBudget_ = 0;
    int layerPreviewThumbnailBudget_ = 0;
    // Which resize handle (if any) is currently hovered or dragging, as a
    // short human-readable label ("nw item=3", "e item=5 (dragging)") -
    // empty when none is. Set by RenderItems from the resolver's answer,
    // cleared at the top of every call so it never shows a stale handle
    // from a frame where the mouse has moved off every handle since.
    // Exists purely for DrawDebugOverlay (gated on AppConfig::
    // showDebugOverlay, same as everything else that setting draws) and
    // the tests - a live
    // readout of exactly what the resize margin thinks is under the
    // cursor, since a screenshot alone can't distinguish "this pixel is
    // covered by a handle that just isn't visually distinguishable from
    // its neighbor" from "this pixel isn't covered by anything."
    std::string debugHoveredResizeHandle_;
    // Which Shortcuts-tab row is armed, waiting for the key the user wants
    // - nullopt when none is, one at a time for the same reason
    // hotkeyCaptureSlot_ is. Escape while armed unbinds the row rather than
    // closing the Overview, which is the only place Escape means something
    // other than "out of here" - see RenderOverview.
    std::optional<ShortcutAction> shortcutCaptureAction_ = std::nullopt;
    // Which hotkey slot's RenderHotkeyEditor row is currently "armed",
    // waiting for the user to press the combo they want - std::nullopt when
    // none is. Only one at a time: arming a different row implicitly
    // disarms whichever was already armed (see RenderHotkeyEditor's own
    // body) rather than tracking capture state per row.
    std::optional<HotkeySlot> hotkeyCaptureSlot_ = std::nullopt;
    // The move or resize in progress, if one is (see HandleItemGesture): a
    // press on a snippet with Select in hand or Alt held moves the
    // selection, a press on a selected snippet's handle resizes that one.
    // Hand-rolled Down/Move/Up state on the raw platform pipeline (see
    // OnMouse), not ImGui's GetMouseDragDelta: no widget backs any of it.
    // Also what keeps the dragged item's highlight showing for the rest of the
    // gesture even if the mouse strays off its rect mid-drag.
    std::optional<ItemGesture> itemGesture_;
    // A press on one of the selection bar's buttons, held until the
    // release that fires it (if it lands on the same button - see
    // HandleItemGesture). Nothing else on the bar or a handle reacts to
    // the pointer while one is held, as with an ImGui button holding
    // ActiveId.
    std::optional<ChromeButton> pressedBarButton_;
    // The selected snippets, in the order they were selected - the last
    // one is the primary (see PrimarySelection). Transient UI state, never
    // persisted, and only ever snippets on the current canvas that are on
    // screen (see PruneSelection). A click selects one, Shift+click adds or
    // removes one, a click on empty canvas or Escape clears it; Delete, the
    // arrow keys and the selection bar act on it. In drawing mode the
    // selection is the snippet being drawn on.
    std::vector<ItemId> selection_;
    // The box being dragged over the canvas to select by, while one is -
    // see BoxSelection.
    std::optional<BoxSelection> boxSelect_;
    // What Copy or Cut last put on the clipboard, in the order it was
    // selected, and which of the two it was.
    //
    // Ids, not copies of the snippets: a paste acts on them as they are
    // at the moment it happens, and a snippet deleted in between is
    // simply not pasted - the alternative, holding a snapshot, quietly
    // resurrects things the user has since thrown away. It is also what
    // makes Cut take nothing away until the paste that moves it: what was
    // cut is still there to change its mind about, drawn faded until then
    // (see IsWaitingToBeCut). A cut clears the clipboard once pasted -
    // those snippets have moved, and pasting again would move them from
    // where they now are - while a copy stays, to be pasted as often as
    // wanted.
    std::vector<ItemId> clipboard_;
    bool clipboardIsCut_ = false;
    // The snippet in drawing mode, if one is. Selecting and moving are the
    // hand at rest; drawing on a snippet is a mode entered by
    // double-clicking or holding on it (or picking a marking tool by key
    // with it selected, or making a new drawing, which is made to be
    // drawn in) and left by a left press anywhere else or Escape. In it
    // the snippet wears a stronger outline, its bar shows the drawing
    // buttons (see ChromeButton), the pen is in hand (activeTool_; a key
    // can enter with another marking tool), a left press on the
    // snippet draws with it, and a right-drag on it erases whatever the
    // tool (see rightErase_); Alt held moves or resizes it instead. A left
    // press anywhere else, or a right click on the snippet itself, leaves
    // the mode and does nothing more - the press was for leaving. Only
    // ever a snippet on the current canvas
    // that is on screen, kept in step with the selection by
    // PruneSelection.
    std::optional<ItemId> drawingItem_;
    // The right button's erase on the snippet being drawn on (see OnMouse):
    // where it was pressed, and whether it has become a drag - and so an
    // erase - yet. A press that never does is a right click, which leaves
    // drawing mode on release.
    struct RightErase {
        float x = 0.0f;
        float y = 0.0f;
        bool erasing = false;
    };
    std::optional<RightErase> rightErase_;
    // The last press on the raw pipeline, for the next one to be judged a
    // double-click against - see NoteDoubleClick. Cleared once it has been
    // the first half of a double, so a third click starts over.
    struct LastPress {
        platform::MouseButton button = platform::MouseButton::Left;
        double atSeconds = 0.0;
        float x = 0.0f;
        float y = 0.0f;
    };
    std::optional<LastPress> lastPress_;
    // Whether the press in progress was the second of a double-click - set
    // by OnMouse on every Down for the handlers that care (HandleItemGesture
    // enters drawing mode on it; HandleCreationGesture makes the snippet
    // fullscreen on it).
    bool pressIsDouble_ = false;
    // The press in progress, while a hold could still make of it what a
    // double-click would have (see HoldPress/MatureHeldPress): where and
    // when it was pressed, and what a hold does - drawing mode on `item`,
    // or a fullscreen snippet of `creates`. Dropped the moment the press
    // moves past kDoubleClickPx or is released. For a finger or a pen,
    // which cannot double-click reliably; a mouse can do either.
    struct HeldPress {
        platform::MouseButton button = platform::MouseButton::Left;
        float x = 0.0f;
        float y = 0.0f;
        double atSeconds = 0.0;
        std::optional<ItemId> item;
        std::optional<ItemCreationKind> creates;
    };
    std::optional<HeldPress> heldPress_;
    // The button that is down, and the other one if it was pressed
    // meanwhile: the first button to press owns the pointer until it lets
    // go, and the other is ignored - its press, its moves and its release -
    // until it lets go too (see OnMouse). No gesture here has ever meant
    // anything by a second button, and one thing does send it: Windows'
    // press-and-hold on a touch screen injects a right press and release
    // while the finger's left press is still down, which would take back
    // what the hold had just done (drawing mode entered, a fullscreen
    // snippet made) even where the platform fails to switch it off.
    std::optional<platform::MouseButton> pressedButton_;
    std::optional<platform::MouseButton> ignoredButton_;

    // Set by AttachTo; used for real screen capture (CaptureShotItem) and
    // releasing a deleted item's captured texture. Never null once
    // AttachTo has been called - a window that outlives this OverlayApp,
    // per the ownership already established by TrayController/main.
    platform::IOverlayWindow* window_ = nullptr;

    // See SetRestartOverlayCallback's own doc comment.
    std::function<void()> restartOverlayCallback_;
    // See SetDisplayListCallback. `displays_` is its last answer, asked when
    // the Overview opens and again whenever the monitor list is opened
    // rather than every frame: listing displays is a trip through the
    // display configuration, and they seldom change while someone is looking
    // at the list.
    std::function<std::vector<platform::DisplayInfo>()> displayListCallback_;
    std::vector<platform::DisplayInfo> displays_;
    // A monitor picked from that list, committed at the start of the next
    // frame rather than the moment it was picked. Committing one moves the
    // overlay and takes the frozen screen again, which releases the old
    // frozen image - and by the time the Settings panel draws, that image
    // is already queued as this frame's backdrop. See OnFrame.
    bool displayChoiceCommitPending_ = false;
    // See SetNoticeFinishedCallback's own doc comment.
    std::function<void()> noticeFinishedCallback_;
    // See SetHotkeyChangeCallback's own doc comment.
    std::function<bool(HotkeySlot, platform::KeyCombo)> hotkeyChangeCallback_;

    // The tool in hand, Select to start with - see the Tool enum. A marking
    // tool is in hand exactly while a snippet is in drawing mode (see
    // drawingItem_); picked from the drawing bar or a key, through
    // PickTool.
    Tool activeTool_ = Tool::Select;
    // Shared by every stroke-based tool (Pen/Rectangle/Line - see the Tool
    // enum's own doc comment) rather than each having its own: they're the
    // same underlying operation (lay down a colored line of some width),
    // just differing in what points make up the stroke, so switching
    // between them mid-session keeps whatever color/width was last set
    // instead of resetting. Eraser has its own eraserWidth_ instead (a
    // radius, not a stroke width, and no color at all).
    uint32_t drawColorRGBA_;
    float drawWidth_;
    // Whether the wheel has changed drawWidth_ since it was last saved: it
    // is kept as AppConfig::strokeWidth once the size preview has faded,
    // so a burst of notches is one write (see RenderBrushSizePreview).
    bool drawWidthDirty_ = false;
    float eraserWidth_ = 28.0f;

    // The tool that was in hand before a creation tool was picked - what a
    // screenshot hands back once placed. See SetTool.
    Tool toolBeforeCreation_ = Tool::Select;

    // The creation gesture in flight: what it places, then a click
    // (fullscreen) or a drag (region) - started by a creation tool's press or
    // by a press on empty canvas (see HandleCreationGesture).
    struct CreationGesture {
        ItemCreationKind kind = ItemCreationKind::Screenshot;
        // Which button it was pressed with: the left for a creation tool
        // picked by key, either for a press on empty canvas.
        platform::MouseButton button = platform::MouseButton::Left;
        float downX = 0.0f;  // where it was pressed
        float downY = 0.0f;
        // Where the pointer is now, once it has travelled far enough from
        // the press to be a drag framing a region - nothing for a click.
        std::optional<ImVec2> dragTo;
        // Whether the press that started it was the second of a
        // double-click - released without a drag, that makes the snippet
        // fullscreen.
        bool isDouble = false;
        // Whether it began as a press on empty canvas rather than with a
        // creation tool in hand - what decides that a drawing it makes is
        // untouchedDrawing_.
        bool fromEmptyCanvas = false;
    };
    std::optional<CreationGesture> creation_;
    // A drawing a press on empty canvas made, with nothing put into it yet.
    // It goes - erased, not merely marked deleted - once the hand moves on
    // without using it: a press anywhere else, another canvas, its own
    // Close, the overlay going away, or an undo (see SettleUntouchedDrawing). What
    // makes a snippet on every click on empty space harmless to miss with.
    // Moving, resizing or nudging it is using it (see KeepPlacedDrawings):
    // a box someone has placed is a box they want, empty or not. And so is
    // putting anything into it: once it has held a stroke it is watched no
    // longer (see OnFrame), so an undo that empties it again leaves it as
    // an empty drawing rather than erasing it, redo and all.
    std::optional<ItemId> untouchedDrawing_;

    // RectEraser's own placement gesture (see OnMouse): unlike Rectangle/
    // Line above, nothing is drawn into liveLayer while dragging - the
    // rect is only ever a preview (see RenderRectEraserOverlay), and the
    // actual erase (CanvasManager::EraseRectAt) happens once, at Up, with
    // the final dragged rect, as one undoable step (see Session::EraseRect).
    struct RectErase {
        float x0 = 0.0f;  // the corner it was pressed at
        float y0 = 0.0f;
        float x1 = 0.0f;  // where the pointer is now
        float y1 = 0.0f;
    };
    std::optional<RectErase> rectErase_;

    // Item properties popover (foreground/background opacity, background
    // color, fullscreen, order, copy, move): which item it's currently
    // open for, if any - set by the selection bar's "More" button (see
    // ActivateBarButton).
    std::optional<ItemId> itemPropertiesPopoverItemId_ = std::nullopt;
    // Where to open the popover above - captured from the "More" button's
    // own screen position at click time (see ActivateBarButton).
    ImVec2 itemPropertiesPopoverAnchor_ = ImVec2(0.0f, 0.0f);
    // The snippet context menu, and which snippet it is up for. The menu
    // owns its own "asked for, not yet opened" state; the id is kept here
    // because the rows are the snippet's, and it has to survive the frame
    // between the right-click and the menu appearing.
    ContextMenu itemContextMenu_{"##item_context_menu"};
    std::optional<ItemId> itemContextMenuItemId_ = std::nullopt;
    // The canvas bar's own, and which tile's canvas it is up for.
    ContextMenu canvasContextMenu_{"##canvas_context_menu"};
    std::optional<CanvasId> canvasContextMenuCanvasId_ = std::nullopt;
    // Why the popovers open through request flags (this one, and
    // itemPropertiesPopoverRequested_ below) rather than calling
    // ImGui::OpenPopup where the button fires: a selection bar button
    // fires from HandleItemGesture, which runs from the platform's raw
    // mouse callback - itself invoked synchronously from
    // glfwPollEvents()/the Win32 message pump, *before* that iteration's
    // ImGui::NewFrame() (see DevLinuxPlatformHost::RunEventLoop /
    // Win32OverlayWindow's own message loop). ImGui::OpenPopup at that
    // point has no current window to scope its ID against - g.CurrentWindow
    // is whatever Render() left behind at the *end* of the previous frame,
    // which ImGuiWindow::GetID then dereferences unconditionally
    // (IDStack.back() on a stack that's empty by then) - a real, verified
    // (ASan-caught) null-pointer segfault, not a hypothetical one. The
    // flag is consumed (and cleared) at the top of the popover's own
    // Render function on the very next frame, properly inside a frame.
    // The colour chooser - see OpenColorChooser. Asked for by the bar's
    // Colour button, opened on the next frame; and whether it was open on
    // the last one, which is how its closing is noticed.
    bool colorChooserRequested_ = false;
    bool colorChooserOpen_ = false;
    ImVec2 colorChooserAnchor_ = ImVec2(0.0f, 0.0f);
    // The Draw or Erase gesture in progress, decided as it starts from the
    // modifiers held then (see OnMouse) - and for Draw, which shape it is
    // making, since a shape can switch between line and rectangle mid-drag.
    enum class StrokeGesture { None, Freehand, Paint, Shape, Erase, EraseRect };
    StrokeGesture strokeGesture_ = StrokeGesture::None;
    DrawShape strokeShape_ = DrawShape::Freehand;
    // What the pen draws and the eraser erases on a plain drag, with no
    // modifier held: the drawing bar's own button cycles the tool in hand
    // through its shapes (pen, line, rectangle; eraser, rectangle eraser -
    // see ActivateBarButton), so a hand with no keyboard can draw a line
    // with a drag alone. A modifier held still wins for that stroke. Both
    // go back to plain when the tool changes (see SetTool): the shape is
    // the tool's for as long as it is in hand, and no longer.
    DrawShape penShape_ = DrawShape::Freehand;
    DrawShape eraserShape_ = DrawShape::Freehand;  // Freehand or Rectangle
    // Whether a note was open for typing when the press in progress began.
    // Such a press is for closing it and makes nothing (see
    // PressMakesASnippet) - even once settling an untouched snippet has
    // closed the note itself.
    bool noteOpenAtPress_ = false;

    // The canvas bar, docked against the bottom edge - see UpdateEdgePanels.
    // How far out it is, and where it was drawn this frame (none while it
    // is all the way in).
    EdgeReveal canvasBarReveal_;
    std::optional<Rect> canvasBarRect_;
    // The top of whatever is out on the bottom edge this frame, or the
    // display's height - what the minimized chips have to stay above.
    float bottomPanelsTop_ = 0.0f;
    // The canvas bar's tiles, scrolled this far; the canvas it last saw, to
    // notice a change; and whether to bring the current tile into view.
    float canvasBarScroll_ = 0.0f;
    std::optional<CanvasId> canvasBarLastCanvas_;
    bool canvasBarScrollToCurrent_ = true;
    // Set when the overlay comes up, for the panels to come out for a moment
    // on the first frame after.
    bool edgePanelsFlashPending_ = false;
    // Where the demo build's mark currently stands, and which ten-second
    // step put it there - see DrawDemoWatermark. The cell is an index into
    // its own 3x3 grid rather than a pixel position, so a resolution change
    // moves the mark with the screen instead of stranding it off the edge.
    // Dead weight in a non-demo build, and 20 bytes of it.
    int64_t demoWatermarkMove_ = -1;
    int demoWatermarkCell_ = 0;
    ImVec2 demoWatermarkJitter_{0.5f, 0.5f};
    bool itemPropertiesPopoverRequested_ = false;

    // Overview (canvas switcher / manager / move-copy picker).
    bool overviewOpen_ = false;
    // Which of the Overview's two tabs is showing - Canvases (the
    // original/default content: folder sidebar + canvas tile grid) or
    // Settings (RenderOverviewSettingsPanel, added once there were enough
    // in-app-relevant AppConfig fields - showDebugOverlay, the colours,
    // etc. - to be worth a UI rather than only a hand-edited config.json
    // line). Not persisted - purely which tab is showing right now, reset
    // to Canvases every time OpenOverview runs (see its own doc comment)
    // so the switcher's own primary purpose is always what greets you.
    // Never shown at all while pickerItemId_ is set (see RenderOverview) -
    // picking a move/copy destination has its own single-purpose header in
    // its place, and Settings has no business being reachable mid-pick.
    enum class OverviewTab { Canvases, Settings, About };
    OverviewTab overviewTab_ = OverviewTab::Canvases;
    // Switching tabs, plus the housekeeping that goes with it: the new tab
    // starts at the top of its own content, and About starts on About
    // rather than wherever its second page was left. One function so a
    // fourth tab, if there is ever one, cannot forget either. Declared here
    // rather than up with the other render helpers because a member
    // function cannot name a nested type declared after it, and
    // OverviewTab belongs with the state it describes.
    void SwitchOverviewTab(OverviewTab tab);
    // Whether the About tab is showing the third-party licences instead of
    // its usual contents. A second page of the same tab rather than a tab
    // of its own: the licences have to be *reachable*, not prominent, and a
    // permanent fourth entry in the tab row would charge every visit to
    // Canvases and Settings for something read once, if ever. Not persisted
    // - a fresh About always opens on About.
    bool aboutShowsNotices_ = false;
    // Whether the Canvases tab shows what is deleted alongside what is not:
    // deleted folders in the sidebar and deleted canvases in the grid,
    // marked out in red with Restore and Delete permanently on each, and
    // everything else dimmed. Not persisted, and off whenever the Overview
    // opens. See ShowingDeleted.
    bool showDeleted_ = false;
    // A deleted folder picked in the sidebar while Show deleted is on, whose
    // canvases the grid shows. Kept here rather than as the browsed folder:
    // that is where a new canvas lands, and the manager never lets it be a
    // deleted one (see CanvasManager::SettleOffDeleted). See
    // SettleDeletedFolderShown for when it lets go.
    std::optional<FolderId> deletedFolderShown_;
    // Set by anything that changes what the body is showing; consumed by
    // the body itself on its next frame. All three tabs and both About
    // pages share one scrolling child, so a page arrived at from halfway
    // down another one would otherwise open halfway down - and the switch
    // is asked for from outside that child (a tab button above it, a
    // footer button below it), where SetScrollY would scroll the panel
    // instead. See SwitchOverviewTab.
    bool overviewBodyScrollToTop_ = false;
    // Which body the Settings tab shows. Not reset with the panel: coming
    // back to Settings usually means coming back to the same section, and
    // the list down the side makes where you are obvious anyway.
    //
    // Appearance/Drawing/Diagnostics are about you and are global;
    // Input/Shortcuts are about whatever is underneath, and are what a
    // per-application profile may override - see
    // RenderOverviewSettingsPanel.
    enum class SettingsSection { Appearance, Interaction, Input, Hotkeys, Profiles, Debug };
    SettingsSection settingsSection_ = SettingsSection::Appearance;
    // Whose values the Input and Shortcuts sections are showing. Nullopt is
    // the defaults; otherwise an index into Settings::Profiles. Reset to the
    // active profile every time the overlay is shown (see OnOverlayShown):
    // coming back to Settings over a
    // game almost always means coming back to that game's settings, and the
    // picker says which either way.
    std::optional<size_t> editProfile_;
    // Set only when the Overview was opened via an item's Move/Copy pill
    // button - picking a tile then moves/copies this item there instead of
    // just switching to it.
    std::optional<ItemId> pickerItemId_ = std::nullopt;
    bool pickerIsCopy_ = false;

    // In-place rename of a folder row or canvas tile name, triggered by a
    // double-click on it (see RenderOverview). At most one of the two ids
    // is ever set; `renameBuffer_` holds the in-progress edit for whichever
    // one it is. `renameJustFocused_` is consumed the first frame after a
    // rename starts, to call ImGui::SetKeyboardFocusHere() exactly once.
    std::optional<FolderId> renamingFolderId_ = std::nullopt;
    std::optional<CanvasId> renamingCanvasId_ = std::nullopt;
    char renameBuffer_[128] = {};
    bool renameJustFocused_ = false;
    // A canvas the Overview should bring into view the next time it draws
    // its grid, then forget - set whenever one is created, since a new
    // canvas goes to the end of its folder (see CanvasManager::AddCanvas)
    // and a folder with more canvases than fit on screen would otherwise
    // put it out of sight, below the fold, with nothing to say it worked.
    // Consumed by the tile loop in RenderOverview, whether or not the
    // canvas is in the folder currently being browsed.
    std::optional<CanvasId> overviewScrollToCanvasId_ = std::nullopt;
    // The same for the folder sidebar, and for the same reason: a new
    // folder goes to the end of the list (see CanvasManager::AddFolder),
    // which in a library with a few of them is past the bottom of a
    // sidebar whose "New folder" button is right there under it.
    std::optional<FolderId> overviewScrollToFolderId_ = std::nullopt;

    // Live text editing of a Note item's body - the same shape as the
    // rename state just above (at most one item being edited at a time,
    // an edit buffer, a just-focused flag consumed once), see
    // BeginEditingNote/EndEditingNote and RenderNoteEditor's editor
    // window. The buffer is a string the widget grows as it types (see
    // RenderNoteEditor), not a fixed array: a note is whatever its record
    // says it is, and a fixed 8 KB buffer silently truncated a longer one
    // the moment it was opened for editing.
    std::optional<ItemId> editingNoteItemId_ = std::nullopt;
    std::string noteEditBuffer_;
    bool noteEditJustFocused_ = false;

    // Small transient "Moved to X" / "Copied to X" banner after a
    // move/copy - text empty or ImGui::GetTime() past the expiry means
    // nothing to draw (see RenderActionToast).
    std::string actionToastText_;
    double actionToastExpireAtSeconds_ = 0.0;
    // The settings file the tray last failed to write, while it stays
    // unwritten - see SetConfigWriteFailed.
    std::optional<std::string> configWriteFailedPath_;
    // The pacing last handed to the window - see OnFrame, which decides it
    // each frame and passes it on only when it changes.
    std::optional<platform::FramePacing> appliedFramePacing_;
    // The accent last applied to the theme and the ImGui style - see
    // OnFrame, which applies AppConfig::accentColorRGBA whenever it differs,
    // so a colour being dragged in Settings recolours everything live.
    std::optional<uint32_t> appliedAccentRGBA_;

    // Transient "this is how big it is now" preview at the cursor, armed
    // by a mouse-wheel size change and expiring on its own shortly after
    // - ImGui::GetTime() past this means nothing to draw (see
    // RenderBrushSizePreview). Deliberately transient rather than a
    // permanent brush-outline cursor: this overlay sits on top of a game,
    // where a ring that follows the pointer forever is exactly the sort of
    // always-there element that makes an overlay feel busy. No
    // companion size/tool field - the renderer reads the live
    // drawWidth_/eraserWidth_/activeTool_, so a burst of wheel steps shows
    // the current value throughout rather than a snapshot of the first.
    double sizePreviewExpireAtSeconds_ = 0.0;

    // Leftover fractions of a wheel notch, carried across frames so the
    // wheel honors how far it was actually turned. Two things make this
    // more than a plain `+= io.MouseWheel`: several notches can land in a
    // single frame on a fast spin (taking one step per frame silently
    // dropped the rest), and a high-resolution wheel or precision touchpad
    // reports *fractions* of a notch, which truncating per-frame would
    // round to nothing and leave the wheel feeling dead. Separate
    // accumulators per gesture rather than one shared: a half-notch left
    // over from resizing a brush must not count toward a canvas switch.
    float sizeWheelRemainder_ = 0.0f;
    float canvasWheelRemainder_ = 0.0f;

    // See RequestWelcomeNote/PlaceWelcomeNote. Cleared the moment the note
    // is placed, so it can never be placed twice.
    bool welcomeNotePending_ = false;
};

}  // namespace sz::ui
