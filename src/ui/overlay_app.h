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
#include "ui/view/canvas_view.h"
#include "ui/view/cheat_sheet.h"
#include "ui/view/messages.h"
#include "ui/view/overview_panel.h"
#include "ui/view/pointer.h"
#include "ui/view/popups.h"
#include "ui/view/screen_chrome.h"
#include "ui/view/settings_page.h"
#include "ui/view/tutorial_card.h"
#include "ui/view/tutorial_world.h"
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
    // Only the message a hotkey just set (see Messages), for a
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
// floats over the selection (see CanvasView::PaintSelectionBar), and everything else
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
//    "Nothing over the canvas is hit-tested by ImGui").
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
    // section 6, step 5). Leaving Edit for any other mode settles with the
    // All scope, which ends everything above the canvas (a gesture, a note
    // being typed, a popup, a panel, drawing mode), so edit mode later
    // starts clean rather than wherever it was left off. The other modes
    // are all read-only, take no input, and switch among themselves
    // without settling.
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
    void DismissActionToast() { messages_.Dismiss(); }
    // At startup, when the retention period deleted `count` folders and
    // canvases for good: said the next time the overlay comes up, rather
    // than while nobody is looking at it - see OnOverlayShown.
    void SayDeletedForGoodAtStart(size_t count, int days) { messages_.SayDeletedForGoodAtStart(count, days); }
    // What that message currently says, empty for none - the readable half
    // of the pair above, and how a test asks whether something was said at
    // all rather than looking at pixels.
    const std::string& ActionToastText() const { return messages_.Text(); }

    // ===== When the disk says no =====
    //
    // What is on screen looks saved whether or not it is, so a save that
    // failed is said out loud, and kept on screen for as long as it stays
    // failed: a line along the bottom naming the library (see
    // Messages), rather than a toast that fades while the
    // problem does not. The same line carries a settings file that could
    // not be written, which the tray reports here.
    void SetConfigWriteFailed(std::optional<std::string> path) { messages_.SetConfigWriteFailed(std::move(path)); }
    // The warning as it would be drawn this frame, or empty when there is
    // nothing wrong - for a test, and for anything else that has to know.
    std::string PersistenceWarning() const { return messages_.PersistenceWarning(); }
    // A settings file left as it is for the run - see
    // TrayController::StartOnStandInSettings - which is no failure to say
    // along the bottom: Settings says that what is changed there is not
    // saved (see SettingsPage::SetFileKept).
    void SetConfigFileKept(core::ConfigSource why, const std::string& path) { settingsPage_.SetFileKept(why, path); }
    const std::string& ConfigFileKeptNotice() const { return settingsPage_.FileKeptNotice(); }
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
    bool ItemsFadedForCreation() const { return canvasView_.ItemsFadedForCreation(); }
    // Whether the Overview, or the cheat sheet, is up - the machine's Panel
    // level (see Panel).
    bool IsOverviewOpen() const { return PanelUp(PanelKind::Overview); }
    bool IsCheatSheetOpen() const { return PanelUp(PanelKind::CheatSheet); }
    // Whether the Overview is up on its Settings tab.
    bool IsOverviewOnSettings() const { return overview_.OnSettingsTab(); }
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
    const std::string& DebugHoveredResizeHandle() const { return canvasView_.DebugHoveredResizeHandle(); }
    // Whether a snippet's strokes are held drawn into a bitmap, in the
    // rasterized mode - memory a test cannot see on screen, since a bitmap
    // nothing draws has no texture.
    bool HasStrokeRaster(ItemId id) const { return canvasView_.HasStrokeRaster(id); }
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
    // Where an anchored widget was drawn in the last frame, if it was -
    // see AnchorBoard.
    std::optional<AnchorRect> AnchorAt(Anchor anchor) const { return anchors_.Find(anchor); }
    // What the tutorial reads of the app - see AppWorld.
    const tutorial::World& TutorialWorld() const { return tutorialWorld_; }
    // The tutorial: started at its first step, in a folder made for it, as
    // the next frame is done (see action::StartTutorial); where it is; and
    // what its spotlight rings in the last frame.
    void StartTutorial() { Act(action::StartTutorial{}); }
    // One of the card's buttons, pressed as the card presses it - for a
    // test, as Dispatch is for a key.
    void PressTutorial(TutorialButton button) { Act(action::TutorialPress{button}); }
    // The button under the hint, if the hint up has one.
    void PressTutorialHint() {
        if (std::optional<ViewAction> action = tutorialCard_.HintAction()) {
            Act(std::move(*action));
        }
    }
    const tutorial::Tutorial& TutorialRunner() const { return tutorialCard_.Runner(); }
    std::optional<AnchorRect> TutorialSpot() const { return tutorialCard_.SpotRect(); }

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

    // Whatever is in flight, finished as the overlay goes away, comes up or
    // turns view-only - one sequence for all of them, in one order: what
    // `scope` covers ended, top down (see Editor::Settle and
    // docs/ARCHITECTURE.md, "The hand"); and what only a frame of edit mode
    // would otherwise have kept, kept now - a slider's preview, the pen as
    // the hand left it. Hand for the overlay put away,
    // restarted or coming up, which leaves drawing mode, a panel and a
    // popup as they were; All for view-only mode and for good, at exit and
    // at the end of the OS session.
    void Settle(Scope scope);

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
    void Say(std::string text) override { messages_.Say(std::move(text)); }
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
    void ToolSized(bool pen) override { pointer_.ToolSized(pen); }
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
    void Mark(Anchor anchor, ImVec2 min, ImVec2 max) override { anchors_.Mark(anchor, AnchorRect{min, max}); }
    // Where the anchored widgets were drawn this frame - cleared in
    // Prepare, marked as they are drawn. See AnchorBoard.
    AnchorBoard anchors_;
    void RestartOverlay() override {
        if (restartOverlayCallback_) {
            restartOverlayCallback_();
        }
    }
    PreviewDrawing Previews() override { return canvasView_.Previews(); }
    void Do(const ViewAction& action);
    std::vector<ViewAction> actions_;
    // Every input event, in the order they happened - see IOverlayWindow::
    // SetInputCallback - offered to the editor's machine (see Machine),
    // with the editor told the modifiers, the time and the display first.
    void OnInput(const platform::InputEvent& event);

public:
    // A hotkey row armed, waiting for the combo the user wants, and the rest
    // of what a test asks of the Settings page's key rows - see SettingsPage.
    bool IsCapturingHotkey() const { return settingsPage_.IsCapturingHotkey(); }
    void CompleteHotkeyCapture(platform::KeyCombo combo) { settingsPage_.CompleteHotkeyCapture(combo); }
    void ArmHotkeyCapture(HotkeySlot slot) { settingsPage_.ArmHotkeyCapture(slot); }
    void ArmShortcutCapture(ShortcutAction action) { settingsPage_.ArmShortcutCapture(action); }
    bool IsCapturingShortcut() const { return settingsPage_.IsCapturingShortcut(); }

private:
    // Places the first-run notes, centered as a group: the welcome, and the
    // two warnings beside it - see RequestWelcomeNote.
    // Ordinary items, deliberately: each can be moved, edited, or closed
    // like anything else, and they are in the library, so they stay until
    // the user is done with them and then they stop existing for good. A modal dialog would
    // have to be dismissed before the app could be touched at all, and
    // would teach nothing about how the app actually works.
    void PlaceWelcomeNotes(float displayW, float displayH);

    // The tutorial's folder, made and switched to, with a canvas in it - 0
    // when it could not be written. And a snippet to practice on, in the
    // middle of the canvas being looked at, off the history: undo cannot
    // take it from under a step (docs/TUTORIAL.md, section 7.3).
    FolderId MakeTutorialFolder();
    void PlacePracticeSnippet();
    // The step the tutorial is on, or how it ended, and its folder, set in
    // the settings as they change (docs/TUTORIAL.md, section 7.6) - so a
    // start after quitting partway comes back to it.
    void KeepTutorialProgress();

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
    // (see Settle).
    void KeepPen();


    // Applied once, on the first OnFrame call (ImGui's style/color tables
    // only exist once a context does, which isn't guaranteed yet at
    // construction or even AttachTo time - see theme::ApplyStyle's own
    // comment for why this couldn't just run from AttachTo).
    bool styleApplied_ = false;

    // See SetMode.
    OverlayMode mode_ = OverlayMode::Edit;
    // What is being worked on, which library is showing, and everything that
    // keeps it in step with the disk and the GPU - see Session. Owned by
    // TrayController; this class is a view of it.
    Session& session_;
    // The visible library's model, which is what nearly everything in this
    // class means by "the library".
    const CanvasManager& Manager() const { return session_.Manager(); }

    // Every setting, stored and resolved - owned by TrayController, which
    // also persists it. Plain fields are read through Cfg(), the
    // profileable ones through settings_.Live(), and every change is an
    // edit through settings_ (Settings::Set).
    Settings& settings_;
    const AppConfig& Cfg() const { return settings_.Stored(); }

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

    // See RequestWelcomeNote/PlaceWelcomeNotes. Cleared the moment the notes
    // are placed, so they can never be placed twice.
    bool welcomeNotePending_ = false;

    // What the tutorial reads of the app, and the showings it counts.
    AppWorld tutorialWorld_{session_, settings_, editor_};

    // ===== The owners of the surfaces (docs/VIEW_LAYER.md, section 7) =====
    //
    // Last, so that everything they are handed is there before them.
    SettingsPage settingsPage_{settings_, editor_, *this};
    OverviewPanel overview_{session_, settings_, editor_, *this};
    CheatSheet cheatSheet_{settings_, editor_, *this};
    Popups popups_{session_, settings_, editor_, *this};
    CanvasBar canvasBar_{session_, settings_, editor_, *this};
    ScreenChrome chrome_{session_, settings_, *this};
    Messages messages_{session_};
    TutorialCard tutorialCard_{session_, editor_, tutorialWorld_, anchors_, *this};
    Pointer pointer_{settings_, editor_, *this};
    CanvasView canvasView_{session_, settings_, editor_, *this};
};

}  // namespace sz::ui
