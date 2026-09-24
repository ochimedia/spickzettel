#pragma once

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
// way to the right of the pen. Lowering the scale instead keeps every
// coordinate in step with every pixel, and every caller multiplies by the
// scale, so nothing downstream has to know the cap exists.
float FitResolutionScale(float nativeW, float nativeH, float requested, int maxExtent);

// The bitmap extent for one native dimension at a scale, never below one
// pixel - the other half of the pairing above, kept together so a bitmap's
// size and the scale its coordinates use can't be computed two ways.
int ScaledPixelExtent(float native, float scale);

// One square block of a painted image, saved so it can be put back. The
// unit undo works in: a stroke touches a handful of these rather than the
// whole image, which is the difference between a few hundred kilobytes an
// entry and eight megabytes.
struct PaintedTile {
    int index = 0;  // row-major tile index, see PaintedImage::TileIndex
    std::vector<uint8_t> pixelsRGBA;
};

// A pixel-authoritative layer's pixels, and the brush that writes them.
//
// The tools are one set of tools: a stroke drawn in bitmap mode lands here
// instead of in a vector layer, and what it leaves behind is the same shape
// either way. The brush is a capsule per segment - the exact figure
// stroke_mesh.h tessellates - so a line drawn in the two modes looks the
// same, and only what you can do to it afterwards differs.
//
// Coverage is analytic rather than sampled: each pixel's alpha comes from
// its distance to the segment's centerline, which gives round caps, round
// joins and clean anti-aliasing without any of the geometry a triangle
// renderer needs.
//
// A stroke is a session, not a sequence of independent stamps. Compositing
// each segment as it arrives would darken every overlap, and a brush
// overlaps itself heavily at ordinary drawing speeds. Instead a
// stroke accumulates *coverage* into a mask (taking the maximum, so an
// overlap is covered once), and every touched tile is recomposited from the
// pixels it had before the stroke started. Those saved pixels are the same
// ones undo needs, so nothing is stored twice.
class PaintedImage {
public:
    // Square tiles, chosen so a typical brush stroke touches few of them
    // and each is small enough to copy cheaply: 64x64 RGBA is 16 KB.
    static constexpr int kTileSize = 64;

    PaintedImage() = default;
    // Fully transparent, which is what a fresh painted layer is: it has to
    // add nothing to what is underneath until something is drawn on it.
    PaintedImage(int width, int height);
    // Takes ownership of exactly width*height*4 bytes; leaves the image
    // empty if the size doesn't match, rather than reading past the end of
    // whatever was handed in.
    static PaintedImage FromPixels(int width, int height, std::vector<uint8_t> pixelsRGBA);

    int Width() const { return width_; }
    int Height() const { return height_; }
    bool Empty() const { return width_ <= 0 || height_ <= 0; }
    const std::vector<uint8_t>& PixelsRGBA() const { return pixels_; }

    // What a brush does where it lands.
    enum class BrushMode {
        // Lays color down over what is there.
        Paint,
        // Takes alpha away - the pixel eraser, and the reason a painted
        // layer can be erased to precisely the shape of the brush rather
        // than to whichever whole strokes happened to be underneath.
        Erase,
    };

    // Starts a stroke. `colorRGBA` is 0xRRGGBBAA and its alpha is the ink's
    // own strength; `radiusPx` is half the pen width, in this image's own
    // pixels. Any stroke already in progress is finished first.
    void BeginStroke(uint32_t colorRGBA, float radiusPx, BrushMode mode);
    // Extends the stroke from (x0, y0) to (x1, y1), both in image pixels,
    // and returns the region whose pixels changed - what a caller uploads
    // to the GPU. A zero-length segment is a dab, which is how a dot is
    // drawn (see the round-cap geometry).
    PixelRect ExtendStroke(float x0, float y0, float x1, float y1);
    // Adds an axis-aligned rectangle to the stroke instead of a capsule -
    // the rectangular eraser, which is the same gesture with a different
    // shape and so is the same session, the same mask and the same undo
    // tiles. Coverage on the boundary is the pixel's own overlap with the
    // rectangle, so the edges are as clean as the brush's.
    PixelRect ExtendRect(float x0, float y0, float x1, float y1);
    // Ends the stroke and hands back the pixels every touched tile held
    // *before* it started - the undo entry. Empty if nothing was touched.
    std::vector<PaintedTile> EndStroke();
    bool StrokeInProgress() const { return strokeActive_; }

    // Puts saved tiles back, for undo (and, with the tiles this returns,
    // for redo - the swap is the caller's). Returns the region that
    // changed, and the pixels those tiles held before being overwritten.
    std::vector<PaintedTile> RestoreTiles(const std::vector<PaintedTile>& tiles, PixelRect& changed);

    // Row-major tile index, and the pixel rectangle it covers (clipped to
    // the image, so edge tiles are partial).
    int TileIndex(int tileX, int tileY) const { return tileY * TilesAcross() + tileX; }
    int TilesAcross() const { return (width_ + kTileSize - 1) / kTileSize; }
    int TilesDown() const { return (height_ + kTileSize - 1) / kTileSize; }
    PixelRect TileBounds(int index) const;

private:
    // The shared half of every brush shape: walks the tiles `touched`
    // covers, asks `coverage` how much of each pixel the shape wants (0..1,
    // at pixel centers for a capsule, by area for a rectangle), takes the
    // maximum into the stroke's mask, and recomposites. Everything that
    // makes a stroke one stroke - the before-image, the maximum, the undo
    // tiles - lives here, so a new shape is a coverage function and
    // nothing else. Defined in the .cpp; every instantiation is there.
    template <typename CoverageFn>
    PixelRect AccumulateCoverage(const PixelRect& touched, CoverageFn coverage);
    // Saves a tile's current pixels into the stroke's own before-image, the
    // first time the stroke touches it, and gives it a zeroed mask.
    void EnsureTileTracked(int index);
    // Rebuilds one tile's pixels from what it held before the stroke plus
    // the stroke's coverage so far - never from what is on screen now,
    // which is what stops an overlap being painted twice.
    void RecompositeTile(int index, const PixelRect& within);

    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> pixels_;  // width*height*4, RGBA8, row-major

    bool strokeActive_ = false;
    uint32_t strokeColorRGBA_ = 0;
    float strokeRadius_ = 0.0f;
    BrushMode strokeMode_ = BrushMode::Paint;
    // Per touched tile: the pixels it held before this stroke, and the
    // stroke's coverage over it so far (0-255, maximum-combined).
    std::unordered_map<int, std::vector<uint8_t>> strokeBefore_;
    std::unordered_map<int, std::vector<uint8_t>> strokeMask_;
};

}  // namespace sz::core
