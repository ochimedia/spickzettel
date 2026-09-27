// Strokes as ImGui is handed them: DrawStroke run against a real ImDrawList.

#include "ui/item_painting.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "core/canvas/canvas_manager.h"

namespace sz::ui {
namespace {

class ItemPaintingTest : public ::testing::Test {
protected:
    void SetUp() override {
        IMGUI_CHECKVERSION();
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
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

// A stroke's mesh goes into the draw list in one piece, and a stroke drawn
// long enough without lifting the pen passes 65536 vertices - half a minute
// of shading. With 16-bit indices its triangles wrapped round onto the
// first 65536 vertices and never reached its last ones; drawn, that was
// triangles across the screen. See cmake/FetchImGui.cmake.
TEST_F(ItemPaintingTest, AStrokePastSixteenBitIndicesDrawsWithAllOfItsVertices) {
    core::Stroke stroke;
    stroke.colorRGBA = 0xFF0000FFu;
    stroke.width = 4.0f;
    // A gentle wave at the pen's own 2 px spacing: a rib, four vertices, a
    // point.
    for (int i = 0; i < 20000; ++i) {
        const float x = static_cast<float>(i) * 2.0f;
        stroke.points.push_back(core::StrokePoint{x, 100.0f + 50.0f * std::sin(x * 0.01f)});
    }
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const int firstVertex = drawList->VtxBuffer.Size;
    const int firstIndex = drawList->IdxBuffer.Size;

    DrawStroke(drawList, stroke, core::StrokeRenderMode::Tessellated, 0.0f, 0.0f, 1.0f, 1.0f);

    ASSERT_GT(drawList->VtxBuffer.Size - firstVertex, 65536) << "not long enough to be a test of anything";
    const unsigned int offset = drawList->CmdBuffer.back().VtxOffset;
    unsigned int highest = 0;
    for (int i = firstIndex; i < drawList->IdxBuffer.Size; ++i) {
        const unsigned int vertex = offset + static_cast<unsigned int>(drawList->IdxBuffer[i]);
        ASSERT_GE(vertex, static_cast<unsigned int>(firstVertex)) << "index " << (i - firstIndex);
        ASSERT_LT(vertex, static_cast<unsigned int>(drawList->VtxBuffer.Size)) << "index " << (i - firstIndex);
        highest = std::max(highest, vertex);
    }
    EXPECT_EQ(highest, static_cast<unsigned int>(drawList->VtxBuffer.Size - 1)) << "its last vertices are in no triangle";
}

// A stroke drawn on a snippet stretched unevenly - twice as wide as its
// own size, and as high - is as wide once it is let go of as it was while
// it was drawn: baked into the snippet's own space and drawn back out, its
// width comes back to what it was. Scaled by the average of the axes each
// way, it came back wider, 1.125 times at 2:1.
TEST_F(ItemPaintingTest, AStrokeOnAStretchedSnippetKeepsItsWidthOnceDrawn) {
    core::Item item;
    item.rect = core::Rect{0.0f, 0.0f, 200.0f, 100.0f};
    item.nativeW = 100.0f;
    item.nativeH = 100.0f;
    core::Stroke live;
    live.colorRGBA = 0xFF0000FFu;
    live.width = 10.0f;
    live.points = {core::StrokePoint{20.0f, 50.0f}, core::StrokePoint{180.0f, 50.0f}};
    const core::Stroke baked = core::CanvasManager::BakeStrokeToNative(item, live);

    // A level line's height on screen is its width.
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const auto heightDrawn = [drawList](const core::Stroke& stroke, float scaleX, float scaleY) {
        const int first = drawList->VtxBuffer.Size;
        DrawStroke(drawList, stroke, core::StrokeRenderMode::Tessellated, 0.0f, 0.0f, scaleX, scaleY);
        float top = drawList->VtxBuffer[first].pos.y;
        float bottom = top;
        for (int i = first; i < drawList->VtxBuffer.Size; ++i) {
            top = std::min(top, drawList->VtxBuffer[i].pos.y);
            bottom = std::max(bottom, drawList->VtxBuffer[i].pos.y);
        }
        return bottom - top;
    };
    const float whileDrawn = heightDrawn(live, 1.0f, 1.0f);
    const float once = heightDrawn(baked, item.rect.w / item.nativeW, item.rect.h / item.nativeH);
    EXPECT_NEAR(once, whileDrawn, 0.01f);
}

}  // namespace
}  // namespace sz::ui
