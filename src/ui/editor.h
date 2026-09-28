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
// How long a burst of wheel notches or arrow-key nudges goes on without a
// step before it ends, filed as one - see ui/interaction/bursts.h.
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
    // A message for a moment - see Messages::Say.
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
    // A panel the machine ended - Escape, the overlay going away: the view
    // puts it away.
    virtual void ClosePanel(PanelKind kind) = 0;
    // The size of the tool in hand changed by the wheel - the pen's width
    // or the eraser's: the view shows it for a moment, and keeps the pen's
    // once it has.
    virtual void ToolSized(bool pen) = 0;
    // A key the input options HUD takes, while it is up - a debugging aid
    // of the view's. False for one it does not.
    virtual bool InputOptionsKey(const Event& event) = 0;
    // The widget held down let go of - a slider's drag, a tile's - as if
    // the button had come up somewhere it means nothing.
    virtual void LetGoOfWidget() = 0;
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
    // Ends what `scope` covers - see Machine::EndFor - and commits a note
    // being typed, whatever ended it: what a command does first, and what
    // the overlay going away, coming up and going view-only do. False when
    // what it ended could not be written, and so went back as it was: a
    // command then does not run, for the reasons Session::EndOpenGesture
    // gives.
    bool Settle(Scope scope);
    // The overlay has just come up: nothing is in the hand, whatever was
    // held when it went away - see Machine::Forget - and no click is
    // remembered.
    void ForgetTheHand();
    // Nothing in flight: no gesture, no press waiting to be understood,
    // and no burst. The rest of a press that has had its say is at rest.
    bool HandAtRest() const;
    // A button held down to some purpose: a gesture, or a press waiting
    // to be understood - not the rest of one, and not a burst, which the
    // keys or the wheel make.
    bool PointerInUse() const;
    // Where the press went down, while the gesture is a press on one of
    // ImGui's windows (a Widget) - so a view can tell a press on itself
    // from one on another window dragged across it.
    std::optional<platform::Vec2> WidgetPressedAt() const;

    // ===== The tool, and drawing mode =====

    // The tool in hand, Select to start with - see the Tool enum. A marking
    // tool is in hand exactly while a snippet is in drawing mode (see
    // DrawingItem), and a creation tool while one is on the Mode level
    // (see CreationTool); picked from the drawing bar or a key, through
    // PickTool.
    Tool ActiveTool() const;
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
    // screen, kept in step with the selection by PruneSelection. The Mode
    // level's - see DrawingMode.
    std::optional<ItemId> DrawingItem() const;
    // Puts `id` into drawing mode: selected alone, outlined for it, its bar
    // showing Pen/Eraser/Text and the color, and `tool` (the pen, if none
    // is given) in hand, so a left press on it draws.
    void EnterDrawingMode(ItemId id, std::optional<Tool> tool = std::nullopt);
    // Back to the hand at rest: no snippet in drawing mode, Select in hand.
    // The selection is left as it was.
    void ExitDrawingMode();
    // What picking a tool from the drawing bar or a key does - the one
    // place a tool is chosen. Select leaves drawing mode and puts a
    // creation tool down; a marking tool switches the tool in drawing
    // mode, or enters it on the snippet selected last, and does nothing
    // with no snippet to draw on; a creation tool leaves drawing mode and
    // is picked up.
    void PickTool(Tool tool);
    // Puts down the creation tool in hand, if one is - after a screenshot
    // is placed, and wherever nothing may be made (view-only). Select is
    // in hand after it.
    void PutDownCreationTool();
    // What the pen draws and the eraser erases on a plain drag, with no
    // modifier held - see DrawingMode::PenShape. So a hand with no keyboard
    // can draw a line with a drag alone; a modifier held still wins for
    // that stroke. Plain outside drawing mode.
    DrawShape PenShape() const;
    DrawShape EraserShape() const;
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
    // Puts every setting a slider or swatch is previewing back as it was -
    // see Settings::CancelPreviews.
    void CancelSettingsPreviews() { settings_.CancelPreviews(); }
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
    // state CanvasManager allows - see its class comment). Null when there
    // was none and the one made could not be written.
    const Canvas* EnsureCanvasForNewItem();

    // ===== Canvases =====

    // A new empty canvas at the end of the browsed folder, named for when
    // it was made, and *not* switched to. 0 if the library couldn't take
    // one.
    CanvasId CreateCanvasInCurrentFolder();
    // The same, in the folder the current canvas lives in - where the work
    // is - rather than the one the Overview happens to be browsing. What
    // every way of making a canvas from the canvas itself uses. 0 as well
    // when that folder could not be made the browsed one.
    CanvasId CreateCanvasBesideCurrent();
    // A new canvas beside the current one, switched to - when it could be
    // made.
    void CreateAndSwitchToNewCanvas();
    // That, taking the selected snippets along.
    void MoveSelectionToNewCanvas();
    // Switches canvas, first ending what the Canvas scope covers - the hand,
    // a note being typed, a popup - which is what every way of switching
    // from the canvas itself has to do.
    void SwitchCanvas(CanvasId id);
    // Steps `delta` canvases along the folder the current canvas lives in,
    // without wrapping, and says where it landed.
    void SwitchCanvasByOffset(int delta);
    // A fullscreen screenshot onto a canvas made for it, at the end of the
    // folder the current canvas lives in, switched to - see
    // OverlayApp::QuickCapture. Whether the shot was made.
    bool QuickCapture(float displayW, float displayH);

    // ===== Placing and changing the selection =====

    // Files a placement change made in one step - a fullscreen toggle, a
    // reset to the original size - as its own undo entry.
    void ToggleFullscreenUndoably(ItemId id, bool stretch);
    void ResetToNativeSizeUndoably(ItemId id);
    // How a change to the selection is filed: as a step of its own, or
    // previewed into the placement or the style edit a burst holds open on
    // the session, to be filed when the burst ends - see
    // ui/interaction/bursts.h.
    enum class Filing { Step, Burst };
    // Moves every selected snippet by (dx, dy), clamped on screen - the
    // arrow keys.
    void NudgeSelection(float dx, float dy, Filing filing);
    // The wheel over a selection outside drawing mode: every selected
    // snippet scaled by kWheelScaleStep per notch, as a group about the
    // middle of the box around them - the same scaling a corner handle
    // does, so shapes and spacing are kept, and the smallest snippet's
    // floor stops all of them. A fullscreen snippet is left as it is.
    // Always a burst's.
    void ScaleSelectionByWheel(int steps);
    // Ctrl or Shift with the wheel: the selection's background or
    // foreground opacity, kWheelOpacityStep per notch, within the ranges
    // the Properties popover's sliders have. Says the new value. Always a
    // burst's.
    void StepSelectionOpacity(int steps, bool background);
    // What a notch of the wheel would do now, by the modifiers held, the
    // pointer and the mode - see Wheel. The selection's size and its
    // opacity are filed, and come in bursts; the rest file nothing.
    enum class WheelKind { Nothing, Canvases, SelectionSize, SelectionOpacity, ToolSize };
    WheelKind KindOfWheel() const;
    // The wheel, turned `notches`, as the Canvas level or a wheel burst
    // has it - see the definition for what each modifier makes of it.
    void Wheel(float notches);
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
    // its scope covers (see Scope) - unless that could not be written (see
    // Settle). Every key, context menu row, selection bar button and global
    // hotkey reaches the app through here - see ui/interaction/command.h.
    // True when it ran.
    bool Dispatch(const Command& command);
    // Whether `command` would do anything now: what grays a menu row out,
    // and what a command is asked before it ends anything. Asked before
    // settling, so it never depends on what settling would file - undo is
    // always available, since a stroke in flight is on the history only
    // once it has been settled.
    bool Available(const Command& command) const;
    // The command a key - KeyCombo's name for it, or a mouse button a
    // shortcut may be - runs with `held` down, if any: the first row of the
    // table it is bound to, which puts the fixed keys ahead of the chosen
    // ones and, among those, the table's order ahead of a profile that
    // bound one key twice. A key matches exactly the modifiers its binding
    // names, but for Escape and the arrows, which take any, and Delete and
    // Backspace, which take Shift and nothing else (see HeldWith). A key
    // held down and repeating runs only a command that repeats; a global
    // hotkey is the tray's, never a key's.
    std::optional<CommandId> CommandForKey(int key, const platform::Modifiers& held, bool repeat) const;
    // Where the global hotkeys' commands run: the tray, which alone knows
    // the window and the modes. Left null they do nothing.
    void SetAppCommandCallback(std::function<void(CommandId)> callback) {
        appCommandCallback_ = std::move(callback);
    }
    // Runs `command` as a step of the burst on top of the stack (see
    // NudgeBurst) if it can act now: nothing is ended first - the burst is
    // the hand - and what it changes is held open by the burst, filed when
    // the burst ends. True when it ran.
    bool Step(const Command& command);
    // How many commands have run, by Dispatch or as a burst's steps, and
    // which ran last.
    uint64_t CommandsRun() const { return commandsRun_; }
    std::optional<CommandId> LastCommand() const { return lastCommand_; }

private:
    // What a command does, once Dispatch has settled what it covers - or
    // as a burst's step.
    void Run(const Command& command, Filing filing);
    // Escape: puts the hand down one stage - see the definition.
    void PutDown();

    // Previews `rects` into the placement a burst holds open on the
    // session, begun here for these snippets when it is not open for them
    // yet - see Filing.
    void PreviewPlacement(const std::vector<std::pair<ItemId, Rect>>& rects);
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

    DrawTool drawTool_;
    uint32_t drawColorRGBA_;
    float drawWidth_;
    float eraserWidth_ = 28.0f;

    std::optional<ItemId> editingNoteItemId_;
    std::string noteEditBuffer_;
    bool noteEditJustBegun_ = false;

    struct Click {
        platform::MouseButton button = platform::MouseButton::Left;
        CreationTrigger trigger = CreationTrigger::Plain;
        double atSeconds = 0.0;
        platform::Vec2 at;
    };
    std::optional<Click> lastClick_;

    // Leftover fractions of a wheel notch, carried across events so the
    // wheel honors how far it was actually turned. Two things make this
    // more than a plain sum: several notches can land in one event on a
    // fast spin, and a high-resolution wheel or precision touchpad reports
    // *fractions* of a notch, which truncating would round to nothing and
    // leave the wheel feeling dead. Separate accumulators per use rather
    // than one shared: a half-notch left over from sizing a brush must not
    // count toward a canvas switch. The selection's scale and its two
    // opacities share one, being one hand on one selection, told apart by a
    // modifier held for the whole spin.
    float sizeWheelRemainder_ = 0.0f;
    float canvasWheelRemainder_ = 0.0f;
    float selectionWheelRemainder_ = 0.0f;

    std::function<void(CommandId)> appCommandCallback_;
    uint64_t commandsRun_ = 0;
    std::optional<CommandId> lastCommand_;
};

}  // namespace sz::ui
