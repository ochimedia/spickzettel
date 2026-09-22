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

// A tile dragged onto another takes its place, the others shifting along.
// A deleted canvas between them is not on the bar but still counts in the
// folder's order, which is what the move is made in - the tile has to land
// where it was dropped all the same.
TEST_F(UiTest, DraggingATileOntoAnotherMovesItThere) {
    ShowEditMode();
    StepFrame();
    CanvasManager& manager = controller_->GetSession().Manager();
    const CanvasId a = manager.CurrentCanvasId();
    const CanvasId b = manager.AddCanvas("B");
    const CanvasId gone = manager.AddCanvas("Gone");
    const CanvasId c = manager.AddCanvas("C");
    ASSERT_TRUE(controller_->GetSession().Delete(gone));
    StepFrame();
    // What the bar shows, in its order: the folder's canvases, deleted ones
    // left out.
    const auto onTheBar = [this] {
        std::vector<CanvasId> ids;
        for (const Canvas& canvas : Canvases().Canvases()) {
            if (!Canvases().IsDeleted(canvas)) {
                ids.push_back(canvas.id);
            }
        }
        return ids;
    };
    ASSERT_EQ(onTheBar(), (std::vector<CanvasId>{a, b, c}));

    RunUi("drag the first tile onto the last", [this](ImGuiTestContext* ctx) {
        ctx->MouseMoveToPos(ImVec2(kDisplayWidth * 0.5f, kDisplayHeight - 1.0f));
        ctx->Yield(30);
        IM_CHECK(App().CanvasBarReveal() >= 1.0f);
        ctx->SetRef("//##canvas_bar");
        ctx->ItemDragAndDrop("##canvasbar_tile_0", "##canvasbar_tile_2");
        ctx->Yield(2);
    });
    EXPECT_EQ(onTheBar(), (std::vector<CanvasId>{b, c, a}));

    RunUi("and back to the front", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##canvas_bar");
        ctx->ItemDragAndDrop("##canvasbar_tile_2", "##canvasbar_tile_0");
        ctx->Yield(2);
    });
    EXPECT_EQ(onTheBar(), (std::vector<CanvasId>{a, b, c}));
    EXPECT_EQ(Canvases().CurrentCanvasId(), a) << "a drag is not a click: no switch";
}

}  // namespace
}  // namespace sz::test
