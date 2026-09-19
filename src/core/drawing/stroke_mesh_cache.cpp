#include "core/drawing/stroke_mesh_cache.h"

#include <cstring>

namespace sz::core {

namespace {

// A stroke's content in 64 bits, so that "is this still the stroke the mesh
// was built from" can be answered without keeping a copy of it.
//
// A kept copy is the obvious alternative, and is what the rasterized
// renderer's cache does - but there are two mesh caches (the canvas's and
// the Overview's previews), and two copies of every stroke on screen is a
// lot to hold purely to compare against. Eight bytes answers the same
// question. The cost of being wrong is bounded and cosmetic - a stroke drawn
// with the mesh of a different stroke of exactly the same length - and at 64
// bits it will not happen.
//
// The float bits are hashed rather than the values, so -0.0f reads as
// different from 0.0f and a NaN as different from itself. Both err towards
// rebuilding a mesh that didn't need it, which costs a frame's tessellation,
// never a wrong picture.
uint64_t Fingerprint(const Stroke& stroke) {
    static_assert(sizeof(StrokePoint) == 2 * sizeof(float),
                  "a point is hashed eight bytes at a time; padding would leave uninitialised bytes in the hash");

    uint64_t hash = 1469598103934665603ull;  // FNV-1a offset basis
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;  // FNV-1a prime
        // FNV mixes a byte at a time; fed whole words it leaves the high
        // bits underworked, and stroke points are very structured input.
        // One shift-xor per word is enough to spread them.
        hash ^= hash >> 29;
    };

    mix(stroke.points.size());
    mix(stroke.colorRGBA);
    uint32_t widthBits = 0;
    std::memcpy(&widthBits, &stroke.width, sizeof(widthBits));
    mix(widthBits);
    for (const StrokePoint& point : stroke.points) {
        uint64_t bits = 0;
        std::memcpy(&bits, &point, sizeof(bits));
        mix(bits);
    }
    return hash;
}

}  // namespace

void StrokeMeshCache::BeginFrame() {
    ++frame_;
    rebuilt_ = 0;
}

const StrokeMesh& StrokeMeshCache::MeshFor(uint64_t itemId, size_t strokeIndex, const Stroke& stroke,
                                            uint64_t generation, float scaleX, float scaleY, float halfWidth) {
    ItemEntry& item = items_[itemId];
    item.lastUsedFrame = frame_;
    if (strokeIndex + 1 > item.usedThisFrame) {
        item.usedThisFrame = strokeIndex + 1;
    }
    if (item.strokes.size() <= strokeIndex) {
        item.strokes.resize(strokeIndex + 1);
    }
    Entry& entry = item.strokes[strokeIndex];

    // The scale is checked first and always: it is two comparisons, and it
    // is the one thing a moving generation says nothing about - resizing an
    // item bumps the generation, but so does dragging one, and only the
    // first invalidates a mesh.
    const bool sameScale = entry.scaleX == scaleX && entry.scaleY == scaleY && entry.halfWidth == halfWidth;
    if (entry.built && sameScale) {
        if (entry.checkedGeneration == generation) {
            return entry.mesh;  // nothing anywhere has changed since this was last checked
        }
        // Something changed somewhere. Usually not this stroke, and the
        // fingerprint says so far more cheaply than rebuilding would.
        if (entry.fingerprint == Fingerprint(stroke)) {
            entry.checkedGeneration = generation;
            return entry.mesh;
        }
    }

    scaledPoints_.clear();
    scaledPoints_.reserve(stroke.points.size());
    for (const StrokePoint& point : stroke.points) {
        scaledPoints_.push_back(StrokePoint{point.x * scaleX, point.y * scaleY});
    }
    BuildStrokeMesh(scaledPoints_, halfWidth, kStrokeFringePx, entry.mesh);

    entry.fingerprint = Fingerprint(stroke);
    entry.checkedGeneration = generation;
    entry.scaleX = scaleX;
    entry.scaleY = scaleY;
    entry.halfWidth = halfWidth;
    entry.built = true;
    ++rebuilt_;
    return entry.mesh;
}

void StrokeMeshCache::EndFrame() {
    rebuiltLastFrame_ = rebuilt_;
    for (auto it = items_.begin(); it != items_.end();) {
        ItemEntry& item = it->second;
        if (item.lastUsedFrame != frame_) {
            // Not drawn this frame: another canvas, a deleted item, a
            // minimized one. It costs nothing to build again if it comes
            // back, and holding it costs memory for as long as the app runs.
            it = items_.erase(it);
            continue;
        }
        if (item.usedThisFrame < item.strokes.size()) {
            item.strokes.resize(item.usedThisFrame);
        }
        item.usedThisFrame = 0;
        ++it;
    }
}

void StrokeMeshCache::Clear() {
    items_.clear();
    rebuilt_ = 0;
    rebuiltLastFrame_ = 0;
}

size_t StrokeMeshCache::EntryCount() const {
    size_t count = 0;
    for (const auto& [itemId, item] : items_) {
        (void)itemId;
        count += item.strokes.size();
    }
    return count;
}

}  // namespace sz::core
