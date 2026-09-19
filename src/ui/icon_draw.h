#pragma once

#include <cstdint>

#include <imgui.h>

namespace sz::ui {

// One step of a tiny vector-icon "replay tape", in a fixed 24x24 icon
// space (matching the source SVGs' own viewBox) - see icons_generated.h
// (built by scripts/gen_icons.py from assets/icons/*.svg) for the actual
// per-icon command tables. DrawIcon (icon_draw.cpp) interprets these
// against a real ImDrawList, scaling/centering the 24x24 space into
// whatever pixel rect the icon is being drawn at - true vector rendering
// (ImGui tessellates curves/arcs at whatever size they're actually drawn,
// same as any other ImDrawList shape), not a rasterized icon-font glyph
// baked at one fixed size.
enum class IconOp : uint8_t {
    MoveTo,      // a,b = point. Starts a new open subpath.
    LineTo,      // a,b = point.
    CubicTo,     // a,b = control1, c,d = control2, e,f = end point.
    ArcTo,       // a,b = center, c = radius, d = start angle (rad), e = end angle (rad).
    EndSubpath,  // Flushes the current open subpath via PathStroke - every
                 // MoveTo up to the next EndSubpath is one continuous
                 // stroked polyline/curve; icons with multiple disjoint
                 // subpaths (e.g. two separate <path> elements) need one
                 // EndSubpath between them so they don't get drawn as a
                 // single shape joined by a connecting line.
    Circle,      // a,b = center, c = radius. Standalone (not part of a subpath).
    RoundedRect, // a,b = min, c,d = max, e = corner radius. Standalone.
};

struct IconCmd {
    IconOp op;
    float a, b, c, d, e, f;
};

struct Icon {
    const IconCmd* cmds;
    int count;
};

// Removes points that repeat the one before them (within a hundredth of a
// pixel) from a path about to be stroked. Public for its own test - see the
// definition for what ImGui's stroker does with a repeated point, which is
// not what anyone would guess.
void DropRepeatedPathPoints(ImVector<ImVec2>& path);

// Draws `icon` (authored in a 24x24 coordinate space) stroked in `color`,
// scaled uniformly and centered to fill `size` square pixels with its
// origin at `pos` (top-left) - the same "contain, centered" fit a CSS/SVG
// icon would get. `thickness` scales along with the icon (matching how
// SVG stroke-width scales with its own viewBox-to-viewport transform), so
// pass the thickness appropriate for a 24x24 icon (2.0f matches this
// icon set's own source SVGs) rather than a fixed screen-pixel width.
void DrawIcon(ImDrawList* drawList, const Icon& icon, ImVec2 pos, float size, ImU32 color,
              float thickness = 2.0f);

}  // namespace sz::ui
