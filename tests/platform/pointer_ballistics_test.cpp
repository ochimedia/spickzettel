#include "platform/pointer_ballistics.h"

#include <gtest/gtest.h>

namespace {

using namespace sz::platform::pointer_ballistics;

// The Windows 11 default curve as the registry holds it, in 16.16 fixed
// point - slightly off the round numbers Curve defaults to.
Curve RegistryDefaultCurve() {
    Curve curve;
    const float in[5] = {0.0f, 0.4300079f, 1.25f, 3.8600006f, 40.0f};
    const float out[5] = {0.0f, 1.0702667f, 4.140625f, 18.984375f, 443.75f};
    for (int i = 0; i < 5; ++i) {
        curve.in[i] = in[i];
        curve.out[i] = out[i];
    }
    return curve;
}

// Pixels per count the real cursor traveled for straight reports of each
// size, slider at 10 - 960 counts per row, measured at 125 Hz and the same
// at 1000 Hz and 50 Hz.
TEST(PointerBallisticsTest, MatchesTheMeasuredDesktopPointer) {
    const struct {
        float counts;
        float pixelsPerCount;
    } measured[] = {{1, 0.5687f},  {2, 0.6396f},  {4, 0.7479f},  {8, 1.0031f},
                    {12, 1.1021f}, {16, 1.3677f}, {20, 1.6302f}, {26, 1.8750f}};
    const Curve curve = RegistryDefaultCurve();
    for (const auto& row : measured) {
        EXPECT_NEAR(CurveGain(curve, row.counts), row.pixelsPerCount, 0.003f) << row.counts << " counts";
    }
}

TEST(PointerBallisticsTest, ADiagonalReportIsItsLargerAxisPlusHalfTheSmaller) {
    EXPECT_FLOAT_EQ(ReportMagnitude(4.0f, 2.0f), 5.0f);
    EXPECT_FLOAT_EQ(ReportMagnitude(-2.0f, 4.0f), 5.0f);
    EXPECT_FLOAT_EQ(ReportMagnitude(0.0f, -3.0f), 3.0f);
    // Measured: (4, 2) reports moved the cursor 0.825 px per count on both
    // axes, the gain of a straight 5-count report.
    EXPECT_NEAR(CurveGain(RegistryDefaultCurve(), ReportMagnitude(4.0f, 2.0f)), 0.825f, 0.003f);
}

TEST(PointerBallisticsTest, NoMovementReadsTheFirstSegmentAndNothingIsNegative) {
    const Curve curve = RegistryDefaultCurve();
    EXPECT_NEAR(CurveGain(curve, 0.0f), CurveGain(curve, 0.5f), 1e-4f) << "the first segment runs through 0";
    EXPECT_GT(CurveGain(curve, 1000.0f), CurveGain(curve, 100.0f)) << "past the last point it keeps rising";
}

TEST(PointerBallisticsTest, TheSliderMeansDifferentThingsWithTheCurveOnAndOff) {
    // Both measured against the real cursor.
    EXPECT_FLOAT_EQ(SliderMultiplier(4, /*curveOn=*/true), 0.4f);
    EXPECT_FLOAT_EQ(SliderMultiplier(20, /*curveOn=*/true), 2.0f);
    EXPECT_FLOAT_EQ(SliderMultiplier(4, /*curveOn=*/false), 0.25f);
    EXPECT_FLOAT_EQ(SliderMultiplier(6, /*curveOn=*/false), 0.5f);
    EXPECT_FLOAT_EQ(SliderMultiplier(14, /*curveOn=*/false), 2.0f);
    EXPECT_FLOAT_EQ(SliderMultiplier(20, /*curveOn=*/false), 3.5f);
    EXPECT_FLOAT_EQ(SliderMultiplier(0, /*curveOn=*/false), SliderMultiplier(1, false)) << "clamped";
}

}  // namespace
