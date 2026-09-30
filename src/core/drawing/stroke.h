#pragma once

#include <cstdint>
#include <vector>

namespace sz::core {

struct StrokePoint {
    float x = 0.0f;
    float y = 0.0f;
    // Exact equality, not tolerance-based: the eraser's undo recognizes an
    // untouched stroke, and a fragment it added, by byte-identical content
    // rather than by a per-stroke id. Two different strokes with identical
    // points, color and width are indistinguishable to this comparison and
    // to the eye, so undo picking the "wrong" one of such a pair is
    // harmless.
    bool operator==(const StrokePoint&) const = default;
};

// What a stroke's turns look like. A round pen leaves a round corner at
// every turn, and a freehand stroke is drawn that way - but a rectangle
// drawn with the shape tool keeps its corners square, as a rectangle is
// expected to look. The mesh sees only points and cannot tell the two
// apart, so the stroke says. See BuildStrokeMesh.
enum class StrokeCorners : uint8_t {
    Round,
    // Mitered up to a turn of about 120 degrees, round past it.
    Sharp,
};

struct Stroke {
    std::vector<StrokePoint> points;
    // Packed 0xRRGGBBAA - the config file's "#RRGGBB" with alpha appended.
    // Converted to the renderer's packing only at the point of drawing.
    uint32_t colorRGBA = 0xFF0000FF;  // opaque red
    float width = 3.0f;
    StrokeCorners corners = StrokeCorners::Round;
    bool operator==(const Stroke&) const = default;
};

}  // namespace sz::core
