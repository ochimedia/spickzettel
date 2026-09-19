// The icon tape as ImGui actually paints it: DrawIcon run against a real
// ImDrawList, asserting on the triangles that come out the other side.
//
// Nothing here looks at pixels - it looks at where the vertices *are*. That
// is enough to catch the whole family of bugs this file exists for, where
// the shape is geometrically right but the stroke around it is not: a
// vertex that lands well outside the outline it is supposed to be tracing
// means the stroke bulged there, which is exactly what a blob looks like.

#include "ui/icon_draw.h"
#include "ui/icons_generated.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <imgui.h>
// For ImRect, which is the natural shape for "where the ink is" and lives
// only in the internal header.
#include <imgui_internal.h>

namespace sz::ui {
namespace {

struct NamedIcon {
    const char* name;
    const Icon* icon;
};

// Every icon in the set, so a new one cannot quietly arrive untested - the
// same list scripts/gen_icons.py generates from.
const NamedIcon kAllIcons[] = {
    {"pen", &icons::kPen},
    {"eraser", &icons::kEraser},
    {"eraser-rect", &icons::kEraserRect},
    {"camera", &icons::kCamera},
    {"layout-grid", &icons::kLayoutGrid},
    {"layer-down", &icons::kLayerDown},
    {"layer-up", &icons::kLayerUp},
    {"move", &icons::kMove},
    {"copy", &icons::kCopy},
    {"trash", &icons::kTrash},
    {"plus", &icons::kPlus},
    {"x", &icons::kX},
    {"undo", &icons::kUndo},
    {"rectangle", &icons::kRectangle},
    {"line", &icons::kLine},
    {"type", &icons::kType},
    {"minimize", &icons::kMinimize},
    {"maximize", &icons::kMaximize},
    {"restore", &icons::kRestore},
    {"more-vertical", &icons::kMoreVertical},
    {"target", &icons::kTarget},
    {"note", &icons::kNote},
    {"pin", &icons::kPin},
    {"select", &icons::kSelect},
    {"scissors", &icons::kScissors},
    {"clipboard", &icons::kClipboard},
};

// Where the icon's ink is, in its own 24x24 space - the outline the stroke
// is drawn around, which is what the stroke may not stray far from.
//
// Curves are bounded rather than solved: a cubic lies inside the hull of
// its own four points, and an arc is sampled. Both are outer bounds, which
// is the safe direction - a bound that is too generous can only make this
// test miss something, never invent a failure.
ImRect InkBounds(const Icon& icon) {
    ImRect bounds(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    const auto add = [&bounds](float x, float y) {
        bounds.Min.x = ImMin(bounds.Min.x, x);
        bounds.Min.y = ImMin(bounds.Min.y, y);
        bounds.Max.x = ImMax(bounds.Max.x, x);
        bounds.Max.y = ImMax(bounds.Max.y, y);
    };
    for (int i = 0; i < icon.count; ++i) {
        const IconCmd& cmd = icon.cmds[i];
        switch (cmd.op) {
            case IconOp::MoveTo:
            case IconOp::LineTo:
                add(cmd.a, cmd.b);
                break;
            case IconOp::CubicTo:
                add(cmd.a, cmd.b);
                add(cmd.c, cmd.d);
                add(cmd.e, cmd.f);
                break;
            case IconOp::ArcTo: {
                constexpr int kSamples = 64;
                for (int s = 0; s <= kSamples; ++s) {
                    const float t = static_cast<float>(s) / static_cast<float>(kSamples);
                    const float angle = cmd.d + (cmd.e - cmd.d) * t;
                    add(cmd.a + cmd.c * std::cos(angle), cmd.b + cmd.c * std::sin(angle));
                }
                break;
            }
            case IconOp::Circle:
                add(cmd.a - cmd.c, cmd.b - cmd.c);
                add(cmd.a + cmd.c, cmd.b + cmd.c);
                break;
            case IconOp::RoundedRect:
                add(cmd.a, cmd.b);
                add(cmd.c, cmd.d);
                break;
            case IconOp::EndSubpath:
                break;
        }
    }
    return bounds;
}

class IconDrawTest : public ::testing::Test {
protected:
    void SetUp() override {
        IMGUI_CHECKVERSION();
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(512.0f, 512.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.Fonts->AddFontDefault();
        // Nothing is uploaded because nothing is presented; without this
        // ImGui asserts that some backend should have built the atlas.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::NewFrame();
    }
    void TearDown() override {
        ImGui::EndFrame();
        ImGui::DestroyContext(context_);
        context_ = nullptr;
    }

    ImGuiContext* context_ = nullptr;
};

// Big enough that a bulge is unmistakable next to the tolerance, and an
// even multiple of the 24-unit icon space so the arithmetic below is exact.
constexpr float kSize = 48.0f;
constexpr float kThickness = 2.0f;
constexpr ImVec2 kPos(64.0f, 64.0f);

TEST_F(IconDrawTest, NoIconStraysOutsideItsOwnOutline) {
    for (const NamedIcon& entry : kAllIcons) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        const int firstVertex = drawList->VtxBuffer.Size;
        DrawIcon(drawList, *entry.icon, kPos, kSize, IM_COL32_WHITE, kThickness);
        ASSERT_GT(drawList->VtxBuffer.Size, firstVertex) << entry.name << " drew nothing at all";

        const float scale = kSize / 24.0f;
        ImRect allowed = InkBounds(*entry.icon);
        allowed.Min = ImVec2(kPos.x + allowed.Min.x * scale, kPos.y + allowed.Min.y * scale);
        allowed.Max = ImVec2(kPos.x + allowed.Max.x * scale, kPos.y + allowed.Max.y * scale);
        // Half the stroke either side of the outline, plus ImGui's own
        // one-pixel anti-aliasing fringe, plus a little for the mitre a
        // sharp corner legitimately pushes out.
        const float slack = kThickness * scale * 0.5f + 1.0f + 1.5f;
        allowed.Expand(slack);

        for (int v = firstVertex; v < drawList->VtxBuffer.Size; ++v) {
            const ImVec2 p = drawList->VtxBuffer[v].pos;
            EXPECT_TRUE(allowed.Contains(p))
                << entry.name << ": vertex " << (v - firstVertex) << " at (" << p.x << ", " << p.y
                << ") is outside (" << allowed.Min.x << ", " << allowed.Min.y << ")-(" << allowed.Max.x
                << ", " << allowed.Max.y << ")";
        }
    }
}

// And the rule on its own, including the cases the icon set does not
// happen to contain today.
TEST(DropRepeatedPathPointsTest, KeepsEveryPointThatGoesSomewhere) {
    ImVector<ImVec2> path;
    path.push_back(ImVec2(0.0f, 0.0f));
    path.push_back(ImVec2(10.0f, 0.0f));
    path.push_back(ImVec2(10.0f, 10.0f));
    DropRepeatedPathPoints(path);
    ASSERT_EQ(path.Size, 3);
    EXPECT_EQ(path[2].x, 10.0f);
    EXPECT_EQ(path[2].y, 10.0f);
}

TEST(DropRepeatedPathPointsTest, DropsRepeatsWhereverTheyFall) {
    ImVector<ImVec2> path;
    path.push_back(ImVec2(0.0f, 0.0f));
    path.push_back(ImVec2(0.0f, 0.0f));        // exactly repeated - the arc case
    path.push_back(ImVec2(10.0f, 0.0f));
    path.push_back(ImVec2(10.0f, 0.000001f));  // near enough to be the same point
    path.push_back(ImVec2(10.0f, 10.0f));
    path.push_back(ImVec2(10.0f, 10.0f));      // and at the very end
    DropRepeatedPathPoints(path);
    ASSERT_EQ(path.Size, 3);
    EXPECT_EQ(path[0].y, 0.0f);
    EXPECT_EQ(path[1].x, 10.0f);
    EXPECT_EQ(path[2].y, 10.0f);
}

// A run of three, and a path that is nothing but repeats: the second must
// come out as a single point rather than as an empty path, because a
// caller about to stroke it should get a dot, not a crash.
TEST(DropRepeatedPathPointsTest, SurvivesAPathThatGoesNowhereAtAll) {
    ImVector<ImVec2> path;
    for (int i = 0; i < 4; ++i) {
        path.push_back(ImVec2(3.0f, 4.0f));
    }
    DropRepeatedPathPoints(path);
    ASSERT_EQ(path.Size, 1);
    EXPECT_EQ(path[0].x, 3.0f);

    ImVector<ImVec2> empty;
    DropRepeatedPathPoints(empty);
    EXPECT_EQ(empty.Size, 0);
}

}  // namespace
}  // namespace sz::ui
