#include "core/drawing/draw_tool.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace sz::core {
namespace {

using platform::MouseButton;
using platform::MouseEvent;
using platform::MouseEventKind;
using platform::Vec2;

// Helpers so a drag reads as a drag rather than as four struct literals.
void Down(DrawTool& tool, CanvasState& canvas, float x, float y) {
    tool.OnMouseEvent(MouseEvent{Vec2{x, y}, MouseButton::Left, MouseEventKind::Down}, canvas);
}
void Move(DrawTool& tool, CanvasState& canvas, float x, float y) {
    tool.OnMouseEvent(MouseEvent{Vec2{x, y}, MouseButton::Left, MouseEventKind::Move}, canvas);
}
void Up(DrawTool& tool, CanvasState& canvas, float x, float y) {
    tool.OnMouseEvent(MouseEvent{Vec2{x, y}, MouseButton::Left, MouseEventKind::Up}, canvas);
}

TEST(DrawToolTest, DragSequenceProducesOneStroke) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    Down(tool, canvas, 0, 0);
    for (int i = 1; i <= 10; ++i) {
        Move(tool, canvas, static_cast<float>(i * 10), static_cast<float>(i * 10));
    }
    Up(tool, canvas, 100, 100);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    EXPECT_GT(canvas.Strokes().front().points.size(), 2u);
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}

// The spacing filter: samples a fraction of a pixel apart carry no
// direction, only noise, and a polyline built from them is what put spikes
// on a wide pen. Twenty of them across two pixels are worth no new points.
TEST(DrawToolTest, SamplesTooCloseTogetherDoNotBecomePoints) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    Down(tool, canvas, 50, 50);
    for (int i = 1; i <= 20; ++i) {
        Move(tool, canvas, 50.0f + static_cast<float>(i) * 0.1f, 50.0f);
    }
    Up(tool, canvas, 52, 50);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    EXPECT_LE(canvas.Strokes().front().points.size(), 2u);
}

// ...but a slow hand still draws. The filter measures from the last point
// kept, not the previous sample, so creeping forward a tenth of a pixel at
// a time still produces points - just no more than the distance earns.
TEST(DrawToolTest, ASlowDragStillAccumulatesPoints) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    Down(tool, canvas, 0, 0);
    for (int i = 1; i <= 400; ++i) {
        Move(tool, canvas, static_cast<float>(i) * 0.1f, 0.0f);
    }
    Up(tool, canvas, 40, 0);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    const size_t points = canvas.Strokes().front().points.size();
    EXPECT_GE(points, 8u);   // 40px of travel, a point every ~2px, minus smoothing lag
    EXPECT_LE(points, 25u);  // and nothing like the 400 samples that came in
}

// Smoothing pulls the ink toward the average of where the pointer has
// been, so a single sample flung sideways doesn't take the line with it.
TEST(DrawToolTest, ASingleJitteredSampleDoesNotThrowTheLineSideways) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    Down(tool, canvas, 0, 0);
    Move(tool, canvas, 10, 0);
    Move(tool, canvas, 20, 0);
    Move(tool, canvas, 30, 40);  // the flinch
    Move(tool, canvas, 40, 0);
    Move(tool, canvas, 50, 0);
    Up(tool, canvas, 60, 0);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    float worst = 0.0f;
    for (const StrokePoint& p : canvas.Strokes().front().points) {
        worst = std::max(worst, std::abs(p.y));
    }
    // The raw sample was 40px off the line; what lands is a fraction of
    // that, and the points either side pull it back rather than following.
    EXPECT_LT(worst, 25.0f);
}

// A press and release without travel is a dot, and stays one point - the
// end-point snap that makes a real line finish under the cursor must not
// turn a tap into a two-ended stub.
TEST(DrawToolTest, ATapProducesASinglePointDot) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    Down(tool, canvas, 12, 34);
    Up(tool, canvas, 12, 34);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    ASSERT_EQ(canvas.Strokes().front().points.size(), 1u);
    EXPECT_FLOAT_EQ(canvas.Strokes().front().points.front().x, 12.0f);
    EXPECT_FLOAT_EQ(canvas.Strokes().front().points.front().y, 34.0f);
}

// A twitch during a click is still a tap. Without this, the smallest
// tremble between press and release would leave a stub instead of a dot.
TEST(DrawToolTest, ATapWithATinyTwitchIsStillADot) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    Down(tool, canvas, 12, 34);
    Move(tool, canvas, 12.4f, 34.3f);
    Move(tool, canvas, 12.1f, 33.8f);
    Up(tool, canvas, 12.2f, 34.1f);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    EXPECT_EQ(canvas.Strokes().front().points.size(), 1u);
}

// Pressed, moved and released between two frames: no move reaches the
// pen, and the release alone says where the line went.
TEST(DrawToolTest, AStrokeWithNoMoveBetweenPressAndReleaseIsALine) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    Down(tool, canvas, 0, 0);
    Up(tool, canvas, 50, 0);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    const std::vector<StrokePoint>& points = canvas.Strokes().front().points;
    ASSERT_GE(points.size(), 2u);
    EXPECT_FLOAT_EQ(points.front().x, 0.0f);
    EXPECT_FLOAT_EQ(points.back().x, 50.0f);
}

// ...and with only a first move too short to earn a point, or one that
// went nowhere: the frame before took that one, and the rest of the stroke
// came between it and the next.
TEST(DrawToolTest, AStrokeWithOnlyAShortMoveBeforeTheReleaseIsALine) {
    for (const float firstX : {1.0f, 0.0f}) {
        CanvasState canvas;
        DrawTool tool(0xFF0000FF, 4.0f);

        Down(tool, canvas, 0, 0);
        Move(tool, canvas, firstX, 0);
        Up(tool, canvas, 50, 0);

        ASSERT_EQ(canvas.Strokes().size(), 1u);
        const std::vector<StrokePoint>& points = canvas.Strokes().front().points;
        ASSERT_GE(points.size(), 2u) << "first move to " << firstX;
        EXPECT_FLOAT_EQ(points.front().x, 0.0f);
        EXPECT_FLOAT_EQ(points.back().x, 50.0f);
    }
}

// A release that close to the press is a twitch, and still the dot.
TEST(DrawToolTest, ATwitchWithNoMoveIsStillADot) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    Down(tool, canvas, 0, 0);
    Up(tool, canvas, 3, 0);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    EXPECT_EQ(canvas.Strokes().front().points.size(), 1u);
}

// A real line ends where the hand stopped. Smoothing leaves the last kept
// point trailing the cursor, which on a deliberate stroke reads as falling
// short of the mark.
TEST(DrawToolTest, ALineEndsAtTheReleasePoint) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    Down(tool, canvas, 0, 0);
    for (int i = 1; i <= 10; ++i) {
        Move(tool, canvas, static_cast<float>(i * 10), 0.0f);
    }
    Up(tool, canvas, 100, 0);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    const StrokePoint& last = canvas.Strokes().front().points.back();
    EXPECT_FLOAT_EQ(last.x, 100.0f);
    EXPECT_FLOAT_EQ(last.y, 0.0f);
}

// The ink reaches the pointer while the pen is still down. It trailed by
// a control point and the smoothing's lag, and caught up only on the next
// control point - at a turn, after the hand had turned, which drew on the
// old way while the pointer went back.
TEST(DrawToolTest, AHeldStrokeReachesThePointer) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 30.0f);

    Down(tool, canvas, 0, 0);
    for (int i = 1; i <= 100; ++i) {
        Move(tool, canvas, static_cast<float>(i), 0.0f);
    }

    ASSERT_TRUE(canvas.ActiveStroke().has_value());
    const StrokePoint& end = canvas.ActiveStroke()->points.back();
    EXPECT_FLOAT_EQ(end.x, 100.0f);
    EXPECT_FLOAT_EQ(end.y, 0.0f);
}

// Turning back draws nothing further the old way: the ink got as far as
// the pointer did, and no further, before the turn rather than after it.
TEST(DrawToolTest, TurningBackDrawsNothingFurtherTheOldWay) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 30.0f);

    Down(tool, canvas, 0, 0);
    for (int i = 1; i <= 100; ++i) {
        Move(tool, canvas, static_cast<float>(i) * 0.3f, static_cast<float>(i));
    }
    const auto lowest = [&canvas] {
        float y = 0.0f;
        for (const StrokePoint& point : canvas.ActiveStroke()->points) {
            y = std::max(y, point.y);
        }
        return y;
    };
    const float held = lowest();
    EXPECT_FLOAT_EQ(held, 100.0f);
    for (int i = 1; i <= 40; ++i) {
        Move(tool, canvas, 30.0f + static_cast<float>(i) * 0.3f, 100.0f - static_cast<float>(i));
        EXPECT_LE(lowest(), held) << "step " << i;
    }
}

// The tail is what lifting the pen would draw, so lifting it where the
// last move was keeps the stroke exactly as it was on screen.
TEST(DrawToolTest, LiftingThePenKeepsTheStrokeAsDrawn) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 30.0f);

    Down(tool, canvas, 0, 0);
    for (int i = 1; i <= 60; ++i) {
        const float t = static_cast<float>(i);
        Move(tool, canvas, t * 1.5f, 40.0f * std::sin(t * 0.1f));
    }
    ASSERT_TRUE(canvas.ActiveStroke().has_value());
    const std::vector<StrokePoint> drawn = canvas.ActiveStroke()->points;
    Up(tool, canvas, 90.0f, 40.0f * std::sin(6.0f));

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    const std::vector<StrokePoint>& kept = canvas.Strokes().front().points;
    ASSERT_EQ(kept.size(), drawn.size());
    for (size_t i = 0; i < kept.size(); ++i) {
        EXPECT_FLOAT_EQ(kept[i].x, drawn[i].x) << "point " << i;
        EXPECT_FLOAT_EQ(kept[i].y, drawn[i].y) << "point " << i;
    }
}

// A twitch too small to earn a second control point is the dot when the
// pen is lifted, and is shown as the dot while it is held: the tail reached
// for the pen there too, a stub the lift took back.
TEST(DrawToolTest, ATwitchIsShownAsTheDotItIsKeptAs) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 3.0f);

    Down(tool, canvas, 0, 0);
    Move(tool, canvas, 3.5f, 0.0f);
    ASSERT_TRUE(canvas.ActiveStroke().has_value());
    const std::vector<StrokePoint> drawn = canvas.ActiveStroke()->points;
    Up(tool, canvas, 3.5f, 0.0f);

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    EXPECT_EQ(canvas.Strokes().front().points, drawn);
    EXPECT_EQ(drawn.size(), 1u);
}

TEST(DrawToolTest, MoveWithoutDownDoesNotDraw) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    tool.OnMouseEvent(MouseEvent{Vec2{5, 5}, MouseButton::Left, MouseEventKind::Move}, canvas);

    EXPECT_TRUE(canvas.Strokes().empty());
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}

TEST(DrawToolTest, NonLeftButtonIsIgnored) {
    CanvasState canvas;
    DrawTool tool(0xFF0000FF, 4.0f);

    tool.OnMouseEvent(MouseEvent{Vec2{0, 0}, MouseButton::Right, MouseEventKind::Down}, canvas);

    EXPECT_TRUE(canvas.Strokes().empty());
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}

}  // namespace
}  // namespace sz::core
