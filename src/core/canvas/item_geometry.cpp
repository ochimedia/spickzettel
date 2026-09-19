#include "core/canvas/item_geometry.h"

#include <algorithm>
#include <cmath>

namespace sz::core {

MinItemSize MinimumSizeForAspectRatio(float ratio) {
    // Written as !(ratio > 0) rather than ratio <= 0 so a NaN ratio (from
    // a zero-height rect upstream) takes this branch too instead of
    // propagating into the division below.
    if (!(ratio > 0.0f)) {
        return MinItemSize{kItemMinWidth, kItemMinHeight};
    }
    // Whichever floor binds first decides both dimensions: take the width
    // that satisfies the height floor at this ratio, versus the width
    // floor itself, and keep the larger. The height is derived from the
    // ratio rather than clamped separately.
    const float w = std::max(kItemMinWidth, kItemMinHeight * ratio);
    return MinItemSize{w, w / ratio};
}

Rect GrowRectToMinimumSize(Rect rect) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        rect.w = std::max(kItemMinWidth, rect.w);
        rect.h = std::max(kItemMinHeight, rect.h);
        return rect;
    }
    const MinItemSize floorSize = MinimumSizeForAspectRatio(rect.w / rect.h);
    // One comparison, not one per axis: the floor is ratio-consistent
    // with `rect`, so being under it on one axis means being under it on
    // both, and either dimension answers the question.
    if (rect.w < floorSize.w) {
        rect.w = floorSize.w;
        rect.h = floorSize.h;
    }
    return rect;
}

Rect ClampRectToViewport(Rect rect, float displayW, float displayH) {
    rect.x = std::clamp(rect.x, kGrabMarginPx - rect.w, displayW - kGrabMarginPx);
    rect.y = std::clamp(rect.y, kGrabMarginPx - rect.h, displayH - kGrabMarginPx);
    return rect;
}


Rect RescaleRectForDisplaySize(Rect rect, float fromW, float fromH, float toW, float toH) {
    if (fromW <= 0.0f || fromH <= 0.0f || (fromW == toW && fromH == toH)) {
        return rect;
    }
    const float scaleX = toW / fromW;
    const float scaleY = toH / fromH;
    const float sizeScale = std::min(scaleX, scaleY);
    return Rect{rect.x * scaleX, rect.y * scaleY, rect.w * sizeScale, rect.h * sizeScale};
}

Rect FitAspectRatioIntoViewport(float aspectRatio, float viewportW, float viewportH) {
    Rect fitted{0.0f, 0.0f, viewportW, viewportH};
    const float viewportRatio = viewportW / viewportH;
    if (aspectRatio > viewportRatio) {
        fitted.h = viewportW / aspectRatio;
    } else {
        fitted.w = viewportH * aspectRatio;
    }
    fitted.x = (viewportW - fitted.w) * 0.5f;
    fitted.y = (viewportH - fitted.h) * 0.5f;
    return fitted;
}

bool RectsOverlap(const Rect& a, const Rect& b) {
    // A rect with no area shares none with anything, wherever it sits -
    // stated outright, because the comparisons below would otherwise call
    // a zero-sized rect *inside* another an overlap.
    if (a.w <= 0.0f || a.h <= 0.0f || b.w <= 0.0f || b.h <= 0.0f) {
        return false;
    }
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

NativePoint ScreenToNative(const Item& item, float screenX, float screenY) {
    const float sx = item.rect.w != 0.0f ? item.nativeW / item.rect.w : 1.0f;
    const float sy = item.rect.h != 0.0f ? item.nativeH / item.rect.h : 1.0f;
    return NativePoint{
        (screenX - item.rect.x) * sx,
        (screenY - item.rect.y) * sy,
        (sx + sy) * 0.5f,
    };
}

void ApplyResizeHandleDelta(Rect& rect, bool movesLeft, bool movesRight, bool movesTop, bool movesBottom, float dx,
                             float dy, bool lockAspect) {
    const bool horizontal = movesLeft || movesRight;
    const bool vertical = movesTop || movesBottom;

    const bool ratioLocked = lockAspect && rect.w > 0.0f && rect.h > 0.0f;
    const float ratio = ratioLocked ? rect.w / rect.h : 1.0f;
    const MinItemSize floorSize =
        ratioLocked ? MinimumSizeForAspectRatio(ratio) : MinItemSize{kItemMinWidth, kItemMinHeight};

    float newW = rect.w;
    if (movesRight) {
        newW = std::max(floorSize.w, rect.w + dx);
    } else if (movesLeft) {
        newW = std::max(floorSize.w, rect.w - dx);
    }
    float newH = rect.h;
    if (movesBottom) {
        newH = std::max(floorSize.h, rect.h + dy);
    } else if (movesTop) {
        newH = std::max(floorSize.h, rect.h - dy);
    }

    if (ratioLocked) {
        if (horizontal && vertical) {
            const float wChange = std::fabs(newW - rect.w) / rect.w;
            const float hChange = std::fabs(newH - rect.h) / rect.h;
            if (wChange >= hChange) {
                newH = newW / ratio;
            } else {
                newW = newH * ratio;
            }
        } else if (horizontal) {
            newH = newW / ratio;
        } else if (vertical) {
            newW = newH * ratio;
        }
    }

    if (movesRight) {
        rect.w = newW;
    } else if (movesLeft) {
        rect.x += rect.w - newW;
        rect.w = newW;
    } else if (lockAspect && vertical && newW != rect.w) {
        rect.x -= (newW - rect.w) * 0.5f;
        rect.w = newW;
    }

    if (movesBottom) {
        rect.h = newH;
    } else if (movesTop) {
        rect.y += rect.h - newH;
        rect.h = newH;
    } else if (lockAspect && horizontal && newH != rect.h) {
        rect.y -= (newH - rect.h) * 0.5f;
        rect.h = newH;
    }
}

}  // namespace sz::core
