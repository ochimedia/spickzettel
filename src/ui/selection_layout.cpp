#include "ui/selection_layout.h"

#include <algorithm>
#include <cmath>

#include "ui/ui_scale.h"

namespace sz::ui {

std::optional<ResizeHandle> ResizeBandAt(const core::Rect& rect, float x, float y, float displayW, float displayH) {
    const float x0 = std::round(rect.x);
    const float y0 = std::round(rect.y);
    const float x1 = std::round(rect.x + rect.w);
    const float y1 = std::round(rect.y + rect.h);
    const float band = std::round(Px(kResizeBandPx));
    if (x < x0 - band || x >= x1 + band || y < y0 - band || y >= y1 + band) {
        return std::nullopt;
    }
    // Each side's band: outside its edge, or inside the edge's visible
    // part where the display leaves less than the band outside it.
    const bool left = x < (x0 < band ? std::max(x0, 0.0f) + band : x0);
    const bool right = x >= (x1 > displayW - band ? std::min(x1, displayW) - band : x1);
    const bool top = y < (y0 < band ? std::max(y0, 0.0f) + band : y0);
    const bool bottom = y >= (y1 > displayH - band ? std::min(y1, displayH) - band : y1);
    if (!left && !right && !top && !bottom) {
        return std::nullopt;  // the snippet itself
    }
    // The corners reach along the edges beside them.
    const float reachX = std::min(std::round(Px(kResizeCornerPx)), std::floor((x1 - x0) * 0.25f));
    const float reachY = std::min(std::round(Px(kResizeCornerPx)), std::floor((y1 - y0) * 0.25f));
    bool west = left || ((top || bottom) && x < x0 + reachX);
    bool east = right || ((top || bottom) && x >= x1 - reachX);
    bool north = top || ((left || right) && y < y0 + reachY);
    bool south = bottom || ((left || right) && y >= y1 - reachY);
    // Both sides at once only where the bands inside a snippet narrower
    // than two of them meet: the nearer edge's.
    if (west && east) {
        (x - x0 < x1 - x ? east : west) = false;
    }
    if (north && south) {
        (y - y0 < y1 - y ? south : north) = false;
    }
    if (north) {
        return west ? ResizeHandle::NW : east ? ResizeHandle::NE : ResizeHandle::N;
    }
    if (south) {
        return west ? ResizeHandle::SW : east ? ResizeHandle::SE : ResizeHandle::S;
    }
    return west ? ResizeHandle::W : ResizeHandle::E;
}

HitRect ResizeCornerRect(const core::Rect& rect) {
    const float x1 = std::round(rect.x + rect.w);
    const float y1 = std::round(rect.y + rect.h);
    const float band = std::round(Px(kResizeBandPx));
    return HitRect{platform::Vec2{x1 - band, y1 - band}, platform::Vec2{x1 + band, y1 + band}};
}

const char* ResizeHandleName(ResizeHandle handle) {
    switch (handle) {
        case ResizeHandle::NW:
            return "nw";
        case ResizeHandle::NE:
            return "ne";
        case ResizeHandle::SE:
            return "se";
        case ResizeHandle::SW:
            return "sw";
        case ResizeHandle::N:
            return "n";
        case ResizeHandle::S:
            return "s";
        case ResizeHandle::E:
            return "e";
        case ResizeHandle::W:
            return "w";
    }
    return "?";
}

void ResizeHandleEdges(ResizeHandle handle, bool& left, bool& right, bool& top, bool& bottom) {
    left = handle == ResizeHandle::NW || handle == ResizeHandle::SW || handle == ResizeHandle::W;
    right = handle == ResizeHandle::NE || handle == ResizeHandle::SE || handle == ResizeHandle::E;
    top = handle == ResizeHandle::NW || handle == ResizeHandle::NE || handle == ResizeHandle::N;
    bottom = handle == ResizeHandle::SW || handle == ResizeHandle::SE || handle == ResizeHandle::S;
}

namespace {
// How far from the bar's first button the left edge of `buttons[index]`
// is: a button and a gap for each before it, and the wider gap where the
// group changes.
float BarButtonOffset(const std::vector<core::ChromeButton>& buttons, size_t index) {
    float offset = 0.0f;
    for (size_t at = 1; at <= index && at < buttons.size(); ++at) {
        const bool newGroup = core::IsDrawingBarButton(buttons[at]) != core::IsDrawingBarButton(buttons[at - 1]);
        offset += Px(kBarButtonSize) + Px(newGroup ? kBarGroupGap : kBarButtonGap);
    }
    return offset;
}

float BarWidth(const std::vector<core::ChromeButton>& buttons) {
    if (buttons.empty()) {
        return 2.0f * Px(kBarPad);
    }
    return BarButtonOffset(buttons, buttons.size() - 1) + Px(kBarButtonSize) + 2.0f * Px(kBarPad);
}
}  // namespace

BarLayout LayoutBar(const core::Rect& bounds, float displayW, float displayH,
                    const std::vector<core::ChromeButton>& buttons) {
    const float width = BarWidth(buttons);
    float x = std::round(bounds.x + bounds.w * 0.5f - width * 0.5f);
    x = std::clamp(x, 0.0f, std::max(0.0f, displayW - width));
    const auto at = [&](float y) {
        return BarLayout{platform::Vec2{x, y}, platform::Vec2{x + width, y + Px(kBarHeight)}};
    };
    const BarLayout above = at(std::round(bounds.y) - Px(kBarGapPx) - Px(kBarHeight));
    if (above.min.y >= 0.0f) {
        return above;
    }
    const BarLayout below = at(std::round(bounds.y + bounds.h) + Px(kBarGapPx));
    if (below.max.y <= displayH) {
        return below;
    }
    return at(std::max(0.0f, std::round(bounds.y) + Px(kBarGapPx)));
}

HitRect BarButtonRect(const BarLayout& bar, const std::vector<core::ChromeButton>& buttons,
                      core::ChromeButton button) {
    const size_t index = static_cast<size_t>(std::find(buttons.begin(), buttons.end(), button) - buttons.begin());
    const float x = bar.min.x + Px(kBarPad) + BarButtonOffset(buttons, index);
    const float y = bar.min.y + Px(kBarPad);
    return HitRect{platform::Vec2{x, y}, platform::Vec2{x + Px(kBarButtonSize), y + Px(kBarButtonSize)}};
}

std::optional<float> BarDividerX(const BarLayout& bar, const std::vector<core::ChromeButton>& buttons,
                                 float width) {
    for (size_t at = 1; at < buttons.size(); ++at) {
        if (core::IsDrawingBarButton(buttons[at]) != core::IsDrawingBarButton(buttons[at - 1])) {
            const float before = BarButtonRect(bar, buttons, buttons[at - 1]).max.x;
            const float after = BarButtonRect(bar, buttons, buttons[at]).min.x;
            const float middle = (before + after) * 0.5f;
            return std::round(middle - width * 0.5f) + width * 0.5f;
        }
    }
    return std::nullopt;
}

}  // namespace sz::ui
