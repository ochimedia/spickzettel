#include "core/canvas/item_geometry.h"

#include <algorithm>

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

    // The size the pointer is asking for, before any floor and before the
    // ratio has a say: the edge or corner under the hand moved by the
    // whole drag, the opposite one left where it was.
    float wantW = rect.w;
    if (movesRight) {
        wantW = rect.w + dx;
    } else if (movesLeft) {
        wantW = rect.w - dx;
    }
    float wantH = rect.h;
    if (movesBottom) {
        wantH = rect.h + dy;
    } else if (movesTop) {
        wantH = rect.h - dy;
    }

    float newW = rect.w;
    float newH = rect.h;
    if (ratioLocked) {
        // One scale for both axes, which is what keeping the shape means.
        //
        // A corner takes it by projecting the corner the pointer asks for
        // onto the item's own diagonal - the scale that fits that corner
        // best, and the reason this is a projection rather than a choice
        // between the two axes. Choosing (whichever axis moved
        // proportionally further drives, the other is derived) is
        // discontinuous wherever the axes disagree about which way they
        // are going: at the crossover one answer says a tenth bigger and
        // the other a tenth smaller, so moving the pointer *across* the
        // diagonal rather than along it made the size jump by up to its
        // own width - measured, from a hundredth of a pixel of movement.
        // A projection has no crossover to jump at, agrees with the old
        // rule exactly for a drag along the diagonal, and for one across
        // it lets the two axes cancel smoothly.
        //
        // An edge handle has one axis to take it from, since there is no
        // opposite edge on the other to anchor to.
        float scale = 1.0f;
        if (horizontal && vertical) {
            scale = (wantW * rect.w + wantH * rect.h) / (rect.w * rect.w + rect.h * rect.h);
        } else if (horizontal) {
            scale = wantW / rect.w;
        } else if (vertical) {
            scale = wantH / rect.h;
        }
        // One floor, on the scale, rather than one per axis: floorSize is
        // already the item's own shape (MinimumSizeForAspectRatio), so at
        // the floor the size *is* that floor, exactly, and a per-axis
        // clamp is precisely what would break the ratio. It also catches
        // a scale driven negative by a pointer dragged past the anchor.
        const float floorScale = std::max(floorSize.w / rect.w, floorSize.h / rect.h);
        if (scale <= floorScale) {
            newW = floorSize.w;
            newH = floorSize.h;
        } else {
            newW = rect.w * scale;
            newH = rect.h * scale;
        }
    } else {
        // Free to reshape: each axis floored on its own, and an axis this
        // handle does not drive is left alone entirely - including one
        // already under the floor, which is not this drag's to correct.
        if (horizontal) {
            newW = std::max(floorSize.w, wantW);
        }
        if (vertical) {
            newH = std::max(floorSize.h, wantH);
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
