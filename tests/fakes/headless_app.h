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
//  - ImGui's own event queue (MoveTo, Click) is what widgets see, and
//    what decides io.WantCaptureMouse;
//  - the platform input stream (RawMouse and friends) is what the canvas,
//    the commands and the wheel run on, as each event arrives - see
//    OverlayApp::OnInput.
// A gesture on the canvas needs both, in step, which is what Drag does. A
// key, a modifier, the wheel and the middle and side buttons go to both at
// once (KeyEvent, MouseButtonEvent, WheelEvent), as the real window sends
// them.

#include <imgui.h>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

#include "app/tray_app.h"
#include "fakes/fake_platform_host.h"
#include "ui/overlay_app_internal.h"
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

        modifiers_ = platform::Modifiers{};
        sentModifiers_ = platform::Modifiers{};
        pointer_ = platform::Vec2{};
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
        // A modifier changed with no event to carry it - let go of while the
        // overlay was hidden, say - is told at the next frame, as the real
        // window does (see Win32OverlayWindow::RenderFrame).
        if (modifiers_ != sentModifiers_) {
            platform::InputEvent event;
            event.kind = platform::InputEventKind::Modifiers;
            SendInput(event);
        }
        // The frame's time passing, as the real window tells it.
        platform::InputEvent tick;
        tick.kind = platform::InputEventKind::Tick;
        SendInput(tick);
        ImGui::NewFrame();
        if (host_.overlayWindow.frameCallback) {
            host_.overlayWindow.frameCallback(ImGui::GetIO().DeltaTime);
        }
        ImGui::Render();
        // What the frame asked to happen after it - see IPlatformHost::Post.
        host_.RunPostedTasks();
    }
    void StepFrames(int count) {
        for (int i = 0; i < count; ++i) {
            StepFrame();
        }
    }

    // ===== Input =====

    // Where ImGui thinks the mouse is. Takes effect on the next frame.
    void MoveTo(float x, float y) {
        ImGui::GetIO().AddMousePosEvent(x, y);
        pointer_ = platform::Vec2{x, y};
    }

    // A key going down or up, as the window hands it on: to ImGui, and into
    // the input stream - a modifier as what every event after it carries.
    void KeyEvent(ImGuiKey key, bool down) {
        ImGui::GetIO().AddKeyEvent(key, down);
        platform::InputEvent event;
        if (key == ImGuiMod_Ctrl || key == ImGuiMod_Shift || key == ImGuiMod_Alt || key == ImGuiMod_Super) {
            bool& held = key == ImGuiMod_Ctrl    ? modifiers_.ctrl
                         : key == ImGuiMod_Shift ? modifiers_.shift
                         : key == ImGuiMod_Alt   ? modifiers_.alt
                                                 : modifiers_.super;
            held = down;
            event.kind = platform::InputEventKind::Modifiers;
            SendInput(event);
            return;
        }
        event.key = KeyForImGuiKey(key);
        if (event.key == 0) {
            return;  // a key the window does not name - see InputEvent::key
        }
        event.kind = down ? platform::InputEventKind::KeyDown : platform::InputEventKind::KeyUp;
        SendInput(event);
    }

    // A mouse button going down or up where ImGui's pointer is. The left
    // and right ones to ImGui alone - a press on the canvas is RawMouse's -
    // and the middle and side ones, which no gesture is made with, to the
    // input stream as well.
    void MouseButtonEvent(ImGuiMouseButton button, bool down) {
        ImGui::GetIO().AddMouseButtonEvent(button, down);
        if (button == ImGuiMouseButton_Left || button == ImGuiMouseButton_Right) {
            return;
        }
        platform::InputEvent event;
        event.kind = down ? platform::InputEventKind::PointerDown : platform::InputEventKind::PointerUp;
        event.position = pointer_;
        event.button = button == ImGuiMouseButton_Middle ? platform::MouseButton::Middle
                       : button == 3                     ? platform::MouseButton::X1
                                                         : platform::MouseButton::X2;
        SendInput(event);
    }

    // Wheel notches, where the pointer is.
    void WheelEvent(float notches) {
        ImGui::GetIO().AddMouseWheelEvent(0.0f, notches);
        platform::InputEvent event;
        event.kind = platform::InputEventKind::Wheel;
        event.position = pointer_;
        event.wheel = notches;
        SendInput(event);
    }

    // KeyCombo's name for an ImGui key, or 0 - the window's own
    // KeyForVirtualKey, from the other side.
    static int KeyForImGuiKey(ImGuiKey key) {
        if (const std::optional<platform::KeyCombo> combo =
                overlay_detail::ComboForImGuiKey(key, false, false, false)) {
            return combo->key;
        }
        switch (key) {
            case ImGuiKey_Escape:
                return platform::KeyCombo::kEscape;
            case ImGuiKey_Delete:
                return platform::KeyCombo::kDelete;
            case ImGuiKey_Backspace:
                return platform::KeyCombo::kBackspace;
            case ImGuiKey_LeftArrow:
                return platform::KeyCombo::kLeftArrow;
            case ImGuiKey_RightArrow:
                return platform::KeyCombo::kRightArrow;
            case ImGuiKey_UpArrow:
                return platform::KeyCombo::kUpArrow;
            case ImGuiKey_DownArrow:
                return platform::KeyCombo::kDownArrow;
            default:
                return 0;
        }
    }

    // A widget click. Two frames because ImGui splits a press and a release
    // that arrive together across frames rather than losing one of them, and
    // a third so whatever the click opened has been through a frame.
    void Click(float x, float y) {
        MoveTo(x, y);
        StepFrame();
        MouseButtonEvent(ImGuiMouseButton_Left, true);
        StepFrame();
        MouseButtonEvent(ImGuiMouseButton_Left, false);
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
        WheelEvent(notches);
        StepFrame();
    }

    void PressKey(ImGuiKey key) {
        KeyEvent(key, true);
        StepFrame();
        KeyEvent(key, false);
        StepFrame();
    }

    // The raw pipeline: what a stroke or an item placement is made of. Kept
    // in step with ImGui's own idea of the pointer, because the handlers
    // ask both (WantCaptureMouse decides whether a press over a widget also
    // starts a stroke underneath it).
    void RawMouse(float x, float y, platform::MouseEventKind kind,
                   platform::MouseButton button = platform::MouseButton::Left) {
        MoveTo(x, y);
        platform::InputEvent event;
        event.kind = kind == platform::MouseEventKind::Down ? platform::InputEventKind::PointerDown
                     : kind == platform::MouseEventKind::Up ? platform::InputEventKind::PointerUp
                                                            : platform::InputEventKind::PointerMove;
        event.position = platform::Vec2{x, y};
        event.button = button;
        event.buttons = kind == platform::MouseEventKind::Move ? platform::ButtonBit(button) : 0;
        SendInput(event);
    }

    // Into the platform's input stream, stamped with ImGui's clock and the
    // modifiers held - while the window is up, as a real one does it.
    void SendInput(platform::InputEvent event) {
        if (!host_.overlayWindow.visible) {
            return;
        }
        event.seconds = ImGui::GetTime();
        event.modifiers = modifiers_;
        sentModifiers_ = modifiers_;
        if (host_.overlayWindow.inputCallback) {
            host_.overlayWindow.inputCallback(event);
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
        KeyEvent(modifier, true);
        StepFrame();
        Drag(fromX, fromY, toX, toY);
        KeyEvent(modifier, false);
        StepFrame();
    }

    // A key with Ctrl held - Ctrl+Z, Ctrl+Y.
    void PressCtrlKey(ImGuiKey key) {
        KeyEvent(ImGuiMod_Ctrl, true);
        KeyEvent(key, true);
        StepFrame();
        KeyEvent(key, false);
        KeyEvent(ImGuiMod_Ctrl, false);
        StepFrame();
    }

    // A key with Ctrl and Shift held - Ctrl+Shift+N.
    void PressCtrlShiftKey(ImGuiKey key) {
        KeyEvent(ImGuiMod_Ctrl, true);
        KeyEvent(ImGuiMod_Shift, true);
        KeyEvent(key, true);
        StepFrame();
        KeyEvent(key, false);
        KeyEvent(ImGuiMod_Shift, false);
        KeyEvent(ImGuiMod_Ctrl, false);
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
        KeyEvent(modifier, true);
        StepFrame();
        gesture();
        KeyEvent(modifier, false);
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
        size_t strokes = AppSession().LiveLayer().Strokes().size();
        for (const Item& item : canvas->items) {
            strokes += item.strokes.size();
        }
        return strokes;
    }

    static constexpr float kDisplayWidth = 1280.0f;
    static constexpr float kDisplayHeight = 768.0f;

    sz::test::FakePlatformHost host_;
    // The modifiers held, and where the pointer is - what the input stream
    // is told - and the modifiers it was last told.
    platform::Modifiers modifiers_;
    platform::Modifiers sentModifiers_;
    platform::Vec2 pointer_;
    AppConfig config_;
    std::unique_ptr<TrayController> controller_;
    ImGuiContext* context_ = nullptr;
};

}  // namespace sz::test
