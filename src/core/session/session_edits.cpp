#include "core/session/session.h"

#include <algorithm>
#include <cassert>
#include <ctime>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "core/canvas/item_geometry.h"

namespace sz::core {

using history::Change;
using history::Step;
using history::What;

namespace {

// A deletion stamp for now - never 0, which is what "not deleted" is
// spelled as.
int64_t DeletionStampNow() { return std::max<int64_t>(static_cast<int64_t>(std::time(nullptr)), 1); }

// Nothing put into it: no ink, no text, and no picture of its own - a
// screenshot is content even when the capture failed.
bool ItemIsUntouched(const Item& item) {
    return !item.hasBackground && item.strokes.empty() && item.noteText.empty();
}

size_t IndexOn(const Canvas& canvas, ItemId id) {
    size_t index = 0;
    while (index < canvas.items.size() && canvas.items[index].id != id) {
        ++index;
    }
    return index;
}

}  // namespace

// ================= The history =================

bool Session::CanUndo() const { return history_.CanUndo(Model().CurrentCanvasId()); }

bool Session::CanRedo() const { return history_.CanRedo(Model().CurrentCanvasId()); }

void Session::EndOpenGesture() {
    EndTextEdit();
    EndPlacement();
    EndStyleEdit();
    EndErase();
    if (shapeItemId_.has_value()) {
        EndShape(shapeLastX_, shapeLastY_);
    }
}

std::optional<Session::UndoStep> Session::StepHistory(bool undo) {
    // Whatever the hand is in the middle of is the most recent thing done,
    // and is filed first - it is what this takes back.
    EndOpenGesture();
    const CanvasId canvas = Model().CurrentCanvasId();
    const Step* next = undo ? history_.NextUndo(canvas) : history_.NextRedo(canvas);
    if (next == nullptr) {
        return std::nullopt;
    }
    // Every change asked before any is applied, so that a step is never
    // applied halfway. The history keeps every step applicable, so one that
    // is not is a gap in keeping it: dropped, rather than left standing in
    // front of every step below it, and loud in a debug build.
    const bool applies = std::all_of(next->changes.begin(), next->changes.end(),
                                     [&](const Change& change) { return history::CanApply(Model(), change, undo); });
    std::vector<ItemId> items;
    for (const Change& change : next->changes) {
        items.push_back(change.item);
    }
    const Checkpoint before = Before(items);
    Step step = undo ? history_.TakeUndo(canvas) : history_.TakeRedo(canvas);
    if (!applies) {
        assert(false && "a step on the history no longer applies");
        return std::nullopt;
    }
    // As it is, to be put back should the write fail: applying a step turns
    // it into what the opposite direction needs.
    const Step taken = step;
    // Undone last first, redone first to last.
    if (undo) {
        for (auto change = step.changes.rbegin(); change != step.changes.rend(); ++change) {
            history::Apply(Model(), *change, /*undo=*/true);
        }
    } else {
        for (Change& change : step.changes) {
            history::Apply(Model(), change, /*undo=*/false);
        }
    }
    Model().MarkChanged();
    if (!Land(before)) {
        history_.PutBack(canvas, taken, undo);
        return std::nullopt;
    }

    UndoStep result{step.what, undo};
    // Where each snippet it moved is now, for its history to follow it
    // there - after the step is on its new stack, so that on a redo what
    // follows goes in below it.
    std::vector<std::pair<ItemId, CanvasId>> moved;
    for (const Change& change : step.changes) {
        const history::Moved* move = std::get_if<history::Moved>(&change.kind);
        if (move == nullptr) {
            continue;
        }
        const CanvasId now = undo ? move->from : move->to;
        moved.emplace_back(change.item, now);
        if (const Canvas* into = Model().FindCanvas(now); undo && into != nullptr && Model().IsDeleted(*into)) {
            result.intoDeletedCanvas = now;
        }
    }
    if (undo) {
        history_.Undone(canvas, std::move(step));
    } else {
        history_.Redone(canvas, std::move(step));
    }
    for (const auto& [item, now] : moved) {
        history_.Migrate(item, now);
    }
    return result;
}

std::optional<Session::UndoStep> Session::Undo() { return StepHistory(/*undo=*/true); }

std::optional<Session::UndoStep> Session::Redo() { return StepHistory(/*undo=*/false); }

// ================= Strokes =================

void Session::CommitLiveStroke(ItemId itemId) {
    EndOpenGesture();
    const Checkpoint before = Before({itemId});
    Canvas* canvasPtr = Model().CurrentOrNull();
    if (!canvasPtr || liveLayer_.Strokes().empty()) {
        liveLayer_.Clear();
        return;
    }
    Canvas& canvas = *canvasPtr;
    const auto itemIt = std::find_if(canvas.items.begin(), canvas.items.end(),
                                      [&](const Item& i) { return i.id == itemId; });
    if (itemIt == canvas.items.end()) {
        liveLayer_.Clear();
        return;
    }
    const Stroke finished = liveLayer_.Strokes().back();
    itemIt->strokes.push_back(CanvasManager::BakeStrokeToNative(*itemIt, finished));
    liveLayer_.Clear();
    Model().MarkChanged();
    // The value, not only the id: redo pushes it back.
    Commit(before, canvas.id, Step{0, What::Stroke, {Change{itemId, history::StrokeAdded{itemIt->strokes.back()}}}});
}

bool Session::ClearDrawing(ItemId itemId) {
    EndOpenGesture();
    const Checkpoint before = Before({itemId});
    Item* item = Model().FindItemAnywhere(itemId);
    if (!item || item->strokes.empty()) {
        return false;
    }
    // Every stroke on the snippet, as the erase an eraser that wholly erased
    // everything would be (no fragments: nothing replaces what's removed) -
    // so one undo brings all of it back.
    history::StrokesErased erased;
    erased.strokeCountBefore = item->strokes.size();
    erased.replacements.reserve(item->strokes.size());
    for (size_t index = 0; index < item->strokes.size(); ++index) {
        erased.replacements.push_back({index, std::move(item->strokes[index]), {}});
    }
    item->strokes.clear();
    Model().MarkChanged();
    const CanvasId canvas = Model().CanvasHoldingItem(itemId).value_or(0);
    return Commit(before, canvas, Step{0, What::Erase, {Change{itemId, std::move(erased)}}});
}

// ================= Deleting and making =================

bool Session::DeleteItem(ItemId itemId) { return DeleteItems({itemId}) == 1; }

size_t Session::DeleteItems(const std::vector<ItemId>& itemIds) {
    EndOpenGesture();
    // Only snippets on this canvas: the step is filed under it. Marked where
    // they are, and one step for all of them is what an undo restores - one
    // per snippet made one Delete that many undos, and past the history's
    // cap the earliest could not be undone at all, which for a deleted
    // snippet is deleted for good.
    const Canvas* canvas = Model().CurrentOrNull();
    if (canvas == nullptr) {
        return 0;
    }
    const CanvasId canvasId = canvas->id;
    const Checkpoint before = Before(itemIds);
    Step step{0, What::Delete, {}};
    for (const ItemId id : itemIds) {
        if (Model().CanvasHoldingItem(id) == canvasId && Model().MarkDeleted(id, DeletionStampNow())) {
            step.changes.push_back(Change{id, history::DeletionChanged{0}});
        }
    }
    const size_t deleted = step.changes.size();
    if (deleted == 0 || !Commit(before, canvasId, std::move(step))) {
        return 0;
    }
    return deleted;
}

ItemId Session::CreateItem(Item prototype, bool undoable) {
    EndOpenGesture();
    const Checkpoint before = Before({});
    const ItemId itemId = Model().CreateItem(std::move(prototype));
    if (itemId == 0) {
        return 0;
    }
    if (Item* item = Model().FindItemAnywhere(itemId); item != nullptr && item->hasBackground) {
        CaptureShotItem(*item);
    }
    // Undone, it is marked deleted - where a capture taken by mistake can
    // still be found - and redone, unmarked.
    Step step{0, What::Create, {}};
    if (undoable) {
        step.changes.push_back(Change{itemId, history::DeletionChanged{DeletionStampNow()}});
    }
    return Commit(before, Model().CurrentCanvasId(), std::move(step)) ? itemId : 0;
}

ItemId Session::CreateItem(bool hasBackground, Rect rect, std::string name) {
    Item prototype;
    prototype.hasBackground = hasBackground;
    prototype.picture.opacity = hasBackground ? 1.0f : 0.0f;
    prototype.rect = rect;
    prototype.name = std::move(name);
    return CreateItem(std::move(prototype));
}

bool Session::IsUntouched(ItemId itemId) const {
    const Item* item = Model().FindItemAnywhere(itemId);
    return item != nullptr && ItemIsUntouched(*item);
}

bool Session::DiscardIfUntouched(ItemId itemId) {
    if (!IsUntouched(itemId)) {
        return false;
    }
    // Erased rather than marked - there is nothing in it to find again - and
    // its own changes go with it: its making, and any stroke taken back off
    // it. The rest of the canvas's history stays.
    return DeletePermanently(itemId);
}

// ================= Where snippets are =================

Session::Placements Session::PlacementsOf(const std::vector<ItemId>& ids) const {
    Placements placements;
    for (const ItemId id : ids) {
        if (const Item* item = Model().FindItemAnywhere(id)) {
            placements.emplace_back(id, history::Placement::Of(*item));
        }
    }
    return placements;
}

bool Session::CommitPlacements(const Checkpoint& checkpoint, const Placements& before, bool merge) {
    // Compared against now: a gesture that ends where it began - a click, a
    // drag back to the start - files nothing. When anything moved, every
    // snippet the gesture held is in the step, so that the next notch of a
    // burst is about the same snippets even when one of them met the edge.
    bool changed = false;
    Step step{0, What::Placement, {}};
    for (const auto& [id, placement] : before) {
        const Item* item = Model().FindItemAnywhere(id);
        if (item == nullptr) {
            continue;
        }
        changed = changed || history::Placement::Of(*item) != placement;
        step.changes.push_back(Change{id, history::PlacementChanged{placement}});
    }
    if (!changed) {
        Land(checkpoint);  // nothing to file, and nothing, or next to it, to write
        return false;
    }
    const CanvasId canvas = Model().CanvasHoldingItem(step.changes.front().item).value_or(0);
    return Commit(checkpoint, canvas, std::move(step), merge);
}

void Session::BeginPlacement(const std::vector<ItemId>& ids) {
    EndOpenGesture();
    placement_ = PlacementGesture{PlacementsOf(ids), Before(ids)};
}

bool Session::Placing(const std::vector<ItemId>& ids) const {
    return placement_.has_value() && placement_->before.size() == ids.size() &&
           std::equal(ids.begin(), ids.end(), placement_->before.begin(),
                      [](ItemId id, const auto& was) { return id == was.first; });
}

namespace {
bool Places(const std::vector<std::pair<ItemId, history::Placement>>& placements, ItemId id) {
    return std::any_of(placements.begin(), placements.end(), [id](const auto& p) { return p.first == id; });
}
}  // namespace

void Session::PreviewRect(ItemId id, Rect rect) {
    Item* item = Model().FindItemAnywhere(id);
    if (!placement_.has_value() || !Places(placement_->before, id) || item == nullptr) {
        return;
    }
    item->rect = rect;
    Model().MarkChanged();
    Model().CommitItemLayout(id);
}

void Session::PreviewLeaveFullscreen(ItemId id) {
    const Item* item = Model().FindItemAnywhere(id);
    if (!placement_.has_value() || !Places(placement_->before, id) || item == nullptr || !item->isFullscreen) {
        return;
    }
    Model().ToggleFullscreen(id, Model().DisplayWidth(), Model().DisplayHeight());
}

bool Session::EndPlacement(bool merge) {
    if (!placement_.has_value()) {
        return false;
    }
    const PlacementGesture gesture = std::move(*placement_);
    placement_.reset();
    return CommitPlacements(gesture.checkpoint, gesture.before, merge);
}

void Session::CancelPlacement() {
    if (!placement_.has_value()) {
        return;
    }
    const PlacementGesture gesture = std::move(*placement_);
    placement_.reset();
    Model().RollBack(gesture.checkpoint);
}

bool Session::SetRects(const std::vector<std::pair<ItemId, Rect>>& rects, bool merge) {
    std::vector<ItemId> ids;
    for (const auto& [id, rect] : rects) {
        ids.push_back(id);
    }
    BeginPlacement(ids);
    for (const auto& [id, rect] : rects) {
        PreviewRect(id, rect);
    }
    return EndPlacement(merge);
}

void Session::ToggleFullscreen(ItemId id, bool stretch) {
    EndOpenGesture();
    const Checkpoint checkpoint = Before({id});
    const Placements before = PlacementsOf({id});
    Model().ToggleFullscreen(id, Model().DisplayWidth(), Model().DisplayHeight(), stretch);
    CommitPlacements(checkpoint, before);
}

void Session::ResetItemToNativeSize(ItemId id) {
    EndOpenGesture();
    const Checkpoint checkpoint = Before({id});
    const Placements before = PlacementsOf({id});
    Model().ResetItemToNativeSize(id);
    CommitPlacements(checkpoint, before);
}

// ================= How snippets look =================

void Session::PreviewStyles(const std::vector<std::pair<ItemId, ItemStyle>>& styles) {
    // The snippets there are, of those named: the edit is about them.
    std::vector<std::pair<Item*, const ItemStyle*>> items;
    std::vector<ItemId> ids;
    for (const auto& [id, style] : styles) {
        if (Item* item = Model().FindItemAnywhere(id)) {
            items.emplace_back(item, &style);
            ids.push_back(id);
        }
    }
    const auto sameSnippets = [&ids](const StyleEdit& edit) {
        return edit.before.size() == ids.size() &&
               std::equal(ids.begin(), ids.end(), edit.before.begin(),
                          [](ItemId id, const std::pair<ItemId, ItemStyle>& was) { return id == was.first; });
    };
    if (styleEdit_.has_value() && !sameSnippets(*styleEdit_)) {
        EndStyleEdit();
    }
    if (!styleEdit_.has_value()) {
        EndOpenGesture();
    }
    if (items.empty()) {
        return;
    }
    if (!styleEdit_.has_value()) {
        StyleEdit edit{{}, Before(ids)};
        for (const auto& [item, style] : items) {
            edit.before.emplace_back(item->id, ItemStyle::Of(*item));
        }
        styleEdit_ = std::move(edit);
    }
    for (const auto& [item, style] : items) {
        style->ApplyTo(*item);
    }
    Model().MarkChanged();
}

bool Session::EndStyleEdit(bool merge) {
    if (!styleEdit_.has_value()) {
        return false;
    }
    const StyleEdit edit = std::move(*styleEdit_);
    styleEdit_.reset();
    // Every snippet the edit held is in the step when any of them changed -
    // see CommitPlacements, for the same reason.
    bool changed = false;
    Step step{0, What::Style, {}};
    for (const auto& [id, before] : edit.before) {
        const Item* item = Model().FindItemAnywhere(id);
        if (item == nullptr) {
            continue;
        }
        changed = changed || ItemStyle::Of(*item) != before;
        step.changes.push_back(Change{id, history::StyleChanged{before}});
    }
    if (!changed) {
        Land(edit.checkpoint);  // nothing to file
        return false;
    }
    const CanvasId canvas = Model().CanvasHoldingItem(step.changes.front().item).value_or(0);
    return Commit(edit.checkpoint, canvas, std::move(step), merge);
}

void Session::CancelStyleEdit() {
    if (!styleEdit_.has_value()) {
        return;
    }
    const StyleEdit edit = std::move(*styleEdit_);
    styleEdit_.reset();
    Model().RollBack(edit.checkpoint);
}

bool Session::SetStyles(const std::vector<std::pair<ItemId, ItemStyle>>& styles, bool merge) {
    EndOpenGesture();
    PreviewStyles(styles);
    return EndStyleEdit(merge);
}

// ================= Copying and moving snippets =================

void Session::OffsetCopy(ItemId copyId) {
    Item* item = Model().FindItemAnywhere(copyId);
    if (!item) {
        return;
    }
    constexpr float kCopyOffsetPx = 24.0f;
    item->rect.x += kCopyOffsetPx;
    item->rect.y += kCopyOffsetPx;
    if (Model().DisplayWidth() > 0.0f && Model().DisplayHeight() > 0.0f) {
        item->rect = ClampRectToViewport(item->rect, Model().DisplayWidth(), Model().DisplayHeight());
    }
    Model().CommitItemLayout(copyId);
}

std::optional<Change> Session::MoveItemTo(ItemId itemId, CanvasId target) {
    const std::optional<CanvasId> from = Model().CanvasHoldingItem(itemId);
    if (!from.has_value() || *from == target || Model().IsItemDeleted(itemId)) {
        return std::nullopt;
    }
    // Where it stands in its stack now - what an undo puts it back at.
    const size_t index = IndexOn(*Model().FindCanvas(*from), itemId);
    if (!Model().MoveItem(itemId, target, std::nullopt)) {
        return std::nullopt;
    }
    migrations_.emplace_back(itemId, target);
    return Change{itemId, history::Moved{*from, index, target}};
}

Session::Placed Session::Paste(const std::vector<ItemId>& ids, bool cut) {
    EndOpenGesture();
    Placed placed;
    if (Model().CurrentOrNull() == nullptr) {
        return placed;
    }
    const CanvasId here = Model().CurrentCanvasId();
    const Checkpoint before = Before({});
    Step step{0, What::Paste, {}};
    bool fromThisCanvas = false;
    for (const ItemId id : ids) {
        const std::optional<CanvasId> from = Model().CanvasHoldingItem(id);
        if (!from.has_value() || Model().IsItemDeleted(id)) {
            continue;  // deleted, or deleted for good, since it was copied
        }
        if (cut && *from == here) {
            placed.items.push_back(id);  // already here: nothing moves, nothing to undo
            continue;
        }
        if (cut) {
            if (std::optional<Change> moved = MoveItemTo(id, here)) {
                step.changes.push_back(std::move(*moved));
                placed.items.push_back(id);
            }
            continue;
        }
        const ItemId copy = Model().PlaceItemOnCanvas(id, here, /*copy=*/true);
        if (copy == 0) {
            continue;
        }
        placed.pictureLost = !ClonePicturesForCopy(id, copy) || placed.pictureLost;
        fromThisCanvas = fromThisCanvas || *from == here;
        step.changes.push_back(Change{copy, history::DeletionChanged{DeletionStampNow()}});
        placed.items.push_back(copy);
    }
    // A copy lands on top of its source when the source is on this canvas,
    // so there it is offset; from another canvas it keeps its place
    // exactly, which is where the eye expects it.
    if (fromThisCanvas) {
        for (const ItemId id : placed.items) {
            OffsetCopy(id);
        }
    }
    if (!Commit(before, here, std::move(step))) {
        return Placed{};
    }
    return placed;
}

Session::Placed Session::Duplicate(const std::vector<ItemId>& ids) {
    EndOpenGesture();
    Placed placed;
    const Checkpoint before = Before({});
    Step step{0, What::Duplicate, {}};
    for (const ItemId id : ids) {
        if (Model().IsItemDeleted(id)) {
            continue;
        }
        const ItemId copy = Model().DuplicateItem(id);
        if (copy == 0) {
            continue;
        }
        placed.pictureLost = !ClonePicturesForCopy(id, copy) || placed.pictureLost;
        OffsetCopy(copy);
        step.changes.push_back(Change{copy, history::DeletionChanged{DeletionStampNow()}});
        placed.items.push_back(copy);
    }
    if (placed.items.empty()) {
        return placed;
    }
    if (!Commit(before, Model().CurrentCanvasId(), std::move(step))) {
        return Placed{};
    }
    return placed;
}

Session::Placed Session::SendItemsTo(const std::vector<ItemId>& ids, CanvasId target, bool copy) {
    EndOpenGesture();
    Placed placed;
    const CanvasId source = Model().CurrentCanvasId();
    const Canvas* targetCanvas = Model().FindCanvas(target);
    if (target == source || targetCanvas == nullptr || Model().IsDeleted(*targetCanvas)) {
        return placed;
    }
    // Filed on the canvas they went to: that is where they are, and where
    // an undo can reach them.
    const Checkpoint before = Before({});
    Step step{0, copy ? What::CopyTo : What::Move, {}};
    for (const ItemId id : ids) {
        if (Model().CanvasHoldingItem(id) != source || Model().IsItemDeleted(id)) {
            continue;  // deleted since it was selected, or not from here
        }
        if (copy) {
            const ItemId made = Model().PlaceItemOnCanvas(id, target, /*copy=*/true);
            if (made == 0) {
                continue;
            }
            placed.pictureLost = !ClonePicturesForCopy(id, made) || placed.pictureLost;
            step.changes.push_back(Change{made, history::DeletionChanged{DeletionStampNow()}});
            placed.items.push_back(made);
        } else if (std::optional<Change> moved = MoveItemTo(id, target)) {
            step.changes.push_back(std::move(*moved));
            placed.items.push_back(id);
        }
    }
    if (!Commit(before, target, std::move(step))) {
        return Placed{};
    }
    return placed;
}

// ================= Text =================

void Session::BeginTextEdit(ItemId itemId) {
    EndOpenGesture();
    const Item* item = Model().FindItemAnywhere(itemId);
    if (!item) {
        return;
    }
    textEditItemId_ = itemId;
    textEditOriginal_ = item->noteText;
    textEditCheckpoint_ = Before({itemId});
}

void Session::PreviewText(std::string text) {
    Item* item = textEditItemId_.has_value() ? Model().FindItemAnywhere(*textEditItemId_) : nullptr;
    if (item == nullptr || item->noteText == text) {
        return;
    }
    item->noteText = std::move(text);
    Model().MarkChanged();
}

void Session::EndTextEdit(std::optional<std::string> text) {
    if (!textEditItemId_.has_value()) {
        return;
    }
    const ItemId itemId = *textEditItemId_;
    if (text.has_value()) {
        PreviewText(std::move(*text));
    }
    textEditItemId_.reset();
    const Item* item = Model().FindItemAnywhere(itemId);
    // One step per edit, not per keystroke - and none at all for an edit
    // that changed nothing, so clicking into a note and back out doesn't
    // spam the history.
    if (!item || item->noteText == textEditOriginal_) {
        return;
    }
    const CanvasId canvas = Model().CanvasHoldingItem(itemId).value_or(0);
    Commit(textEditCheckpoint_, canvas, Step{0, What::TextEdit, {Change{itemId, history::TextChanged{textEditOriginal_}}}});
}

// ================= Erasing =================

void Session::SnapshotStrokesForErase(ItemId itemId) {
    eraseGestureStartSnapshot_.clear();
    eraseOrigins_.clear();
    eraseReplaced_.clear();
    if (const Item* item = Model().FindItemAnywhere(itemId)) {
        eraseGestureStartSnapshot_ = item->strokes;
        eraseOrigins_.resize(item->strokes.size());
        std::iota(eraseOrigins_.begin(), eraseOrigins_.end(), size_t{0});
        eraseReplaced_.assign(item->strokes.size(), false);
    }
}

void Session::NoteEraseOutcome(const std::vector<size_t>& outcome) {
    if (outcome.size() != eraseOrigins_.size()) {
        // Not the list being followed - the item went away mid-gesture, or
        // something other than the eraser changed its strokes. Nothing
        // followed from here on means nothing goes on the history, which is
        // the safe failure.
        eraseOrigins_.clear();
        eraseReplaced_.assign(eraseReplaced_.size(), false);
        return;
    }
    std::vector<size_t> origins;
    origins.reserve(eraseOrigins_.size());
    for (size_t index = 0; index < outcome.size(); ++index) {
        const size_t origin = eraseOrigins_[index];
        if (outcome[index] == CanvasManager::kStrokeUntouched) {
            origins.push_back(origin);
            continue;
        }
        // Clipped: whatever fragments it became stand for the original it
        // stood for - which is how a fragment clipped again later in the
        // drag still traces back to the stroke that came before the drag.
        eraseReplaced_[origin] = true;
        origins.insert(origins.end(), outcome[index], origin);
    }
    eraseOrigins_ = std::move(origins);
}

void Session::BeginErase(ItemId itemId, float screenX, float screenY, float widthScreenPx) {
    // A gesture still open is over, and filed whole. Without this its
    // strokes' snapshot was taken over by this one's.
    EndOpenGesture();
    // The item's whole stroke list as the gesture starts, followed through
    // every call to one step for the whole gesture - see
    // eraseGestureStartSnapshot_.
    eraseCheckpoint_ = Before({itemId});
    SnapshotStrokesForErase(itemId);
    eraseItemId_ = itemId;
    NoteEraseOutcome(Model().EraseAt(itemId, screenX, screenY, widthScreenPx * 0.5f));
}

void Session::ExtendErase(float screenX, float screenY, float widthScreenPx) {
    if (!eraseItemId_.has_value()) {
        return;
    }
    NoteEraseOutcome(Model().EraseAt(*eraseItemId_, screenX, screenY, widthScreenPx * 0.5f));
}

void Session::EndErase() {
    if (!eraseItemId_.has_value()) {
        return;
    }
    const ItemId itemId = *eraseItemId_;
    eraseItemId_.reset();
    // The whole gesture in one step, so it is one undo.
    RecordEraseGesture(itemId);
    eraseGestureStartSnapshot_.clear();
}

void Session::CancelErase() {
    if (!eraseItemId_.has_value()) {
        return;
    }
    eraseItemId_.reset();
    eraseGestureStartSnapshot_.clear();
    Model().RollBack(eraseCheckpoint_);
}

void Session::EraseRect(ItemId itemId, float minX, float minY, float maxX, float maxY) {
    EndOpenGesture();
    // A whole gesture in one call: nothing changes the item between the
    // press that started the rectangle and the release that ends it, so the
    // snapshot taken here is the one the press would have taken.
    eraseCheckpoint_ = Before({itemId});
    SnapshotStrokesForErase(itemId);
    NoteEraseOutcome(Model().EraseRectAt(itemId, minX, minY, maxX, maxY));
    RecordEraseGesture(itemId);
    eraseGestureStartSnapshot_.clear();
}

void Session::RecordEraseGesture(ItemId itemId) {
    const Item* item = Model().FindItemAnywhere(itemId);
    if (!item) {
        return;
    }
    // Every original some call in the gesture clipped, with the fragments
    // now standing for it - which are exactly the current strokes whose
    // origin it is, in order (see NoteEraseOutcome). Only while the list
    // is still the one being followed; a mismatch means what happened is
    // unknown and is left off rather than guessed at.
    history::StrokesErased erased;
    erased.strokeCountBefore = eraseGestureStartSnapshot_.size();
    if (eraseOrigins_.size() == item->strokes.size()) {
        // One pass over both: origins never decrease along the list, since
        // erasing keeps the order and each original's fragments stand
        // together where it stood.
        size_t current = 0;
        for (size_t origin = 0; origin < eraseReplaced_.size(); ++origin) {
            while (current < eraseOrigins_.size() && eraseOrigins_[current] < origin) {
                ++current;
            }
            if (!eraseReplaced_[origin]) {
                continue;
            }
            history::StrokesErased::Replacement replacement;
            replacement.index = origin;
            replacement.original = eraseGestureStartSnapshot_[origin];
            for (; current < eraseOrigins_.size() && eraseOrigins_[current] == origin; ++current) {
                replacement.fragments.push_back(item->strokes[current]);
            }
            erased.replacements.push_back(std::move(replacement));
        }
    } else if (!eraseGestureStartSnapshot_.empty() && item->strokes != eraseGestureStartSnapshot_) {
        // Lost track of which fragment is which, and yet the strokes did
        // change: the whole list as one replacement - the first original
        // standing for everything there is now, the rest for nothing. Exact
        // both ways, only coarser; left off, the strokes would have changed
        // behind the history's back, and every step after it filed against
        // a list it did not describe.
        for (size_t index = 0; index < eraseGestureStartSnapshot_.size(); ++index) {
            erased.replacements.push_back(
                {index, eraseGestureStartSnapshot_[index], index == 0 ? item->strokes : std::vector<Stroke>{}});
        }
    }
    if (erased.replacements.empty()) {
        Land(eraseCheckpoint_);  // the gesture touched nothing
        return;
    }
    const CanvasId canvas = Model().CanvasHoldingItem(itemId).value_or(0);
    Commit(eraseCheckpoint_, canvas, Step{0, What::Erase, {Change{itemId, std::move(erased)}}});
}

}  // namespace sz::core
