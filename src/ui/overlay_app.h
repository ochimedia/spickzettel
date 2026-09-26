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
#include "ui/item_painting.h"
#include "ui/view/canvas_bar.h"
#include "ui/view/cheat_sheet.h"
#include "ui/view/overview_panel.h"
#include "ui/view/popups.h"
#include "ui/view/screen_chrome.h"
#include "ui/view/settings_page.h"
#include "ui/view/view_host.h"
#include "ui/view_action.h"
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
class OverlayApp : private EditorViews, private ViewHost {
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
    // Whether the color chooser is up - see Popups.
    bool IsColorChooserOpen() const { return popups_.Up(PopupKind::ColorChooser); }
    // Whether the snippet context menu is up, and over which snippet.
    bool IsItemContextMenuOpen() const { return popups_.Up(PopupKind::ItemMenu); }
    std::optional<ItemId> ItemContextMenuItem() const { return popups_.ItemOf(PopupKind::ItemMenu); }
    // The same for the canvas bar's tiles.
    bool IsCanvasContextMenuOpen() const { return popups_.Up(PopupKind::CanvasMenu); }
    std::optional<CanvasId> CanvasContextMenuCanvas() const { return popups_.CanvasOf(PopupKind::CanvasMenu); }
    // And for empty canvas.
    bool IsEmptyCanvasMenuOpen() const { return popups_.Up(PopupKind::EmptyCanvasMenu); }
    // How far out the canvas bar is, 0 to 1 - see CanvasBar.
    float CanvasBarReveal() const { return canvasBar_.Reveal(); }
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
    // took - see SettingsPage::TryChangeHotkey, its only caller.
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
    void OpenItemProperties(ItemId item, std::optional<platform::Vec2> at) override {
        popups_.OpenItemProperties(item, at);
    }
    void OpenColorChooser(platform::Vec2 at) override { popups_.OpenColorChooser(ImVec2(at.x, at.y)); }
    void AskToDeleteCanvas(CanvasId canvas) override;
    void CanvasMade(CanvasId canvas) override { overview_.ScrollToCanvas(canvas); }
    void OpenItemMenu(ItemId item, platform::Vec2 at) override { popups_.OpenItemMenu(item, ImVec2(at.x, at.y)); }
    void OpenEmptyCanvasMenu(platform::Vec2 at) override { popups_.OpenEmptyCanvasMenu(ImVec2(at.x, at.y)); }
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
    bool InputOptionsKey(const Event& event) override { return chrome_.HandleKey(event, !IsViewOnly()); }
    void LetGoOfWidget() override { popups_.LetGoOfWidget(); }
    bool PopupOpen() const override;
    // Asked for, a popup is up - including the frames before one draws it,
    // which is what the machine's Popup asks on every tick (see Popup::Offer).
    bool PopupShowing(PopupKind kind) const override { return popups_.Up(kind); }
    void ClosePopup(PopupKind kind) override { popups_.Close(kind); }
    void CloseInnermostPopup() override { popups_.CloseInnermost(); }

    // The canvas as the hand works on it: the selection, the tool, drawing
    // mode, the clipboard, and every command - see Editor. What this class
    // draws from.
    Editor editor_;

    // ===== The frame (docs/VIEW_LAYER.md, section 5) =====
    //
    // Every frame runs these stages in order - see OnFrame. Nothing is
    // changed by being drawn: what the frame's own state calls for is done
    // in Prepare, before anything is drawn from it, and what a widget asks
    // for in Apply, after everything is.
    void OnFrame(float deltaSeconds);
    // 1. The interface scale, the style, the pacing, the textures, the
    // library fitted to the display, and in edit mode the hand's
    // housekeeping and where the canvas bar is.
    void Prepare(float displayW, float displayH);
    // 2. The canvas, the snippets, the note editor, the dock, the canvas bar.
    void DrawCanvas(float displayW, float displayH);
    // 3. Open: the effect queue (see Popups::ApplyEffects). 4. The popups over the
    // canvas.
    void DrawPopups(float displayW, float displayH);
    // 5. What sits over the canvas and takes no input: the drag previews,
    // the size preview, the badge, the HUD, the border and the demo mark.
    void DrawOverCanvas(float displayW, float displayH);
    // 6. The Overview, the cheat sheet, the delete confirmation.
    void DrawPanels(float displayW, float displayH);
    // 7. The message and the persistence warning.
    void DrawMessages();
    // 8. The stack: every window drawn brought to the front in section 3's
    // order, the popups ImGui opened inside one just above it.
    void StackSurfaces();
    // 9. The pointer's shape, and the software pointer.
    void DrawPointer();
    // 10. The actions recorded in 2 to 6, in order; the pen's width once its
    // preview has faded; the HUD's restart; a notice's end.
    void Apply();

    // What the owners of the surfaces ask of this class - see ViewHost.
    // Act records what a widget asks for, for Apply - see ViewAction.
    void Act(ViewAction action) override { actions_.push_back(std::move(action)); }
    platform::IOverlayWindow* Window() const override { return window_; }
    std::vector<platform::DisplayInfo> ListDisplays() override;
    bool ChangeHotkey(HotkeySlot slot, platform::KeyCombo combo) override;
    void OpenCanvasMenu(CanvasId canvas, ImVec2 at) override { popups_.OpenCanvasMenu(canvas, at); }
    void RestartOverlay() override {
        if (restartOverlayCallback_) {
            restartOverlayCallback_();
        }
    }
    PreviewDrawing Previews() override;
    void Do(const ViewAction& action);
    std::vector<ViewAction> actions_;
    // Every input event, in the order they happened - see IOverlayWindow::
    // SetInputCallback - offered to the editor's machine (see Machine),
    // with the editor told the modifiers, the time and the display first.
    void OnInput(const platform::InputEvent& event);
    // The overlay shown or put away, view-only mode entered or left: an
    // event every level is offered before the scope it calls for is ended
    // (see SetMode, SettleForPersistence and OnOverlayShown).
    void OfferLifecycle(Lifecycle which);

    void RenderCanvasLayer(float displayW, float displayH);  // live layer + armed-item overlay + debug text
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
    // Beside the pointer while Draw or Erase is in hand over a snippet and
    // a modifier changes what a press would make - a line or a rectangle -
    // a small glyph of it, so the modifiers are not a secret.
    void RenderToolModifierBadge();

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
public:
    // A hotkey row armed, waiting for the combo the user wants, and the rest
    // of what a test asks of the Settings page's key rows - see SettingsPage.
    bool IsCapturingHotkey() const { return settingsPage_.IsCapturingHotkey(); }
    void CompleteHotkeyCapture(platform::KeyCombo combo) { settingsPage_.CompleteHotkeyCapture(combo); }
    void ArmHotkeyCapture(HotkeySlot slot) { settingsPage_.ArmHotkeyCapture(slot); }
    void ArmShortcutCapture(ShortcutAction action) { settingsPage_.ArmShortcutCapture(action); }
    bool IsCapturingShortcut() const { return settingsPage_.IsCapturingShortcut(); }

private:
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
    PreviewTextureFn PreviewTextureLookup();

    void ShowActionToast(std::string text);

    // What the popover's Delete does, and what a delete Settings > Behavior
    // says not to ask about does (see AppConfig::confirmDelete) - the Delete
    // action's.
    void PerformDelete(const DeleteTarget& target);
    // A Delete button that asks first - a canvas or folder, or Delete
    // permanently on one with Show deleted on: the confirmation, unless
    // Settings > Behavior says not to ask, and then the delete itself, as an
    // action. A snippet on screen doesn't ask: its delete is undoable
    // instead (see Session::DeleteItem).
    void AskToDelete(DeleteTarget target) override;

    // The pen's width and color, kept as AppConfig::strokeWidth and
    // strokeColorRGBA: the width the wheel left once its size preview has
    // faded, and the color the chooser was left on when it closes (see
    // Popups) - and both when the overlay settles, before either could come
    // (see SettleForPersistence and SetMode).
    void KeepPenWidth();
    void KeepPen();


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
    // See SetDisplayListCallback.
    std::function<std::vector<platform::DisplayInfo>()> displayListCallback_;
    // See SetNoticeFinishedCallback's own doc comment.
    std::function<void()> noticeFinishedCallback_;
    // See SetHotkeyChangeCallback's own doc comment.
    std::function<bool(HotkeySlot, platform::KeyCombo)> hotkeyChangeCallback_;
    // Whether the wheel has changed the pen's width since it was last
    // saved: it is kept as AppConfig::strokeWidth once the size preview has
    // faded, so a burst of notches is one write (see KeepPenWidth).
    bool drawWidthDirty_ = false;

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

    // ===== The owners of the surfaces (docs/VIEW_LAYER.md, section 7) =====
    //
    // Last, so that everything they are handed is there before them.
    SettingsPage settingsPage_{settings_, editor_, *this};
    OverviewPanel overview_{session_, settings_, editor_, *this};
    CheatSheet cheatSheet_{settings_, editor_, *this};
    Popups popups_{session_, settings_, editor_, *this};
    CanvasBar canvasBar_{session_, settings_, editor_, *this};
    ScreenChrome chrome_{session_, settings_, *this};
};

}  // namespace sz::ui
