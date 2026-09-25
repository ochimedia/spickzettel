#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sz::core {

// A rectangle of pixels, in whole pixels. Empty when either extent is 0.
struct PixelRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool Empty() const { return w <= 0 || h <= 0; }
    bool operator==(const PixelRect&) const = default;
};

// How many bitmap pixels one unit of an item's native space gets, so that a
// bitmap sized `nativeW x nativeH` times this stays within `maxExtent` on
// its longer side. The `requested` scale when it already fits; a smaller
// scale - uniform, both axes - when it doesn't.
//
// This is the piece that keeps a capped bitmap *aligned*. Clamping the
// bitmap's width and height while still mapping native coordinates 1:1
// into it crops everything past the cap and stretches what is left over
// the whole item: on a 5120-wide capture a stroke lands a quarter of the
// way to the right of where it was drawn. Lowering the scale instead keeps
// every coordinate in step with every pixel, and every caller multiplies
// by the scale, so nothing downstream has to know the cap exists.
float FitResolutionScale(float nativeW, float nativeH, float requested, int maxExtent);

// The bitmap extent for one native dimension at a scale, never below one
// pixel - the other half of the pairing above, kept together so a bitmap's
// size and the scale its coordinates use can't be computed two ways.
int ScaledPixelExtent(float native, float scale);

// The bitmap an item's strokes are drawn into in the Rasterized render mode
// (see StrokeRenderMode), and the brush that draws them. A cache of the
// strokes, never persisted.
//
// The brush is a capsule per segment - the exact figure stroke_mesh.h
// tessellates - so a line looks the same in either render mode. Coverage
// is analytic rather than sampled: each pixel's alpha comes from its
// distance to the segment's centerline, which gives round caps, round
// joins and clean anti-aliasing without any of the geometry a triangle
// renderer needs.
//
// A stroke is a session, not a sequence of independent stamps. Compositing
// each segment as it arrives would darken every overlap, and consecutive
// segments overlap at every join. Instead a stroke accumulates *coverage*
// into a mask (taking the maximum, so an overlap is covered once), and
// every touched tile is recomposited from the pixels it had before the
// stroke started.
class StrokeBitmap {
public:
    // Square tiles, chosen so a typical stroke touches few of them and each
    // is small enough to copy cheaply: 64x64 RGBA is 16 KB.
    static constexpr int kTileSize = 64;

    StrokeBitmap() = default;
    // Fully transparent, which is what a fresh bitmap is: it has to add
    // nothing to what is underneath until a stroke is drawn on it.
    StrokeBitmap(int width, int height);

    int Width() const { return width_; }
    int Height() const { return height_; }
    bool Empty() const { return width_ <= 0 || height_ <= 0; }
    const std::vector<uint8_t>& PixelsRGBA() const { return pixels_; }

    // Starts a stroke. `colorRGBA` is 0xRRGGBBAA and its alpha is the ink's
    // own strength; `radiusPx` is half the pen width, in this bitmap's own
    // pixels. Any stroke already in progress is finished first.
    void BeginStroke(uint32_t colorRGBA, float radiusPx);
    // Extends the stroke from (x0, y0) to (x1, y1), both in bitmap pixels,
    // and returns the region whose pixels changed. Empty when none did: ink
    // over ink of its own color changes nothing. A zero-length segment is a
    // dab, which is how a dot is drawn (see the round-cap geometry).
    PixelRect ExtendStroke(float x0, float y0, float x1, float y1);
    void EndStroke();
    // How many tiles the stroke in progress has touched - what it keeps a
    // copy of. For a test: a long diagonal has to keep a sliver of its
    // bounding box, not all of it.
    size_t TilesInStroke() const { return strokeBefore_.size(); }

private:
    // Row-major tile index, and the pixel rectangle it covers (clipped to
    // the image, so edge tiles are partial).
    int TileIndex(int tileX, int tileY) const { return tileY * TilesAcross() + tileX; }
    int TilesAcross() const { return (width_ + kTileSize - 1) / kTileSize; }
    int TilesDown() const { return (height_ + kTileSize - 1) / kTileSize; }
    PixelRect TileBounds(int index) const;
    // Saves a tile's current pixels into the stroke's own before-image, the
    // first time the stroke touches it, and gives it a zeroed mask.
    void EnsureTileTracked(int index);
    // Rebuilds one tile's pixels from what it held before the stroke plus
    // the stroke's coverage so far - never from what is there now, which is
    // what stops an overlap being painted twice. Whether any pixel came out
    // different from what it was.
    bool RecompositeTile(int index, const PixelRect& within);

    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> pixels_;  // width*height*4, RGBA8, row-major

    bool strokeActive_ = false;
    uint32_t strokeColorRGBA_ = 0;
    float strokeRadius_ = 0.0f;
    // Per touched tile: the pixels it held before this stroke, and the
    // stroke's coverage over it so far (0-255, maximum-combined).
    std::unordered_map<int, std::vector<uint8_t>> strokeBefore_;
    std::unordered_map<int, std::vector<uint8_t>> strokeMask_;
};

}  // namespace sz::core
