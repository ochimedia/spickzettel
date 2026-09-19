#pragma once

#include "core/canvas/item.h"

namespace sz::core {

// px, resize floor - an item can never shrink smaller than this on either
// axis, whether via an interactive resize-handle drag or a
// display-resolution sync (see CanvasManager::SyncItemsToDisplaySize).
//
// Note these two are a *shape* (9:7), not just two numbers, and anywhere
// the item's aspect ratio is meant to be preserved they have to be
// applied as one - see MinimumSizeForAspectRatio below for why applying
// them independently silently reshapes the item.
constexpr float kItemMinWidth = 90.0f;
constexpr float kItemMinHeight = 70.0f;

struct MinItemSize {
    float w = kItemMinWidth;
    float h = kItemMinHeight;

    bool operator==(const MinItemSize&) const = default;
};

// The smallest size an item of aspect ratio `ratio` (width/height) can
// take while clearing *both* kItemMinWidth and kItemMinHeight without
// being reshaped: whichever floor it reaches first becomes the binding
// one, and the other axis is derived from the ratio rather than clamped
// on its own.
//
// Applying the two floors independently silently reshapes the item: a
// 16:9 item reaches the 70px height floor while still 124px wide, height
// then stops while width goes on shrinking to 90, and it ends up 9:7.
// Every item, of whatever shape, would eventually become exactly 90x70.
// Reaching one floor has to stop the whole resize, not just one axis.
//
// Falls back to the plain kItemMinWidth x kItemMinHeight for a
// non-positive `ratio` - a degenerate rect has no shape to preserve, and
// nothing safe to divide by.
MinItemSize MinimumSizeForAspectRatio(float ratio);

// Grows `rect` to MinimumSizeForAspectRatio(rect.w / rect.h) if it's
// under it, keeping its aspect ratio and its top-left corner where they
// are - the rect-shaped counterpart of the above, for callers that
// already have a correctly-proportioned rect and only need it floored
// (CanvasManager::SyncItemsToDisplaySize, whose own
// RescaleRectForDisplaySize deliberately scales both axes by one uniform
// factor precisely so the ratio survives). Returns `rect` untouched if it
// already clears the floor.
Rect GrowRectToMinimumSize(Rect rect);

// px of an item that must stay within the viewport on each axis,
// independently, no matter how far a move/resize/rescale pushes the rest
// of it past an edge - see ClampRectToViewport.
constexpr float kGrabMarginPx = 48.0f;

// A snippet can never fully escape the viewport: at least kGrabMarginPx
// of `rect` stays on screen on each axis, independently. Symmetric across
// all four edges - pushing most of a snippet past any edge on purpose is
// allowed, it just can never vanish, and since this runs on the *result*
// of every move and resize, shrinking an already off-screen snippet can't
// push the remainder out either.
//
// Shared by the UI's interactive move/resize and by
// CanvasManager::SyncItemsToDisplaySize: a display change can move and
// shrink an item as drastically as a drag can (a 4K library reopened on a
// laptop), and deserves the same guarantee rather than a second copy of it.
Rect ClampRectToViewport(Rect rect, float displayW, float displayH);

// Moves `rect` from one reference display size to another: position
// scales per-axis independently (`x * (toW/fromW)`, `y * (toH/fromH)`) so
// an item anchored near an edge or corner stays anchored there
// proportionally even when the two axes change by very different
// amounts, while size scales by one *uniform* factor for both dimensions
// - `std::min(scaleX, scaleY)` - so an item's own aspect ratio is never
// distorted into whatever shape the display change happens to imply.
// Pure and stateless: always computed fresh from the given `rect`,
// `fromW`, `fromH`, never chained onto a previous call's own result, so
// calling this back and forth between the same two sizes is always
// exactly reversible - no compounding drift from one call feeding the
// next, unlike naively re-scaling an already-scaled rect. Returns `rect`
// unchanged whenever `fromW == toW && fromH == toH` (exact identity, not
// just numerically close), and unchanged for a non-positive `fromW`/
// `fromH` (nothing sane to scale from). Doesn't floor at
// kItemMinWidth/Height or clamp to the viewport itself - callers combine
// this with those the same way an interactive resize already does (see
// CanvasManager::SyncItemsToDisplaySize).
Rect RescaleRectForDisplaySize(Rect rect, float fromW, float fromH, float toW, float toH);

// Fits a rect of the given `aspectRatio` (width/height) into a
// `viewportW`x`viewportH` viewport, centered: full width with the height
// scaled down (letterboxed) if the ratio is wider than the viewport's,
// full height with the width scaled down (pillarboxed) if taller. Shared
// by entering fit-mode fullscreen and by the display sync keeping such an
// item fitted. Undefined for a non-positive `viewportH`.
Rect FitAspectRatioIntoViewport(float aspectRatio, float viewportW, float viewportH);

// Whether two rects share any area at all - the "does this snippet touch
// that one" test, used wherever one snippet's place on screen has to be
// read against another's: which snippet a z-order step has to pass (see
// CanvasManager::MoveItemLayer) and which snippets a box drawn over the
// canvas has caught (see OverlayApp::HandleBoxSelection). Touching edges
// do not count: two rects laid side by side, one's right edge exactly on
// the other's left, hide nothing of each other and catch nothing of a box
// drawn along the seam, and neither does a rect with no area at all.
bool RectsOverlap(const Rect& a, const Rect& b);

// A screen-space point carried into an item's native space (see
// Item::nativeW/nativeH), plus the factor a length travels by on the way -
// the average of the two axes' scales, which is what a stroke width or a
// brush radius uses, since neither has an axis of its own.
struct NativePoint {
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.0f;
};

// The one screen-to-native transform, for everything that lands a gesture
// on an item: the pen baking a stroke, the erasers clipping strokes, and
// the brush finding its pixel. One function so the three cannot disagree
// about where the pen is. Pure: depends only on the item's rect and
// native size.
NativePoint ScreenToNative(const Item& item, float screenX, float screenY);

// Applies one resize handle's drag delta to `rect`. A corner handle moves
// two edges, an edge handle one; the opposite edge stays put. `dx`/`dy` is
// the full delta since the gesture began, applied to a copy of the rect as
// it was then, so this is stateless per call.
//
// With `lockAspect` the item keeps its shape: the two per-axis floors
// become one floor on the item's own ratio (MinimumSizeForAspectRatio),
// and the axis not under the cursor is derived from the one that is. An
// edge handle derives the other axis and grows or shrinks it centred,
// since there is no opposite edge on that axis to anchor to; a corner
// handle takes whichever axis moved *proportionally* further this call as
// the driver. Without it, each axis is floored on its own and the item
// may reshape freely. There is deliberately no second clamp on the
// derived axis: the floor is ratio-consistent, and a re-clamp is exactly
// what would break the ratio.
void ApplyResizeHandleDelta(Rect& rect, bool movesLeft, bool movesRight, bool movesTop, bool movesBottom, float dx,
                             float dy, bool lockAspect);

}  // namespace sz::core
