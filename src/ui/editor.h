#pragma once

// The canvas as the hand works on it: what is selected, what tool is in
// hand, which snippet is in drawing mode, what is on the clipboard - and
// every command that acts on them. What docs/INTERACTIONS.md calls the
// Editor: the state the interactions work on, apart from the views that
// draw it. No ImGui in here: the display size and the clock are handed
// in, and what only a view can do - open a panel, show a message - it
// asks of EditorViews. OverlayApp draws from it.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/canvas/canvas_manager.h"
#include "core/canvas/item.h"
#include "core/config/app_config.h"
#include "core/drawing/draw_tool.h"
#include "core/session/actions.h"
#include "core/session/session.h"
#include "core/session/settings.h"
#include "platform/i_overlay_window.h"
#include "platform/platform_types.h"
#include "ui/interaction/command.h"
#include "ui/interaction/levels.h"
#include "ui/interaction/machine.h"
#include "ui/selection_layout.h"

namespace sz::ui {

using namespace ::sz::core;

// What is under a screen point, as far as the canvas is concerned, decided
// by one walk (see Editor::ResolvePointerTarget). The selection's own
// furniture - its bar and its handles - is drawn over every snippet and so
// is asked first; then the snippets, front to back, so a fronter item's
// body beats a backer item's, which is the whole of the occlusion rule.
struct PointerTarget {
    // The frontmost thing that would take a press at the point: a selection
    // bar button, one of a selected snippet's handles, or an item's body -
    // None for open canvas. Handles and buttons exist only while the
    // selection is live (see Editor::SelectionLive).
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

// px, discard smaller region captures/erases/creations as a stray click
// rather than a deliberate drag - shared between the item-creation
// gesture (a region-capture drag too small to keep) and the rectangle
// eraser's own drag threshold.
inline constexpr float kRegionMinSize = 24.0f;

// What a notch of the wheel does to the selection - see
// Editor::ScaleSelectionByWheel and StepSelectionOpacity.
inline constexpr float kWheelScaleStep = 1.1f;
inline constexpr float kWheelOpacityStep = 0.05f;
// How long after one wheel notch or arrow-key nudge the next still belongs
// to the same burst, and is taken back with it by one undo.
inline constexpr double kBurstSeconds = 1.0;

// Smallest positive integer N such that `prefix + std::to_string(N)` isn't
// already exactly one of `existingNames` - see the definition for why this
// beats a plain "count existing + 1". What an *item* is named after:
// "Drawing 3", "Note 2", "Screenshot 5" - a kind and a number, which is as
// much as an item's name is ever asked to carry (a tooltip in the dock, a
// line in a toast).
int NextAvailableNumber(const std::string& prefix, const std::vector<std::string>& existingNames);

// The modifiers `held`, as what they would trigger on empty canvas (see
// AppConfig::screenshotTrigger): none, Ctrl alone or Alt alone - nothing
// for Shift, whose press there is the box that selects, or for two held
// together.
std::optional<CreationTrigger> CreationTriggerFor(const platform::Modifiers& held);

// What the editor asks of whatever draws it - opening a panel or a popup,
// showing a message. OverlayApp's.
class EditorViews {
public:
    virtual ~EditorViews() = default;
    // A message for a moment - see OverlayApp::ShowActionToast.
    virtual void Say(std::string text) = 0;
    virtual void OpenOverview() = 0;
    virtual void OpenSettings() = 0;
    virtual void OpenPicker(ItemId item, bool copy) = 0;
    virtual void ToggleCheatSheet() = 0;
    virtual void OpenItemProperties(ItemId item, std::optional<platform::Vec2> at) = 0;
    virtual void OpenColorChooser(platform::Vec2 at) = 0;
    // A canvas's delete, through the confirmation Settings > Behavior asks
    // for.
    virtual void AskToDeleteCanvas(CanvasId canvas) = 0;
    // A canvas was made, for the Overview to bring it into view.
    virtual void CanvasMade(CanvasId canvas) = 0;
    // A context menu, where a right click let go.
    virtual void OpenItemMenu(ItemId item, platform::Vec2 at) = 0;
    virtual void OpenEmptyCanvasMenu(platform::Vec2 at) = 0;
    // What ImGui knows, as of the last frame (docs/INTERACTIONS.md,
    // section 3): whether the pointer is over one of its windows, or one of
    // them holds a press, or a popup is open (io.WantCaptureMouse); whether
    // a panel covers the canvas; whether a popup is open.
    virtual bool PointerOverView() const = 0;
    virtual bool PanelOpen() const = 0;
    virtual bool PopupOpen() const = 0;
    // A popup the machine has on its Popup level: whether the view still
    // shows it - or is about to, asked for and not yet opened - and
    // closing it, or the innermost popup open, which may be one of ImGui's
    // own inside it.
    virtual bool PopupShowing(PopupKind kind) const = 0;
    virtual void ClosePopup(PopupKind kind) = 0;
    virtual void CloseInnermostPopup() = 0;
};

class Editor {
public:
    // Working on `session` with the `settings` - both outlive it, as they
    // outlive OverlayApp.
    Editor(Settings& settings, Session& session);

    void SetViews(EditorViews* views) { views_ = views; }
    // The view, or one that shows nothing and opens nothing while there is
    // none - a test of the machine alone.
    EditorViews& Views() const;
    // For the keyboard a note being typed needs - see BeginEditingNote.
    void AttachWindow(platform::IOverlayWindow* window) { window_ = window; }

    // ===== What the editor is told =====

    // The display the canvas is on, told at every frame and every event.
    void SetDisplaySize(float width, float height) {
        displayW_ = width;
        displayH_ = height;
    }
    float DisplayWidth() const { return displayW_; }
    float DisplayHeight() const { return displayH_; }
    // The time of the event being handled, on the input stream's clock -
    // what a burst of wheel notches or arrow presses is told apart by.
    void SetNow(double seconds) { now_ = seconds; }
    double Now() const { return now_; }
    // The modifiers held, as the input stream last said: at an event, the
    // ones it happened with.
    void SetHeld(const platform::Modifiers& held) { held_ = held; }
    const platform::Modifiers& Held() const { return held_; }

    // The stack of interactions every input event goes through - see
    // docs/INTERACTIONS.md, section 4.
    Machine& Input() { return machine_; }
    const Machine& Input() const { return machine_; }

    Session& GetSession() { return session_; }
    const CanvasManager& Manager() const { return session_.Manager(); }
    const AppConfig& Cfg() const { return settings_.Stored(); }

    // ===== The selection =====

    // The selected snippets, in the order they were selected - the last
    // one is the primary (see PrimarySelection). Transient UI state, never
    // persisted, and only ever snippets on the current canvas that are on
    // screen (see PruneSelection). A click selects one, Shift+click adds or
    // removes one, a click on empty canvas or Escape clears it; Delete, the
    // arrow keys and the selection bar act on it. In drawing mode the
    // selection is the snippet being drawn on.
    const std::vector<ItemId>& Selection() const { return selection_; }
    bool IsSelected(ItemId id) const;
    void SelectOnly(ItemId id);
    void ToggleSelected(ItemId id);
    void ClearSelection();
    // Drops whatever is no longer on the current canvas, or is deleted or
    // minimized there - run at the top of every frame and around every key,
    // so nothing acts on a snippet that has gone.
    void PruneSelection();
    // The snippet selected last, which the single-snippet actions (More,
    // Maximize) take. Nothing when nothing is selected.
    std::optional<ItemId> PrimarySelection() const;
    // The selected snippets' union rect, for the bar to float over.
    std::optional<Rect> SelectionBounds() const;
    // Every snippet the box touches, added to the selection - a snippet is
    // caught by any overlap at all, however slight.
    void AddTouchedToSelection(const Rect& box);
    // Whether the selection's furniture - the bar, the handles - is on
    // screen and takes presses: no creation tool in hand, and no snippet
    // being framed. Selecting is the hand at rest, so this is almost
    // always true; in drawing mode the bar shows the drawing buttons
    // instead, and a press on the snippet's body draws rather than moves
    // (see PressPicksUp).
    bool SelectionLive() const;
    // Whether a left press on a snippet's body picks it up - selects and
    // moves it - rather than drawing on it: not in drawing mode, or Alt
    // held, which is the way to move the snippet being drawn on without
    // leaving drawing mode.
    bool PressPicksUp() const;
    // What the next left press places, if anything: the snippet being
    // framed, or else what the tool in hand places (see CreationKindFor).
    std::optional<ItemCreationKind> ArmedCreation() const;

    // ===== The hand =====

    // What ImGui knows - see EditorViews; all false with no view.
    bool PointerOverView() const { return views_ != nullptr && views_->PointerOverView(); }
    bool PanelOpen() const { return views_ != nullptr && views_->PanelOpen(); }
    bool PopupOpen() const { return views_ != nullptr && views_->PopupOpen(); }
    // The recognizer's memory of the last click (docs/INTERACTIONS.md,
    // section 6.4): a press is the second half of a double-click when it
    // is the same button, within kDoubleClickSeconds and kDoubleClickPx of
    // the click's press, with the same creation trigger held - none, or
    // one that picks what a press on empty canvas makes. Any other
    // modifier makes a press no half of one: Shift+click twice on one
    // snippet adds it and takes it out again, and nothing else.
    // TakeDoubleClick asks at a press and forgets the click either way, so
    // a double is never the first half of the next one.
    void RememberClick(const Event& press);
    bool TakeDoubleClick(const Event& press);
    // What every command ends first, as far as the hand goes: the gesture
    // in flight, kept, and a note being typed, committed - see Dispatch.
    // For the moments no command follows: the overlay going away, view-only
    // mode.
    void SettleHand();
    // The overlay has just come up: nothing is in the hand, whatever was
    // held when it went away - see Machine::Forget - and no click is
    // remembered.
    void ForgetTheHand();
    // Nothing in flight: no gesture, and no press waiting to be
    // understood. The rest of a press that has had its say is at rest.
    bool HandAtRest() const;

    // ===== The tool, and drawing mode =====

    // The tool in hand, Select to start with - see the Tool enum. A marking
    // tool is in hand exactly while a snippet is in drawing mode (see
    // DrawingItem); picked from the drawing bar or a key, through PickTool.
    Tool ActiveTool() const { return activeTool_; }
    // The snippet in drawing mode, if one is. Selecting and moving are the
    // hand at rest; drawing on a snippet is a mode entered by
    // double-clicking or holding on it (or picking a marking tool by key
    // with it selected, or making a new drawing, which is made to be
    // drawn in) and left by a left press anywhere else or Escape. In it
    // the snippet wears a stronger outline, its bar shows the drawing
    // buttons (see ChromeButton), the pen is in hand (a key can enter with
    // another marking tool), a left press on the snippet draws with it,
    // and a right-drag on it erases whatever the tool; Alt held moves or
    // resizes it instead. A left press anywhere else, or a right click on
    // the snippet itself, leaves the mode and does nothing more - the press
    // was for leaving. Only ever a snippet on the current canvas that is on
    // screen, kept in step with the selection by PruneSelection.
    std::optional<ItemId> DrawingItem() const { return drawingItem_; }
    // Puts `id` into drawing mode: selected alone, outlined for it, its bar
    // showing Pen/Eraser/Text and the color, and `tool` (the pen, if none
    // is given) in hand, so a left press on it draws.
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
    void SetTool(Tool tool);
    // Hands back the tool that was in hand before a creation tool, if one is
    // in hand now - after a screenshot is placed, and wherever nothing may
    // be made (view-only).
    void PutDownCreationTool();
    // What the pen draws and the eraser erases on a plain drag, with no
    // modifier held: the drawing bar's own button cycles the tool in hand
    // through its shapes (pen, line, rectangle; eraser, rectangle eraser),
    // so a hand with no keyboard can draw a line with a drag alone. A
    // modifier held still wins for that stroke. Both go back to plain when
    // the tool changes (see SetTool): the shape is the tool's for as long
    // as it is in hand, and no longer.
    DrawShape PenShape() const { return penShape_; }
    DrawShape EraserShape() const { return eraserShape_; }
    // The shape a Draw or Erase press would make now: the modifiers' if
    // one is held (see DrawShapeFor), else the drawing bar's cycled shape
    // for the tool. What a stroke fixes at its press and the modifier
    // badge shows before one, from one rule.
    DrawShape ShapeForPress() const;
    // The buttons the bar shows: the item buttons, or the drawing buttons
    // in drawing mode - the settings' own list, minus whatever is switched
    // off (see AppConfig::snippetBar). Empty is a legal answer and means
    // no bar is drawn at all.
    std::vector<ChromeButton> BarButtons() const;

    // What every stroke-based tool draws with: one color and one width,
    // shared, so switching between the pen's shapes keeps them. The eraser
    // has a width of its own - a diameter, and no color.
    uint32_t DrawColorRGBA() const { return drawColorRGBA_; }
    // The single place the draw color actually changes.
    void SetDrawColor(uint32_t colorRGBA);
    float DrawWidth() const { return drawWidth_; }
    void SetDrawWidth(float width);
    float EraserWidth() const { return eraserWidth_; }
    void SetEraserWidth(float width) { eraserWidth_ = width; }
    DrawTool& Pen() { return drawTool_; }
    // The active tool's own size in px, or nothing for the tools that
    // don't have one (the rectangle eraser drags out its own region, Text
    // isn't stroke-based at all) - what the wheel sizes and its preview
    // shows.
    std::optional<float> ActiveToolSizePx() const;

    // ===== A note being typed =====

    // Enters live text editing for a snippet's note: copies its text into
    // the edit buffer, arms the editor window to take keyboard focus the
    // next frame it renders, and asks the window for real keyboard focus
    // (see IOverlayWindow::RequestTextInput for why that is needed
    // regardless of the no-activate setting). Commits whatever was being
    // edited before, if it is another snippet: clicking straight from one
    // note into another shouldn't silently drop the first edit.
    void BeginEditingNote(ItemId id);
    // Ends it, committing `text` - which is what was typed, even on
    // Escape: that means "stop editing", not "undo my typing", and the
    // app's own Undo takes the typing back (see Session::EndTextEdit).
    void EndEditingNote(const std::string& text);
    // Ends it with whatever the buffer holds - what every way of ending it
    // from elsewhere does.
    void CommitNoteBeingEdited();
    std::optional<ItemId> EditingNote() const { return editingNoteItemId_; }
    // The text as it is being typed, which the note editor's widget edits
    // in place - a string it grows as it types, not a fixed buffer, so a
    // note longer than one is not cut short the moment it is opened.
    std::string& NoteEditBuffer() { return noteEditBuffer_; }
    // Set when editing begins, for the editor window to take focus once.
    bool TakeNoteEditJustBegun() { return std::exchange(noteEditJustBegun_, false); }
    // A note edit the session ended from elsewhere - every command ends the
    // gesture open before its own (see Session::EndOpenGesture) - has its
    // editor put away with it, rather than typing on into an edit that is
    // over.
    void ForgetNoteEditEndedElsewhere();

    // ===== Making snippets =====

    // What the modifiers held would make with a left press on empty canvas,
    // by AppConfig::screenshotTrigger and drawingTrigger - nothing if
    // neither is set to them.
    std::optional<ItemCreationKind> EmptyCanvasCreationKind() const;
    // A new snippet of `kind` as Settings > Defaults says it starts, at
    // `rect` - what the session makes it from (see Session::CreateItem).
    Item PrototypeForKind(ItemCreationKind kind, Rect rect, std::string name) const;
    // A snippet of `kind` covering the display - or `width` by `height`,
    // for a capture made while hidden, with no frame to have told the
    // display - or framed at `rect` if that is big enough to be meant;
    // handed over as HandOverNewItem says. 0 when nothing was made.
    ItemId CreateFullscreenItem(ItemCreationKind kind);
    ItemId CreateFullscreenItem(ItemCreationKind kind, float width, float height);
    ItemId CreateRegionItem(ItemCreationKind kind, Rect rect);
    // What the hand does with a snippet just made: a screenshot is
    // selected, a drawing entered with the pen.
    void HandOverNewItem(ItemCreationKind kind, ItemId id);
    // The current canvas, creating one first if the library is empty (a
    // state CanvasManager allows - see its class comment). The create
    // path always produces one.
    const Canvas& EnsureCanvasForNewItem();

    // ----- The drawing a stray click made -----
    //
    // A drawing a press on empty canvas made, with nothing put into it yet.
    // It goes - erased, not merely marked deleted - once the hand moves on
    // without using it: a press anywhere else, another canvas, its own
    // Close, the overlay going away, or an undo (see
    // SettleUntouchedDrawing). What makes a snippet on every click on empty
    // space harmless to miss with. Moving, resizing or nudging it is using
    // it (see KeepDrawingsPlaced): a box someone has placed is a box they
    // want, empty or not. And so is putting anything into it: once it has
    // held a stroke it is watched no longer (see WatchUntouchedDrawing), so
    // an undo that empties it again leaves it as an empty drawing rather
    // than erasing it, redo and all.
    std::optional<ItemId> UntouchedDrawing() const { return untouchedDrawing_; }
    // Starts watching `id`, just made by a press on empty canvas.
    void WatchAsUntouched(ItemId id);
    // Discards the drawing if nothing has been put into it by now, and
    // stops watching it either way.
    void SettleUntouchedDrawing();
    // Stops watching it if it is among `ids`, placed on purpose - moved,
    // nudged, scaled by the wheel, made fullscreen, set back to its size.
    // Each files a step on the drawing itself; watched on, an undo took it
    // for a step made elsewhere, took the drawing away for it - and then
    // undid the step before.
    void KeepDrawingsPlaced(const std::vector<ItemId>& ids);
    // Once a frame: settles it once the hand has moved on without a press -
    // another canvas, its own Close - and stops watching it once something
    // has gone into it.
    void WatchUntouchedDrawing();

    // ===== Canvases =====

    // A new empty canvas at the end of the browsed folder, named for when
    // it was made, and *not* switched to. 0 if the library couldn't take
    // one.
    CanvasId CreateCanvasInCurrentFolder();
    // The same, in the folder the current canvas lives in - where the work
    // is - rather than the one the Overview happens to be browsing. What
    // every way of making a canvas from the canvas itself uses.
    CanvasId CreateCanvasBesideCurrent();
    // A new canvas beside the current one, switched to.
    void CreateAndSwitchToNewCanvas();
    // That, taking the selected snippets along.
    void MoveSelectionToNewCanvas();
    // Switches canvas, first ending whatever the hand is doing and
    // committing a note being typed - what every way of switching from the
    // canvas itself has to do.
    void SwitchToCanvasSettled(CanvasId id);
    // Steps `delta` canvases along the folder the current canvas lives in,
    // without wrapping, and says where it landed.
    void SwitchCanvasByOffset(int delta);
    // A fullscreen screenshot onto a canvas made for it, at the end of the
    // folder the current canvas lives in, switched to - see
    // OverlayApp::QuickCapture.
    void QuickCapture(float displayW, float displayH);

    // ===== Placing and changing the selection =====

    // Files a placement change made in one step - a fullscreen toggle, a
    // reset to the original size - as its own undo entry.
    void ToggleFullscreenUndoably(ItemId id, bool stretch);
    void ResetToNativeSizeUndoably(ItemId id);
    // Moves every selected snippet by (dx, dy), clamped on screen - the
    // arrow keys.
    void NudgeSelection(float dx, float dy);
    // The wheel over a selection outside drawing mode: every selected
    // snippet scaled by kWheelScaleStep per notch, as a group about the
    // middle of the box around them - the same scaling a corner handle
    // does, so shapes and spacing are kept, and the smallest snippet's
    // floor stops all of them. A fullscreen snippet is left as it is.
    void ScaleSelectionByWheel(int steps);
    // Ctrl or Shift with the wheel: the selection's background or
    // foreground opacity, kWheelOpacityStep per notch, within the ranges
    // the Properties popover's sliders have. Says the new value.
    void StepSelectionOpacity(int steps, bool background);
    // Deletes every selected snippet, undoably, as Close does.
    void DeleteSelection();
    // Delete on the snippets: one undoable delete of them all (see
    // Session::DeleteItems), said. Those not on the current canvas are
    // passed over.
    void DeleteItemsSaid(const std::vector<ItemId>& itemIds);
    // Every stroke off the snippet as one undoable step (see
    // Session::ClearDrawing), said.
    void ClearItemDrawing(ItemId itemId);

    // ===== The clipboard =====
    //
    // Copy and Cut put the selection on the clipboard - as ids, so nothing
    // is taken away or duplicated until a paste. Paste puts what is on it
    // onto the canvas being looked at, skipping whatever has gone since.
    void CopySelectionToClipboard(bool cut);
    void PasteFromClipboard();
    // The two of those in one step, without going through the clipboard.
    void DuplicateSelection();
    // Whether this snippet is waiting for the paste that will move it,
    // which is what it is drawn faded for.
    bool IsWaitingToBeCut(ItemId id) const;

    // ===== The history =====

    // Undo and redo, as Ctrl+Z and Ctrl+Y reach them: the session steps the
    // current canvas's history (see Session::Undo), and this says what
    // happened.
    void Undo();
    void Redo();

    // ===== What is under the pointer =====

    // The one hit test: what is under a screen point, front to back - see
    // PointerTarget. Pure in position and model, so a press between frames
    // and the frame that draws it get the same answer for the same pixel.
    PointerTarget ResolvePointerTarget(float x, float y) const;
    // Where the middle of a selection bar button is, or nothing while the
    // bar is not showing it.
    std::optional<platform::Vec2> SelectionBarButtonCenter(ChromeButton button) const;
    // A selection bar button's command, run by the release that completes
    // a press on it: the command it is (see CommandForBarButton), about the
    // snippet selected last and from where the button sits. Nothing with
    // nothing selected.
    std::optional<Command> BarButtonCommand(ChromeButton button) const;

    // ===== Commands =====
    //
    // Runs `command` if it can act now (see Available), after ending what
    // its scope covers (see Scope). Every key, context menu row, selection
    // bar button and global hotkey reaches the app through here - see
    // ui/interaction/command.h. True when it ran.
    bool Dispatch(const Command& command);
    // Whether `command` would do anything now: what grays a menu row out,
    // and what a command is asked before it ends anything. Asked before
    // settling, so it never depends on what settling would file - undo is
    // always available, since a stroke in flight is on the history only
    // once it has been settled.
    bool Available(const Command& command) const;
    // Where the global hotkeys' commands run: the tray, which alone knows
    // the window and the modes. Left null they do nothing.
    void SetAppCommandCallback(std::function<void(CommandId)> callback) {
        appCommandCallback_ = std::move(callback);
    }
    // How many commands have run, and which ran last.
    uint64_t CommandsRun() const { return commandsRun_; }
    std::optional<CommandId> LastCommand() const { return lastCommand_; }

private:
    // What a command does, once Dispatch has settled what it covers.
    void Run(const Command& command);
    // Escape: puts the hand down one stage - see the definition.
    void PutDown();

    // Files a wheel notch or an arrow-key nudge, folded into the burst the
    // last one began when it continues it: the same kind of step, soon
    // enough after it, with nothing filed, undone or redone in between - a
    // drag of the same snippets, or an undo that left an older step of
    // theirs on top, would otherwise be taken into the burst. What decides
    // that a burst is one undo (see Session::EndPlacement).
    enum class Burst { None, Wheel, Nudge, Opacity };
    void RecordPlacementBurst(Burst kind, const std::vector<std::pair<ItemId, Rect>>& rects);
    // The same for the opacity wheel's steps.
    void RecordStyleBurst(Burst kind, const std::vector<std::pair<ItemId, ItemStyle>>& styles);
    // Whether a step of `kind` now continues the burst the last one began,
    // and noting that one was filed - the two halves of both of the above.
    bool BurstContinues(Burst kind) const;
    void NoteBurst(Burst kind);
    void ShowUndoStep(const std::optional<Session::UndoStep>& step);
    void Say(std::string text);

    Settings& settings_;
    Session& session_;
    Machine machine_{*this};
    EditorViews* views_ = nullptr;
    platform::IOverlayWindow* window_ = nullptr;

    float displayW_ = 0.0f;
    float displayH_ = 0.0f;
    double now_ = 0.0;
    platform::Modifiers held_;

    std::vector<ItemId> selection_;
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

    std::optional<ItemId> drawingItem_;
    Tool activeTool_ = Tool::Select;
    // The tool that was in hand before a creation tool was picked - what a
    // screenshot hands back once placed. See SetTool.
    Tool toolBeforeCreation_ = Tool::Select;
    DrawShape penShape_ = DrawShape::Freehand;
    DrawShape eraserShape_ = DrawShape::Freehand;  // Freehand or Rectangle
    DrawTool drawTool_;
    uint32_t drawColorRGBA_;
    float drawWidth_;
    float eraserWidth_ = 28.0f;

    std::optional<ItemId> editingNoteItemId_;
    std::string noteEditBuffer_;
    bool noteEditJustBegun_ = false;

    std::optional<ItemId> untouchedDrawing_;
    // The history as it stood once untouchedDrawing_ was made - see Undo.
    uint64_t untouchedDrawingRevision_ = 0;

    // See RecordPlacementBurst and RecordStyleBurst.
    Burst lastBurst_ = Burst::None;
    double lastBurstAtSeconds_ = 0.0;
    uint64_t lastBurstRevision_ = 0;

    struct Click {
        platform::MouseButton button = platform::MouseButton::Left;
        CreationTrigger trigger = CreationTrigger::Plain;
        double atSeconds = 0.0;
        platform::Vec2 at;
    };
    std::optional<Click> lastClick_;

    std::function<void(CommandId)> appCommandCallback_;
    uint64_t commandsRun_ = 0;
    std::optional<CommandId> lastCommand_;
};

}  // namespace sz::ui
