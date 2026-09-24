#pragma once

#include <algorithm>

// The desktop pointer's arithmetic, so the pointer the input grab keeps
// travels as far as the one it replaces. Measured against the real cursor
// rather than taken from documentation, which describes older versions -
// see the pointer ballistics bullet in docs/ARCHITECTURE.md.
//
// What the measurements settled: with "enhance pointer precision" on,
// Windows applies its curve to each mouse report on its own - to how many
// counts the report carries, not to how fast the hand moved. The same 480
// counts travel the same distance in 1-count reports at 1000 Hz, 125 Hz or
// 50 Hz, and a quarter further in 8-count reports. The monitor's refresh
// rate plays no part either (60 Hz and 120 Hz measured identical).
namespace sz::platform::pointer_ballistics {

// SmoothMouseXCurve / SmoothMouseYCurve out of the registry: five points,
// X an input magnitude, Y the output for it. Windows' own defaults, for
// when the registry has none.
struct Curve {
    float in[5] = {0.0f, 0.43f, 1.25f, 3.86f, 40.0f};
    float out[5] = {0.0f, 1.07f, 4.14f, 18.98f, 443.75f};
};

// A report's magnitude as the curve sees it: the larger axis plus half the
// smaller. Measured: a report of (4, 2) moves exactly as far per count as a
// straight one of 5.
inline float ReportMagnitude(float dx, float dy) {
    dx = dx < 0.0f ? -dx : dx;
    dy = dy < 0.0f ? -dy : dy;
    return std::max(dx, dy) + std::min(dx, dy) * 0.5f;
}

// The two constants that tie the curve's units to counts and pixels: the
// curve is read at magnitude / 3.5, and what it gives is scaled by 0.8.
// Fitted to 26 measured report sizes against the Windows 11 default curve,
// RMS error 0.0003 pixels per count.
inline constexpr float kCountsPerCurveUnit = 3.5f;
inline constexpr float kCurveOutputScale = 0.8f;

// Pixels per count for a report of this magnitude, before the speed slider.
// Linear between the curve's points and along its last segment beyond it.
inline float CurveGain(const Curve& curve, float magnitude) {
    const float x = magnitude / kCountsPerCurveUnit;
    int segment = 1;
    while (segment < 4 && x > curve.in[segment]) {
        ++segment;
    }
    const float span = curve.in[segment] - curve.in[segment - 1];
    if (span <= 0.0f) {
        return 1.0f;  // a curve with repeated points: nothing sensible to read
    }
    const float slope = (curve.out[segment] - curve.out[segment - 1]) / span;
    if (magnitude <= 0.0f) {
        // The limit at no movement, where the first segment is a line
        // through the origin.
        return kCurveOutputScale * slope / kCountsPerCurveUnit;
    }
    const float y = curve.out[segment - 1] + slope * (x - curve.in[segment - 1]);
    return kCurveOutputScale * y / magnitude;
}

// Windows' pointer-speed slider (1..20, 10 default) as the multipliers it
// stands for WITH "enhance pointer precision" OFF. This is the documented
// table, and measured: 4 -> 0.25, 6 -> 0.5, 10 -> 1.0, 14 -> 2.0,
// 20 -> 3.5.
//
// With the curve ON the OS does not use this table at all: the ratio to the
// middle notch is slider/10, linear (measured 4, 6, 14 and 20). Applying
// the table in that mode was an earlier bug of this code: at a slider of 15
// it ran the pointer at x2.25 where the OS runs x1.5, which lifted the
// slow-speed gain past one pixel per count and made single mouse reports
// step two pixels.
inline constexpr float kSliderMultiplierCurveOff[21] = {1.0f,   0.03125f, 0.0625f, 0.125f, 0.25f, 0.375f, 0.5f,
                                                         0.625f, 0.75f,    0.875f,  1.0f,   1.25f, 1.5f,   1.75f,
                                                         2.0f,   2.25f,    2.5f,    2.75f,  3.0f,  3.25f,  3.5f};

inline float SliderMultiplier(int slider, bool curveOn) {
    slider = std::clamp(slider, 1, 20);
    return curveOn ? static_cast<float>(slider) / 10.0f : kSliderMultiplierCurveOff[slider];
}

}  // namespace sz::platform::pointer_ballistics
