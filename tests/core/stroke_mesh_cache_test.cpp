#include "core/drawing/stroke_mesh_cache.h"

#include <gtest/gtest.h>

#include <vector>

namespace sz::core {
namespace {

Stroke MakeStroke(float xOffset = 0.0f, uint32_t color = 0xFF0000FFu, float width = 3.0f) {
    Stroke stroke;
    stroke.colorRGBA = color;
    stroke.width = width;
    for (int i = 0; i < 12; ++i) {
        const auto t = static_cast<float>(i);
        stroke.points.push_back(StrokePoint{xOffset + t * 4.0f, t * t * 0.5f});
    }
    return stroke;
}

// One frame's worth of asking for the same stroke, so the tests read the way
// the render loop actually behaves.
const StrokeMesh& Draw(StrokeMeshCache& cache, const Stroke& stroke, uint64_t generation, float scale = 1.0f,
                        size_t index = 0) {
    return cache.MeshFor(/*itemId=*/7, index, stroke, generation, scale, scale, /*halfWidth=*/1.5f);
}

TEST(StrokeMeshCacheTest, BuildsTheSameMeshBuildStrokeMeshWouldHave) {
    const Stroke stroke = MakeStroke();
    StrokeMeshCache cache;
    cache.BeginFrame();
    const StrokeMesh& cached = Draw(cache, stroke, 1);
    cache.EndFrame();

    std::vector<StrokePoint> scaled = stroke.points;  // scale 1, so unchanged
    const StrokeMesh direct = BuildStrokeMesh(scaled, 1.5f, kStrokeFringePx, StrokeCorners::Round);

    ASSERT_EQ(cached.vertices.size(), direct.vertices.size());
    ASSERT_EQ(cached.indices.size(), direct.indices.size());
    for (size_t i = 0; i < direct.vertices.size(); ++i) {
        EXPECT_FLOAT_EQ(cached.vertices[i].x, direct.vertices[i].x) << "vertex " << i;
        EXPECT_FLOAT_EQ(cached.vertices[i].y, direct.vertices[i].y) << "vertex " << i;
        EXPECT_FLOAT_EQ(cached.vertices[i].coverage, direct.vertices[i].coverage) << "vertex " << i;
    }
    EXPECT_EQ(cached.indices, direct.indices);
}

// The whole point: a canvas nobody is touching costs nothing after the first
// frame, however many frames go by.
TEST(StrokeMeshCacheTest, AnUnchangedCanvasIsTessellatedExactlyOnce) {
    const Stroke stroke = MakeStroke();
    StrokeMeshCache cache;
    for (int frame = 0; frame < 10; ++frame) {
        cache.BeginFrame();
        Draw(cache, stroke, /*generation=*/5);
        cache.EndFrame();
        EXPECT_EQ(cache.RebuiltLastFrame(), frame == 0 ? 1u : 0u) << "frame " << frame;
    }
}

// A moving generation is the app saying "something, somewhere, changed" -
// dragging an item says it every frame. It must not mean "rebuild".
TEST(StrokeMeshCacheTest, AMovingGenerationAloneDoesNotRebuild) {
    const Stroke stroke = MakeStroke();
    StrokeMeshCache cache;
    cache.BeginFrame();
    Draw(cache, stroke, 1);
    cache.EndFrame();

    for (uint64_t generation = 2; generation < 10; ++generation) {
        cache.BeginFrame();
        Draw(cache, stroke, generation);
        cache.EndFrame();
        EXPECT_EQ(cache.RebuiltLastFrame(), 0u) << "generation " << generation;
    }
}

// The case a stroke count cannot see, and the reason the check is a
// fingerprint rather than a length: the eraser rewrites points in place.
TEST(StrokeMeshCacheTest, AStrokeEditedInPlaceIsRebuilt) {
    Stroke stroke = MakeStroke();
    StrokeMeshCache cache;
    cache.BeginFrame();
    Draw(cache, stroke, 1);
    cache.EndFrame();

    stroke.points[4].y += 25.0f;  // same count, same color, same width
    cache.BeginFrame();
    Draw(cache, stroke, 2);
    cache.EndFrame();
    EXPECT_EQ(cache.RebuiltLastFrame(), 1u);

    // ...and the rebuilt mesh is the new shape, not the old one.
    const StrokeMesh direct = BuildStrokeMesh(stroke.points, 1.5f, kStrokeFringePx, StrokeCorners::Round);
    cache.BeginFrame();
    const StrokeMesh& cached = Draw(cache, stroke, 2);
    ASSERT_EQ(cached.vertices.size(), direct.vertices.size());
    for (size_t i = 0; i < direct.vertices.size(); ++i) {
        EXPECT_FLOAT_EQ(cached.vertices[i].y, direct.vertices[i].y) << "vertex " << i;
    }
    cache.EndFrame();
}

// A generation that hasn't moved is taken at its word - that is what makes
// the fast path free. Nothing in the app edits a stroke without bumping it
// (see CanvasManager::MarkChanged), so this documents the contract rather
// than a hazard.
TEST(StrokeMeshCacheTest, AStrokeEditedWithoutBumpingTheGenerationIsNotNoticed) {
    Stroke stroke = MakeStroke();
    StrokeMeshCache cache;
    cache.BeginFrame();
    Draw(cache, stroke, 1);
    cache.EndFrame();

    stroke.points[4].y += 25.0f;
    cache.BeginFrame();
    Draw(cache, stroke, 1);  // same generation
    cache.EndFrame();
    EXPECT_EQ(cache.RebuiltLastFrame(), 0u);
}

TEST(StrokeMeshCacheTest, ColorWidthAndCornersArePartOfTheFingerprint) {
    StrokeMeshCache cache;
    cache.BeginFrame();
    Draw(cache, MakeStroke(), 1);
    cache.EndFrame();

    cache.BeginFrame();
    Draw(cache, MakeStroke(0.0f, 0x00FF00FFu), 2);
    cache.EndFrame();
    EXPECT_EQ(cache.RebuiltLastFrame(), 1u) << "a recolored stroke";

    cache.BeginFrame();
    Draw(cache, MakeStroke(0.0f, 0x00FF00FFu, /*width=*/9.0f), 3);
    cache.EndFrame();
    EXPECT_EQ(cache.RebuiltLastFrame(), 1u) << "a stroke of a different width";

    Stroke square = MakeStroke(0.0f, 0x00FF00FFu, /*width=*/9.0f);
    square.corners = StrokeCorners::Sharp;
    cache.BeginFrame();
    Draw(cache, square, 4);
    cache.EndFrame();
    EXPECT_EQ(cache.RebuiltLastFrame(), 1u) << "a stroke with other corners";
}

// Resizing the item changes the geometry - the pen gets wider and the
// one-pixel fringe does not - so the scale has to invalidate even though the
// stroke itself is untouched.
TEST(StrokeMeshCacheTest, ADifferentScaleIsADifferentMesh) {
    const Stroke stroke = MakeStroke();
    StrokeMeshCache cache;
    cache.BeginFrame();
    Draw(cache, stroke, 1, /*scale=*/1.0f);
    cache.EndFrame();

    cache.BeginFrame();
    Draw(cache, stroke, /*generation=*/1, /*scale=*/2.0f);
    cache.EndFrame();
    EXPECT_EQ(cache.RebuiltLastFrame(), 1u) << "the same generation, but drawn at twice the size";
}

TEST(StrokeMeshCacheTest, TwoStrokesOfOneItemAreHeldSeparately) {
    const Stroke first = MakeStroke(0.0f);
    const Stroke second = MakeStroke(100.0f);
    StrokeMeshCache cache;
    cache.BeginFrame();
    Draw(cache, first, 1, 1.0f, /*index=*/0);
    Draw(cache, second, 1, 1.0f, /*index=*/1);
    cache.EndFrame();
    EXPECT_EQ(cache.RebuiltLastFrame(), 2u);
    EXPECT_EQ(cache.EntryCount(), 2u);

    cache.BeginFrame();
    Draw(cache, first, 1, 1.0f, 0);
    Draw(cache, second, 1, 1.0f, 1);
    cache.EndFrame();
    EXPECT_EQ(cache.RebuiltLastFrame(), 0u) << "neither should have displaced the other";
}

// Switching canvas, deleting an item, minimizing one - all of them are
// simply "not drawn this frame", and none of them should leave megabytes of
// triangles behind.
TEST(StrokeMeshCacheTest, AnItemThatStopsBeingDrawnIsDropped) {
    const Stroke stroke = MakeStroke();
    StrokeMeshCache cache;
    cache.BeginFrame();
    cache.MeshFor(/*itemId=*/1, 0, stroke, 1, 1.0f, 1.0f, 1.5f);
    cache.MeshFor(/*itemId=*/2, 0, stroke, 1, 1.0f, 1.0f, 1.5f);
    cache.EndFrame();
    EXPECT_EQ(cache.EntryCount(), 2u);

    cache.BeginFrame();
    cache.MeshFor(/*itemId=*/1, 0, stroke, 1, 1.0f, 1.0f, 1.5f);
    cache.EndFrame();
    EXPECT_EQ(cache.EntryCount(), 1u);
}

// Undo takes a stroke back off the end of the list; its mesh should go too,
// rather than sitting in an item that is still very much alive.
TEST(StrokeMeshCacheTest, AStrokeThatIsUndoneAwayReleasesItsMesh) {
    const Stroke stroke = MakeStroke();
    StrokeMeshCache cache;
    cache.BeginFrame();
    Draw(cache, stroke, 1, 1.0f, 0);
    Draw(cache, stroke, 1, 1.0f, 1);
    Draw(cache, stroke, 1, 1.0f, 2);
    cache.EndFrame();
    EXPECT_EQ(cache.EntryCount(), 3u);

    cache.BeginFrame();
    Draw(cache, stroke, 2, 1.0f, 0);
    Draw(cache, stroke, 2, 1.0f, 1);
    cache.EndFrame();
    EXPECT_EQ(cache.EntryCount(), 2u);
}

TEST(StrokeMeshCacheTest, ClearForgetsEverything) {
    const Stroke stroke = MakeStroke();
    StrokeMeshCache cache;
    cache.BeginFrame();
    Draw(cache, stroke, 1);
    cache.EndFrame();
    ASSERT_EQ(cache.EntryCount(), 1u);

    cache.Clear();
    EXPECT_EQ(cache.EntryCount(), 0u);

    cache.BeginFrame();
    Draw(cache, stroke, 1);
    cache.EndFrame();
    EXPECT_EQ(cache.RebuiltLastFrame(), 1u) << "cleared means built again, not remembered";
}

TEST(StrokeMeshCacheTest, AnEmptyStrokeProducesNoGeometryAndDoesNotUpsetTheCache) {
    const Stroke empty;
    StrokeMeshCache cache;
    cache.BeginFrame();
    const StrokeMesh& mesh = Draw(cache, empty, 1);
    cache.EndFrame();
    EXPECT_TRUE(mesh.vertices.empty());
    EXPECT_TRUE(mesh.indices.empty());
}

}  // namespace
}  // namespace sz::core

