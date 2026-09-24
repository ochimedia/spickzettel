#pragma once

// The headless app plus Dear ImGui's test engine, so a test can click a
// widget by *name*.
//
// The difference from the fixture it extends is the difference between
// "put the mouse at 505,230 and hope" and "click the thing labeled
// Settings". It also reaches the z-order bugs, which are the shape of most
// of the ones this UI has had: the engine clicks where the widget is, so a
// widget with something else in front of it takes no effect - a combo that
// opened behind the Overview panel read as a control that did nothing, and
// cost a screenshot and a guess to find.
//
// Only built when SPICKZETTEL_UI_TESTS is on, which is the debug preset -
// see cmake/FetchImGuiTestEngine.cmake for why it isn't on everywhere.

#include <imgui_test_engine/imgui_te_context.h>
#include <imgui_test_engine/imgui_te_engine.h>

#include <functional>

#include "fakes/headless_app.h"

namespace sz::test {

class UiTest : public HeadlessAppTest {
protected:
    void Shutdown() override {
        // Stopped first: it holds a coroutine that may still be parked
        // inside a frame, and stopping it unwinds that. Destroyed last,
        // *after* the ImGui context - the engine asserts on that order,
        // since it unhooks itself from the context on the way out.
        if (engine_ != nullptr) {
            ImGuiTestEngine_Stop(engine_);
        }
        HeadlessAppTest::Shutdown();
        if (engine_ != nullptr) {
            ImGuiTestEngine_DestroyContext(engine_);
            engine_ = nullptr;
        }
    }

    // Bound to the context before the first frame: the engine counts frames
    // from the moment it starts, and aborts if the app has run more than a
    // couple without it - which is how it tells "not compiled with the
    // hooks" from "started late", and it cannot tell them apart.
    void OnStarted() override { StartEngine(); }

    // Runs one test body against the live app and fails the gtest if it
    // didn't pass. `body` is an ordinary lambda: it may capture the
    // fixture, and it runs on the engine's coroutine, yielding a frame
    // whenever it needs the app to catch up.
    void RunUi(const char* name, std::function<void(ImGuiTestContext*)> body) {
        ImGuiTest* test = ImGuiTestEngine_RegisterTest(engine_, "spickzettel", name, __FILE__, __LINE__);
        test->TestFunc = [body](ImGuiTestContext* ctx) { body(ctx); };
        ImGuiTestEngine_QueueTest(engine_, test);

        // The engine drives the app by yielding back to this loop, so the
        // frames have to keep coming until its queue is empty. The cap is
        // there so a test that never finishes fails instead of hanging the
        // suite.
        constexpr int kFrameLimit = 2000;
        int frames = 0;
        while (!ImGuiTestEngine_IsTestQueueEmpty(engine_) && frames++ < kFrameLimit) {
            StepFrame();
            ImGuiTestEngine_PostSwap(engine_);
        }
        EXPECT_LT(frames, kFrameLimit) << "UI test '" << name << "' never finished";
        EXPECT_EQ(test->Output.Status, ImGuiTestStatus_Success)
            << "UI test '" << name << "' failed - engine log:\n"
            << test->Output.Log.Buffer.c_str();
    }

    // The Overview has no key of its own: it is opened from the canvas
    // bar's button, and the bar comes out when the pointer reaches the
    // bottom edge - so
    // it is brought out first, then clicked by name. Leaves the Overview up
    // on the Canvases tab, which is where every test below starts.
    void OpenOverviewUi() {
        RunUi("open the overview", [this](ImGuiTestContext* ctx) {
            ctx->MouseMoveToPos(ImVec2(kDisplayWidth * 0.5f, kDisplayHeight - 1.0f));
            ctx->Yield(30);
            IM_CHECK(App().CanvasBarReveal() >= 1.0f);
            ctx->SetRef("//##canvas_bar");
            ctx->ItemClick("##canvasbar_overview");
        });
        ASSERT_TRUE(App().IsOverviewOpen());
    }

private:
    void StartEngine() {
        engine_ = ImGuiTestEngine_CreateContext();
        ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(engine_);
        // No throttling and no sleeping: this is a unit test, not a demo of
        // itself, and the frames are already free.
        io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
        io.ConfigNoThrottle = true;
        io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Warning;
        io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Info;
        ImGuiTestEngine_Start(engine_, ImGui::GetCurrentContext());
    }

    ImGuiTestEngine* engine_ = nullptr;
};

}  // namespace sz::test
