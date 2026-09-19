#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/drawing/stroke.h"
#include "core/drawing/stroke_mesh.h"

namespace sz::core {

// How wide the anti-aliasing edge of a stroke is, in *screen* space - so it
// stays a pixel wide whatever the item is scaled to. An item shrunk to a
// third would otherwise carry a third of a pixel of anti-aliasing and alias
// visibly. Here rather than beside either caller because the cached and the
// uncached path have to agree on it: a mesh built at one fringe and reused at
// another would be a stroke that changed appearance for no visible reason.
constexpr float kStrokeFringePx = 1.0f;

// Keeps the tessellated mesh of each stroke between frames.
//
// Why. Without it a canvas nobody is touching would tessellate every mark
// on it sixty times a second. Measured on a release build, a hundred
// strokes of two hundred points each cost 0.93 ms a frame that way - about
// six per cent of a frame's budget, for a picture identical to the last one.
// With the cache: 0.03 ms at rest, 0.06 ms while something is dragged.
//
// What it is keyed on. A mesh is identified by the item it belongs to and
// the stroke's index within that item, and it is valid only while the
// geometry it was built for is unchanged:
//
//   - the *scale* it was built at, because the geometry is built in scaled
//     space (the pen gets wider as the item does) and the anti-aliasing
//     fringe is one screen pixel however small the item is shrunk;
//   - the stroke's own content, as a 64-bit fingerprint - see Fingerprint
//     in the .cpp for why a fingerprint rather than a kept copy.
//
// The *offset* is deliberately not part of any of that: meshes are built
// around the origin and the caller adds its own translation as it writes the
// vertices out. Dragging an item across the screen therefore changes nothing
// this cache holds, which is the case where dropping a frame would be most
// visible.
//
// How the fast path stays fast. Fingerprinting a stroke is far cheaper than
// tessellating one, but it is still a walk over every point, and the common
// case is a canvas where nothing at all is happening. So each entry also
// remembers the CanvasManager generation it was last checked against (see
// CanvasManager::Generation): while that hasn't moved, nothing on any canvas
// has changed and the entry is good without touching the stroke. The
// generation is passed in per call rather than latched per frame on purpose -
// it makes correctness independent of where in the frame anything is drawn,
// so a mutation made halfway through a frame cannot leave a stale mesh on
// screen for the rest of it.
//
// Lifetime. BeginFrame/EndFrame bracket a frame's drawing; EndFrame drops
// everything that frame didn't ask for, so switching canvas, deleting an item
// or undoing a stroke releases the memory by simply not being drawn again.
//
// Not thread-safe, and not meant to be: this lives on the render thread with
// everything else that touches a draw list.
class StrokeMeshCache {
public:
    // Starts a frame's worth of drawing. Every MeshFor call for this frame
    // must come between this and EndFrame.
    void BeginFrame();

    // The mesh for `stroke`, built if there isn't a usable one. `scaleX`/
    // `scaleY` map the stroke's own space to the screen and `halfWidth` is
    // the pen's half width already in that scaled space; the returned
    // vertices are in that space with no translation applied. `generation`
    // is CanvasManager::Generation() - see the class comment.
    //
    // The reference is valid until the next call on this cache.
    const StrokeMesh& MeshFor(uint64_t itemId, size_t strokeIndex, const Stroke& stroke, uint64_t generation,
                              float scaleX, float scaleY, float halfWidth);

    // Ends the frame and releases what it didn't draw.
    void EndFrame();

    // Drops everything - for a mode switch, or a canvas going away entirely.
    void Clear();

    // Diagnostics, and what the tests assert on: how many strokes are held,
    // and how many of the last frame's MeshFor calls had to tessellate.
    size_t EntryCount() const;
    size_t RebuiltLastFrame() const { return rebuiltLastFrame_; }

private:
    struct Entry {
        StrokeMesh mesh;
        // What the mesh was built from and at, all of which has to still be
        // true for it to be reusable. `checkedGeneration` is the cheap gate
        // in front of `fingerprint` - see the class comment.
        uint64_t fingerprint = 0;
        uint64_t checkedGeneration = 0;
        float scaleX = 0.0f;
        float scaleY = 0.0f;
        float halfWidth = 0.0f;
        bool built = false;
    };

    struct ItemEntry {
        std::vector<Entry> strokes;
        uint64_t lastUsedFrame = 0;
        // The highest stroke index asked for this frame, plus one - so a
        // stroke that was undone away has its mesh dropped at EndFrame
        // rather than sitting there for as long as the item lives.
        size_t usedThisFrame = 0;
    };

    std::unordered_map<uint64_t, ItemEntry> items_;
    // Reused across every rebuild: the stroke's points in scaled space.
    std::vector<StrokePoint> scaledPoints_;
    uint64_t frame_ = 0;
    size_t rebuilt_ = 0;
    size_t rebuiltLastFrame_ = 0;
};

// Which cache a stroke should be drawn through, and which entry in it - the
// four arguments MeshFor wants, as one thing small enough to travel down a
// drawing call.
//
// A default-constructed one (`cache` null) means "don't cache": build the
// mesh, draw it, throw it away. That is right for the stroke being drawn
// right now, which grows every frame and would never hit a cached entry.
//
// A caller that draws a whole item passes one with `itemId`/`strokeIndex`
// left unset and aims it at each stroke via For(), so it only has to know
// which cache and which generation.
struct StrokeMeshSlot {
    StrokeMeshCache* cache = nullptr;
    // CanvasManager::Generation(), read at the call - see StrokeMeshCache
    // for why it travels per call rather than being latched per frame.
    uint64_t generation = 0;
    uint64_t itemId = 0;
    size_t strokeIndex = 0;

    StrokeMeshSlot For(uint64_t item, size_t index) const {
        return StrokeMeshSlot{cache, generation, item, index};
    }
};

}  // namespace sz::core
