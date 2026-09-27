#include "ui/editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/canvas/item_geometry.h"
#include "core/util/timestamp_name.h"
#include "generated/ui_strings.h"
#include "ui/interaction/canvas.h"
#include "ui/interaction/gestures.h"
#include "ui/ui_scale.h"

namespace sz::ui {

// ================= Helpers =================

// Smallest positive integer N such that `prefix + std::to_string(N)` isn't
// already exactly one of `existingNames`. Unlike "count existing + 1" this
// can't collide with a sibling that is still around: deleting the *middle*
// one of "Drawing 1/2/3" and adding a new one would otherwise produce a
// second "Drawing 3". Fills the lowest *unused*
// number rather than always climbing past the highest one ever used, so
// deleting the last of a numbered run and adding a new one reuses that
// just-freed number - matches what "Canvas 1/2/3", delete 3, add new ->
// "Canvas 3" again (not "Canvas 4") intuitively suggests. `prefix`
// includes its own trailing separator (e.g. "Canvas ") so a name has to
// match it exactly, digits only, to count - "My Canvas 1" or "Canvas 1a"
// don't.
int NextAvailableNumber(const std::string& prefix, const std::vector<std::string>& existingNames) {
    std::vector<int> used;
    for (const std::string& name : existingNames) {
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const std::string suffix = name.substr(prefix.size());
        const bool allDigits =
            !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (allDigits) {
            used.push_back(std::atoi(suffix.c_str()));
        }
    }
    std::sort(used.begin(), used.end());
    int n = 1;
    for (int u : used) {
        if (u == n) {
            ++n;
        } else if (u > n) {
            break;
        }
    }
    return n;
}

std::optional<CreationTrigger> CreationTriggerFor(const platform::Modifiers& held) {
    if (held.shift || held.super || (held.ctrl && held.alt)) {
        return std::nullopt;
    }
    return held.ctrl ? CreationTrigger::Ctrl : held.alt ? CreationTrigger::Alt : CreationTrigger::Plain;
}

namespace {

std::string ItemNameForKind(ItemCreationKind kind, const Canvas& canvas, bool fullscreen) {
    const bool wantsBackground = kind == ItemCreationKind::Screenshot;
    const char* base = wantsBackground ? (fullscreen ? strings::kItemNameScreenshotPrefix : strings::kItemNameRegionPrefix) : strings::kItemNameDrawingPrefix;
    std::vector<std::string> existingNames;
    for (const Item& item : canvas.items) {
        if (item.hasBackground == wantsBackground) {
            existingNames.push_back(item.name);
        }
    }
    return base + std::to_string(NextAvailableNumber(base, existingNames));
}

}  // namespace

namespace {
// What the editor asks of a view, with none there.
class NoViews final : public EditorViews {
public:
    void Say(std::string /*text*/) override {}
    void OpenOverview() override {}
    void OpenSettings() override {}
    void OpenPicker(ItemId /*item*/, bool /*copy*/) override {}
    void ToggleCheatSheet() override {}
    void OpenItemProperties(ItemId /*item*/, std::optional<platform::Vec2> /*at*/) override {}
    void OpenColorChooser(platform::Vec2 /*at*/) override {}
    void AskToDeleteCanvas(CanvasId /*canvas*/) override {}
    void CanvasMade(CanvasId /*canvas*/) override {}
    void OpenItemMenu(ItemId /*item*/, platform::Vec2 /*at*/) override {}
    void OpenEmptyCanvasMenu(platform::Vec2 /*at*/) override {}
    bool PointerOverView() const override { return false; }
    bool PanelOpen() const override { return false; }
    bool PopupOpen() const override { return false; }
    bool PopupShowing(PopupKind /*kind*/) const override { return false; }
    void ClosePopup(PopupKind /*kind*/) override {}
    void CloseInnermostPopup() override {}
    void ClosePanel(PanelKind /*kind*/) override {}
    void ToolSized(bool /*pen*/) override {}
    bool InputOptionsKey(const Event& /*event*/) override { return false; }
    void LetGoOfWidget() override {}
};

// Whole wheel notches out of `remainder`, which the caller keeps across
// events. Returns however many complete notches `wheelDelta` brings the
// running total to - 3 on a fast spin that lands three at once, 0 partway
// through a high-resolution wheel's own sub-notch reports - and leaves the
// sub-notch fraction behind for next time.
int TakeWheelSteps(float& remainder, float wheelDelta) {
    remainder += wheelDelta;
    // Truncation toward zero, not floor: a remainder of -0.4 has to stay
    // -0.4 rather than becoming a whole step downward it never earned.
    const float whole = std::trunc(remainder);
    remainder -= whole;
    return static_cast<int>(whole);
}
}  // namespace

EditorViews& Editor::Views() const {
    static NoViews none;
    return views_ != nullptr ? *views_ : none;
}

Editor::Editor(Settings& settings, Session& session)
    : settings_(settings),
      session_(session),
      drawTool_(settings.Stored().strokeColorRGBA, settings.Stored().strokeWidth),
      drawColorRGBA_(settings.Stored().strokeColorRGBA),
      drawWidth_(settings.Stored().strokeWidth) {
    machine_.SetRoot(std::make_unique<CanvasLevel>());
}

void Editor::Say(std::string text) {
    if (views_ != nullptr) {
        views_->Say(std::move(text));
    }
}

// ================= The selection =================

bool Editor::IsSelected(ItemId id) const {
    return std::find(selection_.begin(), selection_.end(), id) != selection_.end();
}

void Editor::SelectOnly(ItemId id) {
    selection_.clear();
    selection_.push_back(id);
}

void Editor::ToggleSelected(ItemId id) {
    const auto it = std::find(selection_.begin(), selection_.end(), id);
    if (it != selection_.end()) {
        selection_.erase(it);
    } else {
        selection_.push_back(id);
    }
}

void Editor::ClearSelection() { selection_.clear(); }

void Editor::PruneSelection() {
    if (selection_.empty()) {
        return;
    }
    const Canvas* canvas = Manager().CurrentOrNull();
    const auto onScreen = [&](ItemId id) {
        if (canvas == nullptr) {
            return false;
        }
        for (const Item& item : canvas->items) {
            if (item.id == id) {
                return !item.minimized && !Manager().IsDeleted(*canvas, item);
            }
        }
        return false;
    };
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](ItemId id) { return !onScreen(id); }),
                     selection_.end());
    // Drawing mode is on a selected snippet, so it goes the same way: a
    // canvas switch, a delete, a minimize.
    if (const std::optional<ItemId> drawing = DrawingItem(); drawing.has_value() && !IsSelected(*drawing)) {
        ExitDrawingMode();
    }
}

std::optional<ItemId> Editor::PrimarySelection() const {
    if (selection_.empty()) {
        return std::nullopt;
    }
    return selection_.back();
}

std::optional<Rect> Editor::SelectionBounds() const {
    const Canvas* canvas = Manager().CurrentOrNull();
    if (canvas == nullptr) {
        return std::nullopt;
    }
    std::optional<Rect> bounds;
    for (const Item& item : canvas->items) {
        if (!IsSelected(item.id)) {
            continue;
        }
        if (!bounds.has_value()) {
            bounds = item.rect;
            continue;
        }
        const float x0 = std::min(bounds->x, item.rect.x);
        const float y0 = std::min(bounds->y, item.rect.y);
        const float x1 = std::max(bounds->x + bounds->w, item.rect.x + item.rect.w);
        const float y1 = std::max(bounds->y + bounds->h, item.rect.y + item.rect.h);
        bounds = Rect{x0, y0, x1 - x0, y1 - y0};
    }
    return bounds;
}

void Editor::AddTouchedToSelection(const Rect& box) {
    const Canvas* canvas = Manager().CurrentOrNull();
    if (canvas == nullptr) {
        return;
    }
    // Back to front, so a box over a stack of them leaves the frontmost
    // selected last - which is the one the bar's single-snippet buttons
    // then act on, and the one whose handles are found first.
    for (const Item& item : canvas->items) {
        if (item.minimized || Manager().IsDeleted(*canvas, item) || IsSelected(item.id)) {
            continue;
        }
        // Touched, not enclosed: a box drawn across a row of snippets is
        // meant to have caught the ones it was drawn across, and asking
        // for the whole of a fullscreen snippet to be inside the box
        // would put that one out of reach of any box at all.
        if (RectsOverlap(box, item.rect)) {
            selection_.push_back(item.id);
        }
    }
}

bool Editor::SelectionLive() const { return !ArmedCreation().has_value(); }

bool Editor::PressPicksUp() const { return !DrawingItem().has_value() || held_.alt; }

std::optional<ItemCreationKind> Editor::ArmedCreation() const {
    if (const Framing* framing = machine_.As<Framing>(Level::Gesture)) {
        return framing->Kind();
    }
    if (const Pending* pending = machine_.As<Pending>(Level::Gesture)) {
        if (pending->GetMeaning().framing.has_value()) {
            return pending->GetMeaning().framing;
        }
    }
    return CreationKindFor(ActiveTool());
}

// ================= The hand =================

namespace {
// Whether `trigger` can be half of a double-click: none, or one set to
// make something on empty canvas.
bool DoublesWith(const std::optional<CreationTrigger>& trigger, const AppConfig& config) {
    return trigger.has_value() && (*trigger == CreationTrigger::Plain || *trigger == config.screenshotTrigger ||
                                   *trigger == config.drawingTrigger);
}
}  // namespace

void Editor::RememberClick(const Event& press) {
    const std::optional<CreationTrigger> trigger = CreationTriggerFor(press.modifiers);
    if (!DoublesWith(trigger, Cfg())) {
        lastClick_.reset();
        return;
    }
    lastClick_ = Click{press.button, *trigger, press.seconds, press.position};
}

bool Editor::TakeDoubleClick(const Event& press) {
    const std::optional<Click> last = std::exchange(lastClick_, std::nullopt);
    const std::optional<CreationTrigger> trigger = CreationTriggerFor(press.modifiers);
    if (!last.has_value() || !DoublesWith(trigger, Cfg()) || last->button != press.button ||
        last->trigger != *trigger || press.seconds - last->atSeconds > kDoubleClickSeconds) {
        return false;
    }
    const float dx = press.position.x - last->at.x;
    const float dy = press.position.y - last->at.y;
    return std::sqrt(dx * dx + dy * dy) <= kDoubleClickPx;
}

void Editor::Settle(Scope scope) {
    machine_.EndFor(scope);
    CommitNoteBeingEdited();
}

void Editor::ForgetTheHand() {
    machine_.Forget();
    lastClick_.reset();
}

bool Editor::HandAtRest() const {
    const Interaction* gesture = machine_.At(Level::Gesture);
    return gesture == nullptr || dynamic_cast<const Spent*>(gesture) != nullptr;
}

bool Editor::PointerInUse() const { return dynamic_cast<const Gesture*>(machine_.At(Level::Gesture)) != nullptr; }

// ================= The tool, and drawing mode =================

Tool Editor::ActiveTool() const {
    if (const DrawingMode* drawing = machine_.As<DrawingMode>(Level::Mode)) {
        return drawing->GetTool();
    }
    if (const CreationTool* creation = machine_.As<CreationTool>(Level::Mode)) {
        return creation->Kind() == ItemCreationKind::Screenshot ? Tool::NewScreenshot : Tool::NewDrawing;
    }
    return Tool::Select;
}

std::optional<ItemId> Editor::DrawingItem() const {
    if (const DrawingMode* drawing = machine_.As<DrawingMode>(Level::Mode)) {
        return drawing->Item();
    }
    return std::nullopt;
}

DrawShape Editor::PenShape() const {
    const DrawingMode* drawing = machine_.As<DrawingMode>(Level::Mode);
    return drawing != nullptr ? drawing->PenShape() : DrawShape::Freehand;
}

DrawShape Editor::EraserShape() const {
    const DrawingMode* drawing = machine_.As<DrawingMode>(Level::Mode);
    return drawing != nullptr ? drawing->EraserShape() : DrawShape::Freehand;
}

void Editor::PutDownCreationTool() {
    if (machine_.As<CreationTool>(Level::Mode) != nullptr) {
        machine_.End(Level::Mode);
    }
}

void Editor::SetDrawColor(uint32_t colorRGBA) {
    drawColorRGBA_ = colorRGBA;
    drawTool_.SetColor(colorRGBA);
}

void Editor::SetDrawWidth(float width) {
    drawWidth_ = width;
    drawTool_.SetWidth(width);
}

void Editor::EnterDrawingMode(ItemId id, std::optional<Tool> tool) {
    if (Manager().FindItemAnywhere(id) == nullptr) {
        return;
    }
    // The snippet being drawn on is the selection - alone, and in front
    // while raising is on, as any selected snippet is.
    SelectOnly(id);
    if (Cfg().raiseSelectedSnippet) {
        session_.BringItemsToFront({id});
    }
    // The pen, unless a tool was asked for by name (a key): the mode is
    // entered to draw, and the eraser is a right-drag away in it. Put on the
    // Mode level, it ends drawing mode on another snippet, or a creation
    // tool in hand.
    machine_.Push(std::make_unique<DrawingMode>(id, tool.value_or(Tool::Draw)), Event{});
}

void Editor::ExitDrawingMode() {
    if (machine_.As<DrawingMode>(Level::Mode) == nullptr) {
        return;
    }
    // A press elsewhere is the usual way out, with nothing in flight - but a
    // key and the view-only hotkey can come with the button still held.
    // What it was doing ends here, kept: a stroke as a release would end
    // it, filed as its undo step, and a right-drag erase likewise.
    //
    // Only the mode's own gestures, though: leaving it is also what a
    // press on another snippet does, a frame later (see PruneSelection),
    // and the move or resize that press has just started is not the
    // mode's to end.
    if (machine_.As<Marking>(Level::Gesture) != nullptr) {
        machine_.End(Level::Gesture);
    }
    machine_.End(Level::Mode);
}

void Editor::PickTool(Tool tool) {
    switch (tool) {
        case Tool::Select:
            ExitDrawingMode();
            PutDownCreationTool();
            return;
        case Tool::Draw:
        case Tool::Erase:
        case Tool::Text:
            if (DrawingMode* drawing = machine_.As<DrawingMode>(Level::Mode)) {
                drawing->SetTool(tool);
                return;
            }
            // Not drawing yet: the tool is picked *for* the snippet selected
            // last, and means nothing without one.
            if (const std::optional<ItemId> target = PrimarySelection()) {
                EnterDrawingMode(*target, tool);
            }
            return;
        case Tool::NewDrawing:
        case Tool::NewScreenshot:
            // On the Mode level, where it ends drawing mode.
            ExitDrawingMode();
            machine_.Push(std::make_unique<CreationTool>(*CreationKindFor(tool)), Event{});
            return;
    }
}

DrawShape Editor::ShapeForPress() const {
    if (ActiveTool() == Tool::Erase) {
        // Ctrl for a rectangle; Shift means nothing to the eraser.
        return ErasesRectangle(held_.ctrl) ? DrawShape::Rectangle : EraserShape();
    }
    return held_.ctrl || held_.shift ? DrawShapeFor(held_.ctrl, held_.shift) : PenShape();
}

std::vector<ChromeButton> Editor::BarButtons() const {
    const BarButtonList& configured = DrawingItem().has_value() ? Cfg().drawingBar : Cfg().snippetBar;
    std::vector<ChromeButton> shown;
    shown.reserve(configured.size());
    for (const BarButtonSetting& entry : configured) {
        if (entry.shown) {
            shown.push_back(entry.button);
        }
    }
    return shown;
}

std::optional<float> Editor::ActiveToolSizePx() const {
    switch (ActiveTool()) {
        case Tool::Draw:
            // Stroke thickness, which is the full width of the line drawn -
            // so a dot of exactly this diameter is exactly the mark the pen
            // makes.
            return drawWidth_;
        case Tool::Erase:
            // Diameter too: the erase is given half of it as its radius.
            return eraserWidth_;
        case Tool::Text:
        case Tool::Select:
        case Tool::NewDrawing:
        case Tool::NewScreenshot:
            return std::nullopt;
    }
    return std::nullopt;
}

// ================= A note being typed =================

void Editor::BeginEditingNote(ItemId id) {
    if (Manager().FindItemAnywhere(id) == nullptr) {
        return;
    }
    if (editingNoteItemId_.has_value() && *editingNoteItemId_ != id) {
        // Still the live buffer here (nothing's pressed Escape on it this
        // frame), so it holds whatever was actually typed - safe to commit
        // as-is, same as EndEditingNote's own doc comment describes.
        EndEditingNote(noteEditBuffer_);
    }
    // Found after that edit has ended, not before: a write of it that fails
    // puts the library back as it was, moving every snippet in it.
    const Item* item = Manager().FindItemAnywhere(id);
    if (!item) {
        return;
    }
    editingNoteItemId_ = id;
    noteEditBuffer_ = item->noteText;
    session_.BeginTextEdit(id);
    noteEditJustBegun_ = true;
    if (window_) {
        window_->RequestTextInput();
    }
}

void Editor::EndEditingNote(const std::string& text) {
    if (!editingNoteItemId_.has_value()) {
        return;
    }
    // One undo entry for the whole edit, and none for an edit that changed
    // nothing - see Session::EndTextEdit.
    session_.EndTextEdit(text);
    editingNoteItemId_.reset();
    if (window_) {
        window_->ReleaseTextInput();
    }
}

void Editor::CommitNoteBeingEdited() {
    if (editingNoteItemId_.has_value()) {
        EndEditingNote(noteEditBuffer_);
    }
}

void Editor::ForgetNoteEditEndedElsewhere() {
    if (editingNoteItemId_.has_value() && session_.TextEditItem() != editingNoteItemId_) {
        editingNoteItemId_.reset();
        if (window_) {
            window_->ReleaseTextInput();
        }
    }
}

// ================= Making snippets =================

std::optional<ItemCreationKind> Editor::EmptyCanvasCreationKind() const {
    const std::optional<CreationTrigger> held = CreationTriggerFor(held_);
    if (!held.has_value()) {
        return std::nullopt;
    }
    if (Cfg().screenshotTrigger == *held) {
        return ItemCreationKind::Screenshot;
    }
    if (Cfg().drawingTrigger == *held) {
        return ItemCreationKind::Drawing;
    }
    return std::nullopt;
}

// Somewhere to put a new item, always. The library is allowed to hold no
// canvases at all now (see CanvasManager's own class comment), and the
// answer to "screenshot, please" in that state is a canvas with the
// screenshot on it - not a dead button. That makes the empty state
// self-healing: the first thing created gets out of it on its own, and
// only an explicit "New canvas" is needed if what you want *is* an empty
// canvas.
const Canvas& Editor::EnsureCanvasForNewItem() {
    if (const Canvas* existing = Manager().CurrentOrNull()) {
        return *existing;
    }
    CreateAndSwitchToNewCanvas();
    // AddCanvas (via CreateAndSwitchToNewCanvas) always produces one, and
    // makes it current - there is no failure path.
    return *Manager().CurrentOrNull();
}

ItemId Editor::CreateFullscreenItem(ItemCreationKind kind) { return CreateFullscreenItem(kind, displayW_, displayH_); }

ItemId Editor::CreateFullscreenItem(ItemCreationKind kind, float width, float height) {
    const Canvas& canvas = EnsureCanvasForNewItem();
    Item prototype =
        PrototypeForKind(kind, Rect{0.0f, 0.0f, width, height}, ItemNameForKind(kind, canvas, /*fullscreen=*/true));
    prototype.isFullscreen = true;
    // Through the session, so that making it is on the history - see
    // Session::CreateItem.
    const ItemId id = session_.CreateItem(std::move(prototype));
    if (id == 0) {
        return 0;
    }
    HandOverNewItem(kind, id);
    return id;
}

ItemId Editor::CreateRegionItem(ItemCreationKind kind, Rect rect) {
    if (rect.w < kRegionMinSize || rect.h < kRegionMinSize) {
        return 0;
    }
    const Canvas& canvas = EnsureCanvasForNewItem();
    const ItemId id =
        session_.CreateItem(PrototypeForKind(kind, rect, ItemNameForKind(kind, canvas, /*fullscreen=*/false)));
    if (id == 0) {
        return 0;
    }
    HandOverNewItem(kind, id);
    return id;
}

Item Editor::PrototypeForKind(ItemCreationKind kind, Rect rect, std::string name) const {
    // Settings > Defaults.
    const SnippetDefaults& defaults =
        kind == ItemCreationKind::Screenshot ? Cfg().screenshotDefaults : Cfg().drawingDefaults;
    Item item;
    item.hasBackground = kind == ItemCreationKind::Screenshot;
    item.name = std::move(name);
    item.rect = rect;
    item.keepAspect = defaults.keepAspect;
    item.foregroundOpacity = defaults.foregroundOpacity;
    item.picture.opacity = defaults.backgroundOpacity;
    if (kind == ItemCreationKind::Drawing) {
        item.picture.tintColorRGBA = Cfg().drawingBackgroundColorRGBA;
    }
    if (Cfg().noteTextSizePx > 0.0f) {
        item.noteTextSizePx = Cfg().noteTextSizePx;
    }
    item.noteTextColorRGBA = Cfg().noteTextColorRGBA;
    return item;
}

void Editor::HandOverNewItem(ItemCreationKind kind, ItemId id) {
    switch (kind) {
        case ItemCreationKind::Screenshot:
            // Selected as made, so its bar is there to act on it at once -
            // as a drawing is, by entering drawing mode.
            SelectOnly(id);
            break;
        case ItemCreationKind::Drawing:
            // A drawing is made to be drawn in: straight into drawing mode,
            // with the pen.
            EnterDrawingMode(id, Tool::Draw);
            break;
    }
}

// ----- The drawing a stray click made -----

void Editor::WatchAsUntouched(ItemId id) {
    untouchedDrawing_ = id;
    untouchedDrawingRevision_ = session_.HistoryRevision();
}

void Editor::SettleUntouchedDrawing() {
    if (!untouchedDrawing_.has_value()) {
        return;
    }
    const ItemId id = *untouchedDrawing_;
    untouchedDrawing_.reset();
    // A note still being typed into is committed first, so what counts is
    // whatever was typed - the press that settles it would end the edit a
    // frame later anyway.
    if (editingNoteItemId_ == id) {
        EndEditingNote(noteEditBuffer_);
    }
    // Does nothing to a snippet something went into, or one that is gone.
    session_.DiscardIfUntouched(id);
}

void Editor::KeepDrawingsPlaced(const std::vector<ItemId>& ids) {
    if (untouchedDrawing_.has_value() && std::find(ids.begin(), ids.end(), *untouchedDrawing_) != ids.end()) {
        untouchedDrawing_.reset();
    }
}

void Editor::WatchUntouchedDrawing() {
    // A press elsewhere settles it as it happens; this catches moving on
    // without one - another canvas, its own Close - on the frame after.
    // And it stops being watched the frame after something has gone into
    // it: from then on it is a drawing like any other, and an undo that
    // empties it again is no reason to erase it for good, with the redo of
    // what it held.
    if (!untouchedDrawing_.has_value()) {
        return;
    }
    const Canvas* current = Manager().CurrentOrNull();
    const bool onScreen =
        current != nullptr && std::any_of(current->items.begin(), current->items.end(), [&](const Item& item) {
            return item.id == *untouchedDrawing_ && !Manager().IsDeleted(*current, item);
        });
    if (!onScreen) {
        SettleUntouchedDrawing();
    } else if (!session_.IsUntouched(*untouchedDrawing_)) {
        untouchedDrawing_.reset();
    }
}

// ================= Canvases =================

CanvasId Editor::CreateCanvasInCurrentFolder() {
    const CanvasId id = session_.AddCanvas(TimestampName());
    // It lands at the end of the folder, which in a long folder is off the
    // bottom of the Overview's grid.
    if (views_ != nullptr) {
        views_->CanvasMade(id);
    }
    return id;
}

CanvasId Editor::CreateCanvasBesideCurrent() {
    // The browsed folder and the current canvas's are deliberately
    // decoupled (see CanvasManager's class comment); browsing elsewhere
    // without opening anything is what tells them apart. A switch to the
    // new canvas re-syncs the browsed folder anyway.
    if (const Canvas* current = Manager().CurrentOrNull()) {
        session_.SwitchToFolder(current->folderId);
    }
    return CreateCanvasInCurrentFolder();
}

void Editor::CreateAndSwitchToNewCanvas() {
    // Settled like any other switch: the shortcut can land mid-gesture.
    // Nothing to settle when the new canvas is already current - the
    // library had none, and the press that asked for a snippet is what is
    // in flight (see EnsureCanvasForNewItem).
    SwitchCanvas(CreateCanvasBesideCurrent());
}

// A new canvas that the selected snippets come along to - "these belong
// somewhere of their own", which otherwise takes a new canvas, a switch
// back, a cut, a switch forward and a paste.
//
// With nothing selected this is exactly CreateAndSwitchToNewCanvas, and
// says so by doing nothing else: an empty selection is not a reason to
// refuse the canvas.
//
// The moves happen before the switch, because MoveOrCopyItemToCanvas
// takes snippets off the *current* canvas (see its header note), and the
// switch happens even if every move failed - there is a new canvas either
// way, and leaving the app on the old one would make the shortcut look
// like it had done nothing.
//
// The hand is settled before the moves rather than at the switch - by the
// command's Canvas scope: a shortcut can land mid-gesture, and a stroke or
// a drag in flight on a selected snippet has to end on the canvas it
// started on, before the snippet leaves it.
void Editor::MoveSelectionToNewCanvas() {
    const CanvasId target = CreateCanvasBesideCurrent();
    const std::vector<ItemId> moved = session_.SendItemsTo(selection_, target, /*copy=*/false).items;
    session_.SwitchToCanvas(target);
    // What arrived is what is selected, so it can be arranged straight
    // away - and the canvas bar is over it rather than over nothing.
    selection_ = moved;
    if (!moved.empty()) {
        const Canvas* canvas = Manager().FindCanvas(target);
        Say(std::string(strings::kToastMovedToPrefix) + (canvas != nullptr ? canvas->name : std::string()));
    }
}

void Editor::SwitchCanvas(CanvasId id) {
    if (Manager().CurrentCanvasId() == id) {
        return;
    }
    // Whatever the hand is in the middle of ends on the canvas it started
    // on, and a note being typed is committed to the item it belongs to -
    // only the current canvas is drawn, so an editor left open across the
    // switch would strand what was typed.
    Settle(Scope::Canvas);
    session_.SwitchToCanvas(id);
}

// Alt+wheel's other half: a quick canvas switcher, so moving between
// canvases doesn't have to mean opening the whole Overview and closing it
// again. Clamped, not wrapped: running off either end stops there rather
// than teleporting to the far side, so holding the gesture is a safe way
// to reach the first or last canvas.
void Editor::SwitchCanvasByOffset(int delta) {
    const Canvas* current = Manager().CurrentOrNull();
    if (!current || delta == 0) {
        return;
    }
    // The folder the *current canvas* lives in, not CurrentFolderId().
    // Those two are deliberately decoupled (browsing a folder in the
    // Overview doesn't switch away from the canvas being edited - see
    // CanvasManager's own class comment), so stepping relative to a merely
    // *browsed* folder could jump somewhere with no relation to what's on
    // screen.
    const FolderId folderId = current->folderId;
    std::vector<CanvasId> siblings;
    size_t index = 0;
    for (const Canvas& canvas : Manager().Canvases()) {
        if (canvas.folderId != folderId || Manager().IsDeleted(canvas)) {
            continue;
        }
        if (canvas.id == current->id) {
            index = siblings.size();
        }
        siblings.push_back(canvas.id);
    }
    if (siblings.empty()) {
        return;  // unreachable - `current` is one of them - but nothing below is safe on an empty list
    }

    // Clamped, not wrapped: at either end this resolves to where it
    // already is.
    const auto lastIndex = static_cast<long long>(siblings.size()) - 1;
    const auto target = static_cast<size_t>(
        std::clamp(static_cast<long long>(index) + delta, static_cast<long long>(0), lastIndex));

    if (siblings[target] != current->id) {
        SwitchCanvas(siblings[target]);
        current = Manager().CurrentOrNull();
    }

    // Shown even when the position didn't change, so hitting either end
    // reads as "you're at the last one" rather than as the gesture having
    // stopped working - the same reasoning as the size preview arming at
    // its own clamp.
    // Parenthesized rather than just spaced apart: canvases are named
    // "Canvas N" by default, so "Canvas 1 5/5" puts two unrelated numbers
    // next to each other and reads like one of them is a typo.
    char text[160];
    std::snprintf(text, sizeof(text), "%s  (%zu/%zu)", current->name.c_str(), target + 1, siblings.size());
    Say(text);
}

void Editor::QuickCapture(float displayW, float displayH) {
    // Whatever the hand was in the middle of has ended on the canvas it
    // started on, as before any other canvas switch: a capture hotkey can
    // arrive mid-stroke, and its command's scope, Canvas, ends it (the tray
    // runs a capture only as that command).
    //
    // A canvas of its own, beside the one being worked on - a capture is
    // about where you are, not where you were last looking - and we go to
    // it: a screen full of captures piled on the canvas you were drawing
    // on is hard to tell apart later, where one capture per canvas is a
    // strip of tiles you can read at a glance in the Overview.
    session_.SwitchToCanvas(CreateCanvasBesideCurrent());
    const ItemId made = CreateFullscreenItem(ItemCreationKind::Screenshot, displayW, displayH);
    // Not made when it could not be written - see Session::Land.
    Say(made != 0 ? strings::kToastCapturedScreenshot : strings::kToastNotWritten);
}

// ================= Placing and changing the selection =================

void Editor::ToggleFullscreenUndoably(ItemId id, bool stretch) {
    session_.ToggleFullscreen(id, stretch);
    KeepDrawingsPlaced({id});
}

void Editor::ResetToNativeSizeUndoably(ItemId id) {
    session_.ResetItemToNativeSize(id);
    KeepDrawingsPlaced({id});
}

void Editor::PreviewPlacement(const std::vector<std::pair<ItemId, Rect>>& rects) {
    std::vector<ItemId> ids;
    for (const auto& [id, rect] : rects) {
        ids.push_back(id);
    }
    if (!session_.Placing(ids)) {
        session_.BeginPlacement(ids);
    }
    for (const auto& [id, rect] : rects) {
        session_.PreviewRect(id, rect);
    }
}

void Editor::NudgeSelection(float dx, float dy, Filing filing) {
    // A run of presses - or a key held down, repeating - is one undo, back
    // to where the run began: a burst's (see NudgeBurst).
    std::vector<std::pair<ItemId, Rect>> rects;
    for (const ItemId id : selection_) {
        const Item* item = Manager().FindItemAnywhere(id);
        if (item == nullptr || item->isFullscreen) {
            continue;  // a fullscreen snippet has nowhere to go
        }
        KeepDrawingsPlaced({id});
        rects.emplace_back(id, ClampRectToViewport(Rect{item->rect.x + dx, item->rect.y + dy, item->rect.w,
                                                        item->rect.h},
                                                   displayW_, displayH_));
    }
    if (filing == Filing::Burst) {
        PreviewPlacement(rects);
    } else {
        session_.SetRects(rects);
    }
}

void Editor::ScaleSelectionByWheel(int steps) {
    if (steps == 0 || !SelectionLive()) {
        return;
    }
    // The snippets to scale, as they are now: fullscreen ones fill the
    // screen by definition and have no size of their own to change.
    std::vector<const Item*> items;
    for (const ItemId id : selection_) {
        const Item* item = Manager().FindItemAnywhere(id);
        if (item != nullptr && !item->isFullscreen && item->rect.w > 0.0f && item->rect.h > 0.0f) {
            items.push_back(item);
        }
    }
    if (items.empty()) {
        return;
    }
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    float floorScale = 0.0f;
    for (const Item* item : items) {
        minX = std::min(minX, item->rect.x);
        minY = std::min(minY, item->rect.y);
        maxX = std::max(maxX, item->rect.x + item->rect.w);
        maxY = std::max(maxY, item->rect.y + item->rect.h);
        const MinItemSize smallest = MinimumSizeForAspectRatio(item->rect.w / item->rect.h);
        floorScale = std::max({floorScale, smallest.w / item->rect.w, smallest.h / item->rect.h});
    }
    const float boxW = maxX - minX;
    const float boxH = maxY - minY;
    float scale = std::pow(kWheelScaleStep, static_cast<float>(steps));
    // Down to the smallest snippet's floor, and up to where the group fills
    // the screen - past that, growing would only push snippets against its
    // edges, one at a time, and out of step with the rest.
    const float ceilingScale = std::max(
        1.0f, std::min(displayW_ > 0.0f ? displayW_ / boxW : 1.0f, displayH_ > 0.0f ? displayH_ / boxH : 1.0f));
    scale = std::clamp(scale, std::min(floorScale, 1.0f), ceilingScale);
    if (scale == 1.0f) {
        return;
    }
    const float anchorX = minX + boxW * 0.5f;
    const float anchorY = minY + boxH * 0.5f;
    std::vector<ItemId> ids;
    std::vector<std::pair<ItemId, Rect>> rects;
    for (const Item* item : items) {
        ids.push_back(item->id);
        const Rect scaled{anchorX + (item->rect.x - anchorX) * scale, anchorY + (item->rect.y - anchorY) * scale,
                          item->rect.w * scale, item->rect.h * scale};
        rects.emplace_back(item->id, ClampRectToViewport(scaled, displayW_, displayH_));
    }
    // A drawing placed and then scaled is one someone wants, as one moved
    // or resized by hand is.
    KeepDrawingsPlaced(ids);
    // A spin of the wheel is one undo, back to the size it started at: a
    // burst's (see WheelBurst).
    PreviewPlacement(rects);
}

void Editor::StepSelectionOpacity(int steps, bool background) {
    if (steps == 0 || !SelectionLive()) {
        return;
    }
    const float delta = kWheelOpacityStep * static_cast<float>(steps);
    // On the popover's whole-percent grid, so the wheel and the slider
    // never disagree about what a value is.
    const auto stepped = [delta](float value, float lowest) {
        return std::clamp(std::round((value + delta) * 100.0f) / 100.0f, lowest, 1.0f);
    };
    std::optional<float> shown;
    std::vector<std::pair<ItemId, ItemStyle>> styles;
    for (const ItemId id : selection_) {
        const Item* item = Manager().FindItemAnywhere(id);
        if (item == nullptr) {
            continue;
        }
        ItemStyle style = ItemStyle::Of(*item);
        if (background) {
            style.pictureOpacity = stepped(style.pictureOpacity, 0.0f);
            shown = style.pictureOpacity;
        } else {
            // Not below a tenth - see Popups::RenderItemOpacity.
            style.foregroundOpacity = stepped(style.foregroundOpacity, 0.1f);
            shown = style.foregroundOpacity;
        }
        styles.emplace_back(id, style);
    }
    if (!shown.has_value()) {
        return;
    }
    // A spin of the wheel is one undo, back to where it started: a
    // burst's (see WheelBurst).
    session_.PreviewStyles(styles);
    // The value, since the change itself can be hard to judge by eye: the
    // last snippet's, which with several selected is the one selected last.
    char text[64];
    std::snprintf(text, sizeof(text), background ? strings::kToastBackgroundOpacity : strings::kToastForegroundOpacity,
                  static_cast<int>(std::round(*shown * 100.0f)));
    Say(text);
}

// What the wheel does is told apart by a modifier, and without one by the
// mode. It reaches here only with nothing above the canvas that takes it:
// a panel has its own scroll, a popup its own widgets, and a gesture in
// flight is on the mouse the wheel is on - a notch mid-stroke would switch
// the canvas under it, and one mid-drag would be filed inside the drag.
//
// Alt held: step between the canvases of the current canvas's own folder
// - Alt being the modifier that already means "pick up, whatever tool is
// in hand" for a press. This one deliberately does not yield to an ImGui
// widget under the pointer, matching an Alt press; everything below does.
//
// Ctrl or Shift held: the selection's background or foreground opacity, in
// either mode - in drawing mode the selection is the snippet being drawn
// on.
//
// Nothing held: in drawing mode, stroke/eraser size, the near-universal
// convention in drawing tools and the one tool "option" reached for
// *during* work rather than while configuring - which is why there is no
// width slider anywhere. Outside it, the selection's size. The mode is
// what decides, not whether something happens to be selected: in drawing
// mode something always is, and the wheel must not start scaling the
// snippet under the pen.
Editor::WheelKind Editor::KindOfWheel() const {
    if (held_.alt) {
        return WheelKind::Canvases;
    }
    if (PointerOverView()) {
        return WheelKind::Nothing;  // a widget under the pointer has the wheel
    }
    if (held_.ctrl != held_.shift) {
        return WheelKind::SelectionOpacity;
    }
    if (held_.ctrl) {
        return WheelKind::Nothing;  // both held: neither opacity is meant more than the other
    }
    return DrawingItem().has_value() ? WheelKind::ToolSize : WheelKind::SelectionSize;
}

void Editor::Wheel(float notches) {
    if (notches == 0.0f) {
        return;
    }
    switch (KindOfWheel()) {
        case WheelKind::Nothing:
            return;
        case WheelKind::Canvases:
            // Wheel up goes back through the list, wheel down forward - the
            // direction a page scrolls, applied to canvases.
            if (const int steps = TakeWheelSteps(canvasWheelRemainder_, notches); steps != 0) {
                SwitchCanvasByOffset(-steps);
            }
            return;
        case WheelKind::SelectionOpacity:
            if (const int steps = TakeWheelSteps(selectionWheelRemainder_, notches); steps != 0) {
                StepSelectionOpacity(steps, /*background=*/held_.ctrl);
            }
            return;
        case WheelKind::SelectionSize:
            if (const int steps = TakeWheelSteps(selectionWheelRemainder_, notches); steps != 0) {
                ScaleSelectionByWheel(steps);
            }
            return;
        case WheelKind::ToolSize:
            break;
    }
    if (const int steps = TakeWheelSteps(sizeWheelRemainder_, notches); steps != 0) {
        // Nothing whole out of the accumulator yet - a high-resolution wheel
        // mid-notch - is no size change, and nothing to show either.
        const auto step = static_cast<float>(steps);
        switch (ActiveTool()) {
            case Tool::Draw:
                SetDrawWidth(std::clamp(drawWidth_ + step, 1.0f, 24.0f));
                // Said even when the value was already at its clamp:
                // "you're at the maximum" is feedback too, and a wheel step
                // that showed nothing would read as the wheel not working.
                Views().ToolSized(/*pen=*/true);
                break;
            case Tool::Erase:
                SetEraserWidth(std::clamp(eraserWidth_ + step * 2.0f, 8.0f, 64.0f));
                Views().ToolSized(/*pen=*/false);
                break;
            case Tool::Text:
            case Tool::Select:
            case Tool::NewDrawing:
            case Tool::NewScreenshot:
                break;  // none of these has a size of its own - see the Tool enum
        }
    }
}

void Editor::DeleteSelection() {
    // A copy: deleting clears nothing itself, but the message and the
    // session are free to look at the selection while this runs.
    const std::vector<ItemId> doomed = selection_;
    DeleteItemsSaid(doomed);
    ClearSelection();
}

void Editor::DeleteItemsSaid(const std::vector<ItemId>& itemIds) {
    // Marked, and onto the history as one step - see Session::DeleteItems.
    if (session_.DeleteItems(itemIds) == 0) {
        return;
    }
    Say(strings::kToastDeleted);
}

void Editor::ClearItemDrawing(ItemId itemId) {
    // Every stroke, as one undoable step - see Session::ClearDrawing.
    if (session_.ClearDrawing(itemId)) {
        Say(strings::kToastClearedDrawing);
    }
}

// ================= The clipboard =================

void Editor::CopySelectionToClipboard(bool cut) {
    if (selection_.empty()) {
        return;  // nothing selected is nothing to copy, and says so by
                 // leaving whatever is on the clipboard alone
    }
    clipboard_ = selection_;
    clipboardIsCut_ = cut;
    Say(cut ? strings::kToastCut : strings::kToastCopied);
}

bool Editor::IsWaitingToBeCut(ItemId id) const {
    return clipboardIsCut_ && std::find(clipboard_.begin(), clipboard_.end(), id) != clipboard_.end();
}

// What is on the clipboard, onto the canvas being looked at: copies of it,
// or - after a Cut - the snippets themselves, moved here.
//
// Every snippet is looked up as this runs rather than trusted from when it
// was copied: one deleted since is skipped, and a paste that finds nothing
// left says so instead of pasting an empty selection. That check is the
// whole reason the clipboard holds ids.
//
// A copy lands on top of its source when the source is on this canvas, so
// there it is offset the way the Properties popover's own Copy is - far
// enough to see that there are now two. Pasted onto another canvas it
// keeps its place exactly, which is where the eye expects it. A cut pasted
// back onto its own canvas is the snippet itself, and there is nothing to
// tell apart: it stays exactly where it was.
//
// One undo takes the whole paste back: the copies go, and what a cut
// moved here goes back where it came from - see Session::Paste.
void Editor::PasteFromClipboard() {
    if (clipboard_.empty() || Manager().CurrentOrNull() == nullptr) {
        return;
    }
    const bool cut = clipboardIsCut_;
    const Session::Placed pasted = session_.Paste(clipboard_, cut);
    if (pasted.items.empty()) {
        // Nothing left to paste - or a paste whose write failed, which the
        // line along the bottom says more of (see PersistenceWarning).
        Say(session_.LastWriteFailed() ? strings::kToastNotWritten : strings::kToastNothingToPaste);
        return;
    }
    // What was pasted is what is selected, so it can be moved straight
    // away - and, for a cut pasted onto another canvas, so that what
    // arrived is the thing the bar is over.
    selection_ = pasted.items;
    if (cut) {
        clipboard_.clear();
        clipboardIsCut_ = false;
    }
    Say(pasted.pictureLost ? strings::kToastCopiedWithoutPicture : strings::kToastPasted);
}

// A copy of every selected snippet, on this canvas, offset the way a
// paste onto its own canvas is - Copy and Paste in one step, and what
// Ctrl+D means everywhere else.
//
// Deliberately not routed through the clipboard: duplicating something is
// not a reason to lose what was copied earlier, and the clipboard holds
// ids rather than snippets (see PasteFromClipboard), so borrowing it here
// would also mean deciding what a later paste of those ids should do.
//
// What was made is what ends up selected, so it can be dragged straight
// off the original - the same rule a paste follows, and the reason the
// copies are offset at all.
void Editor::DuplicateSelection() {
    if (selection_.empty() || Manager().CurrentOrNull() == nullptr) {
        return;
    }
    const Session::Placed made = session_.Duplicate(selection_);
    if (made.items.empty()) {
        return;
    }
    selection_ = made.items;
    Say(made.pictureLost ? strings::kToastCopiedWithoutPicture : strings::kToastDuplicated);
}

// ================= The history =================
//
// The history itself is the session's (see Session::Undo); what is left
// here is saying what a step did.

void Editor::Undo() {
    // Dispatch has settled the hand: mid-drag or mid-stroke, what the hand
    // had done so far is the most recent thing done, and this takes it
    // back. Carried on, a drag would overwrite whatever the undo restored
    // and drop that step from the history unseen.
    //
    // A drawing a click made and nothing was put into is taken back by
    // going, not by leaving an empty snippet behind, marked deleted - when
    // it is the most recent thing done on its canvas. A press anywhere else
    // settles it as it happens, but a key does not: a paste made after it
    // is the step to take back, and the hand has moved on from the drawing
    // - which goes the way moving on takes it, without being the undo.
    if (untouchedDrawing_.has_value() && session_.HistoryRevision() != untouchedDrawingRevision_) {
        SettleUntouchedDrawing();
    }
    if (untouchedDrawing_.has_value() && session_.DiscardIfUntouched(*untouchedDrawing_)) {
        untouchedDrawing_.reset();
        ShowUndoStep(Session::UndoStep{Session::UndoWhat::Create, /*undone=*/true});
        return;
    }
    ShowUndoStep(session_.Undo());
}

void Editor::Redo() {
    // See Undo. A drag that moved anything is a new step, so this finds
    // nothing left to redo - as it would once the drag was let go.
    ShowUndoStep(session_.Redo());
}

void Editor::ShowUndoStep(const std::optional<Session::UndoStep>& step) {
    if (!step.has_value()) {
        return;
    }
    if (step->intoDeletedCanvas != 0) {
        // Where it went is out of sight, and the message says where to find
        // it - see Session::UndoStep::intoDeletedCanvas.
        const Canvas* canvas = Manager().FindCanvas(step->intoDeletedCanvas);
        Say(std::string(strings::kToastSentToDeletedCanvasPrefix) + (canvas != nullptr ? canvas->name : std::string()));
        return;
    }
    // What the message calls it - chosen once per kind, so the two
    // directions can't name the same thing differently.
    const char* what = strings::kUndoStroke;
    switch (step->what) {
        case Session::UndoWhat::Stroke:
            what = strings::kUndoStroke;
            break;
        case Session::UndoWhat::Erase:
            what = strings::kUndoErase;
            break;
        case Session::UndoWhat::Delete:
            what = strings::kUndoDelete;
            break;
        case Session::UndoWhat::TextEdit:
            what = strings::kUndoTextEdit;
            break;
        case Session::UndoWhat::Create:
            what = strings::kUndoCreate;
            break;
        case Session::UndoWhat::Placement:
            what = strings::kUndoPlacement;
            break;
        case Session::UndoWhat::Paste:
            what = strings::kUndoPaste;
            break;
        case Session::UndoWhat::Duplicate:
            what = strings::kUndoDuplicate;
            break;
        case Session::UndoWhat::Style:
            what = strings::kUndoStyle;
            break;
        case Session::UndoWhat::Move:
            what = strings::kUndoMove;
            break;
        case Session::UndoWhat::CopyTo:
            what = strings::kUndoCopyTo;
            break;
    }
    Say(std::string(step->undone ? strings::kToastUndidPrefix : strings::kToastRedidPrefix) + what);
}

// ================= What is under the pointer =================

// One walk over the current canvas, answering what a screen point is on -
// see PointerTarget for what each answer is for:
//  - the target: the frontmost thing that would take a press there - a
//    selection bar button, a selected snippet's handle, or an item's body;
//  - `body`: the frontmost item whose content rect holds the point.
// The selection's furniture is drawn over every snippet, so it is asked
// first, the bar before the handles and the snippet selected last before
// the others - the same order it is painted in. It exists only while the
// selection is live (see SelectionLive), and never on a fullscreen
// snippet, which has no handles.
PointerTarget Editor::ResolvePointerTarget(float x, float y) const {
    PointerTarget target;
    const Canvas* canvasPtr = Manager().CurrentOrNull();
    if (!canvasPtr) {
        return target;  // no canvas, so nothing anywhere to hit
    }
    const Canvas& canvas = *canvasPtr;
    for (size_t revIdx = canvas.items.size(); revIdx-- > 0;) {
        const Item& item = canvas.items[revIdx];
        // A minimized item keeps its last on-screen rect (only the drawing's
        // own skip-if-minimized stops it from actually rendering there -
        // see Item::minimized's own doc comment), so it has to be skipped
        // here too, or a stroke over the patch it last covered would go
        // into the minimized item instead of whatever is visible there. A
        // deleted one is nowhere.
        if (item.minimized || Manager().IsDeleted(canvas, item)) {
            continue;
        }
        const Rect& r = item.rect;
        // Closed on the far edges, unlike the furniture's own rects: the
        // item's outermost pixel column is drawable, and a stroke started
        // exactly there has always landed in the item.
        if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
            target.body = item.id;
            break;
        }
    }
    if (SelectionLive() && !selection_.empty()) {
        if (const std::optional<Rect> bounds = SelectionBounds()) {
            const std::vector<ChromeButton> buttons = BarButtons();
            const BarLayout bar = LayoutBar(*bounds, displayW_, displayH_, buttons.size());
            for (const ChromeButton button : buttons) {
                if (BarButtonRect(bar, buttons, button).Contains(x, y)) {
                    target.kind = PointerTarget::Kind::Button;
                    target.button = button;
                    return target;
                }
            }
        }
        for (auto it = selection_.rbegin(); it != selection_.rend(); ++it) {
            const Item* item = nullptr;
            for (const Item& candidate : canvas.items) {
                if (candidate.id == *it) {
                    item = &candidate;
                    break;
                }
            }
            if (item == nullptr || item->isFullscreen) {
                continue;
            }
            for (const HandleSpec& h : HandleSpecs(item->rect)) {
                if (HandleHitRect(h.center).Contains(x, y)) {
                    target.kind = PointerTarget::Kind::Handle;
                    target.item = item->id;
                    target.handle = h.handle;
                    return target;
                }
            }
        }
    }
    if (target.body.has_value()) {
        target.kind = PointerTarget::Kind::Body;
        target.item = *target.body;
    }
    return target;
}

std::optional<platform::Vec2> Editor::SelectionBarButtonCenter(ChromeButton button) const {
    if (!SelectionLive()) {
        return std::nullopt;
    }
    const std::optional<Rect> bounds = SelectionBounds();
    if (!bounds.has_value()) {
        return std::nullopt;
    }
    const std::vector<ChromeButton> buttons = BarButtons();
    if (std::find(buttons.begin(), buttons.end(), button) == buttons.end()) {
        return std::nullopt;  // not on the bar the mode shows
    }
    const HitRect rect = BarButtonRect(LayoutBar(*bounds, displayW_, displayH_, buttons.size()), buttons, button);
    return platform::Vec2{(rect.min.x + rect.max.x) * 0.5f, (rect.min.y + rect.max.y) * 0.5f};
}

std::optional<Command> Editor::BarButtonCommand(ChromeButton button) const {
    const std::optional<ItemId> primaryId = PrimarySelection();
    if (!primaryId.has_value()) {
        return std::nullopt;
    }
    Command command{CommandForBarButton(button), *primaryId};
    if (const std::optional<platform::Vec2> center = SelectionBarButtonCenter(button)) {
        if (button == ChromeButton::More) {
            // The popover opens below the button's bottom-right corner,
            // paired with the pivot the popover opens with, so its right
            // edge (not its left) tracks this point however wide it ends up
            // - the bar can sit near the screen's right edge.
            command.at = platform::Vec2{center->x + Px(kBarButtonSize) * 0.5f,
                                        center->y + Px(kBarButtonSize) * 0.5f + Px(6.0f)};
        } else {
            command.at = *center;
        }
    }
    return command;
}

}  // namespace sz::ui
