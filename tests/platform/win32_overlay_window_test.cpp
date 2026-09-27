#include "platform/win32/win32_overlay_window.h"

#include <vector>

#include <gtest/gtest.h>

#include <windows.h>

namespace sz::platform::win32 {
namespace {

TEST(Win32OverlayWindowTest, AKeyIsNamedTheWayABindingNamesIt) {
    EXPECT_EQ(KeyForVirtualKey('Z'), 'Z');
    EXPECT_EQ(KeyForVirtualKey('7'), '7');
    EXPECT_EQ(KeyForVirtualKey(VK_F1), KeyCombo::kFunctionKeyBase + 1);
    EXPECT_EQ(KeyForVirtualKey(VK_F24), KeyCombo::kFunctionKeyBase + 24);
    EXPECT_EQ(KeyForVirtualKey(VK_ESCAPE), KeyCombo::kEscape);
    EXPECT_EQ(KeyForVirtualKey(VK_BACK), KeyCombo::kBackspace);
    EXPECT_EQ(KeyForVirtualKey(VK_DOWN), KeyCombo::kDownArrow);
    // The modifiers are not keys here but what every event carries; the
    // number pad is keys of its own, as it is to ImGui.
    EXPECT_EQ(KeyForVirtualKey(VK_CONTROL), 0);
    EXPECT_EQ(KeyForVirtualKey(VK_LSHIFT), 0);
    EXPECT_EQ(KeyForVirtualKey(VK_NUMPAD7), 0);
}

// The window's messages, the way the input grab posts them or the OS sends
// them, come out as one stream in the order they went in - a key between
// two moves of a drag between them - each told apart and nothing merged.
TEST(Win32OverlayWindowTest, EveryInputMessageArrivesAsOneEventInOrder) {
    Win32OverlayWindow window;
    window.Initialize(GetModuleHandleW(nullptr));
    DisplayInfo display;
    display.width = 320;
    display.height = 240;
    ASSERT_TRUE(window.EnsureCreated(display));
    // Shown, as only a shown window hands its input on - click-through and
    // without focus, so nothing else on the desktop is disturbed.
    window.Present(Presentation::ClickThrough);
    std::vector<InputEvent> events;
    window.SetInputCallback([&events](const InputEvent& event) {
        // Not whatever the real keyboard holds, nor the frames' ticks.
        if (event.kind != InputEventKind::Modifiers && event.kind != InputEventKind::Tick) {
            events.push_back(event);
        }
    });
    const HWND hwnd = FindWindowW(L"SpickzettelOverlayWindowClass", nullptr);
    ASSERT_NE(hwnd, nullptr);

    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20));
    SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(11, 21));
    SendMessageW(hwnd, WM_KEYDOWN, 'Z', 1);
    SendMessageW(hwnd, WM_KEYDOWN, 'Z', 1);  // posted by the grab: a repeat says nothing of itself
    SendMessageW(hwnd, WM_KEYUP, 'Z', 1 | (1LL << 30) | (1LL << 31));
    SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON | MK_RBUTTON, MAKELPARAM(12, 22));
    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(12, 22));
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(13, 23));
    SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), MAKELPARAM(13, 23));
    SendMessageW(hwnd, WM_MBUTTONUP, 0, MAKELPARAM(13, 23));
    POINT screen{13, 23};
    ClientToScreen(hwnd, &screen);
    SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA / 2), MAKELPARAM(screen.x, screen.y));
    SendMessageW(hwnd, WM_KEYDOWN, VK_DELETE, 1);
    SendMessageW(hwnd, WM_KEYDOWN, VK_DELETE, 1 | (1LL << 30));  // the OS's own repeat
    SendMessageW(hwnd, WM_KEYDOWN, VK_SHIFT, 1);  // no key of its own

    ASSERT_EQ(events.size(), 13u);
    const auto is = [&](size_t i, InputEventKind kind) {
        EXPECT_EQ(events[i].kind, kind) << "event " << i;
        return events[i];
    };
    EXPECT_EQ(is(0, InputEventKind::PointerDown).button, MouseButton::Left);
    EXPECT_EQ(is(0, InputEventKind::PointerDown).position.x, 10.0f);
    EXPECT_EQ(is(1, InputEventKind::PointerMove).buttons, ButtonBit(MouseButton::Left));
    EXPECT_EQ(is(2, InputEventKind::KeyDown).key, 'Z');
    EXPECT_FALSE(events[2].repeat);
    EXPECT_TRUE(is(3, InputEventKind::KeyDown).repeat);
    EXPECT_EQ(is(4, InputEventKind::KeyUp).key, 'Z');
    EXPECT_EQ(is(5, InputEventKind::PointerMove).buttons, ButtonBit(MouseButton::Left) | ButtonBit(MouseButton::Right));
    EXPECT_EQ(is(6, InputEventKind::PointerUp).button, MouseButton::Left);
    EXPECT_EQ(is(7, InputEventKind::PointerMove).buttons, 0);  // a hover
    EXPECT_EQ(is(8, InputEventKind::PointerDown).button, MouseButton::X2);
    EXPECT_EQ(is(9, InputEventKind::PointerUp).button, MouseButton::Middle);
    EXPECT_EQ(is(10, InputEventKind::Wheel).wheel, -0.5f);
    EXPECT_EQ(events[10].position.x, 13.0f);
    EXPECT_EQ(events[10].position.y, 23.0f);
    EXPECT_FALSE(is(11, InputEventKind::KeyDown).repeat);
    EXPECT_TRUE(is(12, InputEventKind::KeyDown).repeat);
    for (size_t i = 1; i < events.size(); ++i) {
        EXPECT_GE(events[i].seconds, events[i - 1].seconds) << "event " << i;
    }

    // Hidden, it is nobody's input: the grab hands on the key of the hotkey
    // that hid the window after the hide.
    window.Present(Presentation::Hidden);
    events.clear();
    SendMessageW(hwnd, WM_KEYDOWN, 'S', 1);
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20));
    EXPECT_TRUE(events.empty());

    window.Destroy();
}

// A button held while the capture is taken away - Alt+Tab mid-drag, the
// other window taking the pointer - goes up where the pointer was last:
// its real up goes to that other window. Unheard, the drag went on, the
// snippet following the pointer until the next click. An up of the
// window's own, from letting go of the capture, is not one.
TEST(Win32OverlayWindowTest, AButtonHeldWhenTheCaptureIsLostGoesUp) {
    Win32OverlayWindow window;
    window.Initialize(GetModuleHandleW(nullptr));
    DisplayInfo display;
    display.width = 320;
    display.height = 240;
    ASSERT_TRUE(window.EnsureCreated(display));
    window.Present(Presentation::ClickThrough);
    std::vector<InputEvent> events;
    window.SetInputCallback([&events](const InputEvent& event) {
        if (event.kind == InputEventKind::PointerDown || event.kind == InputEventKind::PointerUp) {
            events.push_back(event);
        }
    });
    const HWND hwnd = FindWindowW(L"SpickzettelOverlayWindowClass", nullptr);
    ASSERT_NE(hwnd, nullptr);
    const HWND other = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(other, nullptr);
    const auto pump = [] {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&msg);
        }
    };

    // An ordinary click: one down, one up.
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20));
    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(10, 20));
    pump();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].kind, InputEventKind::PointerUp);

    // Held, and the capture taken.
    events.clear();
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 40));
    ASSERT_EQ(GetCapture(), hwnd);
    SetCapture(other);
    pump();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].kind, InputEventKind::PointerUp);
    EXPECT_EQ(events[1].button, MouseButton::Left);
    EXPECT_EQ(events[1].position.x, 30.0f);
    EXPECT_EQ(events[1].position.y, 40.0f);
    // Should its up come after all, it is told once.
    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(50, 60));
    EXPECT_EQ(events.size(), 2u);

    ReleaseCapture();
    DestroyWindow(other);
    window.Destroy();
}

}  // namespace
}  // namespace sz::platform::win32
