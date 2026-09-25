#include "core/drawing/painted_image.h"

#include <algorithm>
#include <cmath>

namespace sz::core {

namespace {

// Distance from (px, py) to the segment (x0,y0)-(x1,y1). A zero-length
// segment collapses to the distance from a point, which is what makes a
// dab - and therefore a dot - fall out of the same code as a line.
float DistanceToSegment(float px, float py, float x0, float y0, float x1, float y1) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float lengthSq = dx * dx + dy * dy;
    float t = 0.0f;
    if (lengthSq > 1e-9f) {
        t = std::clamp(((px - x0) * dx + (py - y0) * dy) / lengthSq, 0.0f, 1.0f);
    }
    const float cx = x0 + dx * t;
    const float cy = y0 + dy * t;
    return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

uint8_t ToByte(float value01) {
    return static_cast<uint8_t>(std::clamp(value01, 0.0f, 1.0f) * 255.0f + 0.5f);
}

}  // namespace

float FitResolutionScale(float nativeW, float nativeH, float requested, int maxExtent) {
    if (requested <= 0.0f || maxExtent <= 0) {
        return requested;
    }
    const float longest = std::max(nativeW, nativeH) * requested;
    if (longest <= static_cast<float>(maxExtent)) {
        return requested;
    }
    return requested * (static_cast<float>(maxExtent) / longest);
}

int ScaledPixelExtent(float native, float scale) {
    return std::max(1, static_cast<int>(std::lround(native * scale)));
}

PaintedImage::PaintedImage(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    width_ = width;
    height_ = height;
    pixels_.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
}

PaintedImage PaintedImage::FromPixels(int width, int height, std::vector<uint8_t> pixelsRGBA) {
    PaintedImage image;
    if (width <= 0 || height <= 0 ||
        pixelsRGBA.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
        return image;
    }
    image.width_ = width;
    image.height_ = height;
    image.pixels_ = std::move(pixelsRGBA);
    return image;
}

PixelRect PaintedImage::TileBounds(int index) const {
    if (Empty() || index < 0 || index >= TilesAcross() * TilesDown()) {
        return PixelRect{};
    }
    const int tileX = index % TilesAcross();
    const int tileY = index / TilesAcross();
    const int x = tileX * kTileSize;
    const int y = tileY * kTileSize;
    return PixelRect{x, y, std::min(kTileSize, width_ - x), std::min(kTileSize, height_ - y)};
}

void PaintedImage::BeginStroke(uint32_t colorRGBA, float radiusPx, BrushMode mode) {
    if (strokeActive_) {
        EndStroke();
    }
    strokeActive_ = true;
    strokeColorRGBA_ = colorRGBA;
    strokeRadius_ = std::max(radiusPx, 0.05f);
    strokeMode_ = mode;
    strokeBefore_.clear();
    strokeMask_.clear();
}

void PaintedImage::EnsureTileTracked(int index) {
    if (strokeBefore_.find(index) != strokeBefore_.end()) {
        return;
    }
    const PixelRect bounds = TileBounds(index);
    std::vector<uint8_t> before(static_cast<size_t>(bounds.w) * static_cast<size_t>(bounds.h) * 4);
    for (int row = 0; row < bounds.h; ++row) {
        const size_t src = (static_cast<size_t>(bounds.y + row) * width_ + bounds.x) * 4;
        std::copy_n(pixels_.begin() + static_cast<std::ptrdiff_t>(src), static_cast<size_t>(bounds.w) * 4,
                    before.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(row) * bounds.w * 4));
    }
    strokeBefore_.emplace(index, std::move(before));
    strokeMask_.emplace(index, std::vector<uint8_t>(static_cast<size_t>(bounds.w) * bounds.h, 0));
}

bool PaintedImage::RecompositeTile(int index, const PixelRect& within) {
    const PixelRect bounds = TileBounds(index);
    const std::vector<uint8_t>& before = strokeBefore_[index];
    const std::vector<uint8_t>& mask = strokeMask_[index];

    const int x0 = std::max(bounds.x, within.x);
    const int y0 = std::max(bounds.y, within.y);
    const int x1 = std::min(bounds.x + bounds.w, within.x + within.w);
    const int y1 = std::min(bounds.y + bounds.h, within.y + within.h);

    const float inkR = static_cast<float>((strokeColorRGBA_ >> 24) & 0xFF);
    const float inkG = static_cast<float>((strokeColorRGBA_ >> 16) & 0xFF);
    const float inkB = static_cast<float>((strokeColorRGBA_ >> 8) & 0xFF);
    const float inkA = static_cast<float>(strokeColorRGBA_ & 0xFF) / 255.0f;

    bool changed = false;
    const auto put = [&](size_t dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        changed = changed || pixels_[dst + 0] != r || pixels_[dst + 1] != g || pixels_[dst + 2] != b ||
                  pixels_[dst + 3] != a;
        pixels_[dst + 0] = r;
        pixels_[dst + 1] = g;
        pixels_[dst + 2] = b;
        pixels_[dst + 3] = a;
    };
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t local = static_cast<size_t>(y - bounds.y) * bounds.w + (x - bounds.x);
            const float coverage = static_cast<float>(mask[local]) / 255.0f;
            const size_t dst = (static_cast<size_t>(y) * width_ + x) * 4;
            if (coverage <= 0.0f) {
                put(dst, before[local * 4 + 0], before[local * 4 + 1], before[local * 4 + 2], before[local * 4 + 3]);
                continue;
            }
            const float dr = static_cast<float>(before[local * 4 + 0]);
            const float dg = static_cast<float>(before[local * 4 + 1]);
            const float db = static_cast<float>(before[local * 4 + 2]);
            const float da = static_cast<float>(before[local * 4 + 3]) / 255.0f;

            if (strokeMode_ == BrushMode::Erase) {
                // Takes alpha away and leaves the color alone: the pixel is
                // the same ink, just less of it, so a half-erased edge
                // fades rather than shifting hue toward black.
                const float outA = da * (1.0f - coverage);
                put(dst, static_cast<uint8_t>(dr), static_cast<uint8_t>(dg), static_cast<uint8_t>(db), ToByte(outA));
                continue;
            }

            // Source-over, un-premultiplied: the ink's own alpha times how
            // much of this pixel the brush covers.
            const float srcA = inkA * coverage;
            const float outA = srcA + da * (1.0f - srcA);
            if (outA <= 0.0f) {
                put(dst, 0, 0, 0, 0);
                continue;
            }
            put(dst, static_cast<uint8_t>((inkR * srcA + dr * da * (1.0f - srcA)) / outA + 0.5f),
                static_cast<uint8_t>((inkG * srcA + dg * da * (1.0f - srcA)) / outA + 0.5f),
                static_cast<uint8_t>((inkB * srcA + db * da * (1.0f - srcA)) / outA + 0.5f), ToByte(outA));
        }
    }
    return changed;
}

template <typename TileFn, typename CoverageFn>
PixelRect PaintedImage::AccumulateCoverage(const PixelRect& touchedIn, TileFn mayCover, CoverageFn coverage) {
    lastChanged_.clear();
    PixelRect touched = touchedIn;
    touched.x = std::clamp(touched.x, 0, width_);
    touched.y = std::clamp(touched.y, 0, height_);
    const int right = std::clamp(touchedIn.x + touchedIn.w, 0, width_);
    const int bottom = std::clamp(touchedIn.y + touchedIn.h, 0, height_);
    touched.w = right - touched.x;
    touched.h = bottom - touched.y;
    if (touched.Empty()) {
        return PixelRect{};
    }

    bool changed = false;
    const int firstTileX = touched.x / kTileSize;
    const int lastTileX = (right - 1) / kTileSize;
    const int firstTileY = touched.y / kTileSize;
    const int lastTileY = (bottom - 1) / kTileSize;

    for (int tileY = firstTileY; tileY <= lastTileY; ++tileY) {
        for (int tileX = firstTileX; tileX <= lastTileX; ++tileX) {
            const int index = TileIndex(tileX, tileY);
            const PixelRect bounds = TileBounds(index);
            if (!mayCover(bounds)) {
                continue;
            }
            EnsureTileTracked(index);
            std::vector<uint8_t>& mask = strokeMask_[index];

            const int px0 = std::max(bounds.x, touched.x);
            const int py0 = std::max(bounds.y, touched.y);
            const int px1 = std::min(bounds.x + bounds.w, right);
            const int py1 = std::min(bounds.y + bounds.h, bottom);
            for (int y = py0; y < py1; ++y) {
                for (int x = px0; x < px1; ++x) {
                    const float amount = coverage(x, y);
                    if (amount <= 0.0f) {
                        continue;
                    }
                    const size_t local = static_cast<size_t>(y - bounds.y) * bounds.w + (x - bounds.x);
                    // Maximum, not sum: where this piece of the gesture
                    // overlaps one it already laid down, the ink is not
                    // doubled.
                    mask[local] = std::max(mask[local], ToByte(amount));
                }
            }
            const PixelRect within{px0, py0, px1 - px0, py1 - py0};
            if (RecompositeTile(index, within)) {
                changed = true;
                // Joined to the last if it is the tile before it in the
                // same row and spans the same rows - tiles are walked row
                // by row, so a horizontal run comes out as one rectangle.
                PixelRect* last = lastChanged_.empty() ? nullptr : &lastChanged_.back();
                if (last != nullptr && last->y == within.y && last->h == within.h && last->x + last->w == within.x) {
                    last->w += within.w;
                } else {
                    lastChanged_.push_back(within);
                }
            }
        }
    }
    // Nothing to upload when nothing changed - and so, for the caller,
    // nothing touched: no undo entry, and no layer to write out again.
    return changed ? touched : PixelRect{};
}

PixelRect PaintedImage::ExtendStroke(float x0, float y0, float x1, float y1) {
    if (!strokeActive_ || Empty()) {
        return PixelRect{};
    }
    // The capsule's bounding box, grown by one pixel so the anti-aliased
    // edge - which reaches half a pixel past the radius - is inside it.
    const int left = static_cast<int>(std::floor(std::min(x0, x1) - strokeRadius_ - 1.0f));
    const int top = static_cast<int>(std::floor(std::min(y0, y1) - strokeRadius_ - 1.0f));
    const int right = static_cast<int>(std::ceil(std::max(x0, x1) + strokeRadius_ + 1.0f)) + 1;
    const int bottom = static_cast<int>(std::ceil(std::max(y0, y1) + strokeRadius_ + 1.0f)) + 1;

    const float radius = strokeRadius_;
    // A tile is out of reach when even its center is farther from the
    // segment than the brush reaches plus half the tile's diagonal. Asked
    // once per tile, where a corner-to-corner line over a fullscreen layer
    // used to save every tile of it for undo - 8 MB at 1080p, 33 MB at 4K -
    // and evaluate every pixel.
    const auto mayCover = [&](const PixelRect& tile) {
        const float halfW = static_cast<float>(tile.w) * 0.5f;
        const float halfH = static_cast<float>(tile.h) * 0.5f;
        const float centerDistance = DistanceToSegment(static_cast<float>(tile.x) + halfW,
                                                       static_cast<float>(tile.y) + halfH, x0, y0, x1, y1);
        return centerDistance <= radius + 1.0f + std::sqrt(halfW * halfW + halfH * halfH);
    };
    return AccumulateCoverage(PixelRect{left, top, right - left, bottom - top}, mayCover, [&](int x, int y) {
        // Pixel centers, so a stroke down a pixel's middle is symmetric
        // rather than half a pixel off.
        const float distance =
            DistanceToSegment(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, x0, y0, x1, y1);
        // Coverage falls from 1 to 0 across the last pixel of the radius -
        // a box filter, which for a shape this smooth is indistinguishable
        // from anything cleverer.
        return std::clamp(radius + 0.5f - distance, 0.0f, 1.0f);
    });
}

PixelRect PaintedImage::ExtendRect(float x0, float y0, float x1, float y1) {
    if (!strokeActive_ || Empty()) {
        return PixelRect{};
    }
    const float minX = std::min(x0, x1);
    const float maxX = std::max(x0, x1);
    const float minY = std::min(y0, y1);
    const float maxY = std::max(y0, y1);

    const int left = static_cast<int>(std::floor(minX));
    const int top = static_cast<int>(std::floor(minY));
    const int right = static_cast<int>(std::ceil(maxX)) + 1;
    const int bottom = static_cast<int>(std::ceil(maxY)) + 1;

    // The rectangle covers all of its bounding box, so every tile in it.
    const auto mayCover = [](const PixelRect&) { return true; };
    return AccumulateCoverage(PixelRect{left, top, right - left, bottom - top}, mayCover, [&](int x, int y) {
        // How much of this pixel's own square the rectangle covers - 1
        // inside, a fraction on the boundary, so an edge that falls between
        // pixels doesn't come out jagged.
        const float overlapX = std::min(maxX, static_cast<float>(x) + 1.0f) - std::max(minX, static_cast<float>(x));
        const float overlapY = std::min(maxY, static_cast<float>(y) + 1.0f) - std::max(minY, static_cast<float>(y));
        return std::clamp(overlapX, 0.0f, 1.0f) * std::clamp(overlapY, 0.0f, 1.0f);
    });
}

std::vector<PaintedTile> PaintedImage::EndStroke() {
    std::vector<PaintedTile> before;
    before.reserve(strokeBefore_.size());
    for (auto& [index, pixels] : strokeBefore_) {
        // A tile the stroke reached and left as it was - the eraser over
        // its transparent part, say - is nothing to take back.
        const PixelRect bounds = TileBounds(index);
        bool same = true;
        for (int row = 0; row < bounds.h && same; ++row) {
            const size_t now = (static_cast<size_t>(bounds.y + row) * width_ + bounds.x) * 4;
            same = std::equal(pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(row) * bounds.w * 4),
                              pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(row + 1) * bounds.w * 4),
                              pixels_.begin() + static_cast<std::ptrdiff_t>(now));
        }
        if (!same) {
            before.push_back(PaintedTile{index, std::move(pixels)});
        }
    }
    // Sorted so an undo entry is deterministic - two identical strokes
    // produce identical entries, which is what makes them comparable in a
    // test rather than only in a debugger.
    std::sort(before.begin(), before.end(),
              [](const PaintedTile& a, const PaintedTile& b) { return a.index < b.index; });
    strokeBefore_.clear();
    strokeMask_.clear();
    strokeActive_ = false;
    return before;
}

std::vector<PaintedTile> PaintedImage::RestoreTiles(const std::vector<PaintedTile>& tiles, PixelRect& changed) {
    changed = PixelRect{};
    std::vector<PaintedTile> replaced;
    replaced.reserve(tiles.size());
    int minX = width_;
    int minY = height_;
    int maxX = 0;
    int maxY = 0;

    for (const PaintedTile& tile : tiles) {
        const PixelRect bounds = TileBounds(tile.index);
        if (bounds.Empty() ||
            tile.pixelsRGBA.size() != static_cast<size_t>(bounds.w) * static_cast<size_t>(bounds.h) * 4) {
            continue;  // not a tile of this image - ignore rather than corrupt it
        }
        PaintedTile previous{tile.index, std::vector<uint8_t>(tile.pixelsRGBA.size())};
        for (int row = 0; row < bounds.h; ++row) {
            const size_t dst = (static_cast<size_t>(bounds.y + row) * width_ + bounds.x) * 4;
            const size_t src = static_cast<size_t>(row) * bounds.w * 4;
            std::copy_n(pixels_.begin() + static_cast<std::ptrdiff_t>(dst), static_cast<size_t>(bounds.w) * 4,
                        previous.pixelsRGBA.begin() + static_cast<std::ptrdiff_t>(src));
            std::copy_n(tile.pixelsRGBA.begin() + static_cast<std::ptrdiff_t>(src), static_cast<size_t>(bounds.w) * 4,
                        pixels_.begin() + static_cast<std::ptrdiff_t>(dst));
        }
        replaced.push_back(std::move(previous));
        minX = std::min(minX, bounds.x);
        minY = std::min(minY, bounds.y);
        maxX = std::max(maxX, bounds.x + bounds.w);
        maxY = std::max(maxY, bounds.y + bounds.h);
    }
    if (!replaced.empty()) {
        changed = PixelRect{minX, minY, maxX - minX, maxY - minY};
    }
    return replaced;
}

}  // namespace sz::core
