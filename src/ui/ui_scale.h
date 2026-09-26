#pragma once

#include <algorithm>
#include <cmath>

namespace sz::ui {

// How large the interface is drawn: 1 at 100%, 1.5 at 150%. Everything the
// overlay draws as interface - a panel's padding, a button's size, a gap -
// is written in pixels at 100% and goes through Px on its way to the
// screen. The ImGui style carries the same scale (see
// OverlayApp::ApplyUiScale), which covers text and ImGui's own widgets.
//
// What is on the screen rather than the interface to it - a snippet, a
// stroke, a note's text - has a size of its own and is not scaled.
//
// One value for the whole process, set at the start of a frame and only
// there, so everything drawn in one frame agrees on it.
float UiScale();
void SetUiScale(float scale);

// `px` pixels at 100%, at the current scale.
inline float Px(float px) { return px * UiScale(); }
// The same, rounded to a whole pixel and never under one - for the width of
// a line drawn along a pixel grid, which a fraction would blur across two.
inline float PxWhole(float px) { return std::max(1.0f, std::round(Px(px))); }

}  // namespace sz::ui
