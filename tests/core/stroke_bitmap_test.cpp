#include "core/drawing/stroke_bitmap.h"

#include <algorithm>
#include <cmath>
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

Rgba At(const StrokeBitmap& image, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * image.Width() + x) * 4;
    const std::vector<uint8_t>& p = image.PixelsRGBA();
    return Rgba{p[i], p[i + 1], p[i + 2], p[i + 3]};
}

void PaintSegment(StrokeBitmap& image, float x0, float y0, float x1, float y1, float radius,
                  uint32_t color = kOpaqueRed) {
    image.BeginStroke(color, radius);
    image.ExtendStroke(x0, y0, x1, y1);
    image.EndStroke();
}

TEST(StrokeBitmapTest, StartsFullyTransparent) {
    const StrokeBitmap image(16, 8);
    EXPECT_EQ(image.Width(), 16);
    EXPECT_EQ(image.Height(), 8);
    for (const uint8_t byte : image.PixelsRGBA()) {
        EXPECT_EQ(byte, 0);
    }
}

// A dab - a zero-length segment - is a disc, which is how the brush makes
// a dot. The center is fully covered and the outside is untouched.
TEST(StrokeBitmapTest, AZeroLengthSegmentPaintsADisc) {
    StrokeBitmap image(40, 40);
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
TEST(StrokeBitmapTest, TheEdgeOfABrushIsPartiallyCovered) {
    StrokeBitmap image(40, 40);
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
TEST(StrokeBitmapTest, AStrokeThatOverlapsItselfIsNotPaintedTwice) {
    StrokeBitmap overlapping(60, 20);
    overlapping.BeginStroke(kHalfRed, 4.0f);
    for (int i = 0; i < 20; ++i) {
        // Back and forth over the same pixels, twenty times.
        overlapping.ExtendStroke(10.0f, 10.0f, 30.0f, 10.0f);
        overlapping.ExtendStroke(30.0f, 10.0f, 10.0f, 10.0f);
    }
    overlapping.EndStroke();

    StrokeBitmap once(60, 20);
    PaintSegment(once, 10.0f, 10.0f, 30.0f, 10.0f, 4.0f, kHalfRed);

    EXPECT_EQ(At(overlapping, 20, 10), At(once, 20, 10));
    EXPECT_EQ(At(overlapping, 20, 10).a, 128);
}

// ...but two *separate* strokes do build up, because each is its own
// deliberate mark.
TEST(StrokeBitmapTest, SeparateStrokesAccumulate) {
    StrokeBitmap image(60, 20);
    PaintSegment(image, 10.0f, 10.0f, 30.0f, 10.0f, 4.0f, kHalfRed);
    const int afterOne = At(image, 20, 10).a;
    PaintSegment(image, 10.0f, 10.0f, 30.0f, 10.0f, 4.0f, kHalfRed);
    const int afterTwo = At(image, 20, 10).a;

    EXPECT_EQ(afterOne, 128);
    EXPECT_GT(afterTwo, afterOne);
}

// A corner-to-corner line crosses a sliver of the tiles in its bounding
// box, and only those are copied and walked. All of the box was 8 MB a
// line at 1080p.
TEST(StrokeBitmapTest, ALongDiagonalTouchesOnlyTheTilesItCrosses) {
    StrokeBitmap image(1920, 1080);
    image.BeginStroke(0xFF0000FFu, 3.0f);
    image.ExtendStroke(0.0f, 0.0f, 1919.0f, 1079.0f);
    const size_t touched = image.TilesInStroke();
    image.EndStroke();

    const int across = (1920 + StrokeBitmap::kTileSize - 1) / StrokeBitmap::kTileSize;
    const int down = (1080 + StrokeBitmap::kTileSize - 1) / StrokeBitmap::kTileSize;
    EXPECT_LT(touched, static_cast<size_t>(3 * (across + down)));
    EXPECT_LT(touched * 4, static_cast<size_t>(across * down));

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
}

TEST(StrokeBitmapTest, ExtendReportsTheRegionItChanged) {
    StrokeBitmap image(100, 100);
    image.BeginStroke(kOpaqueRed, 3.0f);
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

TEST(StrokeBitmapTest, AStrokeIsClippedToTheImage) {
    StrokeBitmap image(20, 20);
    image.BeginStroke(kOpaqueRed, 6.0f);
    const PixelRect dirty = image.ExtendStroke(-50.0f, -50.0f, 5.0f, 5.0f);
    image.EndStroke();

    EXPECT_GE(dirty.x, 0);
    EXPECT_GE(dirty.y, 0);
    EXPECT_LE(dirty.x + dirty.w, 20);
    EXPECT_LE(dirty.y + dirty.h, 20);
    EXPECT_EQ(At(image, 5, 5).a, 255);
}

// Only the tiles a stroke actually touched are copied, a fraction of the
// bitmap rather than all of it.
TEST(StrokeBitmapTest, AStrokeOnlyKeepsTheTilesItTouched) {
    StrokeBitmap image(640, 640);  // 10x10 tiles of 64
    image.BeginStroke(kOpaqueRed, 3.0f);
    image.ExtendStroke(100.0f, 100.0f, 140.0f, 100.0f);

    EXPECT_GE(image.TilesInStroke(), 1u);
    EXPECT_LE(image.TilesInStroke(), 4u);  // of a hundred
    image.EndStroke();
    EXPECT_EQ(image.TilesInStroke(), 0u);
}

// A stroke spanning a tile boundary must be continuous across it - the
// classic seam bug, where each tile composites without knowing its
// neighbor.
TEST(StrokeBitmapTest, AStrokeAcrossATileBoundaryHasNoSeam) {
    StrokeBitmap image(192, 64);
    PaintSegment(image, 10.0f, 32.0f, 180.0f, 32.0f, 6.0f);

    for (int x = 12; x < 178; ++x) {
        EXPECT_EQ(At(image, x, 32).a, 255) << "gap at x=" << x;
    }
}

// An item that fits under the cap keeps the scale it asked for; one that
// doesn't gets a uniformly smaller one, with its longer side landing exactly
// on the cap. Cropping the bitmap instead would leave its coordinates out of
// register with its pixels - see FitResolutionScale.
TEST(StrokeBitmapTest, FitResolutionScaleKeepsTheRequestedScaleWhenItFits) {
    EXPECT_FLOAT_EQ(FitResolutionScale(1920.0f, 1080.0f, 1.0f, 4096), 1.0f);
    EXPECT_FLOAT_EQ(FitResolutionScale(1920.0f, 1080.0f, 2.0f, 4096), 2.0f);
    EXPECT_FLOAT_EQ(FitResolutionScale(4096.0f, 100.0f, 1.0f, 4096), 1.0f);  // exactly at the cap still fits
}

TEST(StrokeBitmapTest, FitResolutionScaleShrinksUniformlyToTheCap) {
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

TEST(StrokeBitmapTest, ScaledPixelExtentNeverDropsBelowOnePixel) {
    EXPECT_EQ(ScaledPixelExtent(0.2f, 1.0f), 1);
    EXPECT_EQ(ScaledPixelExtent(0.0f, 0.5f), 1);
    EXPECT_EQ(ScaledPixelExtent(300.0f, 0.5f), 150);
}

}  // namespace
}  // namespace sz::core
