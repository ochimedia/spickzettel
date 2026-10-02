#include "core/drawing/canvas_state.h"

#include <gtest/gtest.h>

namespace sz::core {
namespace {

TEST(CanvasStateTest, StartsEmpty) {
    CanvasState canvas;
    EXPECT_TRUE(canvas.Strokes().empty());
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}

TEST(CanvasStateTest, BeginExtendEndProducesCompletedStroke) {
    CanvasState canvas;
    canvas.BeginStroke(StrokePoint{0.0f, 0.0f}, 0xAABBCCDD, 5.0f);
    ASSERT_TRUE(canvas.ActiveStroke().has_value());
    canvas.ExtendStroke(StrokePoint{1.0f, 1.0f});
    canvas.ExtendStroke(StrokePoint{2.0f, 2.0f});
    canvas.EndStroke();

    EXPECT_FALSE(canvas.ActiveStroke().has_value());
    ASSERT_EQ(canvas.Strokes().size(), 1u);
    const Stroke& stroke = canvas.Strokes().front();
    EXPECT_EQ(stroke.points.size(), 3u);
    EXPECT_EQ(stroke.colorRGBA, 0xAABBCCDDu);
    EXPECT_FLOAT_EQ(stroke.width, 5.0f);
}

// A tail is drawn after the stroke's own points, replaced by the next
// one or by a real point, and never committed.
TEST(CanvasStateTest, AStrokesTailIsShownButNotKept) {
    CanvasState canvas;
    canvas.BeginStroke(StrokePoint{0.0f, 0.0f}, 0xFF0000FF, 5.0f);
    canvas.ExtendStroke(StrokePoint{1.0f, 0.0f});
    canvas.SetActiveStrokeTail({StrokePoint{2.0f, 0.0f}, StrokePoint{3.0f, 0.0f}});
    ASSERT_EQ(canvas.ActiveStroke()->points.size(), 4u);
    EXPECT_FLOAT_EQ(canvas.ActiveStroke()->points.back().x, 3.0f);

    canvas.SetActiveStrokeTail({StrokePoint{5.0f, 0.0f}});
    ASSERT_EQ(canvas.ActiveStroke()->points.size(), 3u) << "the old tail replaced";
    EXPECT_FLOAT_EQ(canvas.ActiveStroke()->points.back().x, 5.0f);

    canvas.ExtendStroke(StrokePoint{2.0f, 0.0f});
    ASSERT_EQ(canvas.ActiveStroke()->points.size(), 3u) << "a real point drops the tail";
    EXPECT_FLOAT_EQ(canvas.ActiveStroke()->points.back().x, 2.0f);

    canvas.SetActiveStrokeTail({StrokePoint{9.0f, 0.0f}});
    canvas.EndStroke();
    ASSERT_EQ(canvas.Strokes().size(), 1u);
    ASSERT_EQ(canvas.Strokes().front().points.size(), 3u);
    EXPECT_FLOAT_EQ(canvas.Strokes().front().points.back().x, 2.0f);
}

// A tap is a dot, not nothing.
TEST(CanvasStateTest, SinglePointStrokeIsKeptOnEndAsADot) {
    CanvasState canvas;
    canvas.BeginStroke(StrokePoint{4.0f, 7.0f}, 0xFF0000FF, 3.0f);
    canvas.EndStroke();

    ASSERT_EQ(canvas.Strokes().size(), 1u);
    const Stroke& dot = canvas.Strokes().front();
    ASSERT_EQ(dot.points.size(), 1u);
    EXPECT_FLOAT_EQ(dot.points.front().x, 4.0f);
    EXPECT_FLOAT_EQ(dot.points.front().y, 7.0f);
    EXPECT_FLOAT_EQ(dot.width, 3.0f);
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}

// Empty still isn't a mark - SetActiveStrokePoints can leave a stroke with
// no points at all, and there's nothing to draw for that.
TEST(CanvasStateTest, PointlessStrokeIsDiscardedOnEnd) {
    CanvasState canvas;
    canvas.BeginStroke(StrokePoint{0.0f, 0.0f}, 0xFF0000FF, 3.0f);
    canvas.SetActiveStrokePoints({});
    canvas.EndStroke();

    EXPECT_TRUE(canvas.Strokes().empty());
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}

TEST(CanvasStateTest, ExtendWithoutBeginIsNoOp) {
    CanvasState canvas;
    canvas.ExtendStroke(StrokePoint{1.0f, 1.0f});
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}

TEST(CanvasStateTest, SetActiveStrokePointsReplacesPointsButKeepsColorAndWidth) {
    CanvasState canvas;
    canvas.BeginStroke(StrokePoint{0.0f, 0.0f}, 0xAABBCCDD, 5.0f);
    canvas.ExtendStroke(StrokePoint{1.0f, 1.0f});

    canvas.SetActiveStrokePoints({StrokePoint{10.0f, 20.0f}, StrokePoint{30.0f, 40.0f}});

    ASSERT_TRUE(canvas.ActiveStroke().has_value());
    const Stroke& active = *canvas.ActiveStroke();
    ASSERT_EQ(active.points.size(), 2u);
    EXPECT_FLOAT_EQ(active.points[0].x, 10.0f);
    EXPECT_FLOAT_EQ(active.points[1].y, 40.0f);
    EXPECT_EQ(active.colorRGBA, 0xAABBCCDDu);
    EXPECT_FLOAT_EQ(active.width, 5.0f);
}

TEST(CanvasStateTest, SetActiveStrokePointsWithoutBeginIsNoOp) {
    CanvasState canvas;
    canvas.SetActiveStrokePoints({StrokePoint{1.0f, 1.0f}});
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}

TEST(CanvasStateTest, CancelActiveStrokeDiscardsItWithoutCommitting) {
    CanvasState canvas;
    canvas.BeginStroke(StrokePoint{0.0f, 0.0f}, 0xFF0000FF, 3.0f);
    canvas.ExtendStroke(StrokePoint{5.0f, 5.0f});

    canvas.CancelActiveStroke();

    EXPECT_FALSE(canvas.ActiveStroke().has_value());
    EXPECT_TRUE(canvas.Strokes().empty());
}

TEST(CanvasStateTest, CancelActiveStrokeWithoutBeginIsNoOp) {
    CanvasState canvas;
    canvas.CancelActiveStroke();
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}


TEST(CanvasStateTest, ClearRemovesEverything) {
    CanvasState canvas;
    canvas.BeginStroke(StrokePoint{0.0f, 0.0f}, 0xFF0000FF, 3.0f);
    canvas.ExtendStroke(StrokePoint{1.0f, 1.0f});
    canvas.EndStroke();
    canvas.BeginStroke(StrokePoint{5.0f, 5.0f}, 0xFF0000FF, 3.0f);

    canvas.Clear();

    EXPECT_TRUE(canvas.Strokes().empty());
    EXPECT_FALSE(canvas.ActiveStroke().has_value());
}

}  // namespace
}  // namespace sz::core
