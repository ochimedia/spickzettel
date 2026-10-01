#pragma once

// Where a selected snippet's resize band and the selection bar are, in
// pixels, from the rects alone. What Editor::ResolvePointerTarget reads to
// say what is under a point, and what OverlayApp paints the bar to - one
// set of rects for both, so that what is hit is what is drawn. No ImGui:
// the editor asks this between frames.

#include <cstddef>
#include <optional>
#include <vector>

#include "core/canvas/item.h"
#include "core/config/bar_layout.h"
#include "core/session/actions.h"
#include "platform/platform_types.h"

namespace sz::ui {

// What a snippet is resized from - a corner, which moves two edges, or an
// edge - by compass point. Named for the handles it used to be grabbed by.
enum class ResizeHandle { NW, NE, SE, SW, N, S, E, W };

// Half-open on the far edges, the way ImGui's own ImRect::Contains is, so a
// pixel exactly on the seam between two adjacent rects belongs to exactly
// one of them.
struct HitRect {
    platform::Vec2 min;
    platform::Vec2 max;
    bool Contains(float x, float y) const { return x >= min.x && y >= min.y && x < max.x && y < max.y; }
};

// A selected snippet is resized from a band around it, the way a window
// is by its frame, so that all of the snippet itself is for moving it
// whatever its size - see docs/INTERACTIONS.md, 6.5. The band is this
// wide outside each edge, and within kResizeCornerPx of a corner, along
// either edge, it is the corner's - at most a quarter of the side, so a
// small snippet keeps some of each edge.
inline constexpr float kResizeBandPx = 8.0f;
inline constexpr float kResizeCornerPx = 16.0f;

// Which edge or corner of `rect` the band has at (x, y), or nothing - the
// snippet itself, or beyond the band. Worked out on the rect's whole
// pixels, as the pointer is. On a side where the display's edge leaves
// less than the band outside, the band is inside the snippet's visible
// edge there instead, so a snippet against the screen's edge can still
// be resized from that side.
std::optional<ResizeHandle> ResizeBandAt(const core::Rect& rect, float x, float y, float displayW, float displayH);
// The band's corner at the lower right, which the tutorial rings.
HitRect ResizeCornerRect(const core::Rect& rect);
// The compass name the debug overlay prints for an edge or corner.
const char* ResizeHandleName(ResizeHandle handle);
// Which edges a resize handle moves - a corner two, an edge one.
void ResizeHandleEdges(ResizeHandle handle, bool& left, bool& right, bool& top, bool& bottom);

// The selection bar: [Pen][Eraser][Text][Color] | [Pin][More][Minimize]
// [Maximize/Restore][Close] - buttons of this size, this far apart, the two
// groups (see IsDrawingBarButton) this far apart with a divider between
// them, on a pill this much bigger than them, floating just above the
// selection's bounding box - or below it when there is no room above, or
// inside its top edge when there is no room either way (a fullscreen
// snippet). Centered on the box and kept on screen.
inline constexpr float kBarButtonSize = 28.0f;
inline constexpr float kBarButtonGap = 2.0f;
inline constexpr float kBarGroupGap = 11.0f;
inline constexpr float kBarPad = 6.0f;
inline constexpr float kBarGapPx = 8.0f;  // between the box and the bar
inline constexpr float kBarHeight = kBarButtonSize + 2.0f * kBarPad;

struct BarLayout {
    platform::Vec2 min;
    platform::Vec2 max;
};
BarLayout LayoutBar(const core::Rect& bounds, float displayW, float displayH,
                    const std::vector<core::ChromeButton>& buttons);
HitRect BarButtonRect(const BarLayout& bar, const std::vector<core::ChromeButton>& buttons,
                      core::ChromeButton button);
// The divider between the two groups, if both are on the bar: a line
// `width` wide, centered on the middle of the gap between them - with its
// edges on whole pixels where its width allows, the nearest that is to
// the middle, so it is drawn sharp. Where its middle is.
std::optional<float> BarDividerX(const BarLayout& bar, const std::vector<core::ChromeButton>& buttons,
                                 float width);

}  // namespace sz::ui
