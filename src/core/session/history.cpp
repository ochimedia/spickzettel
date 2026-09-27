#include "core/session/history.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace sz::core::history {

namespace {

template <typename... Fns>
struct Overloaded : Fns... {
    using Fns::operator()...;
};
template <typename... Fns>
Overloaded(Fns...) -> Overloaded<Fns...>;

size_t StrokeBytes(const Stroke& stroke) { return stroke.points.size() * sizeof(StrokePoint); }

// How long the list is after an erase: every original the entry names
// replaced by the fragments that stand for it.
size_t CountAfter(const StrokesErased& erased) {
    size_t after = erased.strokeCountBefore - erased.replacements.size();
    for (const StrokesErased::Replacement& replacement : erased.replacements) {
        after += replacement.fragments.size();
    }
    return after;
}

// The two directions of an erase, each a rebuild of one list from the
// other by position - see StrokesErased.
//
// Undo: the snippet holds the after-list. Every original the entry names
// goes back where it was, in place of the fragments that stood for it;
// every other stroke is carried over in order.
void RestoreBeforeErase(Item& item, const StrokesErased& erased) {
    std::vector<Stroke> before;
    before.reserve(erased.strokeCountBefore);
    auto replacement = erased.replacements.begin();
    size_t after = 0;
    for (size_t index = 0; index < erased.strokeCountBefore; ++index) {
        if (replacement != erased.replacements.end() && replacement->index == index) {
            before.push_back(replacement->original);
            after += replacement->fragments.size();
            ++replacement;
        } else {
            before.push_back(std::move(item.strokes[after++]));
        }
    }
    item.strokes = std::move(before);
}

// Redo: the snippet holds the before-list. Every original the entry names
// is replaced, in place, by the fragments the gesture left of it.
void ReapplyErase(Item& item, const StrokesErased& erased) {
    std::vector<Stroke> after;
    after.reserve(CountAfter(erased));
    auto replacement = erased.replacements.begin();
    for (size_t index = 0; index < item.strokes.size(); ++index) {
        if (replacement != erased.replacements.end() && replacement->index == index) {
            after.insert(after.end(), replacement->fragments.begin(), replacement->fragments.end());
            ++replacement;
        } else {
            after.push_back(std::move(item.strokes[index]));
        }
    }
    item.strokes = std::move(after);
}

size_t IndexOn(const CanvasManager& manager, CanvasId canvasId, ItemId item) {
    const Canvas* canvas = manager.FindCanvas(canvasId);
    size_t index = 0;
    while (canvas != nullptr && index < canvas->items.size() && canvas->items[index].id != item) {
        ++index;
    }
    return index;
}

}  // namespace

// ================= Placement =================

Placement Placement::Of(const Item& item) {
    return Placement{item.rect,       item.isFullscreen,       item.isFullscreenStretch,
                     item.anchorRect, item.anchorDisplayWidth, item.anchorDisplayHeight};
}

void Placement::ApplyTo(Item& item) const {
    item.rect = rect;
    item.isFullscreen = isFullscreen;
    item.isFullscreenStretch = isFullscreenStretch;
    item.anchorRect = anchorRect;
    item.anchorDisplayWidth = anchorDisplayWidth;
    item.anchorDisplayHeight = anchorDisplayHeight;
}

// ================= Changes =================

size_t Bytes(const Step& step) {
    size_t bytes = 0;
    for (const Change& change : step.changes) {
        bytes += std::visit(Overloaded{
                                [](const StrokeAdded& c) { return StrokeBytes(c.stroke); },
                                [](const StrokesErased& c) {
                                    size_t total = 0;
                                    for (const StrokesErased::Replacement& r : c.replacements) {
                                        total += StrokeBytes(r.original);
                                        for (const Stroke& fragment : r.fragments) {
                                            total += StrokeBytes(fragment);
                                        }
                                    }
                                    return total;
                                },
                                // Small for a note typed by hand, but a note
                                // is whatever a record says it is.
                                [](const TextChanged& c) { return c.text.size(); },
                                [](const auto&) { return sizeof(Change); },
                            },
                            change.kind);
    }
    return bytes;
}

bool CanApply(const CanvasManager& manager, const Change& change, bool undo) {
    const Item* item = manager.FindItemAnywhere(change.item);
    if (item == nullptr) {
        return false;
    }
    return std::visit(Overloaded{
                          // Taken back off the end, where it went: anything
                          // else there means the list is not the one this
                          // was filed against.
                          [&](const StrokeAdded& c) {
                              return !undo || (!item->strokes.empty() && item->strokes.back() == c.stroke);
                          },
                          [&](const StrokesErased& c) {
                              return !c.replacements.empty() &&
                                     item->strokes.size() == (undo ? CountAfter(c) : c.strokeCountBefore);
                          },
                          [&](const Moved& c) {
                              // On the canvas it is to leave, with the one it
                              // goes to there to take it.
                              return manager.CanvasHoldingItem(change.item) == (undo ? c.to : c.from) &&
                                     manager.FindCanvas(undo ? c.from : c.to) != nullptr;
                          },
                          [](const auto&) { return true; },
                      },
                      change.kind);
}

void Apply(CanvasManager& manager, Change& change, bool undo) {
    Item* item = manager.FindItemAnywhere(change.item);
    std::visit(Overloaded{
                   [&](StrokeAdded& c) {
                       if (undo) {
                           item->strokes.pop_back();
                       } else {
                           item->strokes.push_back(c.stroke);
                       }
                   },
                   [&](StrokesErased& c) {
                       if (undo) {
                           RestoreBeforeErase(*item, c);
                       } else {
                           ReapplyErase(*item, c);
                       }
                   },
                   [&](TextChanged& c) { std::swap(item->noteText, c.text); },
                   [&](PlacementChanged& c) {
                       const Placement now = Placement::Of(*item);
                       c.placement.ApplyTo(*item);
                       c.placement = now;
                   },
                   [&](StyleChanged& c) {
                       const ItemStyle now = ItemStyle::Of(*item);
                       c.style.ApplyTo(*item);
                       c.style = now;
                   },
                   [&](DeletionChanged& c) { std::swap(item->deletedAt, c.deletedAt); },
                   [&](Moved& c) {
                       // From wherever it stands in its stack now, which is
                       // where the opposite direction puts it back.
                       if (undo) {
                           c.toIndex = IndexOn(manager, c.to, change.item);
                           manager.MoveItem(change.item, c.from, c.fromIndex);
                       } else {
                           c.fromIndex = IndexOn(manager, c.from, change.item);
                           manager.MoveItem(change.item, c.to, c.toIndex);
                       }
                   },
               },
               change.kind);
}

// ================= The stacks =================

bool History::CanUndo(CanvasId canvas) const { return NextUndo(canvas) != nullptr; }

bool History::CanRedo(CanvasId canvas) const { return NextRedo(canvas) != nullptr; }

const Step* History::NextUndo(CanvasId canvas) const {
    const auto it = stacks_.find(canvas);
    return it == stacks_.end() || it->second.undo.empty() ? nullptr : &it->second.undo.back();
}

const Step* History::NextRedo(CanvasId canvas) const {
    const auto it = stacks_.find(canvas);
    return it == stacks_.end() || it->second.redo.empty() ? nullptr : &it->second.redo.back();
}

void History::Record(CanvasId canvas, Step step) {
    if (canvas == 0 || step.changes.empty()) {
        return;
    }
    for (const Change& change : step.changes) {
        DropRedoOf(change.item, /*except=*/0);
    }
    Stacks& stacks = stacks_[canvas];
    stacks.redo.clear();
    for (size_t place = 0; place < step.changes.size(); ++place) {
        step.changes[place].place = place;
    }
    step.seq = nextSeq_++;
    PushCapped(stacks.undo, std::move(step));
}

Step History::TakeUndo(CanvasId canvas) {
    std::deque<Step>& stack = stacks_.at(canvas).undo;
    Step step = std::move(stack.back());
    stack.pop_back();
    return step;
}

Step History::TakeRedo(CanvasId canvas) {
    std::deque<Step>& stack = stacks_.at(canvas).redo;
    Step step = std::move(stack.back());
    stack.pop_back();
    return step;
}

void History::Undone(CanvasId canvas, Step step) {
    for (const Change& change : step.changes) {
        DropRedoOf(change.item, canvas);
    }
    PushCapped(stacks_[canvas].redo, std::move(step));
}

void History::Redone(CanvasId canvas, Step step) {
    for (const Change& change : step.changes) {
        DropRedoOf(change.item, canvas);
    }
    step.seq = nextSeq_++;
    PushCapped(stacks_[canvas].undo, std::move(step));
}

void History::PutBack(CanvasId canvas, Step step, bool undo) {
    Stacks& stacks = stacks_[canvas];
    (undo ? stacks.undo : stacks.redo).push_back(std::move(step));
}

void History::Migrate(ItemId item, CanvasId canvas) {
    // Every part of every step about `item`, from every other canvas: a step
    // about several snippets leaves the rest where they are.
    std::vector<Step> parts;
    for (auto& [id, stacks] : stacks_) {
        if (id == canvas) {
            continue;
        }
        for (Step& step : stacks.undo) {
            Step part{step.seq, step.what, {}};
            for (auto it = step.changes.begin(); it != step.changes.end();) {
                if (it->item == item) {
                    part.changes.push_back(std::move(*it));
                    it = step.changes.erase(it);
                } else {
                    ++it;
                }
            }
            if (!part.changes.empty()) {
                parts.push_back(std::move(part));
            }
        }
        std::erase_if(stacks.undo, [](const Step& step) { return step.changes.empty(); });
    }
    if (parts.empty()) {
        return;
    }
    // In by when each was done - the stack is in that order - and a part
    // whose step already has a part here rejoins it, in its own place
    // among the rest. Both are in that order already, a part taken out
    // keeping it. Appended, a group move's snippet came back at the end:
    // its moves are undone in the reverse of the order the snippets left
    // in, each to the index it left at, and undone out of order they put
    // two snippets back in each other's places in the stack.
    std::deque<Step>& target = stacks_[canvas].undo;
    for (Step& part : parts) {
        const auto same =
            std::find_if(target.begin(), target.end(), [&part](const Step& step) { return step.seq == part.seq; });
        if (same != target.end()) {
            const auto kept = static_cast<std::ptrdiff_t>(same->changes.size());
            std::move(part.changes.begin(), part.changes.end(), std::back_inserter(same->changes));
            std::inplace_merge(same->changes.begin(), same->changes.begin() + kept, same->changes.end(),
                               [](const Change& a, const Change& b) { return a.place < b.place; });
            continue;
        }
        const auto at = std::upper_bound(target.begin(), target.end(), part.seq,
                                         [](uint64_t seq, const Step& step) { return seq < step.seq; });
        target.insert(at, std::move(part));
    }
    // Over the caps now, the oldest go, as they would have from here.
    Cap(target);
    DropEmptyCanvases();
}

void History::ForgetItem(ItemId item) {
    for (auto& [id, stacks] : stacks_) {
        Remove(stacks.undo, item);
        Remove(stacks.redo, item);
    }
    DropEmptyCanvases();
}

void History::ForgetCanvas(CanvasId canvas) {
    stacks_.erase(canvas);
    const auto touches = [canvas](const Change& change) {
        const Moved* moved = std::get_if<Moved>(&change.kind);
        return moved != nullptr && (moved->from == canvas || moved->to == canvas);
    };
    for (auto& [id, stacks] : stacks_) {
        for (std::deque<Step>* stack : {&stacks.undo, &stacks.redo}) {
            for (Step& step : *stack) {
                std::erase_if(step.changes, touches);
            }
            std::erase_if(*stack, [](const Step& step) { return step.changes.empty(); });
        }
    }
    DropEmptyCanvases();
}

const std::deque<Step>* History::UndoStack(CanvasId canvas) const {
    const auto it = stacks_.find(canvas);
    return it == stacks_.end() ? nullptr : &it->second.undo;
}

const std::deque<Step>* History::RedoStack(CanvasId canvas) const {
    const auto it = stacks_.find(canvas);
    return it == stacks_.end() ? nullptr : &it->second.redo;
}

std::vector<CanvasId> History::Canvases() const {
    std::vector<CanvasId> canvases;
    for (const auto& [id, stacks] : stacks_) {
        canvases.push_back(id);
    }
    return canvases;
}

void History::PushCapped(std::deque<Step>& stack, Step step) {
    stack.push_back(std::move(step));
    Cap(stack);
}

void History::Cap(std::deque<Step>& stack) {
    // Summed fresh each push rather than tracked: a stack is fifty steps at
    // most, and a step's size is a handful of vector sizes.
    size_t total = 0;
    for (const Step& s : stack) {
        total += Bytes(s);
    }
    while (stack.size() > 1 && (stack.size() > kStackCap || total > kStackCapBytes)) {
        total -= Bytes(stack.front());
        stack.pop_front();
    }
}

void History::Remove(std::deque<Step>& stack, ItemId item) {
    for (Step& step : stack) {
        std::erase_if(step.changes, [item](const Change& change) { return change.item == item; });
    }
    std::erase_if(stack, [](const Step& step) { return step.changes.empty(); });
}

void History::DropRedoOf(ItemId item, CanvasId except) {
    for (auto& [id, stacks] : stacks_) {
        if (id != except) {
            Remove(stacks.redo, item);
        }
    }
}

void History::DropEmptyCanvases() {
    std::erase_if(stacks_, [](const auto& entry) { return entry.second.undo.empty() && entry.second.redo.empty(); });
}

}  // namespace sz::core::history
