#pragma once

namespace sz::core {

// How a vector stroke is turned into pixels on screen. Purely a rendering
// choice: the stroke itself is the same data in all three, and switching
// applies to strokes already drawn as well as new ones.
enum class StrokeRenderMode {
    // This app's own tessellation (see core/drawing/stroke_mesh.h): a
    // miter that stays inside a sane limit however sharp the turn, round
    // joins past it, round caps, and one connected mesh whose triangles
    // never overlap.
    Tessellated,
    // ImGui's AddPolyline plus a disc at each end, so the ends are round
    // either way and caps are not what is being compared. Kept so the
    // tessellation can be judged against something.
    Polyline,
    // Drawn into a bitmap and composited once (see
    // core/drawing/painted_image.h). The only one
    // of the three where a stroke that crosses over *itself* doesn't
    // darken at the crossing: coverage is accumulated per stroke and the
    // color applied once, which no triangle renderer can do. The cost is
    // that the result is pixels at the item's native size, so scaling the
    // item up softens it - which is precisely the trade the other two
    // don't make.
    Rasterized,
};

}  // namespace sz::core
