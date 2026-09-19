// The input grab against the real Windows registration API. Its hooks and
// its raw-input sink live on a thread of their own that comes and goes with
// the grab, and whether what that thread set up is really there is a
// question only Windows can answer - see HookThreadMain for the lifecycle
// bug this exists to keep out.
//
// While the grab is active here, a low-level mouse hook is installed in
// this process and swallows every mouse event on the machine, exactly as
// it would under the overlay. The activations below are as short as
// Windows lets them be; a failing run costs a second or two of dead mouse.
#include "platform/win32/win32_input_grab.h"

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <windows.h>

namespace sz::platform::win32 {
namespace {

struct RawMouseRegistration {
    bool present = false;          // this process has registered for raw mouse input at all
    bool targetIsAWindow = false;  // ...and the window it asked to be told through still exists
};

RawMouseRegistration QueryRawMouse() {
    UINT count = 0;
    GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
    std::vector<RAWINPUTDEVICE> devices(count);
    if (count > 0) {
        GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
    }
    RawMouseRegistration result;
    for (const RAWINPUTDEVICE& device : devices) {
        if (device.usUsagePage == 0x01 && device.usUsage == 0x02) {
            result.present = true;
            result.targetIsAWindow = IsWindow(device.hwndTarget) != 0;
        }
    }
    return result;
}

// The hook thread does its work asynchronously; this is the wait for it.
bool Eventually(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

TEST(Win32InputGrabTest, RawMouseInputIsRegisteredAgainAfterHideAndShow) {
    // A message-only window stands in for the overlay: the grab only needs
    // something to post to.
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);

    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    options.dontForwardKeystrokes = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);
    // The pointer grab only engages against a game that keeps focus - which
    // is why ordinary desktop use never showed this.
    grab.SetGameKeepsFocus(true);
    const auto limit = std::chrono::milliseconds(1500);

    grab.SetActive(true);
    EXPECT_TRUE(Eventually([] { return QueryRawMouse().targetIsAWindow; }, limit)) << "first show";

    grab.SetActive(false);
    EXPECT_TRUE(Eventually([] { return !QueryRawMouse().present; }, limit)) << "hidden";

    grab.SetActive(true);
    EXPECT_TRUE(Eventually([] { return QueryRawMouse().targetIsAWindow; }, limit))
        << "second show: the sink from the first thread was remembered after Windows destroyed it";

    grab.SetActive(false);
    grab.Shutdown();
    DestroyWindow(overlay);
}

}  // namespace
}  // namespace sz::platform::win32
