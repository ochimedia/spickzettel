#pragma once

#include <cstdint>
#include <vector>

namespace sz::core {

struct StrokePoint {
    float x = 0.0f;
    float y = 0.0f;
    // Exact equality, not tolerance-based: the eraser's undo recognises an
    // untouched stroke, and a fragment it added, by byte-identical content
    // rather than by a per-stroke id. Two different strokes with identical
    // points, colour and width are indistinguishable to this comparison and
    // to the eye, so undo picking the "wrong" one of such a pair is
    // harmless.
    bool operator==(const StrokePoint&) const = default;
};

struct Stroke {
    std::vector<StrokePoint> points;
    // Packed 0xRRGGBBAA - the config file's "#RRGGBB" with alpha appended.
    // Converted to the renderer's packing only at the point of drawing.
    uint32_t colorRGBA = 0xFF0000FF;  // opaque red
    float width = 3.0f;
    bool operator==(const Stroke&) const = default;
};

}  // namespace sz::core
