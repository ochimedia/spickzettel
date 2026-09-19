#include "core/drawing/draw_tool.h"

#include <algorithm>
#include <cmath>

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
