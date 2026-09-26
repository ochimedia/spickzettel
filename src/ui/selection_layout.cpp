#include "ui/selection_layout.h"

#include <algorithm>
#include <cmath>

#include "ui/ui_scale.h"

namespace sz::ui {

std::array<HandleSpec, 8> HandleSpecs(const core::Rect& r) {
    const float x0 = std::round(r.x);
    const float y0 = std::round(r.y);
    const float x1 = std::round(r.x + r.w);
    const float y1 = std::round(r.y + r.h);
    const float xm = std::round((x0 + x1) * 0.5f);
    const float ym = std::round((y0 + y1) * 0.5f);
    return {{
        {ResizeHandle::NW, "nw", platform::Vec2{x0, y0}},
        {ResizeHandle::NE, "ne", platform::Vec2{x1, y0}},
        {ResizeHandle::SE, "se", platform::Vec2{x1, y1}},
        {ResizeHandle::SW, "sw", platform::Vec2{x0, y1}},
        {ResizeHandle::N, "n", platform::Vec2{xm, y0}},
        {ResizeHandle::S, "s", platform::Vec2{xm, y1}},
        {ResizeHandle::E, "e", platform::Vec2{x1, ym}},
        {ResizeHandle::W, "w", platform::Vec2{x0, ym}},
    }};
}

HitRect HandleDrawRect(platform::Vec2 center) {
    const float half = std::round(Px(kHandleSizePx) * 0.5f);
    return HitRect{platform::Vec2{center.x - half, center.y - half}, platform::Vec2{center.x + half, center.y + half}};
}

HitRect HandleHitRect(platform::Vec2 center) {
    const float half = std::round(Px(kHandleSizePx) * 0.5f) + std::round(Px(kHandleHitSlopPx));
    return HitRect{platform::Vec2{center.x - half, center.y - half}, platform::Vec2{center.x + half, center.y + half}};
}

const char* ResizeHandleName(ResizeHandle handle) {
    for (const HandleSpec& h : HandleSpecs(core::Rect{})) {
        if (h.handle == handle) {
            return h.name;
        }
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
float BarWidth(size_t buttonCount) {
    const auto n = static_cast<float>(buttonCount);
    return n * Px(kBarButtonSize) + (n - 1.0f) * Px(kBarButtonGap) + 2.0f * Px(kBarPad);
}
}  // namespace

BarLayout LayoutBar(const core::Rect& bounds, float displayW, float displayH, size_t buttonCount) {
    const float width = BarWidth(buttonCount);
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
    float slot = 0.0f;
    for (const core::ChromeButton candidate : buttons) {
        if (candidate == button) {
            break;
        }
        slot += 1.0f;
    }
    const float x = bar.min.x + Px(kBarPad) + slot * (Px(kBarButtonSize) + Px(kBarButtonGap));
    const float y = bar.min.y + Px(kBarPad);
    return HitRect{platform::Vec2{x, y}, platform::Vec2{x + Px(kBarButtonSize), y + Px(kBarButtonSize)}};
}

}  // namespace sz::ui
