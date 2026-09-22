#include "core/session/session.h"

#include <algorithm>
#include <ctime>
#include <iterator>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace sz::core {

// ================= Undo =================

namespace {
// See Session::PushUndo - plenty of headroom for "take back what I just
// did" without growing unbounded across a long session.
constexpr size_t kUndoStackCap = 50;
// ...and how much those entries may hold between them, per stack. The
// count alone was the only bound, and an entry can be a few bytes or
// eight megabytes: a brush stroke keeps the tiles it touched (a few
// hundred KB), Clear drawing keeps a fullscreen layer's whole image. Fifty
// clears of a fullscreen layer was 400 MB on one canvas's undo stack, and
// the redo stack could hold as much again. 128 MB is sixteen fullscreen
// clears or several hundred strokes, on each of the two stacks.
constexpr size_t kUndoStackCapBytes = size_t{128} << 20;

size_t StrokesBytes(const std::vector<Stroke>& strokes) {
    size_t bytes = 0;
    for (const Stroke& stroke : strokes) {
        bytes += stroke.points.size() * sizeof(StrokePoint);
    }
    return bytes;
}

}  // namespace

// The two directions of an Erased entry's vector half, each a rebuild of
// one list from the other by position - see UndoEntry::Kind::Erased. Both
// refuse, changing nothing, when the list is not the length the entry says
// it should be: that means something other than this history has changed
// the strokes since, and rebuilding from a list that is not the one the
// entry describes could only scramble it.
//
// Undo: the item holds the after-list. Every original the entry names goes
// back where it was, in place of the fragments that stood for it; every
// other stroke is carried over in order.
bool Session::RestoreStrokesBeforeErase(Item& item, const UndoEntry& entry) {
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
bool Session::ReapplyErase(Item& item, const UndoEntry& entry) {
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

size_t Session::UndoEntryBytes(const UndoEntry& entry) {
    size_t bytes = StrokesBytes(entry.strokes);
    for (const UndoEntry::StrokeReplacement& replacement : entry.replacements) {
        bytes += replacement.original.points.size() * sizeof(StrokePoint) + StrokesBytes(replacement.fragments);
    }
    for (const PaintedTile& tile : entry.paintedTiles) {
        bytes += tile.pixelsRGBA.size();
    }
    if (entry.paintedBefore) {
        bytes += entry.paintedBefore->PixelsRGBA().size();
    }
    // The text a NoteTextChanged entry holds - small for a note typed by
    // hand, but a note is whatever a record says it is.
    bytes += entry.previousNoteText.size();
    // An ItemDeleted entry holds an id; the snippet itself stays in the
    // library, marked (see UndoEntry::deletedItemId).
    return bytes;
}

void Session::PushCapped(std::deque<UndoEntry>& stack, UndoEntry entry) {
    stack.push_back(std::move(entry));
    // Summed fresh each push rather than tracked incrementally: the stacks
    // are fifty entries at most, an entry's size is a handful of vector
    // sizes, and the swaps Undo/Redo do on an entry in place would make a
    // running total another thing to keep right.
    size_t total = 0;
    for (const UndoEntry& e : stack) {
        total += UndoEntryBytes(e);
    }
    while (stack.size() > 1 && (stack.size() > kUndoStackCap || total > kUndoStackCapBytes)) {
        total -= UndoEntryBytes(stack.front());
        stack.pop_front();
    }
}

bool Session::CanUndo() const {
    const auto it = undoStacks_.find(Manager().CurrentCanvasId());
    return it != undoStacks_.end() && !it->second.empty();
}

bool Session::CanRedo() const {
    const auto it = redoStacks_.find(Manager().CurrentCanvasId());
    return it != redoStacks_.end() && !it->second.empty();
}

void Session::DropHistoryOfCanvas(CanvasId canvasId) {
    undoStacks_.erase(canvasId);
    redoStacks_.erase(canvasId);
}

void Session::ForgetHistoryOfItem(CanvasId canvasId, ItemId itemId) {
    // ItemDeleted is matched on deletedItemId rather than entry.itemId -
    // that kind names the deleted snippet there, and leaves entry.itemId
    // unset (see UndoEntry's own doc comment).
    const auto namesItem = [itemId](const UndoEntry& entry) {
        return entry.kind == UndoEntry::Kind::ItemDeleted ? entry.deletedItemId == itemId : entry.itemId == itemId;
    };
    for (auto* stacks : {&undoStacks_, &redoStacks_}) {
        const auto it = stacks->find(canvasId);
        if (it == stacks->end()) {
            continue;
        }
        std::deque<UndoEntry>& stack = it->second;
        stack.erase(std::remove_if(stack.begin(), stack.end(), namesItem), stack.end());
    }
}

void Session::PushUndo(UndoEntry entry) {
    // Filed under whichever canvas is current, which is by definition the
    // one the action just happened on - every edit that pushes is made on
    // the visible canvas.
    const CanvasId canvasId = Manager().CurrentCanvasId();
    if (canvasId == 0) {
        // No canvas at all (see CanvasManager's class comment) - there's
        // no edit that can reach here in that state, but filing an entry
        // under the "none" id would make CanUndo report a non-empty history
        // for an empty library and light an Undo button up over nothing.
        return;
    }
    PushCapped(undoStacks_[canvasId], std::move(entry));
    // A brand-new action makes this canvas's redo stack unreachable by any
    // sequence of undos - the usual "a fresh edit prunes redo" rule. Only
    // this canvas's: editing here says nothing about whether another
    // canvas's redo entries are still replayable.
    redoStacks_.erase(canvasId);
}

bool Session::SwapPaintedUndoState(UndoEntry& entry) {
    if (entry.paintedTiles.empty() && !entry.paintedBefore) {
        return false;  // no painted half
    }
    Item* item = Manager().FindItemAnywhere(entry.itemId);
    if (!item || entry.layerIndex >= item->layers.size() || !item->layers[entry.layerIndex].painted) {
        return false;
    }
    Layer& layer = item->layers[entry.layerIndex];
    if (entry.paintedBefore) {
        // The whole image, a pointer each way - no pixels copied.
        std::swap(layer.painted, entry.paintedBefore);
        UploadPaintedRegion(layer, PixelRect{0, 0, layer.painted->Width(), layer.painted->Height()});
        return true;
    }
    // Put the saved tiles back and keep what they replaced: the entry
    // becomes its own redo (or undo) entry, with no second copy of the
    // pixels anywhere.
    PixelRect changed;
    entry.paintedTiles = layer.painted->RestoreTiles(entry.paintedTiles, changed);
    UploadPaintedRegion(layer, changed);
    return true;
}

// The one dispatch for both directions. Every kind is either its own
// inverse - a swap, for NoteTextChanged, PaintedTilesChanged and Erased's
// painted half - or a mirror in which two arrays trade roles (Erased's
// strokes) or one action has an opposite (StrokeBaked's pop/push,
// ItemDeleted's restore/delete). So a single function with the direction as
// a parameter says each of those once.
//
// Returns what was undone or redone, or nullopt if nothing changed. A no-op
// - the item or canvas the entry names is already gone - has nothing
// meaningful to put on the opposite stack either, so the caller drops it
// rather than leaving a dead entry there.
std::optional<Session::UndoWhat> Session::ApplyUndoEntry(UndoEntry& entry, bool undo) {
    std::optional<UndoWhat> what;
    switch (entry.kind) {
        case UndoEntry::Kind::StrokeBaked: {
            Item* item = Manager().FindItemAnywhere(entry.itemId);
            if (!item) {
                return std::nullopt;
            }
            if (undo) {
                // The stroke this entry is for - which is the last one
                // whenever history alone has touched the list, and is
                // looked for from the back so that it still is the last
                // of two equal strokes. Nothing to do if it is gone.
                if (entry.strokes.empty()) {
                    return std::nullopt;
                }
                const auto it = std::find(item->strokes.rbegin(), item->strokes.rend(), entry.strokes.front());
                if (it == item->strokes.rend()) {
                    return std::nullopt;
                }
                item->strokes.erase(std::next(it).base());
            } else {
                // entry.strokes[0] is the exact value the undo popped - see
                // UndoEntry::Kind::StrokeBaked's own doc comment for why
                // it's captured at push time rather than reconstructed.
                if (entry.strokes.empty()) {
                    return std::nullopt;
                }
                item->strokes.push_back(entry.strokes.front());
            }
            what = UndoWhat::Stroke;
            break;
        }
        case UndoEntry::Kind::Erased: {
            Item* item = Manager().FindItemAnywhere(entry.itemId);
            if (!item) {
                return std::nullopt;
            }
            // The vector half, rebuilt by position in either direction -
            // see RestoreStrokesBeforeErase/ReapplyErase - and the painted
            // half, which is its own inverse. Either half having something
            // to do is enough for the entry to count.
            const bool strokesChanged =
                !entry.replacements.empty() && (undo ? RestoreStrokesBeforeErase(*item, entry) : ReapplyErase(*item, entry));
            const bool paintChanged = SwapPaintedUndoState(entry);
            if (!strokesChanged && !paintChanged) {
                return std::nullopt;
            }
            what = UndoWhat::Erase;
            break;
        }
        case UndoEntry::Kind::ItemDeleted: {
            // The snippet is still where it was, marked: undo clears the
            // mark, redo makes it again, and the pictures come and go with
            // it (see Delete). False when there is nothing to change - the
            // snippet deleted for good since, say - which is this case's
            // no-op.
            const bool changed = undo ? Restore(entry.deletedItemId) : Delete(entry.deletedItemId);
            if (!changed) {
                return std::nullopt;
            }
            what = UndoWhat::Delete;
            break;
        }
        case UndoEntry::Kind::ItemCreated: {
            // ItemDeleted the other way round: undo marks the new snippet
            // deleted, redo restores it.
            const bool changed = undo ? Delete(entry.itemId) : Restore(entry.itemId);
            if (!changed) {
                return std::nullopt;
            }
            what = UndoWhat::Create;
            break;
        }
        case UndoEntry::Kind::NoteTextChanged: {
            // Not while the note is open - see StepHistory, which keeps the
            // entry for later rather than letting it reach here.
            Item* item = Manager().FindItemAnywhere(entry.itemId);
            if (!item) {
                return std::nullopt;
            }
            // Swap rather than assign: on the way in item->noteText holds
            // one direction's text and entry.previousNoteText the other's -
            // after this the item has the right one and the entry has become
            // exactly what the opposite direction needs, with no separate
            // capture step.
            std::swap(item->noteText, entry.previousNoteText);
            what = UndoWhat::TextEdit;
            break;
        }
        case UndoEntry::Kind::PaintedTilesChanged: {
            // Its own inverse - see SwapPaintedUndoState.
            if (!SwapPaintedUndoState(entry)) {
                return std::nullopt;
            }
            what = UndoWhat::Painting;
            break;
        }
    }
    if (what) {
        Manager().MarkChanged();
    }
    return what;
}

std::optional<Session::UndoStep> Session::StepHistory(bool undo) {
    // This canvas's own history, never another's - see undoStacks_.
    auto& from = undo ? undoStacks_ : redoStacks_;
    auto& to = undo ? redoStacks_ : undoStacks_;
    const CanvasId canvasId = Manager().CurrentCanvasId();
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
    const UndoEntry& next = stackIt->second.back();
    if (next.kind == UndoEntry::Kind::NoteTextChanged && textEditItemId_ == next.itemId) {
        return std::nullopt;
    }
    // Not const - the swap kinds mutate it in place (see ApplyUndoEntry)
    // before it goes onto the opposite stack, which is what makes it that
    // stack's entry.
    UndoEntry entry = std::move(stackIt->second.back());
    stackIt->second.pop_back();
    const std::optional<UndoWhat> what = ApplyUndoEntry(entry, undo);
    if (!what) {
        return std::nullopt;
    }
    // Onto the opposite stack only if it took effect. Straight onto it,
    // deliberately not via PushUndo: that would clear the redo stack, which
    // on a redo is the very stack this was just taken from, and break a run
    // of redos after the first.
    PushCapped(to[canvasId], std::move(entry));
    return UndoStep{*what, undo};
}

std::optional<Session::UndoStep> Session::Undo() { return StepHistory(/*undo=*/true); }

std::optional<Session::UndoStep> Session::Redo() { return StepHistory(/*undo=*/false); }

// ================= Edits that can be undone =================

void Session::CommitLiveStroke(ItemId itemId) {
    Canvas* canvasPtr = Manager().CurrentOrNull();
    if (!canvasPtr) {
        return;
    }
    Canvas& canvas = *canvasPtr;
    if (canvas.liveLayer.Strokes().empty()) {
        return;
    }
    const auto itemIt = std::find_if(canvas.items.begin(), canvas.items.end(),
                                      [&](const Item& i) { return i.id == itemId; });
    if (itemIt == canvas.items.end()) {
        canvas.liveLayer.Clear();
        return;
    }
    const Stroke finished = canvas.liveLayer.Strokes().back();
    itemIt->strokes.push_back(CanvasManager::BakeStrokeToNative(*itemIt, finished));
    canvas.liveLayer.Clear();
    // Mutates itemIt->strokes directly rather than through one of
    // CanvasManager's own methods - see MarkChanged()'s own doc comment.
    Manager().MarkChanged();

    UndoEntry entry;
    entry.kind = UndoEntry::Kind::StrokeBaked;
    entry.itemId = itemId;
    // Undo itself only needs itemId (it just pops the last stroke), but
    // redo needs the actual value back - see UndoEntry::Kind::StrokeBaked.
    entry.strokes = {itemIt->strokes.back()};
    PushUndo(std::move(entry));
}

bool Session::DeleteItem(ItemId itemId) {
    const CanvasId canvasId = Manager().CurrentCanvasId();
    // Only a snippet on this canvas: the entry pushed afterwards is filed
    // under it. Marked where it is (see Delete), and that entry is what an
    // undo restores.
    const Canvas* canvas = Manager().CurrentOrNull();
    const bool onThisCanvas = canvas != nullptr && std::any_of(canvas->items.begin(), canvas->items.end(),
                                                               [itemId](const Item& i) { return i.id == itemId; });
    if (!onThisCanvas || !Delete(itemId)) {
        return false;
    }
    UndoEntry entry;
    entry.kind = UndoEntry::Kind::ItemDeleted;
    entry.canvasId = canvasId;
    entry.deletedItemId = itemId;
    PushUndo(std::move(entry));
    return true;
}

ItemId Session::CreateItem(bool hasBackground, Rect rect, std::string name) {
    const ItemId itemId = Manager().CreateItem(hasBackground, rect, std::move(name));
    if (itemId == 0) {
        return 0;
    }
    UndoEntry entry;
    entry.kind = UndoEntry::Kind::ItemCreated;
    entry.itemId = itemId;
    PushUndo(std::move(entry));
    return itemId;
}

namespace {
// Nothing put into it: no ink of either kind, no text, and no picture of
// its own - a screenshot is content even when the capture failed.
bool ItemIsUntouched(const Item& item) {
    return !item.hasBackground && item.strokes.empty() && item.noteText.empty() &&
           std::none_of(item.layers.begin(), item.layers.end(),
                        [](const Layer& layer) { return layer.HasPaintedPixels(); });
}
}  // namespace

bool Session::IsUntouched(ItemId itemId) const {
    for (const Canvas& canvas : Manager().Canvases()) {
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
    return DeletePermanently(itemId) != Removal::NotFound;
}

bool Session::ClearDrawing(ItemId itemId) {
    Item* item = Manager().FindItemAnywhere(itemId);
    if (!item) {
        return false;
    }
    // Ink drawn on top of the snippet, in both senses of drawn: the vector
    // strokes and whatever has been painted. Clearing one and silently
    // leaving the other is what this did before painted layers existed,
    // and is not what the button says. Both halves go into one Erased
    // entry - the same one an eraser gesture that wholly erased everything
    // would push (`addedFragments` stays empty: nothing replaces what's
    // removed, it's just gone) - so one undo brings all of it back.
    PaintedUndo painted = ClearPaintedLayers(*item);
    if (item->strokes.empty() && painted.Empty()) {
        return false;
    }
    UndoEntry entry;
    entry.kind = UndoEntry::Kind::Erased;
    entry.itemId = itemId;
    entry.strokeCountBefore = item->strokes.size();
    entry.replacements.reserve(item->strokes.size());
    for (size_t index = 0; index < item->strokes.size(); ++index) {
        entry.replacements.push_back({index, std::move(item->strokes[index]), {}});
    }
    entry.layerIndex = painted.layerIndex;
    entry.paintedBefore = std::move(painted.wholeImage);
    PushUndo(std::move(entry));
    item->strokes.clear();
    Manager().MarkChanged();
    return true;
}

void Session::BeginTextEdit(ItemId itemId) {
    EndTextEdit(std::nullopt);
    const Item* item = Manager().FindItemAnywhere(itemId);
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
    Item* item = Manager().FindItemAnywhere(itemId);
    // One entry per edit, not per keystroke - and none at all for an edit
    // that changed nothing, so clicking into a note and back out doesn't
    // spam the stack.
    if (!item || !text.has_value() || *text == textEditOriginal_) {
        return;
    }
    UndoEntry entry;
    entry.kind = UndoEntry::Kind::NoteTextChanged;
    entry.itemId = itemId;
    entry.previousNoteText = textEditOriginal_;
    PushUndo(std::move(entry));
    item->noteText = std::move(*text);
    Manager().MarkChanged();
}

// ================= Erasing =================

void Session::SnapshotStrokesForErase(ItemId itemId) {
    eraseGestureStartSnapshot_.clear();
    eraseOrigins_.clear();
    eraseReplaced_.clear();
    if (const Item* item = Manager().FindItemAnywhere(itemId)) {
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
    // Snapshot the item's whole stroke list right as the gesture starts,
    // diffed against its final state when it ends to build one combined
    // undo entry for the whole gesture - see eraseGestureStartSnapshot_ for
    // why a before/after diff rather than accumulating each erase call's
    // own effect.
    SnapshotStrokesForErase(itemId);
    eraseItemId_ = itemId;
    NoteEraseOutcome(Manager().EraseAt(itemId, screenX, screenY, widthScreenPx * 0.5f));
    // ...and the same gesture takes pixels off any painted layer it passes
    // over - the brush gesture the pen uses, with the erase blend. The
    // eraser deliberately ignores the drawing mode: an item can hold both
    // kinds of mark, and an eraser that silently refuses half of them -
    // with nothing on screen to say why - is the worst failure this design
    // can have.
    if (Item* item = Manager().FindItemAnywhere(itemId)) {
        BeginPaintStroke(*item, screenX, screenY, /*erase=*/true, 0xFFFFFFFFu, widthScreenPx);
    }
}

void Session::ExtendErase(float screenX, float screenY, float widthScreenPx) {
    if (!eraseItemId_.has_value()) {
        return;
    }
    NoteEraseOutcome(Manager().EraseAt(*eraseItemId_, screenX, screenY, widthScreenPx * 0.5f));
    ExtendPaintStroke(screenX, screenY);
}

void Session::EndErase() {
    if (!eraseItemId_.has_value()) {
        return;
    }
    // Both halves of the gesture - the strokes it clipped and the pixels it
    // took - in one entry, so it is one undo.
    PushEraseGestureUndoEntry(*eraseItemId_, EndPaintStroke());
    eraseItemId_.reset();
    eraseGestureStartSnapshot_.clear();
}

void Session::EraseRect(ItemId itemId, float minX, float minY, float maxX, float maxY) {
    // A whole gesture in one call: nothing changes the item between the
    // press that started the rectangle and the release that ends it, so the
    // snapshot taken here is the one the press would have taken.
    SnapshotStrokesForErase(itemId);
    NoteEraseOutcome(Manager().EraseRectAt(itemId, minX, minY, maxX, maxY));
    // ...and the same rectangle out of any painted layer, for the same
    // reason the circular eraser does it: an eraser acts on whatever is
    // under it, whichever mode the marks were made in. One entry for both.
    PaintedUndo painted;
    if (Item* item = Manager().FindItemAnywhere(itemId)) {
        painted = ErasePaintedLayersInRect(*item, minX, minY, maxX, maxY);
    }
    PushEraseGestureUndoEntry(itemId, std::move(painted));
    eraseGestureStartSnapshot_.clear();
}

void Session::PushEraseGestureUndoEntry(ItemId itemId, PaintedUndo painted) {
    Item* item = Manager().FindItemAnywhere(itemId);
    if (!item) {
        return;
    }
    // Every original some call in the gesture clipped, with the fragments
    // now standing for it - which are exactly the current strokes whose
    // origin it is, in order (see NoteEraseOutcome). Only while the list
    // is still the one being followed; a mismatch means the vector half
    // is unknown and is left off rather than guessed at.
    std::vector<UndoEntry::StrokeReplacement> replacements;
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
            UndoEntry::StrokeReplacement replacement;
            replacement.index = origin;
            replacement.original = eraseGestureStartSnapshot_[origin];
            for (; current < eraseOrigins_.size() && eraseOrigins_[current] == origin; ++current) {
                replacement.fragments.push_back(item->strokes[current]);
            }
            replacements.push_back(std::move(replacement));
        }
    }
    if (replacements.empty() && painted.Empty()) {
        return;  // the gesture touched nothing of either kind
    }
    UndoEntry entry;
    entry.kind = UndoEntry::Kind::Erased;
    entry.itemId = itemId;
    entry.strokeCountBefore = eraseGestureStartSnapshot_.size();
    entry.replacements = std::move(replacements);
    // The painted half, if the gesture had one. Its own item is by
    // construction this one - the eraser only acts on the snippet being
    // drawn on.
    entry.layerIndex = painted.layerIndex;
    entry.paintedTiles = std::move(painted.tiles);
    entry.paintedBefore = std::move(painted.wholeImage);
    PushUndo(std::move(entry));
}

}  // namespace sz::core
