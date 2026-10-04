// The tooltips' delay and the switch for the help ones: HelpTooltip and
// InfoTooltip asked for once a frame, as the view does while the pointer
// is over what they explain.

#include "ui/widgets.h"

#include <gtest/gtest.h>

#include <imgui.h>
#include <imgui_internal.h>

namespace sz::ui {
namespace {

class TooltipTest : public ::testing::Test {
protected:
    void SetUp() override {
        IMGUI_CHECKVERSION();
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 0.1f;
        io.IniFilename = nullptr;
        io.Fonts->AddFontDefault();
        // Nothing is uploaded because nothing is presented; without this
        // ImGui asserts that some backend should have built the atlas.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        SetHelpTooltipsShown(true);
    }
    void TearDown() override {
        SetHelpTooltipsShown(true);
        ImGui::DestroyContext(context_);
        context_ = nullptr;
    }

    // One frame, a tenth of a second long, that runs `ask`; whether a
    // tooltip is up at its end.
    template <class Ask>
    bool Frame(Ask ask) {
        ImGui::NewFrame();
        ImGui::Begin("window");
        ask();
        ImGui::End();
        bool up = false;
        for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
            up = up || ((window->Flags & ImGuiWindowFlags_Tooltip) != 0 && window->Active);
        }
        ImGui::EndFrame();
        return up;
    }

    ImGuiContext* context_ = nullptr;
};

// Not at once: only once the pointer has rested on it for the delay.
TEST_F(TooltipTest, ATooltipWaitsForThePointerToRest) {
    int frames = 0;
    while (!Frame([] { HelpTooltip("%s", "what this does"); })) {
        ++frames;
        ASSERT_LT(frames, 20);
    }
    EXPECT_GE(frames * 0.1, kTooltipDelaySeconds - 0.11);
    EXPECT_TRUE(Frame([] { HelpTooltip("%s", "what this does"); })) << "and stays";
}

// Another one, or a frame with none, starts the wait again.
TEST_F(TooltipTest, MovingOnStartsTheWaitAgain) {
    for (int i = 0; i < 8; ++i) {
        Frame([] { InfoTooltip("%s", "one"); });
    }
    ASSERT_TRUE(Frame([] { InfoTooltip("%s", "one"); }));
    EXPECT_FALSE(Frame([] { InfoTooltip("%s", "the next"); }));
    for (int i = 0; i < 8; ++i) {
        Frame([] { InfoTooltip("%s", "the next"); });
    }
    ASSERT_TRUE(Frame([] { InfoTooltip("%s", "the next"); }));
    Frame([] {});
    EXPECT_FALSE(Frame([] { InfoTooltip("%s", "the next"); }));
}

// Switched off, help is never shown; information still is.
TEST_F(TooltipTest, SwitchedOffOnlyHelpIsLeftOut) {
    SetHelpTooltipsShown(false);
    bool helpUp = false;
    bool infoUp = false;
    for (int i = 0; i < 20; ++i) {
        helpUp = helpUp || Frame([] { HelpTooltip("%s", "what this does"); });
    }
    for (int i = 0; i < 20; ++i) {
        infoUp = infoUp || Frame([] { InfoTooltip("%s", "deleted yesterday"); });
    }
    EXPECT_FALSE(helpUp);
    EXPECT_TRUE(infoUp);
}

}  // namespace
}  // namespace sz::ui
