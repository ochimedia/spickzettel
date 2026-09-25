#include "core/drawing/painted_image.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include <gtest/gtest.h>

namespace sz::core {
namespace {

constexpr uint32_t kOpaqueRed = 0xFF0000FFu;
constexpr uint32_t kHalfRed = 0xFF000080u;  // same red, alpha 128

struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
    bool operator==(const Rgba&) const = default;
};

Rgba At(const PaintedImage& image, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * image.Width() + x) * 4;
    const std::vector<uint8_t>& p = image.PixelsRGBA();
    return Rgba{p[i], p[i + 1], p[i + 2], p[i + 3]};
}

void PaintSegment(PaintedImage& image, float x0, float y0, float x1, float y1, float radius,
                  uint32_t color = kOpaqueRed,
                  PaintedImage::BrushMode mode = PaintedImage::BrushMode::Paint) {
    image.BeginStroke(color, radius, mode);
    image.ExtendStroke(x0, y0, x1, y1);
    image.EndStroke();
}

TEST(PaintedImageTest, StartsFullyTransparent) {
    const PaintedImage image(16, 8);
    EXPECT_EQ(image.Width(), 16);
    EXPECT_EQ(image.Height(), 8);
    for (const uint8_t byte : image.PixelsRGBA()) {
        EXPECT_EQ(byte, 0);
    }
}

TEST(PaintedImageTest, RejectsAMismatchedPixelBuffer) {
    const PaintedImage image = PaintedImage::FromPixels(4, 4, std::vector<uint8_t>(10, 0));
    EXPECT_TRUE(image.Empty());
}

// A dab - a zero-length segment - is a disc, which is how the pixel brush
// makes a dot. The center is fully covered and the outside is untouched.
TEST(PaintedImageTest, AZeroLengthSegmentPaintsADisc) {
    PaintedImage image(40, 40);
    PaintSegment(image, 20.0f, 20.0f, 20.0f, 20.0f, 6.0f);

    EXPECT_EQ(At(image, 20, 20), (Rgba{255, 0, 0, 255}));
    EXPECT_EQ(At(image, 24, 20).a, 255);   // still inside the radius
    EXPECT_EQ(At(image, 30, 20).a, 0);     // well outside
    EXPECT_EQ(At(image, 20, 30).a, 0);
    // Round, not square: the diagonal at the radius is outside the disc.
    EXPECT_EQ(At(image, 25, 25).a, 0);
}

// The edge is anti-aliased rather than a hard cut - the whole reason
// coverage is computed from distance instead of a fill.
TEST(PaintedImageTest, TheEdgeOfABrushIsPartiallyCovered) {
    PaintedImage image(40, 40);
    PaintSegment(image, 20.0f, 20.0f, 20.0f, 20.0f, 6.0f);

    bool sawPartial = false;
    for (int x = 20; x < 32; ++x) {
        const int alpha = At(image, x, 20).a;
        if (alpha > 0 && alpha < 255) {
            sawPartial = true;
        }
    }
    EXPECT_TRUE(sawPartial);
}

// The property that makes a stroke one stroke: where it overlaps itself,
// the ink lands once. A brush moving a pixel at a time overlaps almost
// completely, so summing coverage would turn a translucent line opaque
// within a few steps.
TEST(PaintedImageTest, AStrokeThatOverlapsItselfIsNotPaintedTwice) {
    PaintedImage overlapping(60, 20);
    overlapping.BeginStroke(kHalfRed, 4.0f, PaintedImage::BrushMode::Paint);
    for (int i = 0; i < 20; ++i) {
        // Back and forth over the same pixels, twenty times.
        overlapping.ExtendStroke(10.0f, 10.0f, 30.0f, 10.0f);
        overlapping.ExtendStroke(30.0f, 10.0f, 10.0f, 10.0f);
    }
    overlapping.EndStroke();

    PaintedImage once(60, 20);
    PaintSegment(once, 10.0f, 10.0f, 30.0f, 10.0f, 4.0f, kHalfRed);

    EXPECT_EQ(At(overlapping, 20, 10), At(once, 20, 10));
    EXPECT_EQ(At(overlapping, 20, 10).a, 128);
}

// ...but two *separate* strokes do build up, because each is its own
// deliberate mark.
TEST(PaintedImageTest, SeparateStrokesAccumulate) {
    PaintedImage image(60, 20);
    PaintSegment(image, 10.0f, 10.0f, 30.0f, 10.0f, 4.0f, kHalfRed);
    const int afterOne = At(image, 20, 10).a;
    PaintSegment(image, 10.0f, 10.0f, 30.0f, 10.0f, 4.0f, kHalfRed);
    const int afterTwo = At(image, 20, 10).a;

    EXPECT_EQ(afterOne, 128);
    EXPECT_GT(afterTwo, afterOne);
}

// A corner-to-corner line crosses a sliver of the tiles in its bounding
// box, and only those are saved for undo - every one it paints, and none
// of the rest. Saving the whole box was 8 MB of undo per line at 1080p.
TEST(PaintedImageTest, ALongDiagonalSavesOnlyTheTilesItCrosses) {
    PaintedImage image(1920, 1080);
    image.BeginStroke(0xFF0000FFu, 3.0f, PaintedImage::BrushMode::Paint);
    image.ExtendStroke(0.0f, 0.0f, 1919.0f, 1079.0f);
    const std::vector<PaintedTile> saved = image.EndStroke();

    const int allTiles = image.TilesAcross() * image.TilesDown();
    EXPECT_LT(saved.size(), static_cast<size_t>(3 * (image.TilesAcross() + image.TilesDown())));
    EXPECT_LT(saved.size() * 4, static_cast<size_t>(allTiles));

    // And nothing of the line is left out: every pixel whose center is
    // well inside the brush has ink. Asked of the geometry, not of another
    // stroke - which would rule out the same tiles by the same mistake. A
    // tile left out is a gap in the line.
    const auto distanceToLine = [](float px, float py) {
        const float dx = 1919.0f;
        const float dy = 1079.0f;
        const float t = std::clamp((px * dx + py * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
        return std::hypot(px - t * dx, py - t * dy);
    };
    for (int y = 0; y < image.Height(); ++y) {
        for (int x = 0; x < image.Width(); ++x) {
            if (distanceToLine(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) <= 2.0f) {
                ASSERT_NE(image.PixelsRGBA()[(static_cast<size_t>(y) * image.Width() + x) * 4 + 3], 0)
                    << "a gap at " << x << "," << y << ": a tile the line crosses left out";
            }
        }
    }

    // Every painted pixel is in a saved tile, so undo takes all of it back.
    std::set<int> savedIndices;
    for (const PaintedTile& tile : saved) {
        savedIndices.insert(tile.index);
    }
    const std::vector<uint8_t>& pixels = image.PixelsRGBA();
    for (int y = 0; y < image.Height(); ++y) {
        for (int x = 0; x < image.Width(); ++x) {
            if (pixels[(static_cast<size_t>(y) * image.Width() + x) * 4 + 3] != 0) {
                const int index = image.TileIndex(x / PaintedImage::kTileSize, y / PaintedImage::kTileSize);
                ASSERT_EQ(savedIndices.count(index), 1u) << x << "," << y;
            }
        }
    }
}

// And uploads only them: what changed is reported tile by tile, a sliver
// of the bounding box, and every pixel that changed is inside it.
TEST(PaintedImageTest, ALongDiagonalReportsOnlyTheTilesItChanged) {
    PaintedImage image(1920, 1080);
    image.BeginStroke(0xFF0000FFu, 3.0f, PaintedImage::BrushMode::Paint);
    const PixelRect box = image.ExtendStroke(0.0f, 0.0f, 1919.0f, 1079.0f);
    const std::vector<PixelRect>& regions = image.LastChangedRegions();
    ASSERT_FALSE(regions.empty());
    size_t area = 0;
    for (const PixelRect& region : regions) {
        area += static_cast<size_t>(region.w) * static_cast<size_t>(region.h);
    }
    EXPECT_LT(area * 8, static_cast<size_t>(box.w) * static_cast<size_t>(box.h));

    const std::vector<uint8_t>& pixels = image.PixelsRGBA();
    for (int y = 0; y < image.Height(); ++y) {
        for (int x = 0; x < image.Width(); ++x) {
            if (pixels[(static_cast<size_t>(y) * image.Width() + x) * 4 + 3] == 0) {
                continue;
            }
            const bool inside = std::any_of(regions.begin(), regions.end(), [x, y](const PixelRect& r) {
                return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
            });
            ASSERT_TRUE(inside) << x << "," << y;
        }
    }
}

// An eraser over transparent pixels changes nothing: nothing to upload,
// and nothing to take back.
TEST(PaintedImageTest, AStrokeThatChangesNothingReportsNothing) {
    PaintedImage image(256, 256);
    image.BeginStroke(0xFF0000FFu, 4.0f, PaintedImage::BrushMode::Paint);
    ASSERT_FALSE(image.ExtendStroke(20.0f, 20.0f, 40.0f, 20.0f).Empty());
    ASSERT_FALSE(image.EndStroke().empty());

    image.BeginStroke(0xFFFFFFFFu, 4.0f, PaintedImage::BrushMode::Erase);
    EXPECT_TRUE(image.ExtendStroke(150.0f, 150.0f, 200.0f, 200.0f).Empty()) << "nothing there to erase";
    EXPECT_TRUE(image.ExtendRect(100.0f, 180.0f, 250.0f, 180.0f).Empty()) << "a rectangle of no height";
    EXPECT_TRUE(image.EndStroke().empty());

    // A stroke that changes some tiles keeps only those.
    image.BeginStroke(0xFFFFFFFFu, 4.0f, PaintedImage::BrushMode::Erase);
    EXPECT_FALSE(image.ExtendStroke(30.0f, 20.0f, 200.0f, 200.0f).Empty());
    const std::vector<PaintedTile> saved = image.EndStroke();
    ASSERT_EQ(saved.size(), 1u);
    EXPECT_EQ(saved[0].index, image.TileIndex(0, 0));
}

// Pixel-perfect erasing: the point of a painted layer. What comes away is
// the shape of the brush, not whichever whole stroke was underneath.
TEST(PaintedImageTest, EraseTakesAwayExactlyTheBrushShape) {
    PaintedImage image(60, 60);
    PaintSegment(image, 5.0f, 30.0f, 55.0f, 30.0f, 10.0f);
    ASSERT_EQ(At(image, 30, 30).a, 255);

    PaintSegment(image, 30.0f, 30.0f, 30.0f, 30.0f, 5.0f, kOpaqueRed, PaintedImage::BrushMode::Erase);

    EXPECT_EQ(At(image, 30, 30).a, 0);  // gone where the eraser was
    EXPECT_EQ(At(image, 45, 30).a, 255);  // and only there
    EXPECT_EQ(At(image, 15, 30).a, 255);
}

// Erasing leaves the color and takes the alpha, so a half-erased edge
// fades out instead of darkening toward black.
TEST(PaintedImageTest, ErasingLeavesTheColorAndTakesTheAlpha) {
    PaintedImage image(40, 40);
    PaintSegment(image, 20.0f, 20.0f, 20.0f, 20.0f, 10.0f);
    image.BeginStroke(kOpaqueRed, 5.0f, PaintedImage::BrushMode::Erase);
    image.ExtendStroke(20.0f, 14.0f, 20.0f, 14.0f);
    image.EndStroke();

    bool sawPartialEdge = false;
    for (int y = 14; y < 24; ++y) {
        const Rgba p = At(image, 20, y);
        if (p.a > 0 && p.a < 255) {
            sawPartialEdge = true;
            EXPECT_EQ(p.r, 255);
            EXPECT_EQ(p.g, 0);
            EXPECT_EQ(p.b, 0);
        }
    }
    EXPECT_TRUE(sawPartialEdge);
}

// The rectangular eraser: the same session and the same undo tiles as the
// brush, only a different shape - which is the whole reason it lives here
// rather than as a second mechanism.
TEST(PaintedImageTest, ExtendRectErasesExactlyItsRectangle) {
    PaintedImage image(80, 80);
    PaintSegment(image, 5.0f, 40.0f, 75.0f, 40.0f, 20.0f);
    ASSERT_EQ(At(image, 40, 40).a, 255);

    image.BeginStroke(kOpaqueRed, 1.0f, PaintedImage::BrushMode::Erase);
    image.ExtendRect(30.0f, 20.0f, 50.0f, 60.0f);
    image.EndStroke();

    EXPECT_EQ(At(image, 40, 40).a, 0);   // inside the rectangle
    EXPECT_EQ(At(image, 31, 25).a, 0);   // ...including its corners
    EXPECT_EQ(At(image, 20, 40).a, 255); // outside, untouched
    EXPECT_EQ(At(image, 60, 40).a, 255);
}

// A rectangle edge that falls between pixels comes out anti-aliased, same
// as the brush's.
TEST(PaintedImageTest, ARectangleEdgeBetweenPixelsIsPartiallyCovered) {
    PaintedImage image(40, 40);
    PaintSegment(image, 2.0f, 20.0f, 38.0f, 20.0f, 15.0f);
    image.BeginStroke(kOpaqueRed, 1.0f, PaintedImage::BrushMode::Erase);
    image.ExtendRect(10.5f, 5.0f, 30.0f, 35.0f);
    image.EndStroke();

    EXPECT_EQ(At(image, 20, 20).a, 0);
    EXPECT_GT(At(image, 10, 20).a, 0);
    EXPECT_LT(At(image, 10, 20).a, 255);
}

// ...and it is undone by the same tiles as any other stroke.
TEST(PaintedImageTest, ARectangleEraseIsUndoneByItsTiles) {
    PaintedImage image(128, 128);
    PaintSegment(image, 10.0f, 64.0f, 118.0f, 64.0f, 20.0f);
    const std::vector<uint8_t> before = image.PixelsRGBA();

    image.BeginStroke(kOpaqueRed, 1.0f, PaintedImage::BrushMode::Erase);
    image.ExtendRect(40.0f, 30.0f, 90.0f, 100.0f);
    const std::vector<PaintedTile> tiles = image.EndStroke();
    ASSERT_NE(image.PixelsRGBA(), before);

    PixelRect changed;
    image.RestoreTiles(tiles, changed);
    EXPECT_EQ(image.PixelsRGBA(), before);
}

TEST(PaintedImageTest, ExtendReportsTheRegionItChanged) {
    PaintedImage image(100, 100);
    image.BeginStroke(kOpaqueRed, 3.0f, PaintedImage::BrushMode::Paint);
    const PixelRect dirty = image.ExtendStroke(40.0f, 50.0f, 60.0f, 50.0f);
    image.EndStroke();

    EXPECT_FALSE(dirty.Empty());
    // Covers the capsule, and nothing far outside it.
    EXPECT_LE(dirty.x, 36);
    EXPECT_GE(dirty.x + dirty.w, 64);
    EXPECT_LE(dirty.y, 46);
    EXPECT_GE(dirty.y + dirty.h, 54);
    EXPECT_LT(dirty.w, 40);
    EXPECT_LT(dirty.h, 20);
}

TEST(PaintedImageTest, AStrokeIsClippedToTheImage) {
    PaintedImage image(20, 20);
    image.BeginStroke(kOpaqueRed, 6.0f, PaintedImage::BrushMode::Paint);
    const PixelRect dirty = image.ExtendStroke(-50.0f, -50.0f, 5.0f, 5.0f);
    image.EndStroke();

    EXPECT_GE(dirty.x, 0);
    EXPECT_GE(dirty.y, 0);
    EXPECT_LE(dirty.x + dirty.w, 20);
    EXPECT_LE(dirty.y + dirty.h, 20);
    EXPECT_EQ(At(image, 5, 5).a, 255);
}

// Undo: a stroke hands back the pixels of every tile it touched, and
// putting them back restores the image exactly.
TEST(PaintedImageTest, RestoringAStrokesTilesUndoesItExactly) {
    PaintedImage image(200, 200);
    PaintSegment(image, 20.0f, 20.0f, 180.0f, 20.0f, 5.0f);
    const std::vector<uint8_t> beforeSecond = image.PixelsRGBA();

    image.BeginStroke(0x00FF00FFu, 8.0f, PaintedImage::BrushMode::Paint);
    image.ExtendStroke(20.0f, 100.0f, 180.0f, 140.0f);
    const std::vector<PaintedTile> undoTiles = image.EndStroke();
    EXPECT_NE(image.PixelsRGBA(), beforeSecond);

    PixelRect changed;
    image.RestoreTiles(undoTiles, changed);

    EXPECT_EQ(image.PixelsRGBA(), beforeSecond);
    EXPECT_FALSE(changed.Empty());
}

// ...and restoring hands back what it replaced, which is the redo state.
TEST(PaintedImageTest, RestoringReturnsWhatItReplacedSoRedoWorks) {
    PaintedImage image(200, 200);
    image.BeginStroke(kOpaqueRed, 6.0f, PaintedImage::BrushMode::Paint);
    image.ExtendStroke(20.0f, 20.0f, 180.0f, 180.0f);
    const std::vector<PaintedTile> undoTiles = image.EndStroke();
    const std::vector<uint8_t> painted = image.PixelsRGBA();

    PixelRect changed;
    const std::vector<PaintedTile> redoTiles = image.RestoreTiles(undoTiles, changed);
    EXPECT_NE(image.PixelsRGBA(), painted);

    image.RestoreTiles(redoTiles, changed);
    EXPECT_EQ(image.PixelsRGBA(), painted);
}

// Only the tiles a stroke actually touched, so an undo entry stays a
// fraction of the image rather than a copy of it.
TEST(PaintedImageTest, AStrokeOnlySavesTheTilesItTouched) {
    PaintedImage image(640, 640);  // 10x10 tiles of 64
    image.BeginStroke(kOpaqueRed, 3.0f, PaintedImage::BrushMode::Paint);
    image.ExtendStroke(100.0f, 100.0f, 140.0f, 100.0f);
    const std::vector<PaintedTile> tiles = image.EndStroke();

    EXPECT_GE(tiles.size(), 1u);
    EXPECT_LE(tiles.size(), 4u);  // of a hundred
    // Sorted, so two identical strokes produce identical entries.
    EXPECT_TRUE(std::is_sorted(tiles.begin(), tiles.end(),
                                [](const PaintedTile& a, const PaintedTile& b) { return a.index < b.index; }));
}

// A stroke spanning a tile boundary must be continuous across it - the
// classic seam bug, where each tile composites without knowing its
// neighbor.
TEST(PaintedImageTest, AStrokeAcrossATileBoundaryHasNoSeam) {
    PaintedImage image(192, 64);
    PaintSegment(image, 10.0f, 32.0f, 180.0f, 32.0f, 6.0f);

    for (int x = 12; x < 178; ++x) {
        EXPECT_EQ(At(image, x, 32).a, 255) << "gap at x=" << x;
    }
}

// An item that fits under the cap keeps the scale it asked for; one that
// doesn't gets a uniformly smaller one, with its longer side landing exactly
// on the cap. Cropping the bitmap instead would leave its coordinates out of
// register with its pixels - see FitResolutionScale.
TEST(PaintedImageTest, FitResolutionScaleKeepsTheRequestedScaleWhenItFits) {
    EXPECT_FLOAT_EQ(FitResolutionScale(1920.0f, 1080.0f, 1.0f, 4096), 1.0f);
    EXPECT_FLOAT_EQ(FitResolutionScale(1920.0f, 1080.0f, 2.0f, 4096), 2.0f);
    EXPECT_FLOAT_EQ(FitResolutionScale(4096.0f, 100.0f, 1.0f, 4096), 1.0f);  // exactly at the cap still fits
}

TEST(PaintedImageTest, FitResolutionScaleShrinksUniformlyToTheCap) {
    // A 5120x2160 capture at 1x is a quarter too wide for 4096.
    const float scale = FitResolutionScale(5120.0f, 2160.0f, 1.0f, 4096);
    EXPECT_FLOAT_EQ(scale, 0.8f);
    EXPECT_EQ(ScaledPixelExtent(5120.0f, scale), 4096);
    EXPECT_EQ(ScaledPixelExtent(2160.0f, scale), 1728);  // the same factor on both axes
    // The same item asked for at 2x lands on the same cap.
    EXPECT_EQ(ScaledPixelExtent(5120.0f, FitResolutionScale(5120.0f, 2160.0f, 2.0f, 4096)), 4096);
    // Tall items are capped by their height.
    EXPECT_EQ(ScaledPixelExtent(9000.0f, FitResolutionScale(300.0f, 9000.0f, 1.0f, 4096)), 4096);
}

TEST(PaintedImageTest, ScaledPixelExtentNeverDropsBelowOnePixel) {
    EXPECT_EQ(ScaledPixelExtent(0.2f, 1.0f), 1);
    EXPECT_EQ(ScaledPixelExtent(0.0f, 0.5f), 1);
    EXPECT_EQ(ScaledPixelExtent(300.0f, 0.5f), 150);
}

// Restoring tiles that don't belong to this image is ignored rather than
// corrupting it - an undo entry from a layer that has since been resized.
TEST(PaintedImageTest, RestoringForeignTilesIsIgnored) {
    PaintedImage image(64, 64);
    PaintSegment(image, 32.0f, 32.0f, 32.0f, 32.0f, 8.0f);
    const std::vector<uint8_t> painted = image.PixelsRGBA();

    std::vector<PaintedTile> foreign = {PaintedTile{0, std::vector<uint8_t>(7, 0)},
                                        PaintedTile{99, std::vector<uint8_t>(64 * 64 * 4, 0)}};
    PixelRect changed;
    image.RestoreTiles(foreign, changed);

    EXPECT_EQ(image.PixelsRGBA(), painted);
    EXPECT_TRUE(changed.Empty());
}

}  // namespace
}  // namespace sz::core
