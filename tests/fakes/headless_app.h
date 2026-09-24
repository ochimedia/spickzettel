#pragma once

// The whole app, running in a unit test: a real OverlayApp driven through a
// real ImGui frame, with a fake platform underneath it and nothing drawn
// anywhere.
//
// ImGui needs a context and a font atlas; it does not need a window, a
// backend or a GPU, because nothing here submits the draw data. That makes
// every path the app has - including the ones that only exist inside a
// render function - reachable from an ordinary test, and it is what these
// tests are for: asserting on what the app *did* rather than on what a
// screenshot looks like.
//
// Input arrives the same two ways it does in the real app, and the
// difference matters:
//  - ImGui's own event queue (Move/Click/Key below) is what widgets see, and
//    what decides io.WantCaptureMouse;
//  - the platform mouse callback (Press/Release/DragTo) is the raw pipeline
//    that drawing and item placement run on, deliberately decoupled from
//    the frame rate - see OverlayApp's class comment.
// A gesture on the canvas needs both, in step, which is what Drag does.

#include <imgui.h>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

#include "app/tray_app.h"
#include "fakes/fake_platform_host.h"
#include "ui/ui_scale.h"

namespace sz::test {

// The tests speak every layer's vocabulary at once.
using namespace ::sz::core;
using namespace ::sz::ui;
using namespace ::sz::app;

class HeadlessAppTest : public ::testing::Test {
protected:
    void SetUp() override { StartWith(DefaultConfig()); }
    void TearDown() override { Shutdown(); }

    // Everything StartWith and TearDown both have to undo. Virtual because
    // a subclass may own something bound to the ImGui context (the test
    // engine does) and has to let go of it first.
    virtual void Shutdown() {
        controller_.reset();
        if (context_ != nullptr) {
            ImGui::DestroyContext(context_);
            context_ = nullptr;
        }
        // The interface scale is one value for the process (see UiScale),
        // and a test that set it must not hand it to the next one.
        SetUiScale(1.0f);
    }

    // Re-runs setup with a different config - for the tests that need a
    // profile, or a shortcut, in place before anything starts.
    void StartWith(AppConfig config) {
        Shutdown();
        // The fake outlives the controller (it is a fixture member, so a
        // test can set up what is underneath before starting), and what
        // the last one registered still points into it - see
        // ForgetRegistrations.
        host_.ForgetRegistrations();
        IMGUI_CHECKVERSION();
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(kDisplayWidth, kDisplayHeight);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;  // no imgui.ini left behind by a test run
        io.Fonts->AddFontDefault();
        // Claiming the renderer manages textures is honest here: nothing is
        // uploaded because nothing is drawn, and without it ImGui asserts
        // that some backend should have built the atlas.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

        config_ = std::move(config);
        controller_ = std::make_unique<TrayController>(host_, config_);
        ASSERT_TRUE(controller_->Initialize());
        OnStarted();
    }

    // For a subclass that has to attach something to the fresh ImGui
    // context before any frame is drawn - the test engine counts frames
    // from the moment it is bound and complains if it has missed some.
    virtual void OnStarted() {}

    // A library with nothing in it at all - legal, reachable by deleting
    // the last canvas (see CanvasManager's class comment), and the state
    // most likely to be missing a null check. A fresh start has one empty
    // canvas instead, so this has to be arranged.
    void StartWithEmptyLibrary() {
        StartWith(DefaultConfig());
        controller_->GetSession().ImportLibrary(CanvasManagerSnapshot{});
    }

    const OverlayApp& App() const { return controller_->Overlay(); }
    const CanvasManager& Canvases() const { return AppSession().Manager(); }
    // The settings as the app holds them - stored, and resolved against the
    // profile in effect. Where a settings edit is checked.
    const Settings& AppSettings() const { return controller_->GetSettings(); }
    // What is being worked on - the library, and whether deleted things show.
    const Session& AppSession() const { return controller_->GetSession(); }

    // ===== Getting the overlay on screen =====

    void ShowEditMode() { TriggerHotkey(config_.hotkeyEditMode); }
    void ShowViewMode() { TriggerHotkey(config_.hotkeyViewMode); }

    void TriggerHotkey(const platform::KeyCombo& combo) {
        for (const auto& [id, registered] : host_.registeredCombos) {
            if (registered == combo) {
                host_.TriggerHotkey(id);
                return;
            }
        }
        FAIL() << "no such hotkey registered";
    }

    // ===== Frames =====

    void StepFrame() {
        ImGui::NewFrame();
        if (host_.overlayWindow.frameCallback) {
            host_.overlayWindow.frameCallback(ImGui::GetIO().DeltaTime);
        }
        ImGui::Render();
    }
    void StepFrames(int count) {
        for (int i = 0; i < count; ++i) {
            StepFrame();
        }
    }

    // ===== Input =====

    // Where ImGui thinks the mouse is. Takes effect on the next frame.
    void MoveTo(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }

    // A widget click. Two frames because ImGui splits a press and a release
    // that arrive together across frames rather than losing one of them, and
    // a third so whatever the click opened has been through a frame.
    void Click(float x, float y) {
        MoveTo(x, y);
        StepFrame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        StepFrame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        StepFrames(2);
    }

    // A click on the canvas - an item's chrome, say - which is the raw
    // pipeline's press and release (see RawMouse), with a frame between so
    // the app sees the pointer before the press, the way it does for real.
    void RawClick(float x, float y) {
        MoveTo(x, y);
        StepFrame();
        RawMouse(x, y, platform::MouseEventKind::Down);
        StepFrame();
        RawMouse(x, y, platform::MouseEventKind::Up);
        StepFrames(2);
    }

    // The mouse wheel, in notches, with the pointer where it is.
    void Wheel(float notches) {
        ImGui::GetIO().AddMouseWheelEvent(0.0f, notches);
        StepFrame();
    }

    void PressKey(ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        StepFrame();
        ImGui::GetIO().AddKeyEvent(key, false);
        StepFrame();
    }

    // The raw pipeline: what a stroke or an item placement is made of. Kept
    // in step with ImGui's own idea of the pointer, because the handlers
    // ask both (WantCaptureMouse decides whether a press over a widget also
    // starts a stroke underneath it).
    void RawMouse(float x, float y, platform::MouseEventKind kind,
                   platform::MouseButton button = platform::MouseButton::Left) {
        MoveTo(x, y);
        if (host_.overlayWindow.mouseCallback) {
            host_.overlayWindow.mouseCallback(platform::MouseEvent{platform::Vec2{x, y}, button, kind});
        }
    }

    // One complete gesture on the canvas: press, a few moves, release.
    void Drag(float fromX, float fromY, float toX, float toY, int steps = 4,
              platform::MouseButton button = platform::MouseButton::Left) {
        MoveTo(fromX, fromY);
        StepFrame();
        RawMouse(fromX, fromY, platform::MouseEventKind::Down, button);
        StepFrame();
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            RawMouse(fromX + (toX - fromX) * t, fromY + (toY - fromY) * t,
                      platform::MouseEventKind::Move, button);
            StepFrame();
        }
        RawMouse(toX, toY, platform::MouseEventKind::Up, button);
        StepFrames(2);
    }

    // A whole drag with a modifier held from before the press until after
    // the release, the way a hand does it - ImGuiMod_Shift, ImGuiMod_Ctrl.
    void DragWith(ImGuiKey modifier, float fromX, float fromY, float toX, float toY) {
        ImGui::GetIO().AddKeyEvent(modifier, true);
        StepFrame();
        Drag(fromX, fromY, toX, toY);
        ImGui::GetIO().AddKeyEvent(modifier, false);
        StepFrame();
    }

    // A key with Ctrl held - Ctrl+Z, Ctrl+Y.
    void PressCtrlKey(ImGuiKey key) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(key, true);
        StepFrame();
        io.AddKeyEvent(key, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        StepFrame();
    }

    // A key with Ctrl and Shift held - Ctrl+Shift+N.
    void PressCtrlShiftKey(ImGuiKey key) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(key, true);
        StepFrame();
        io.AddKeyEvent(key, false);
        io.AddKeyEvent(ImGuiMod_Shift, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        StepFrame();
    }

    void RightClick(float x, float y) {
        MoveTo(x, y);
        StepFrame();
        RawMouse(x, y, platform::MouseEventKind::Down, platform::MouseButton::Right);
        StepFrame();
        RawMouse(x, y, platform::MouseEventKind::Up, platform::MouseButton::Right);
        StepFrames(2);
    }

    // Two clicks in quick succession at one point, with either button -
    // what makes a fullscreen snippet on empty canvas and enters drawing
    // mode on a snippet. Six frames apart at sixty a second: well inside
    // the double-click window.
    void DoubleClick(float x, float y, platform::MouseButton button = platform::MouseButton::Left) {
        for (int i = 0; i < 2; ++i) {
            MoveTo(x, y);
            StepFrame();
            RawMouse(x, y, platform::MouseEventKind::Down, button);
            StepFrame();
            RawMouse(x, y, platform::MouseEventKind::Up, button);
            StepFrame();
        }
        StepFrames(2);
    }
    // A press held still, and let go only after the hold has matured: what
    // a finger does instead of a double-click. Thirty-five frames at sixty
    // a second is well past kHoldSeconds.
    void Hold(float x, float y, platform::MouseButton button = platform::MouseButton::Left) {
        PressFor(x, y, 35, button);
    }
    // A press held still for this many frames, then let go.
    void PressFor(float x, float y, int frames, platform::MouseButton button = platform::MouseButton::Left) {
        MoveTo(x, y);
        StepFrame();
        RawMouse(x, y, platform::MouseEventKind::Down, button);
        StepFrames(frames);
        RawMouse(x, y, platform::MouseEventKind::Up, button);
        StepFrames(2);
    }
    // The other way in: Ctrl-drag frames a drawing (the default - see
    // AppConfig::drawingTrigger), which is then in drawing mode with the
    // pen in hand.
    void MakeADrawing(float fromX, float fromY, float toX, float toY) {
        DragWith(ImGuiMod_Ctrl, fromX, fromY, toX, toY);
    }
    // Whatever `gesture` does, done with `modifier` held from before it
    // until after it - ImGuiMod_Ctrl, ImGuiMod_Alt.
    template <class Gesture>
    void With(ImGuiKey modifier, Gesture gesture) {
        ImGui::GetIO().AddKeyEvent(modifier, true);
        StepFrame();
        gesture();
        ImGui::GetIO().AddKeyEvent(modifier, false);
        StepFrame();
    }

    // ===== Handy assertions =====

    // What is on the current canvas to be seen: a deleted snippet stays in
    // canvas->items, hidden, and does not count here.
    size_t ItemCountOnCurrentCanvas() const {
        const Canvas* canvas = Canvases().CurrentOrNull();
        if (canvas == nullptr) {
            return 0;
        }
        size_t count = 0;
        for (const Item& item : canvas->items) {
            if (!Canvases().IsDeleted(*canvas, item)) {
                ++count;
            }
        }
        return count;
    }
    size_t StrokeCountOnCurrentCanvas() const {
        const Canvas* canvas = Canvases().CurrentOrNull();
        if (canvas == nullptr) {
            return 0;
        }
        size_t strokes = canvas->liveLayer.Strokes().size();
        for (const Item& item : canvas->items) {
            strokes += item.strokes.size();
        }
        return strokes;
    }

    static constexpr float kDisplayWidth = 1280.0f;
    static constexpr float kDisplayHeight = 768.0f;

    sz::test::FakePlatformHost host_;
    AppConfig config_;
    std::unique_ptr<TrayController> controller_;
    ImGuiContext* context_ = nullptr;
};

}  // namespace sz::test
