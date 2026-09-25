#include "platform/win32/win32_screen_capture.h"

#include <string>

#include <gtest/gtest.h>

#include <dwmapi.h>

namespace sz::platform::win32 {
namespace {

// A small solid window in the top-left corner, topmost, the color the
// captures below look for.
class RedWindow {
public:
    RedWindow() {
        WNDCLASSEXA windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcA;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        windowClass.hbrBackground = CreateSolidBrush(RGB(255, 0, 0));
        windowClass.lpszClassName = "SpickzettelCaptureTestRed";
        RegisterClassExA(&windowClass);
        hwnd_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, windowClass.lpszClassName, "", WS_POPUP, kX, kY,
                                kSize, kSize, nullptr, nullptr, windowClass.hInstance, nullptr);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd_);
        DwmFlush();
        DwmFlush();
    }
    ~RedWindow() { DestroyWindow(hwnd_); }
    HWND Handle() const { return hwnd_; }

    static constexpr int kX = 40;
    static constexpr int kY = 40;
    static constexpr int kSize = 64;

private:
    HWND hwnd_ = nullptr;
};

// How much of a capture is the red window's red.
double RedShare(const std::vector<uint8_t>& bgra) {
    size_t red = 0;
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        red += bgra[i + 2] > 240 && bgra[i + 1] < 16 && bgra[i + 0] < 16 ? 1 : 0;
    }
    return static_cast<double>(red) / static_cast<double>(bgra.size() / 4);
}

// The overlay is left out of its own capture without being hidden - which
// handed the game focus and took it back - and is in the next one again,
// as it is on screen.
TEST(Win32ScreenCaptureTest, AWindowLeftOutIsNotInTheCaptureAndStaysUp) {
    const RedWindow window;
    ASSERT_NE(window.Handle(), nullptr);
    const POINT inside{RedWindow::kX + 8, RedWindow::kY + 8};
    constexpr int kInner = RedWindow::kSize - 16;
    std::vector<uint8_t> pixels;

    ASSERT_TRUE(CaptureScreen(nullptr, inside, kInner, kInner, pixels));
    ASSERT_GT(RedShare(pixels), 0.9) << "the window is there to be captured";

    ASSERT_TRUE(CaptureScreen(window.Handle(), inside, kInner, kInner, pixels));
    EXPECT_LT(RedShare(pixels), 0.1) << "and left out when asked";
    EXPECT_TRUE(IsWindowVisible(window.Handle()));
    DWORD affinity = WDA_MONITOR;
    ASSERT_TRUE(GetWindowDisplayAffinity(window.Handle(), &affinity));
    EXPECT_EQ(affinity, static_cast<DWORD>(WDA_NONE)) << "only for the capture";
}

}  // namespace
}  // namespace sz::platform::win32
