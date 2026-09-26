#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ui/context_menu.h"
#include "ui/editor.h"
#include "ui/interaction/gestures.h"
#include "ui/icon_draw.h"
#include "ui/interaction/command.h"
#include "core/build_info/build_info.h"
#include "core/canvas/canvas_manager.h"
#include "core/canvas/item.h"
#include "core/config/app_config.h"
#include "core/config/settings_catalog.h"
#include "core/drawing/draw_tool.h"
#include "core/drawing/stroke.h"
#include "core/drawing/stroke_bitmap.h"
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

// What the overlay draws, and whether it takes input: one of the states of
// docs/OVERLAY_STATES.md that are up. Hidden is not a mode - nothing is
// drawn then - so the overlay keeps the last one. See OverlayApp::SetMode.
enum class OverlayMode {
    // Everything, interactive.
    Edit,
    // The current canvas, read-only.
    View,
    // The current canvas's pinned snippets (Item::pinned) and nothing else
    // - what is left on screen when the overlay is put away with any there.
    Pinned,
    // Only the message a hotkey just set (see ShowActionToast), for a
    // hotkey that acts while the overlay is hidden. Over when the message
    // is - see SetNoticeFinishedCallback.
    Notice,
};

// How a snippet's pictures are resampled: the setting (AppConfig::
// imageFilter) and the renderer's callback that applies it, which is null
// where nothing renders. See overlay_detail::DrawPicture.
struct ImageSampling {
    platform::ImageFilter filter = platform::ImageFilter::Bilinear;
    platform::DrawCallback apply = nullptr;
};

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

// The context menus' ImGui popup ids - see ContextMenu.
inline constexpr const char* kItemContextMenuId = "##item_context_menu";
inline constexpr const char* kCanvasContextMenuId = "##canvas_context_menu";
inline constexpr const char* kEmptyCanvasMenuId = "##empty_canvas_menu";

// Owns the canvas/item UI and renders it into whatever IOverlayWindow it's
// attached to via Dear ImGui. Contains no OS-specific code: rendering is
// entirely through ImGui's platform-agnostic API. What it draws - the
// selection, the tool, drawing mode, the commands - is the Editor's (see
// editor_), which it is the view of.
//
// No always-visible chrome at the *canvas* level: snippets are objects
// with a selection, the way a drawing program's are (see Editor::Selection), and
// drawing on one is a mode entered by double-clicking or holding on it
// (see Editor::DrawingItem); everything that acts on a snippet is on the bar that
// floats over the selection (see PaintSelectionBar), and everything else
// is a key or the Overview. Input arrives two ways accordingly:
//  - Everything over the canvas - freehand pen/eraser strokes, the "double
//    click or hold for fullscreen, drag for a region" item-creation
//    gesture, and the selection's own gestures (a press on a snippet, on a
//    selected snippet's handle, or on the selection bar - see
//    ui/interaction/recognizer.cpp) - is driven by the window's input
//    stream (see OnInput: as it arrives, decoupled from render/frame rate),
//    through the editor's interactions (docs/INTERACTIONS.md). What a press
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
class OverlayApp : private EditorViews {
public:
    // A view of `session`, editing it and the `settings` - both outlive it;
    // TrayController owns all three. Nothing is copied out: every frame reads
    // the models and the settings as they are.
    OverlayApp(Settings& settings, Session& session);

    // Subscribes to the window's frame and input callbacks. The window is not
    // required to exist yet in the OS sense; callbacks simply won't fire
    // until the platform layer creates and shows it.
    void AttachTo(platform::IOverlayWindow& window);

    // Sets what is drawn and whether input is taken - TrayController's, as
    // it moves the overlay between its states (docs/OVERLAY_STATES.md,
    // section 6, step 5). Leaving Edit for any other mode offers the
    // ViewOnly lifecycle event and ends everything above the canvas (an
    // armed item, a note being typed, a popup, a panel, drawing mode), so
    // edit mode later starts clean rather than wherever it was left off;
    // entering Edit offers EditMode. The other modes are all read-only,
    // take no input, and switch among themselves without either.
    void SetMode(OverlayMode mode);
    OverlayMode Mode() const { return mode_; }

    // The overlay has just been put back on screen. Anything this class
    // remembers about state the OS owns is stale at that moment - the
    // installed pointer shape, since while the overlay was hidden the
    // application underneath owned the cursor, and which buttons and keys
    // are down (see Editor::ForgetTheHand). Distinct from SetMode, which
    // offers nothing when the mode is unchanged and so nothing on a plain
    // hide-then-show.
    void OnOverlayShown();
    // Every mode but Edit: read-only, taking no input.
    bool IsViewOnly() const { return mode_ != OverlayMode::Edit; }
    bool IsNoticeOnly() const { return mode_ == OverlayMode::Notice; }
    bool IsPinnedOnly() const { return mode_ == OverlayMode::Pinned; }
    // Called once, from the frame in which a notice's message has faded,
    // to say there is nothing left to show. TrayController hides the
    // window after that frame.
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
    // At startup, when the retention period deleted `count` folders and
    // canvases for good: said the next time the overlay comes up, rather
    // than while nobody is looking at it - see OnOverlayShown.
    void SayDeletedForGoodAtStart(size_t count, int days);
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
    Tool ActiveTool() const { return editor_.ActiveTool(); }
    // The color the next stroke would use. Public for the same reason
    // ActiveTool is: it is what a test asks instead of reading pixels.
    uint32_t DrawColorRGBA() const { return editor_.DrawColorRGBA(); }
    // What the next left press places, if anything: the creation gesture in
    // flight, or else what the tool in hand places (see CreationKindFor).
    std::optional<ItemCreationKind> ArmedCreation() const { return editor_.ArmedCreation(); }
    // Whether the snippets are faded back so that what a new snippet is
    // made from shows through them: while a creation tool is in hand, and
    // while a region is being dragged out - not for a press that has not
    // moved yet, which may still be a click, and would flicker. See
    // kCreationFadeAlpha.
    bool ItemsFadedForCreation() const {
        return CreationKindFor(editor_.ActiveTool()).has_value() ||
               editor_.Input().As<Framing>(Level::Gesture) != nullptr;
    }
    // Whether the Overview, or the cheat sheet, is up - the machine's Panel
    // level (see Panel).
    bool IsOverviewOpen() const { return PanelUp(PanelKind::Overview); }
    bool IsCheatSheetOpen() const { return PanelUp(PanelKind::CheatSheet); }
    // Whether the color chooser is up - see RenderColorChooser.
    bool IsColorChooserOpen() const { return colorChooserOpen_; }
    // Whether the snippet context menu is up, and over which snippet -
    // see RenderItemContextMenu.
    bool IsItemContextMenuOpen() const { return itemContextMenu_.IsOpen(); }
    std::optional<ItemId> ItemContextMenuItem() const { return itemContextMenuItemId_; }
    // The same for the canvas bar's tiles - see RenderCanvasContextMenu.
    bool IsCanvasContextMenuOpen() const { return canvasContextMenu_.IsOpen(); }
    std::optional<CanvasId> CanvasContextMenuCanvas() const { return canvasContextMenuCanvasId_; }
    // And for empty canvas - see RenderEmptyCanvasMenu.
    bool IsEmptyCanvasMenuOpen() const { return emptyCanvasMenu_.IsOpen(); }
    // How far out the canvas bar is, 0 to 1 - see EdgeReveal and
    // UpdateEdgePanels.
    float CanvasBarReveal() const { return canvasBarReveal_.amount; }
    // Whether a note is being typed into, and which.
    std::optional<ItemId> EditingNote() const { return editor_.EditingNote(); }
    // Which resize handle of which item thinks the cursor is on it right
    // now, empty for none - the same string the debug overlay draws (see
    // debugHoveredResizeHandle_). Exposed because "whose handle is live
    // where two items overlap" is a question about occlusion that a
    // screenshot answers badly and a test answers exactly.
    const std::string& DebugHoveredResizeHandle() const { return debugHoveredResizeHandle_; }
    // The selected snippets, in the order they were selected - see
    // Editor::Selection.
    const std::vector<ItemId>& Selection() const { return editor_.Selection(); }
    bool IsSelected(ItemId id) const { return editor_.IsSelected(id); }
    // The snippet in drawing mode, if one is - see Editor::DrawingItem.
    std::optional<ItemId> DrawingItem() const { return editor_.DrawingItem(); }
    // What the drawing bar's pen and eraser draw or erase on a plain drag -
    // see Editor::PenShape.
    DrawShape PenShape() const { return editor_.PenShape(); }
    DrawShape EraserShape() const { return editor_.EraserShape(); }
    // Where the middle of a selection bar button is this frame, or nothing
    // while the bar is not showing - for a test to press it where it is
    // rather than where it computes it to be.
    std::optional<ImVec2> SelectionBarButtonCenter(ChromeButton button) const;

    // Captures a fullscreen screenshot onto a canvas made for it (see the
    // definition, and the other declaration of this below).
    // Doesn't touch the overlay's visibility or mode itself - it's purely
    // the capture step; TrayController::QuickCaptureAndShow switches to
    // edit mode afterward so the capture is noticed. Safe to call while
    // hidden: needs no active ImGui frame (it's plain CanvasManager/
    // IOverlayWindow calls), and `window`'s own CaptureRegion leaves the
    // overlay out of the capture whether it is up or not.
    // A fullscreen screenshot onto a canvas of its own: a new one at the
    // end of the folder the current canvas lives in, switched to, with the
    // shot on it and a message saying so. Both capture hotkeys come here -
    // the difference between them is only whether the overlay is shown
    // afterwards (see TrayController::QuickCaptureAndShow and
    // SilentCapture).
    //
    // A canvas each rather than all of them piling onto whatever canvas
    // was current: every capture is the same size and shape, so stacked
    // they hide each other, while one per canvas is a row of tiles in the
    // Overview that can be told apart at a glance. The user's own call.
    void QuickCapture(float displayW, float displayH) { editor_.QuickCapture(displayW, displayH); }

    // The overlay going away - hidden, restarted, the app exiting, the OS
    // ending the session: offered to the input machine, and then what a
    // command's Hand scope ends is ended (see Editor::Settle and
    // docs/ARCHITECTURE.md, "The hand"), and a drawing nothing went into is
    // discarded (see Editor::SettleUntouchedDrawing).
    void SettleForPersistence(Lifecycle why = Lifecycle::Hidden);

    // ===== Commands =====
    //
    // Runs `command` if it can act now (see Available), after ending what
    // its scope covers (see Scope). Every key, context menu
    // row, selection bar button and global hotkey reaches the app through
    // here - see ui/interaction/command.h. True when it ran.
    bool Dispatch(const Command& command) { return editor_.Dispatch(command); }
    // A global hotkey, bound to `combo`, which runs `command`: offered to
    // the machine as an event of its own (docs/INTERACTIONS.md, section 7),
    // which every level passes on to the command. May come while the
    // overlay is hidden, with no frames.
    void OnHotkey(CommandId command, const platform::KeyCombo& combo);
    // Whether `command` would do anything now: what grays a menu row out,
    // and what a command is asked before it ends anything. Asked before
    // settling, so it never depends on what settling would file - undo is
    // always available, since a stroke in flight is on the history only
    // once it has been settled.
    bool Available(const Command& command) const { return editor_.Available(command); }
    // Where the global hotkeys' commands run: the tray, which alone knows
    // the window and the modes. Left null - a test that drives the overlay
    // alone - they do nothing.
    void SetAppCommandCallback(std::function<void(CommandId)> callback) {
        editor_.SetAppCommandCallback(std::move(callback));
    }
    // How many commands have run, and which ran last - what a test asks
    // to learn whether a key or a hotkey did anything, rather than reading
    // its effects.
    uint64_t CommandsRun() const { return editor_.CommandsRun(); }
    std::optional<CommandId> LastCommand() const { return editor_.LastCommand(); }

    // Whether the hand has nothing in flight - see Editor::HandAtRest.
    // What a test asks after a command.
    bool HandAtRest() const { return editor_.HandAtRest(); }
    // The stack of interactions, bottom to top, as names - see
    // Machine::Describe. What a test asks of the state it is in.
    std::string InputStack() const { return editor_.Input().Describe(); }

    // Asks for the first-run notes to be placed on the current
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



    // Installed once by TrayController - called from the Settings panel's
    // hotkey editor whenever the user picks a new combo for one of the
    // global hotkeys. Unlike every other setting, a hotkey has a real way to
    // fail that only the OS can tell you about (the combo's already taken
    // by another app) - so, unlike the commit's "tell the host after the
    // fact" shape, this is "ask the host first": the callback itself
    // attempts the actual RegisterGlobalHotkey/UnregisterGlobalHotkey swap,
    // makes the edit (Settings::Set) on success, and returns whether it
    // took - see TryChangeHotkey, the Settings panel's only caller of this.
    // Left null (e.g. a test), every requested change is made at once -
    // there's no real OS hotkey to fail against.
    void SetHotkeyChangeCallback(std::function<bool(HotkeySlot, platform::KeyCombo)> callback) {
        hotkeyChangeCallback_ = std::move(callback);
    }


private:
    // What the editor asks of the view - see EditorViews.
    void Say(std::string text) override { ShowActionToast(std::move(text)); }
    void OpenOverview() override;
    void OpenSettings() override;
    void OpenPicker(ItemId itemId, bool isCopy) override;
    void ToggleCheatSheet() override;
    void OpenItemProperties(ItemId item, std::optional<platform::Vec2> at) override;
    void OpenColorChooser(platform::Vec2 at) override;
    void AskToDeleteCanvas(CanvasId canvas) override;
    void CanvasMade(CanvasId canvas) override { overviewScrollToCanvasId_ = canvas; }
    void OpenItemMenu(ItemId item, platform::Vec2 at) override { OpenItemContextMenu(item, ImVec2(at.x, at.y)); }
    void OpenEmptyCanvasMenu(platform::Vec2 at) override;
    bool PointerOverView() const override;
    // Whether a panel covering the canvas is up, which the canvas's own
    // keys, wheel and pointer then leave alone - the Overview or the cheat
    // sheet.
    bool PanelOpen() const override { return editor_.Input().At(Level::Panel) != nullptr; }
    bool PanelUp(PanelKind kind) const {
        const Panel* panel = editor_.Input().As<Panel>(Level::Panel);
        return panel != nullptr && panel->Kind() == kind;
    }
    void ClosePanel(PanelKind kind) override;
    void ToolSized(bool pen) override;
    bool InputOptionsKey(const Event& event) override { return HandleInputOptionsHudKey(event); }
    void LetGoOfWidget() override { Queue(Effect{Effect::Kind::LetGoOfWidget}); }
    bool PopupOpen() const override;
    bool PopupShowing(PopupKind kind) const override;
    void ClosePopup(PopupKind kind) override;
    void CloseInnermostPopup() override;
    // Every popup this class opens is put on the machine's Popup level
    // first (ending the one that was there, which closes it), then asked
    // for - see Popup.
    void PushPopup(PopupKind kind);
    // Whether the delete confirmation was up on the last frame.
    bool confirmDeleteShown_ = false;

    // The canvas as the hand works on it: the selection, the tool, drawing
    // mode, the clipboard, and every command - see Editor. What this class
    // draws from.
    Editor editor_;

    void OnFrame(float deltaSeconds);
    // Every input event, in the order they happened - see IOverlayWindow::
    // SetInputCallback - offered to the editor's machine (see Machine),
    // with the editor told the modifiers, the time and the display first.
    void OnInput(const platform::InputEvent& event);
    // The overlay shown or put away, view-only mode entered or left: an
    // event every level is offered before the scope it calls for is ended
    // (see SetMode, SettleForPersistence and OnOverlayShown).
    void OfferLifecycle(Lifecycle which);

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
    // Keeps the window told how many number keys the HUD takes, and restarts
    // the overlay once a toggle that needs it has had its key let go of -
    // once a frame.
    void UpdateInputOptionsHud();
    // A number key the HUD advertises, as it goes down: toggles the option,
    // persists it, and asks for edit mode to be re-entered for an option
    // that only applies on entry. False for a key that is not the HUD's.
    bool HandleInputOptionsHudKey(const Event& event);
    // What a HUD row currently reads - the resolved value: the HUD reports
    // what is running - and whether it can do anything.
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
    void PaintItemBody(ImDrawList* drawList, const Item& item, bool drawing, bool highlighted,
                       bool isFrontmost);
    // What a selected snippet wears while the selection is live, *drawn*:
    // an accent outline and, unless it is fullscreen, its eight handles.
    // Nothing in it takes input. Which of it is under the pointer is
    // ResolvePointerTarget's answer, what the pointer looks like over it
    // is RenderItems', and a press on any of it is the recognizer's -
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
    // The live text editor over an item whose note is being edited - the
    // one piece of an item that is a real ImGui widget, in a window of its
    // own above the items layer. See the definition.
    void RenderNoteEditor(const Item& item, ImVec2 pMin, ImVec2 pMax);
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
    // The context menu a right-click on a canvas bar tile opens. Rendered
    // at the top level of the frame rather than inside the bar's own
    // window, like every other popup here; the bar is held out for as long
    // as the menu is up (see UpdateEdgePanels), since a menu floating over
    // the panel it belongs to having slid away would be a puzzle.
    void OpenCanvasContextMenu(CanvasId canvasId, ImVec2 at);
    void RenderCanvasContextMenu();
    void BuildCanvasContextMenuRows(const Canvas& canvas, std::vector<ContextMenuEntry>& rows) const;
    // The color chooser: one picker, and what it is set to is the color
    // drawn with. Opened by the drawing bar's color button, next to
    // `from`, the point it was pressed from; OpenColorChooser only asks,
    // since the bar's buttons fire from the input stream outside any
    // frame, and it opens on the next one (see Effect). What
    // was picked is kept as AppConfig::strokeColorRGBA when the chooser
    // closes.
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
    // opacities, the background color, and the text's size and color.
    // What is done *to* a snippet - fullscreen, copy, restack, move - is
    // the context menu's (see below), not the popover's. Each widget
    // previews its change through the session (see Session::PreviewStyle),
    // and the edit ends as the hand lets go of it.
    void RenderItemOpacity(const Item& item);
    void RenderItemBackgroundColor(const Item& item);
    void RenderItemTextStyle(const Item& item);

    // The context menu a right-click on a snippet opens - the popover's
    // actions as a list of named rows with their shortcuts beside them,
    // plus the ones that only had a key until now. Asked for from the input
    // stream, which is why opening is queued rather than done (see
    // Effect).
    void OpenItemContextMenu(ItemId itemId, ImVec2 at);
    void RenderItemContextMenu();
    // The rows, for the snippet the menu is open over. Rebuilt every frame
    // it is up, so "nothing to clear" and "nothing behind it" are answered
    // from the canvas as it is now rather than as it was when the menu
    // opened.
    void BuildItemContextMenuRows(const Item& item, std::vector<ContextMenuEntry>& rows);
    // The context menu a right click on empty canvas opens: the ways to
    // make a snippet, Paste, and the way to the Overview and Settings -
    // what the canvas itself offers, with no snippet to act on. Asked for
    // the same way as the snippet's.
    void RenderEmptyCanvasMenu();
    void BuildEmptyCanvasMenuRows(std::vector<ContextMenuEntry>& rows) const;
    // "Ctrl+D" for a command a key reaches, empty for one none does - what
    // a menu row shows on its right. FormatKeyComboLabel's "(none)" is the
    // right answer for a key editor and the wrong one here, where a row
    // without a key should simply show nothing.
    std::string MenuShortcutLabel(CommandId id) const;
    // A context menu row for `command`: grayed out when it is not
    // available (see Available), with its key beside it.
    ContextMenuEntry MenuRow(const Command& command, const char* id, const Icon* icon, const char* label,
                             bool separatorAbove = false) const;

    void RenderRegionCaptureOverlay();
    // RectEraser's own drag-preview overlay (see StrokeInFlight) -
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
    // The Overview, in the order it is drawn (Escape is its interaction's -
    // see Panel): the dimming backdrop, which closes the Overview on a click
    // outside
    // the panel; the header, which is the picker's prompt while a snippet
    // is being sent somewhere and the tabs otherwise; the body - the
    // folder sidebar and the canvas grid (with what is deleted in them, see
    // ShowingDeleted), or the Settings or About panel; and the footer, whose buttons belong to
    // whichever body is showing.
    void RenderOverview(float displayW, float displayH);
    // Dims the whole screen behind a panel - the Overview, the cheat sheet
    // - and is true for a click on it, outside the panel, which closes it.
    bool RenderPanelBackdrop(const char* windowId, float displayW, float displayH);
    // Every key and gesture, grouped, with the keys as they are bound - see
    // BuildCheatSheet. A panel over a dimmed canvas like the Overview, but
    // with nothing in it to click: Escape, its own key again, or a click
    // outside it closes it.
    void RenderCheatSheet(float displayW, float displayH);
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
    // comment): the section list, and the section picked. Every row in it is
    // bound to its setting's row in the catalog and makes its own edit as it
    // is changed - see settings_widgets.h.
    void RenderOverviewSettingsPanel();
    // The Settings tab's own bodies, one per section in its list - see
    // RenderOverviewSettingsPanel for what decides which settings live
    // where.
    void RenderSettingsAppearance();
    void RenderSettingsInteraction();
    // One bar's buttons as a row to arrange: each is a tile that switches
    // it on or off when clicked and can be dragged onto another to move it
    // there.
    void RenderBarButtonRow(const char* id, const char* label, const GlobalSetting<BarRule>& row);
    void RenderSettingsBehavior();
    // What a new snippet starts with - see AppConfig::screenshotDefaults.
    void RenderSettingsDefaults();
    void RenderSettingsDebug();
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
    // The Settings calls, aimed at whichever of the defaults or a profile
    // the panel is showing (editProfile_) - which is not necessarily the
    // profile in effect. What the section being edited currently resolves
    // to; what it would resolve to with this target's own overrides taken
    // away is Settings::Base and needs no call: a profile inherits the
    // defaults and nothing else.
    ProfileableSettings EditedSettings() const { return settings_.ResolvedFor(editProfile_); }
    // A shortcut binding's override in the target being edited: handed back
    // to the defaults, and whether there is one.
    void ClearShortcutOverride(ShortcutAction action) { settings_.ClearShortcutOverride(action, editProfile_); }
    bool IsShortcutOverriddenHere(ShortcutAction action) const {
        return settings_.IsShortcutOverridden(action, editProfile_);
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
    // `buttonX` is where the key's button starts, the same for every row
    // of the section - see KeyButtonColumn.
    void RenderHotkeyEditor(const char* id, const char* label, HotkeySlot slot, float buttonX);
    // Offers `combo` to hotkeyChangeCallback_ (see its own doc comment),
    // which stores it once the OS has registered it. Returns false (and
    // the hotkey is left untouched) if the callback rejects it - a real
    // OS-level conflict - so RenderHotkeyEditor's widgets can show the edit
    // didn't take rather than silently keeping a value nothing downstream
    // actually agreed to. A collision with one of this app's own other
    // hotkeys is not a rejection: that one is unbound instead (see
    // TrayController::ChangeHotkey).
    bool TryChangeHotkey(HotkeySlot slot, platform::KeyCombo combo);

public:
    // Whether a hotkey row is armed, waiting for the combo the user wants -
    // a KeyCapture on the machine's Text level (see KeyCapture, which also
    // says why a hotkey that fires meanwhile is the press).
    bool IsCapturingHotkey() const { return CapturingHotkey().has_value(); }
    // Ends the capture with `combo` as the answer. Nothing while none is
    // armed.
    void CompleteHotkeyCapture(platform::KeyCombo combo);
    // What clicking a hotkey row's button, or a shortcut row's, does - here
    // so a test can arm one without driving the Settings panel. Arming
    // either disarms the other: both rows are on one page, each waits for
    // the next key, and one press bound it to both.
    void ArmHotkeyCapture(HotkeySlot slot);
    void ArmShortcutCapture(ShortcutAction action);
    bool IsCapturingShortcut() const { return CapturingShortcut().has_value(); }

private:
    // The row waiting, if one is.
    std::optional<HotkeySlot> CapturingHotkey() const;
    std::optional<ShortcutAction> CapturingShortcut() const;
    // Stops a row waiting, binding nothing.
    void DisarmCapture();
    // The Settings tab's Shortcuts section: every tool and create action,
    // each with the key it answers to. A section of its own rather than
    // rows appended to another one - eleven key editors would be most of
    // whatever panel they were put in, and "what is bound to what" is a
    // question people come to answer on its own. A summon hotkey goes to
    // the OS before it is stored - see TryChangeHotkey.
    void RenderSettingsHotkeys();
    // One row of it. A row waiting takes the next key pressed, or Escape,
    // Backspace or Delete for none - see KeyCapture.
    void RenderShortcutEditor(ShortcutAction action, const Icon& icon, const char* label, float buttonX);
    float KeyButtonColumn() const;
    // Cancel/Delete confirmation for a canvas or folder Delete button
    // clicked in the Overview - see confirmDeleteTarget_'s own doc
    // comment for why canvas/folder deletion gets this extra step while
    // item/stroke deletion (DeleteItemsWithToast) stays instant + undoable.
    void RenderConfirmDeletePopover();
    void RenderActionToast();
    // See SetConfigWriteFailed.
    void RenderPersistenceWarning();

    // Places the first-run notes, centered as a group: the welcome, and the
    // two warnings beside it - see RequestWelcomeNote.
    // Ordinary items, deliberately: each can be moved, edited, or closed
    // like anything else, and they are in the library, so they stay until
    // the user is done with them and then they stop existing for good. A modal dialog would
    // have to be dismissed before the app could be touched at all, and
    // would teach nothing about how the app actually works.
    void PlaceWelcomeNotes(float displayW, float displayH);

    // ===== Rasterized vector strokes (StrokeRenderMode::Rasterized) =====

    // An item's vector strokes, drawn into a bitmap so they can be
    // composited in one go. Purely derived - the strokes are still the
    // truth, this is thrown away and rebuilt from them, and is never
    // persisted.
    struct StrokeRaster {
        StrokeBitmap pixels;
        // Moved on whenever `pixels` change: what its texture is told to
        // bring itself up to date by (see TextureCache::Get).
        uint64_t revision = 0;
        // What it was built from, kept so a stale raster is recognized by
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
    // drops every other one. A no-op in the other two render modes, which
    // also frees whatever was cached the moment the mode changes. Called
    // once a frame, after every input that could have changed what is on
    // the canvas and before anything draws it.
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
    uint64_t StrokeRasterTextureFor(ItemId itemId);
    void ReleaseStrokeRasters();

    // ===== Textures =====

    // Every texture is the session's TextureCache's, asked for by what it
    // shows as it is about to be drawn, and never held here - see
    // TextureCache for what that settles.
    TextureCache& Textures() { return session_.Textures(); }
    // A snippet's picture's texture, read from the library the first time
    // it is asked for: 0 while there are no pixels to show - none stored,
    // or they cannot be read - which draws the placeholder or the fill.
    uint64_t PictureTexture(const Item& item);
    // Asks for the textures of every snippet on the current canvas, drawn
    // this frame or not - minimized, or left out of the pinned view - so
    // that none is let go of and read again the moment it is drawn: the
    // current canvas's pictures stay on the GPU, and no other canvas's do.
    // Once a frame, before anything draws.
    void KeepCurrentCanvasTextures();

    // ===== Overview bitmap previews (AppConfig::overviewShowsBitmaps) =====

    // A picture's pixels, decoded and scaled down to thumbnail size, for the
    // canvas overview - which draws canvases that aren't current and whose
    // full-size pixels are deliberately not in memory. Kept, like every
    // texture, only while something draws it: while a panel shows its
    // canvas, so a library of screenshots costs nothing while it isn't
    // being browsed. The expensive part is the decode, not the memory - a
    // preview is at most kOverviewPreviewMaxExtent on its long edge, a
    // couple of hundred kilobytes against the eight megabytes it came from.
    //
    // Resets the per-frame decode budget - called once, at the top of the
    // overview's own rendering.
    void BeginOverviewPreviewFrame();
    // The preview texture for one snippet's picture, loading it if there is
    // budget left this frame and it hasn't already failed. Three answers,
    // and the Overview draws each differently: a handle to draw with; 0 for
    // a picture with no pixels to show at all (or whose read failed), which
    // gets
    // the placeholder gradient; and nothing at all for one whose turn to be
    // read hasn't come yet, which gets drawn as an empty tile rather than a
    // stand-in that will be replaced a few frames later. A slow library
    // fills in over those frames rather than stalling the panel.
    std::optional<uint64_t> PicturePreviewTexture(const Item& item);
    // How a preview finds a picture's pixels: PicturePreviewTexture while the
    // Overview shows bitmaps (see AppConfig::overviewShowsBitmaps), else
    // nothing - an empty lookup, which DrawCanvasPreview tests for. Shared
    // by the canvas grid, the canvas bar and the recently-deleted list.
    using PreviewTextureFn = std::function<std::optional<uint64_t>(const Item&)>;
    PreviewTextureFn PreviewTextureLookup();

    // Overview: switch canvases, delete/reorder them, or (when opened from
    // the context menu's Move to canvas) pick a target canvas for that
    // item.
    void CloseOverview();
    void ShowActionToast(std::string text);

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
    // What the popover's Delete does, and what a delete Settings > Behavior
    // says not to ask about does on the press (see AppConfig::confirmDelete).
    void PerformDelete(const ConfirmDeleteTarget& target);
    std::optional<ConfirmDeleteTarget> confirmDeleteTarget_;
    // Asks to delete `target`, through the popover unless Settings >
    // Behavior says not to ask - see OpenConfirmDelete.
    void AskToDelete(ConfirmDeleteTarget target);
    // The effect AskToDelete queues.
    void OpenConfirmDelete();


    // Applied once, on the first OnFrame call (ImGui's style/color tables
    // only exist once a context does, which isn't guaranteed yet at
    // construction or even AttachTo time - see ApplySpickzettelStyle's own
    // comment for why this couldn't just run from AttachTo).
    bool styleApplied_ = false;

    // See SetMode.
    OverlayMode mode_ = OverlayMode::Edit;
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
    const CanvasManager& Manager() const { return session_.Manager(); }
    persistence::LibraryStore* Store() const { return session_.Store(); }

    // Every setting, stored and resolved - owned by TrayController, which
    // also persists it. Plain fields are read through Cfg(), the
    // profileable ones through settings_.Live(), and every change is an
    // edit through settings_ (Settings::Set).
    Settings& settings_;
    const AppConfig& Cfg() const { return settings_.Stored(); }

    // The last count handed to IOverlayWindow::SetInputOptionsHudDigits, so
    // that only an actual change reaches the platform - it can install or
    // remove a keyboard hook, which is not something to ask for every frame.
    int appliedInputOptionsHudDigits_ = 0;
    // A HUD toggle that needs edit mode re-entered waits here until the key is
    // released - see UpdateInputOptionsHud for why holding the restart is
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
    // every change goes through the session's commands, which bump it, so
    // there is one place to get it right. nullopt means "check regardless"
    // - a fresh start, or the mode having just been switched on.
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
    // The picture filter from settings, with the callback that applies it -
    // what every drawing call that may meet a picture is handed.
    ImageSampling PictureSampling() const {
        return ImageSampling{Cfg().imageFilter, window_ != nullptr ? window_->ImageFilterCallback() : nullptr};
    }
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
    int picturePreviewLoadBudget_ = 0;
    int picturePreviewThumbnailBudget_ = 0;
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

    // Set by AttachTo; what the overlay asks of the platform itself - the
    // frame pacing, the pointer, the keyboard. Never null once AttachTo has
    // been called - a window that outlives this OverlayApp, per the
    // ownership already established by TrayController/main.
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
    // See SetNoticeFinishedCallback's own doc comment.
    std::function<void()> noticeFinishedCallback_;
    // See SetHotkeyChangeCallback's own doc comment.
    std::function<bool(HotkeySlot, platform::KeyCombo)> hotkeyChangeCallback_;
    // Whether the wheel has changed the pen's width since it was last
    // saved: it is kept as AppConfig::strokeWidth once the size preview has
    // faded, so a burst of notches is one write (see RenderBrushSizePreview).
    bool drawWidthDirty_ = false;

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
    ContextMenu itemContextMenu_{kItemContextMenuId};
    std::optional<ItemId> itemContextMenuItemId_ = std::nullopt;
    // The canvas bar's own, and which tile's canvas it is up for.
    ContextMenu canvasContextMenu_{kCanvasContextMenuId};
    std::optional<CanvasId> canvasContextMenuCanvasId_ = std::nullopt;
    // Empty canvas's, which is up for nothing in particular.
    ContextMenu emptyCanvasMenu_{kEmptyCanvasMenuId};
    // The color chooser - see OpenColorChooser: whether it was open on the
    // last frame, which is how its closing is noticed, and where it opens.
    bool colorChooserOpen_ = false;
    ImVec2 colorChooserAnchor_ = ImVec2(0.0f, 0.0f);
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

    // Something only a frame can do, asked for from wherever - between
    // frames included - and done in the next frame, just before the popups
    // are drawn: opening a popup. ImGui::OpenPopup scopes its id against the window being
    // drawn, and between frames there is none - a bar button fires from
    // the input stream, before the frame's NewFrame, and OpenPopup there
    // dereferenced an empty id stack (a verified crash, not a hypothetical
    // one). Asked for inside a frame, it waits too, which keeps every
    // popup's id at the top level of the frame whatever nesting it was
    // asked from. Not at the very start of the frame: opened there, the
    // canvas bar's menu was closed again before it was drawn - ImGui wants
    // a popup opened after the frame's other windows, shortly before it is
    // begun. What it opens on - which snippet, which canvas - is set where
    // it is asked for. See docs/INTERACTIONS.md, section 3.
    struct Effect {
        enum class Kind {
            OpenItemProperties,
            OpenItemMenu,
            OpenCanvasMenu,
            OpenEmptyCanvasMenu,
            OpenColorChooser,
            OpenConfirmDelete,
            // A popup of the machine's closed from outside (see
            // Popup::Interrupt), and the innermost popup open closed, as
            // Escape does to it - which may be one of ImGui's own inside it.
            ClosePopup,
            CloseInnermostPopup,
            // ImGui's active widget let go of, and a drag and drop dropped -
            // see Widget.
            LetGoOfWidget,
        };
        Kind kind = Kind::OpenItemProperties;
        ImVec2 at{0.0f, 0.0f};  // where a menu or the color chooser opens
        PopupKind popup = PopupKind::ItemMenu;  // which, for ClosePopup
    };
    // Asked twice before a frame, done once - as asked the second time, and
    // in its place: a popup closed and asked for again is up.
    void Queue(const Effect& effect);
    void ApplyEffects();
    std::vector<Effect> effects_;

    // Which of the Overview's two tabs is showing - Canvases (the
    // original/default content: folder sidebar + canvas tile grid) or
    // Settings (RenderOverviewSettingsPanel, added once there were enough
    // in-app-relevant AppConfig fields - showDebugOverlay, the colors,
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
    // Whether the About tab is showing the third-party licenses instead of
    // its usual contents. A second page of the same tab rather than a tab
    // of its own: the licenses have to be *reachable*, not prominent, and a
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
    enum class SettingsSection { Appearance, Interaction, Behavior, Defaults, Hotkeys, Profiles, Debug };
    SettingsSection settingsSection_ = SettingsSection::Appearance;
    // Whose values the Input and Shortcuts sections are showing. Nullopt is
    // the defaults; otherwise an index into Settings::Profiles. Reset to the
    // active profile every time the overlay is shown (see OnOverlayShown):
    // coming back to Settings over a
    // game almost always means coming back to that game's settings, and the
    // picker says which either way.
    std::optional<size_t> editProfile_;
    // A profile's name field while it says another profile's name, which
    // the rename refused: the row, and what was typed - said under the field
    // until it lets go. See RenderProfileRow.
    std::optional<std::pair<size_t, std::string>> takenProfileName_;
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
    // Starts renaming a folder or a canvas, from `name` - and puts a
    // NameEdit on the machine's Text level for as long as it lasts.
    void BeginRenaming(std::optional<FolderId> folder, std::optional<CanvasId> canvas, const std::string& name);
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


    // Small transient "Moved to X" / "Copied to X" banner after a
    // move/copy - text empty or ImGui::GetTime() past the expiry means
    // nothing to draw (see RenderActionToast).
    std::string actionToastText_;
    double actionToastExpireAtSeconds_ = 0.0;
    // See SayDeletedForGoodAtStart.
    std::string messageForNextShow_;
    // The settings file the tray last failed to write, while it stays
    // unwritten - see SetConfigWriteFailed.
    std::optional<std::string> configWriteFailedPath_;
    // The pacing last handed to the window - see OnFrame, which decides it
    // each frame and passes it on only when it changes.
    std::optional<platform::FramePacing> appliedFramePacing_;
    // The accent last applied to the theme and the ImGui style - see
    // OnFrame, which applies AppConfig::accentColorRGBA whenever it differs,
    // so a color being dragged in Settings recolors everything live.
    std::optional<uint32_t> appliedAccentRGBA_;
    // The interface scale the style was last built for, in percent - see
    // OnFrame.
    int appliedUiScalePercent_ = 0;

    // Transient "this is how big it is now" preview at the cursor, armed
    // by a mouse-wheel size change and expiring on its own shortly after
    // - ImGui::GetTime() past this means nothing to draw (see
    // RenderBrushSizePreview). Deliberately transient rather than a
    // permanent brush-outline cursor: this overlay sits on top of a game,
    // where a ring that follows the pointer forever is exactly the sort of
    // always-there element that makes an overlay feel busy. No
    // companion size/tool field - the renderer reads the live
    // editor's tool and its size, so a burst of wheel steps shows
    // the current value throughout rather than a snapshot of the first.
    double sizePreviewExpireAtSeconds_ = 0.0;



    // See RequestWelcomeNote/PlaceWelcomeNotes. Cleared the moment the notes
    // are placed, so they can never be placed twice.
    bool welcomeNotePending_ = false;
};

}  // namespace sz::ui
