#pragma once

// Where a selected snippet's handles and the selection bar are, in pixels,
// from the rects alone. What Editor::ResolvePointerTarget reads to say what
// is under a point, and what OverlayApp paints the outline, the handles and
// the bar to - one set of rects for both, so that what is hit is what is
// drawn. No ImGui: the editor asks this between frames.

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "core/canvas/item.h"
#include "core/config/bar_layout.h"
#include "core/session/actions.h"
#include "platform/platform_types.h"

namespace sz::ui {

// One of a selected snippet's eight resize handles, by compass point.
enum class ResizeHandle { NW, NE, SE, SW, N, S, E, W };

// Half-open on the far edges, the way ImGui's own ImRect::Contains is, so a
// pixel exactly on the seam between two adjacent rects belongs to exactly
// one of them.
struct HitRect {
    platform::Vec2 min;
    platform::Vec2 max;
    bool Contains(float x, float y) const { return x >= min.x && y >= min.y && x < max.x && y < max.y; }
};

// A handle is a small square centered *on* the border - a corner or the
// middle of an edge - the way a drawing program draws them. It covers a
// few of the snippet's own pixels, which is fine: handles show only while
// the selection is live, when nothing can be drawn anyway. Its hit rect
// reaches a little past what is drawn, so it needn't be hit dead on.
inline constexpr float kHandleSizePx = 8.0f;
inline constexpr float kHandleHitSlopPx = 3.0f;

// One of the 8 handles: where its center is, and its compass name as the
// debug overlay prints it.
struct HandleSpec {
    ResizeHandle handle;
    const char* name;
    platform::Vec2 center;
};

// The 8 handles of a rect, corners first, at whole pixels: every one is
// tested against a whole-pixel pointer position, and a fractional edge
// left a sub-pixel column that belonged to nothing.
std::array<HandleSpec, 8> HandleSpecs(const core::Rect& r);
HitRect HandleDrawRect(platform::Vec2 center);
HitRect HandleHitRect(platform::Vec2 center);
// The compass name the debug overlay prints for a handle.
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
// Where the divider between the two groups stands, if both are on the bar.
std::optional<float> BarDividerX(const BarLayout& bar, const std::vector<core::ChromeButton>& buttons);

}  // namespace sz::ui
