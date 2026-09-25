#include "core/drawing/stroke_bitmap.h"

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

StrokeBitmap::StrokeBitmap(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    width_ = width;
    height_ = height;
    pixels_.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
}

PixelRect StrokeBitmap::TileBounds(int index) const {
    if (Empty() || index < 0 || index >= TilesAcross() * TilesDown()) {
        return PixelRect{};
    }
    const int tileX = index % TilesAcross();
    const int tileY = index / TilesAcross();
    const int x = tileX * kTileSize;
    const int y = tileY * kTileSize;
    return PixelRect{x, y, std::min(kTileSize, width_ - x), std::min(kTileSize, height_ - y)};
}

void StrokeBitmap::BeginStroke(uint32_t colorRGBA, float radiusPx) {
    if (strokeActive_) {
        EndStroke();
    }
    strokeActive_ = true;
    strokeColorRGBA_ = colorRGBA;
    strokeRadius_ = std::max(radiusPx, 0.05f);
    strokeBefore_.clear();
    strokeMask_.clear();
}

void StrokeBitmap::EnsureTileTracked(int index) {
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

bool StrokeBitmap::RecompositeTile(int index, const PixelRect& within) {
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

PixelRect StrokeBitmap::ExtendStroke(float x0, float y0, float x1, float y1) {
    if (!strokeActive_ || Empty()) {
        return PixelRect{};
    }
    // The capsule's bounding box, grown by one pixel so the anti-aliased
    // edge - which reaches half a pixel past the radius - is inside it, and
    // clipped to the bitmap.
    const int left = std::clamp(static_cast<int>(std::floor(std::min(x0, x1) - strokeRadius_ - 1.0f)), 0, width_);
    const int top = std::clamp(static_cast<int>(std::floor(std::min(y0, y1) - strokeRadius_ - 1.0f)), 0, height_);
    const int right =
        std::clamp(static_cast<int>(std::ceil(std::max(x0, x1) + strokeRadius_ + 1.0f)) + 1, 0, width_);
    const int bottom =
        std::clamp(static_cast<int>(std::ceil(std::max(y0, y1) + strokeRadius_ + 1.0f)) + 1, 0, height_);
    if (right <= left || bottom <= top) {
        return PixelRect{};
    }

    const float radius = strokeRadius_;
    bool changed = false;
    for (int tileY = top / kTileSize; tileY <= (bottom - 1) / kTileSize; ++tileY) {
        for (int tileX = left / kTileSize; tileX <= (right - 1) / kTileSize; ++tileX) {
            const int index = TileIndex(tileX, tileY);
            const PixelRect bounds = TileBounds(index);
            // A tile is out of reach when even its center is farther from
            // the segment than the brush reaches plus half the tile's
            // diagonal. Asked once per tile: a corner-to-corner line over a
            // fullscreen bitmap crosses a sliver of the tiles in its
            // bounding box, and copying and walking all of them was 8 MB at
            // 1080p, 33 MB at 4K.
            const float halfW = static_cast<float>(bounds.w) * 0.5f;
            const float halfH = static_cast<float>(bounds.h) * 0.5f;
            const float centerDistance = DistanceToSegment(static_cast<float>(bounds.x) + halfW,
                                                           static_cast<float>(bounds.y) + halfH, x0, y0, x1, y1);
            if (centerDistance > radius + 1.0f + std::sqrt(halfW * halfW + halfH * halfH)) {
                continue;
            }
            EnsureTileTracked(index);
            std::vector<uint8_t>& mask = strokeMask_[index];

            const int px0 = std::max(bounds.x, left);
            const int py0 = std::max(bounds.y, top);
            const int px1 = std::min(bounds.x + bounds.w, right);
            const int py1 = std::min(bounds.y + bounds.h, bottom);
            for (int y = py0; y < py1; ++y) {
                for (int x = px0; x < px1; ++x) {
                    // Pixel centers, so a stroke down a pixel's middle is
                    // symmetric rather than half a pixel off.
                    const float distance = DistanceToSegment(static_cast<float>(x) + 0.5f,
                                                             static_cast<float>(y) + 0.5f, x0, y0, x1, y1);
                    // Coverage falls from 1 to 0 across the last pixel of
                    // the radius - a box filter, which for a shape this
                    // smooth is indistinguishable from anything cleverer.
                    const float amount = std::clamp(radius + 0.5f - distance, 0.0f, 1.0f);
                    if (amount <= 0.0f) {
                        continue;
                    }
                    const size_t local = static_cast<size_t>(y - bounds.y) * bounds.w + (x - bounds.x);
                    // Maximum, not sum: where this segment overlaps one the
                    // stroke already laid down, the ink is not doubled.
                    mask[local] = std::max(mask[local], ToByte(amount));
                }
            }
            changed = RecompositeTile(index, PixelRect{px0, py0, px1 - px0, py1 - py0}) || changed;
        }
    }
    return changed ? PixelRect{left, top, right - left, bottom - top} : PixelRect{};
}

void StrokeBitmap::EndStroke() {
    strokeBefore_.clear();
    strokeMask_.clear();
    strokeActive_ = false;
}

}  // namespace sz::core
