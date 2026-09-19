// The canvas bar, driven by widget name - see RenderCanvasBar.
#include "fakes/ui_test.h"

namespace sz::test {
namespace {


// Brought out by the pointer at the bottom edge, then used by name: its
// "+" makes a canvas and switches to it, and a tile switches back.
TEST_F(UiTest, TheCanvasBarMakesACanvasAndSwitchesBetweenThem) {
    ShowEditMode();
    StepFrame();
    const CanvasId first = Canvases().CurrentCanvasId();
    const size_t before = Canvases().Canvases().size();

    RunUi("canvas bar", [this, first](ImGuiTestContext* ctx) {
        ctx->MouseMoveToPos(ImVec2(kDisplayWidth * 0.5f, kDisplayHeight - 1.0f));
        ctx->Yield(30);
        IM_CHECK(App().CanvasBarReveal() >= 1.0f);

        ctx->SetRef("//##canvas_bar");
        ctx->ItemClick("##canvasbar_new");
        ctx->Yield(2);
        IM_CHECK(Canvases().CurrentCanvasId() != first);

        ctx->ItemClick("##canvasbar_tile_0");
        ctx->Yield(2);
        IM_CHECK(Canvases().CurrentCanvasId() == first);
    });

    EXPECT_EQ(Canvases().Canvases().size(), before + 1);
}

}  // namespace
}  // namespace sz::test
