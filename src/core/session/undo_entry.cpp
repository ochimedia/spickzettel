#include "core/session/undo_entry.h"

#include <algorithm>

namespace sz::core::undo {

namespace {

size_t StrokesBytes(const std::vector<Stroke>& strokes) {
    size_t bytes = 0;
    for (const Stroke& stroke : strokes) {
        bytes += stroke.points.size() * sizeof(StrokePoint);
    }
    return bytes;
}

// Every kind's share of Bytes and Forget, said once each - an overload per
// kind, so that a kind added to Entry without them does not compile.
size_t BytesOf(const StrokeBaked& e) { return e.stroke.points.size() * sizeof(StrokePoint); }
size_t BytesOf(const Erased& e) {
    size_t bytes = e.painted.Bytes();
    for (const Erased::Replacement& replacement : e.replacements) {
        bytes += replacement.original.points.size() * sizeof(StrokePoint) + StrokesBytes(replacement.fragments);
    }
    return bytes;
}
// The snippets themselves stay in the library, marked: ids are all it holds.
size_t BytesOf(const ItemDeleted& e) { return e.itemIds.size() * sizeof(ItemId); }
// Small for a note typed by hand, but a note is whatever a record says it is.
size_t BytesOf(const NoteTextChanged& e) { return e.previousText.size(); }
size_t BytesOf(const PaintedTilesChanged& e) { return e.painted.Bytes(); }
size_t BytesOf(const ItemCreated&) { return 0; }
size_t BytesOf(const PlacementChanged& e) { return e.placements.size() * sizeof(Placement); }
size_t BytesOf(const ItemsArrived& e) { return e.arrivals.size() * sizeof(Arrival); }

bool ForgetIn(StrokeBaked& e, ItemId id) { return e.itemId == id; }
bool ForgetIn(Erased& e, ItemId id) { return e.itemId == id; }
bool ForgetIn(ItemDeleted& e, ItemId id) {
    std::erase(e.itemIds, id);
    return e.itemIds.empty();
}
bool ForgetIn(NoteTextChanged& e, ItemId id) { return e.itemId == id; }
bool ForgetIn(PaintedTilesChanged& e, ItemId id) { return e.painted.itemId == id; }
bool ForgetIn(ItemCreated& e, ItemId id) { return e.itemId == id; }
bool ForgetIn(PlacementChanged& e, ItemId id) {
    std::erase_if(e.placements, [id](const Placement& p) { return p.itemId == id; });
    return e.placements.empty();
}
bool ForgetIn(ItemsArrived& e, ItemId id) {
    std::erase_if(e.arrivals, [id](const Arrival& a) { return a.itemId == id; });
    return e.arrivals.empty();
}

}  // namespace

size_t Painted::Bytes() const {
    size_t bytes = 0;
    for (const PaintedTile& tile : tiles) {
        bytes += tile.pixelsRGBA.size();
    }
    if (wholeImage) {
        bytes += wholeImage->PixelsRGBA().size();
    }
    return bytes;
}

size_t Bytes(const Entry& entry) {
    return std::visit([](const auto& e) { return BytesOf(e); }, entry);
}

bool Forget(Entry& entry, ItemId itemId) {
    return std::visit([itemId](auto& e) { return ForgetIn(e, itemId); }, entry);
}

}  // namespace sz::core::undo
