#include "core/session/session.h"

#include <algorithm>
#include <ctime>
#include <iterator>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "core/canvas/item_geometry.h"

namespace sz::core {

// ================= Undo =================

namespace {
// See Session::PushUndo - plenty of headroom for "take back what I just
// did" without growing unbounded across a long session.
constexpr size_t kUndoStackCap = 50;
// ...and how much those entries may hold between them, per stack. An
// entry can be a few bytes or a whole drawing's strokes: Clear drawing
// keeps every stroke it took.
constexpr size_t kUndoStackCapBytes = size_t{128} << 20;

}  // namespace

// The two directions of an Erased entry's vector half, each a rebuild of
// one list from the other by position - see undo::Erased. Both
// refuse, changing nothing, when the list is not the length the entry says
// it should be: that means something other than this history has changed
// the strokes since, and rebuilding from a list that is not the one the
// entry describes could only scramble it.
//
// Undo: the item holds the after-list. Every original the entry names goes
// back where it was, in place of the fragments that stood for it; every
// other stroke is carried over in order.
bool Session::RestoreStrokesBeforeErase(Item& item, const undo::Erased& entry) {
    size_t expectedAfter = entry.strokeCountBefore - entry.replacements.size();
    for (const auto& replacement : entry.replacements) {
        expectedAfter += replacement.fragments.size();
    }
    if (item.strokes.size() != expectedAfter) {
        return false;
    }
    std::vector<Stroke> before;
    before.reserve(entry.strokeCountBefore);
    auto replacement = entry.replacements.begin();
    size_t after = 0;
    for (size_t index = 0; index < entry.strokeCountBefore; ++index) {
        if (replacement != entry.replacements.end() && replacement->index == index) {
            before.push_back(replacement->original);
            after += replacement->fragments.size();
            ++replacement;
        } else {
            before.push_back(std::move(item.strokes[after++]));
        }
    }
    item.strokes = std::move(before);
    return true;
}

// Redo: the item holds the before-list. Every original the entry names is
// replaced, in place, by the fragments the gesture left of it.
bool Session::ReapplyErase(Item& item, const undo::Erased& entry) {
    if (item.strokes.size() != entry.strokeCountBefore) {
        return false;
    }
    std::vector<Stroke> after;
    after.reserve(item.strokes.size());
    auto replacement = entry.replacements.begin();
    for (size_t index = 0; index < item.strokes.size(); ++index) {
        if (replacement != entry.replacements.end() && replacement->index == index) {
            after.insert(after.end(), replacement->fragments.begin(), replacement->fragments.end());
            ++replacement;
        } else {
            after.push_back(std::move(item.strokes[index]));
        }
    }
    item.strokes = std::move(after);
    return true;
}

void Session::PushCapped(std::deque<undo::Entry>& stack, undo::Entry entry) {
    stack.push_back(std::move(entry));
    // Summed fresh each push rather than tracked incrementally: the stacks
    // are fifty entries at most, an entry's size is a handful of vector
    // sizes, and the swaps Undo/Redo do on an entry in place would make a
    // running total another thing to keep right.
    size_t total = 0;
    for (const undo::Entry& e : stack) {
        total += undo::Bytes(e);
    }
    while (stack.size() > 1 && (stack.size() > kUndoStackCap || total > kUndoStackCapBytes)) {
        total -= undo::Bytes(stack.front());
        stack.pop_front();
    }
}

bool Session::CanUndo() const {
    const auto it = undoStacks_.find(Model().CurrentCanvasId());
    return it != undoStacks_.end() && !it->second.empty();
}

bool Session::CanRedo() const {
    const auto it = redoStacks_.find(Model().CurrentCanvasId());
    return it != redoStacks_.end() && !it->second.empty();
}

void Session::DropHistoryOfCanvas(CanvasId canvasId) {
    undoStacks_.erase(canvasId);
    redoStacks_.erase(canvasId);
}

void Session::ForgetHistoryOfItem(CanvasId canvasId, ItemId itemId) {
    // An entry about several snippets loses this one and stays for the
    // rest - see undo::Forget.
    const auto namesItem = [itemId](undo::Entry& entry) { return undo::Forget(entry, itemId); };
    for (auto* stacks : {&undoStacks_, &redoStacks_}) {
        const auto it = stacks->find(canvasId);
        if (it == stacks->end()) {
            continue;
        }
        std::deque<undo::Entry>& stack = it->second;
        stack.erase(std::remove_if(stack.begin(), stack.end(), namesItem), stack.end());
    }
}

void Session::PushUndo(undo::Entry entry) {
    // Filed under whichever canvas is current, which is by definition the
    // one the action just happened on - every edit that pushes is made on
    // the visible canvas.
    const CanvasId canvasId = Model().CurrentCanvasId();
    if (canvasId == 0) {
        // No canvas at all (see CanvasManager's class comment) - there's
        // no edit that can reach here in that state, but filing an entry
        // under the "none" id would make CanUndo report a non-empty history
        // for an empty library and light an Undo button up over nothing.
        return;
    }
    ++historyRevision_;
    PushCapped(undoStacks_[canvasId], std::move(entry));
    // A brand-new action makes this canvas's redo stack unreachable by any
    // sequence of undos - the usual "a fresh edit prunes redo" rule. Only
    // this canvas's: editing here says nothing about whether another
    // canvas's redo entries are still replayable.
    redoStacks_.erase(canvasId);
}

// One overload per kind, each for both directions. Every kind is either its
// own inverse - a swap, for NoteTextChanged and PlacementChanged - or a
// mirror in which two
// arrays trade roles (Erased's strokes) or one action has an opposite
// (StrokeBaked's pop/push, ItemDeleted's restore/delete, ItemsArrived's
// send-back/bring). So a function with the direction as a parameter says
// each of those once.
//
// Each returns what was undone or redone, or nullopt if nothing changed. A
// no-op - the item or canvas the entry names is already gone - has nothing
// meaningful to put on the opposite stack either, so the caller drops it
// rather than leaving a dead entry there.

std::optional<Session::UndoWhat> Session::Apply(undo::StrokeBaked& entry, bool undo) {
    Item* item = Model().FindItemAnywhere(entry.itemId);
    if (!item) {
        return std::nullopt;
    }
    if (undo) {
        // The stroke this entry is for - which is the last one whenever
        // history alone has touched the list, and is looked for from the
        // back so that it still is the last of two equal strokes. Nothing to
        // do if it is gone.
        const auto it = std::find(item->strokes.rbegin(), item->strokes.rend(), entry.stroke);
        if (it == item->strokes.rend()) {
            return std::nullopt;
        }
        item->strokes.erase(std::next(it).base());
    } else {
        item->strokes.push_back(entry.stroke);
    }
    return UndoWhat::Stroke;
}

std::optional<Session::UndoWhat> Session::Apply(undo::Erased& entry, bool undo) {
    Item* item = Model().FindItemAnywhere(entry.itemId);
    if (!item) {
        return std::nullopt;
    }
    // Rebuilt by position in either direction - see
    // RestoreStrokesBeforeErase/ReapplyErase.
    const bool strokesChanged =
        !entry.replacements.empty() && (undo ? RestoreStrokesBeforeErase(*item, entry) : ReapplyErase(*item, entry));
    if (!strokesChanged) {
        return std::nullopt;
    }
    return UndoWhat::Erase;
}

std::optional<Session::UndoWhat> Session::Apply(undo::ItemDeleted& entry, bool undo) {
    // The snippets are still where they were, marked: undo clears the marks,
    // redo makes them again, and the pictures come and go with them (see
    // Delete). Nothing to change for any of them - all deleted for good
    // since, say - is this kind's no-op.
    bool changed = false;
    for (const ItemId id : entry.itemIds) {
        changed |= undo ? Restore(id) : Delete(id);
    }
    if (!changed) {
        return std::nullopt;
    }
    return UndoWhat::Delete;
}

std::optional<Session::UndoWhat> Session::Apply(undo::NoteTextChanged& entry, bool /*undo*/) {
    // Not while the note is open - see StepHistory, which keeps the entry
    // for later rather than letting it reach here.
    Item* item = Model().FindItemAnywhere(entry.itemId);
    if (!item) {
        return std::nullopt;
    }
    // Swap rather than assign: on the way in item->noteText holds one
    // direction's text and the entry the other's - after this the item has
    // the right one and the entry has become exactly what the opposite
    // direction needs, with no separate capture step.
    std::swap(item->noteText, entry.previousText);
    return UndoWhat::TextEdit;
}

std::optional<Session::UndoWhat> Session::Apply(undo::ItemCreated& entry, bool undo) {
    // ItemDeleted the other way round: undo marks the new snippet deleted,
    // redo restores it.
    if (!(undo ? Delete(entry.itemId) : Restore(entry.itemId))) {
        return std::nullopt;
    }
    return UndoWhat::Create;
}

std::optional<Session::UndoWhat> Session::Apply(undo::PlacementChanged& entry, bool /*undo*/) {
    // The same swap both ways. A snippet deleted since, or gone, is
    // skipped; if none is left the entry has nothing to do.
    bool changed = false;
    for (undo::Placement& held : entry.placements) {
        Item* item = Model().FindItemAnywhere(held.itemId);
        if (item == nullptr || Model().IsItemDeleted(held.itemId)) {
            continue;
        }
        std::swap(item->rect, held.rect);
        std::swap(item->isFullscreen, held.isFullscreen);
        std::swap(item->isFullscreenStretch, held.isFullscreenStretch);
        std::swap(item->anchorRect, held.anchorRect);
        std::swap(item->anchorDisplayWidth, held.anchorDisplayWidth);
        std::swap(item->anchorDisplayHeight, held.anchorDisplayHeight);
        changed = true;
    }
    if (!changed) {
        return std::nullopt;
    }
    return UndoWhat::Placement;
}

std::optional<Session::UndoWhat> Session::Apply(undo::ItemsArrived& entry, bool undo) {
    // StepHistory has already asked ArrivalsCanMove, so every moved snippet
    // has somewhere to go; what may have gone since is a copy deleted for
    // good, which is skipped.
    bool changed = false;
    bool moved = false;
    // Undone last first: each fromIndex was taken once the snippets before
    // it in the list had left (see MoveItemHere), so putting them back in
    // the reverse order undoes each removal against the very stack it was
    // made from, and every one lands where it stood. Forwards, two cut
    // from one canvas came back swapped. Redone in the order they came.
    for (size_t step = 0; step < entry.arrivals.size(); ++step) {
        const undo::Arrival& arrival = entry.arrivals[undo ? entry.arrivals.size() - 1 - step : step];
        if (arrival.fromCanvas == 0) {
            changed = (undo ? Delete(arrival.itemId) : Restore(arrival.itemId)) || changed;
            continue;
        }
        const std::optional<CanvasId> holder = Model().CanvasHoldingItem(arrival.itemId);
        if (!holder.has_value()) {
            continue;
        }
        const CanvasId to = undo ? arrival.fromCanvas : entry.canvasId;
        if (*holder == to ||
            Model().PlaceItemOnCanvas(arrival.itemId, to, /*copy=*/false,
                                        undo ? std::optional<size_t>(arrival.fromIndex) : std::nullopt) == 0) {
            continue;
        }
        // Back here from the canvas it was sent back to: whatever was done
        // to it there is filed there, and would edit a snippet that is not
        // on it - see ForgetHistoryOfItem. Sent back, it leaves nothing
        // behind here that is not this canvas's to keep: its later edits,
        // already undone, wait on the redo stack for it to come again.
        if (!undo) {
            ForgetHistoryOfItem(*holder, arrival.itemId);
        }
        moved = true;
    }
    if (moved) {
        // Arrived on, or left, the canvas being looked at.
        SyncTexturesToCurrentCanvas();
    }
    if (!changed && !moved) {
        return std::nullopt;
    }
    return entry.duplicate ? UndoWhat::Duplicate : UndoWhat::Paste;
}

std::optional<Session::UndoStep> Session::StepHistory(bool undo) {
    // This canvas's own history, never another's - see undoStacks_.
    auto& from = undo ? undoStacks_ : redoStacks_;
    auto& to = undo ? redoStacks_ : undoStacks_;
    const CanvasId canvasId = Model().CurrentCanvasId();
    const auto stackIt = from.find(canvasId);
    if (stackIt == from.end() || stackIt->second.empty()) {
        return std::nullopt;
    }
    // An entry that applies, only not now, stays where it is: a note's text
    // while that note is being edited (a second note opened straight from
    // a first commits the first without closing the second). Yanking the
    // committed text out from under the edit would be undone by the edit's
    // own commit anyway, and the entry is still good once the note is
    // closed. A stale entry is another matter, and is dropped below.
    const undo::Entry& next = stackIt->second.back();
    if (const auto* note = std::get_if<undo::NoteTextChanged>(&next); note && textEditItemId_ == note->itemId) {
        return std::nullopt;
    }
    // A paste whose snippets cannot go the way asked is not left on the
    // stack, where it would stand in front of every older step for good:
    // it is dropped, with nothing moved, and the step says so.
    if (const auto* arrived = std::get_if<undo::ItemsArrived>(&next); arrived && !ArrivalsCanMove(*arrived, undo)) {
        const UndoWhat what = arrived->duplicate ? UndoWhat::Duplicate : UndoWhat::Paste;
        stackIt->second.pop_back();
        ++historyRevision_;
        return UndoStep{what, undo, /*refused=*/true};
    }
    // Not const - the swap kinds mutate it in place (see Apply) before it
    // goes onto the opposite stack, which is what makes it that stack's
    // entry.
    undo::Entry entry = std::move(stackIt->second.back());
    stackIt->second.pop_back();
    ++historyRevision_;
    const std::optional<UndoWhat> what = std::visit([&](auto& e) { return Apply(e, undo); }, entry);
    if (!what) {
        return std::nullopt;
    }
    Model().MarkChanged();
    // Onto the opposite stack only if it took effect. Straight onto it,
    // deliberately not via PushUndo: that would clear the redo stack, which
    // on a redo is the very stack this was just taken from, and break a run
    // of redos after the first.
    PushCapped(to[canvasId], std::move(entry));
    return UndoStep{*what, undo};
}

bool Session::ArrivalsCanMove(const undo::ItemsArrived& entry, bool undo) const {
    for (const undo::Arrival& arrival : entry.arrivals) {
        if (arrival.fromCanvas == 0) {
            continue;
        }
        // Where it has to go (undo), or where it is now (redo): a canvas
        // that is there and not deleted - with its folder - either way.
        const std::optional<CanvasId> holder = Model().CanvasHoldingItem(arrival.itemId);
        const CanvasId canvasId = undo ? arrival.fromCanvas : holder.value_or(0);
        const Canvas* canvas = Model().FindCanvas(canvasId);
        if (canvas == nullptr || Model().IsDeleted(*canvas)) {
            return false;
        }
        if (!undo && Model().IsItemDeleted(arrival.itemId)) {
            return false;
        }
        // Brought again only from where the undo sent it. Moved on since,
        // it is another canvas's now, with that canvas's history of it -
        // which the redo would pull it out from under, off a canvas
        // nobody is looking at.
        if (!undo && holder != arrival.fromCanvas && holder != entry.canvasId) {
            return false;
        }
    }
    return true;
}

std::optional<Session::UndoStep> Session::Undo() { return StepHistory(/*undo=*/true); }

std::optional<Session::UndoStep> Session::Redo() { return StepHistory(/*undo=*/false); }

// ================= Edits that can be undone =================

void Session::CommitLiveStroke(ItemId itemId) {
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
    // Mutates itemIt->strokes directly rather than through one of
    // CanvasManager's own methods - see MarkChanged()'s own doc comment.
    Model().MarkChanged();

    // The value, not only the id: redo pushes it back - see
    // undo::StrokeBaked.
    PushUndo(undo::StrokeBaked{itemId, itemIt->strokes.back()});
}

bool Session::DeleteItem(ItemId itemId) { return DeleteItems({itemId}) == 1; }

size_t Session::DeleteItems(const std::vector<ItemId>& itemIds) {
    // Only snippets on this canvas: the entry pushed afterwards is filed
    // under it. Marked where they are (see Delete), and one entry for all of
    // them is what an undo restores - one per snippet made one Delete that
    // many undos, and past the history's cap the earliest could not be
    // undone at all, which for a deleted snippet is deleted for good.
    const Canvas* canvas = Model().CurrentOrNull();
    if (canvas == nullptr) {
        return 0;
    }
    undo::ItemDeleted entry;
    for (const ItemId id : itemIds) {
        const bool onThisCanvas =
            std::any_of(canvas->items.begin(), canvas->items.end(), [id](const Item& i) { return i.id == id; });
        if (onThisCanvas && Delete(id)) {
            entry.itemIds.push_back(id);
        }
    }
    const size_t deleted = entry.itemIds.size();
    if (deleted > 0) {
        PushUndo(std::move(entry));
    }
    return deleted;
}

std::vector<Session::Placement> Session::PlacementsOf(const std::vector<ItemId>& ids) const {
    std::vector<Placement> placements;
    for (const ItemId id : ids) {
        for (const Canvas& canvas : Model().Canvases()) {
            const auto item =
                std::find_if(canvas.items.begin(), canvas.items.end(), [id](const Item& i) { return i.id == id; });
            if (item != canvas.items.end()) {
                placements.push_back(Placement{id, item->rect, item->isFullscreen, item->isFullscreenStretch,
                                               item->anchorRect, item->anchorDisplayWidth, item->anchorDisplayHeight});
                break;
            }
        }
    }
    return placements;
}

bool Session::RecordPlacements(std::vector<Placement> before, bool merge) {
    // Compared field by field against now: a gesture that ends where it
    // began - a click, a drag back to the start - files nothing.
    std::vector<ItemId> ids;
    for (const Placement& p : before) {
        ids.push_back(p.itemId);
    }
    const std::vector<Placement> now = PlacementsOf(ids);
    const auto same = [](const Placement& a, const Placement& b) {
        return a.itemId == b.itemId && a.rect == b.rect && a.isFullscreen == b.isFullscreen &&
               a.isFullscreenStretch == b.isFullscreenStretch && a.anchorRect == b.anchorRect &&
               a.anchorDisplayWidth == b.anchorDisplayWidth && a.anchorDisplayHeight == b.anchorDisplayHeight;
    };
    if (now.size() == before.size() && std::equal(now.begin(), now.end(), before.begin(), same)) {
        return false;
    }
    const CanvasId canvasId = Model().CurrentCanvasId();
    if (merge) {
        const auto it = undoStacks_.find(canvasId);
        if (it != undoStacks_.end() && !it->second.empty()) {
            const auto* last = std::get_if<undo::PlacementChanged>(&it->second.back());
            const auto sameItem = [](const Placement& a, const Placement& b) { return a.itemId == b.itemId; };
            if (last != nullptr && last->placements.size() == before.size() &&
                std::equal(last->placements.begin(), last->placements.end(), before.begin(), sameItem)) {
                // Its `before` is where the burst began, which is what an
                // undo of the burst goes back to; this step's own is
                // somewhere in the middle, and not wanted. A new edit
                // still prunes the redo stack, as PushUndo would.
                redoStacks_.erase(canvasId);
                return true;
            }
        }
    }
    PushUndo(undo::PlacementChanged{std::move(before)});
    return true;
}

ItemId Session::CreateItem(Item prototype, bool undoable) {
    const ItemId itemId = Model().CreateItem(std::move(prototype));
    if (itemId == 0) {
        return 0;
    }
    if (Item* item = Model().FindItemAnywhere(itemId); item != nullptr && item->hasBackground) {
        CaptureShotItem(*item);
    }
    if (undoable) {
        PushUndo(undo::ItemCreated{itemId});
    }
    return itemId;
}

ItemId Session::CreateItem(bool hasBackground, Rect rect, std::string name) {
    Item prototype;
    prototype.hasBackground = hasBackground;
    prototype.picture.opacity = hasBackground ? 1.0f : 0.0f;
    prototype.rect = rect;
    prototype.name = std::move(name);
    return CreateItem(std::move(prototype));
}

// ================= Where snippets are =================

void Session::BeginPlacement(const std::vector<ItemId>& ids) {
    EndPlacement();
    placementBefore_ = PlacementsOf(ids);
}

namespace {
bool Places(const std::vector<undo::Placement>& placements, ItemId id) {
    return std::any_of(placements.begin(), placements.end(), [id](const undo::Placement& p) { return p.itemId == id; });
}
}  // namespace

void Session::PreviewRect(ItemId id, Rect rect) {
    Item* item = Model().FindItemAnywhere(id);
    if (!placementBefore_.has_value() || !Places(*placementBefore_, id) || item == nullptr) {
        return;
    }
    item->rect = rect;
    Model().MarkChanged();
    Model().CommitItemLayout(id);
}

void Session::PreviewLeaveFullscreen(ItemId id) {
    const Item* item = Model().FindItemAnywhere(id);
    if (!placementBefore_.has_value() || !Places(*placementBefore_, id) || item == nullptr || !item->isFullscreen) {
        return;
    }
    Model().ToggleFullscreen(id, Model().DisplayWidth(), Model().DisplayHeight());
}

bool Session::EndPlacement(bool merge) {
    if (!placementBefore_.has_value()) {
        return false;
    }
    std::vector<Placement> before = std::move(*placementBefore_);
    placementBefore_.reset();
    return RecordPlacements(std::move(before), merge);
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
    EndPlacement();
    std::vector<Placement> before = PlacementsOf({id});
    Model().ToggleFullscreen(id, Model().DisplayWidth(), Model().DisplayHeight(), stretch);
    RecordPlacements(std::move(before));
}

void Session::ResetItemToNativeSize(ItemId id) {
    EndPlacement();
    std::vector<Placement> before = PlacementsOf({id});
    Model().ResetItemToNativeSize(id);
    RecordPlacements(std::move(before));
}

// ================= How snippets look =================

void Session::PreviewStyle(ItemId id, const ItemStyle& style) {
    if (styleEditBefore_.has_value() && styleEditBefore_->first != id) {
        EndStyleEdit();
    }
    Item* item = Model().FindItemAnywhere(id);
    if (item == nullptr) {
        return;
    }
    if (!styleEditBefore_.has_value()) {
        styleEditBefore_ = std::make_pair(id, ItemStyle::Of(*item));
    }
    style.ApplyTo(*item);
    Model().MarkChanged();
}

void Session::EndStyleEdit() { styleEditBefore_.reset(); }

void Session::SetStyles(const std::vector<std::pair<ItemId, ItemStyle>>& styles, bool /*merge*/) {
    EndStyleEdit();
    for (const auto& [id, style] : styles) {
        if (Item* item = Model().FindItemAnywhere(id)) {
            style.ApplyTo(*item);
        }
    }
    Model().MarkChanged();
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

Session::Placed Session::Paste(const std::vector<ItemId>& ids, bool cut) {
    Placed placed;
    if (Model().CurrentOrNull() == nullptr) {
        return placed;
    }
    const CanvasId here = Model().CurrentCanvasId();
    std::vector<Arrival> arrivals;
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
            if (const std::optional<Arrival> moved = MoveItemTo(id, here)) {
                arrivals.push_back(*moved);
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
        arrivals.push_back(Arrival{copy});
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
    RecordArrivals(std::move(arrivals), /*duplicate=*/false);
    // Whatever arrived needs a texture now: this is the current canvas.
    SyncTexturesToCurrentCanvas();
    return placed;
}

Session::Placed Session::Duplicate(const std::vector<ItemId>& ids) {
    Placed placed;
    std::vector<Arrival> arrivals;
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
        arrivals.push_back(Arrival{copy});
        placed.items.push_back(copy);
    }
    if (placed.items.empty()) {
        return placed;
    }
    RecordArrivals(std::move(arrivals), /*duplicate=*/true);
    SyncTexturesToCurrentCanvas();
    return placed;
}

Session::Placed Session::SendItemsTo(const std::vector<ItemId>& ids, CanvasId target, bool copy) {
    Placed placed;
    const CanvasId source = Model().CurrentCanvasId();
    if (target == source) {
        return placed;
    }
    for (const ItemId id : ids) {
        if (Model().IsItemDeleted(id)) {
            continue;  // deleted since it was selected
        }
        if (copy) {
            const ItemId made = Model().MoveOrCopyItemToCanvas(id, target, /*copy=*/true);
            if (made == 0) {
                continue;
            }
            placed.pictureLost = !ClonePicturesForCopy(id, made) || placed.pictureLost;
            placed.items.push_back(made);
            continue;
        }
        Model().MoveOrCopyItemToCanvas(id, target, /*copy=*/false);
        // A move reports nothing, so what happened is read from where the
        // snippet is now.
        if (Model().CanvasHoldingItem(id) != target) {
            continue;
        }
        ForgetHistoryOfItem(source, id);
        placed.items.push_back(id);
    }
    if (!placed.items.empty()) {
        // What went is on a canvas nobody is looking at, and gives its
        // textures back now rather than at the next switch.
        SyncTexturesToCurrentCanvas();
    }
    return placed;
}

namespace {
// Nothing put into it: no ink, no text, and no picture of its own - a
// screenshot is content even when the capture failed.
bool ItemIsUntouched(const Item& item) {
    return !item.hasBackground && item.strokes.empty() && item.noteText.empty();
}
}  // namespace

std::optional<Session::Arrival> Session::MoveItemTo(ItemId itemId, CanvasId here) {
    const std::optional<CanvasId> from = Model().CanvasHoldingItem(itemId);
    if (!from.has_value() || *from == here || Model().IsItemDeleted(itemId)) {
        return std::nullopt;
    }
    // Where it stands in its stack now - what an undo puts it back at.
    const Canvas* source = Model().FindCanvas(*from);
    size_t index = 0;
    while (index < source->items.size() && source->items[index].id != itemId) {
        ++index;
    }
    if (Model().PlaceItemOnCanvas(itemId, here, /*copy=*/false) == 0) {
        return std::nullopt;
    }
    // Its history is filed under the canvas it has left, and an undo there
    // would now edit a snippet living here.
    ForgetHistoryOfItem(*from, itemId);
    return Arrival{itemId, *from, index};
}

void Session::RecordArrivals(std::vector<Arrival> arrivals, bool duplicate) {
    if (arrivals.empty()) {
        return;
    }
    PushUndo(undo::ItemsArrived{Model().CurrentCanvasId(), std::move(arrivals), duplicate});
}

bool Session::IsUntouched(ItemId itemId) const {
    for (const Canvas& canvas : Model().Canvases()) {
        for (const Item& candidate : canvas.items) {
            if (candidate.id == itemId) {
                return ItemIsUntouched(candidate);
            }
        }
    }
    return false;
}

bool Session::DiscardIfUntouched(ItemId itemId) {
    if (!IsUntouched(itemId)) {
        return false;
    }
    // Erased rather than marked - there is nothing in it to find again - and
    // its own entries go with it: its making, and any stroke taken back off
    // it. The rest of the canvas's history stays.
    return DeletePermanently(itemId);
}

bool Session::ClearDrawing(ItemId itemId) {
    Item* item = Model().FindItemAnywhere(itemId);
    if (!item) {
        return false;
    }
    // Every stroke on the snippet, as one Erased entry - the same one an
    // eraser gesture that wholly erased everything would push (no
    // fragments: nothing replaces what's removed, it's just gone) - so one
    // undo brings all of it back.
    if (item->strokes.empty()) {
        return false;
    }
    undo::Erased entry;
    entry.itemId = itemId;
    entry.strokeCountBefore = item->strokes.size();
    entry.replacements.reserve(item->strokes.size());
    for (size_t index = 0; index < item->strokes.size(); ++index) {
        entry.replacements.push_back({index, std::move(item->strokes[index]), {}});
    }
    PushUndo(std::move(entry));
    item->strokes.clear();
    Model().MarkChanged();
    return true;
}

void Session::BeginTextEdit(ItemId itemId) {
    EndTextEdit(std::nullopt);
    const Item* item = Model().FindItemAnywhere(itemId);
    if (!item) {
        return;
    }
    textEditItemId_ = itemId;
    textEditOriginal_ = item->noteText;
}

void Session::EndTextEdit(std::optional<std::string> text) {
    if (!textEditItemId_.has_value()) {
        return;
    }
    const ItemId itemId = *textEditItemId_;
    textEditItemId_.reset();
    Item* item = Model().FindItemAnywhere(itemId);
    // One entry per edit, not per keystroke - and none at all for an edit
    // that changed nothing, so clicking into a note and back out doesn't
    // spam the stack.
    if (!item || !text.has_value() || *text == textEditOriginal_) {
        return;
    }
    PushUndo(undo::NoteTextChanged{itemId, textEditOriginal_});
    item->noteText = std::move(*text);
    Model().MarkChanged();
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
        // followed from here on means nothing goes on the history for the
        // vector half, which is the safe failure.
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
    EndErase();
    // Snapshot the item's whole stroke list right as the gesture starts,
    // diffed against its final state when it ends to build one combined
    // undo entry for the whole gesture - see eraseGestureStartSnapshot_ for
    // why a before/after diff rather than accumulating each erase call's
    // own effect.
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
    // The whole gesture in one entry, so it is one undo.
    PushEraseGestureUndoEntry(*eraseItemId_);
    eraseItemId_.reset();
    eraseGestureStartSnapshot_.clear();
}

void Session::EraseRect(ItemId itemId, float minX, float minY, float maxX, float maxY) {
    // An erase still open is over, and filed whole, as BeginErase has it:
    // left open, this took over its snapshot, and its own end then read
    // the one this cleared.
    EndErase();
    // A whole gesture in one call: nothing changes the item between the
    // press that started the rectangle and the release that ends it, so the
    // snapshot taken here is the one the press would have taken.
    SnapshotStrokesForErase(itemId);
    NoteEraseOutcome(Model().EraseRectAt(itemId, minX, minY, maxX, maxY));
    PushEraseGestureUndoEntry(itemId);
    eraseGestureStartSnapshot_.clear();
}

void Session::PushEraseGestureUndoEntry(ItemId itemId) {
    Item* item = Model().FindItemAnywhere(itemId);
    if (!item) {
        return;
    }
    // Every original some call in the gesture clipped, with the fragments
    // now standing for it - which are exactly the current strokes whose
    // origin it is, in order (see NoteEraseOutcome). Only while the list
    // is still the one being followed; a mismatch means the vector half
    // is unknown and is left off rather than guessed at.
    std::vector<undo::Erased::Replacement> replacements;
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
            undo::Erased::Replacement replacement;
            replacement.index = origin;
            replacement.original = eraseGestureStartSnapshot_[origin];
            for (; current < eraseOrigins_.size() && eraseOrigins_[current] == origin; ++current) {
                replacement.fragments.push_back(item->strokes[current]);
            }
            replacements.push_back(std::move(replacement));
        }
    }
    if (replacements.empty()) {
        return;  // the gesture touched nothing
    }
    PushUndo(undo::Erased{itemId, eraseGestureStartSnapshot_.size(), std::move(replacements)});
}

}  // namespace sz::core
