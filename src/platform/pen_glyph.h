#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>

#include "platform/platform_types.h"

// The pen pointer, as the one shape both pointers draw from.
//
// Neither ImGui's cursor set nor Windows' stock cursors have a pen, so the
// overlay draws its own - twice: ui::OverlayApp fills these polygons into
// ImGui's draw list for the software pointer, and Win32OverlayWindow
// rasterizes them into the bitmap it hands the OS when the real cursor is
// used. One definition, so the two pens are the same pen.
namespace sz::platform::pen_glyph {

// The outline as two convex pieces: a nib triangle and the body behind it,
// in pixels relative to the nib tip, +x right and +y down. The body runs up
// and to the right, so the nib marks exactly the pixel drawn on. Two
// polygons rather than one outline because the seam between them is
// stroked too and reads as the ferrule.
inline constexpr Vec2 kNib[3] = {{0.0f, 0.0f}, {6.0f, -2.0f}, {2.0f, -6.0f}};
inline constexpr Vec2 kBody[4] = {{6.0f, -2.0f}, {18.0f, -14.0f}, {14.0f, -18.0f}, {2.0f, -6.0f}};
// Wide enough to read, narrow enough that the white body survives it.
inline constexpr float kOutlineWidth = 1.4f;

// What the glyph shows at one point: its dark outline, its white body, or
// nothing.
enum class Ink { None, Edge, Fill };

inline float DistanceToSegment(float px, float py, Vec2 a, Vec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSquared = dx * dx + dy * dy;
    float t = 0.0f;
    if (lengthSquared > 0.0f) {
        t = std::clamp(((px - a.x) * dx + (py - a.y) * dy) / lengthSquared, 0.0f, 1.0f);
    }
    const float cx = a.x + t * dx;
    const float cy = a.y + t * dy;
    return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

// Both pieces are convex, so "same side of every edge" is the whole test,
// whichever way they wind.
inline bool InsideConvex(const Vec2* points, size_t count, float px, float py) {
    bool anyPositive = false;
    bool anyNegative = false;
    for (size_t i = 0; i < count; ++i) {
        const Vec2& a = points[i];
        const Vec2& b = points[(i + 1) % count];
        const float cross = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
        anyPositive = anyPositive || cross > 0.0f;
        anyNegative = anyNegative || cross < 0.0f;
    }
    return !(anyPositive && anyNegative);
}

// Outline first, so a stroke centered on an edge covers the body either
// side of it exactly as the drawn pen's does (which fills, then strokes).
inline Ink InkAt(float x, float y) {
    constexpr float half = kOutlineWidth * 0.5f;
    const auto onOutline = [x, y](const Vec2* points, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (DistanceToSegment(x, y, points[i], points[(i + 1) % count]) <= half) {
                return true;
            }
        }
        return false;
    };
    if (onOutline(kNib, std::size(kNib)) || onOutline(kBody, std::size(kBody))) {
        return Ink::Edge;
    }
    if (InsideConvex(kNib, std::size(kNib), x, y) || InsideConvex(kBody, std::size(kBody), x, y)) {
        return Ink::Fill;
    }
    return Ink::None;
}

}  // namespace sz::platform::pen_glyph
