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
#include <tlhelp32.h>

namespace sz::platform::win32 {
namespace {

// How many threads this process has - the hook thread is one of them
// while the grab is active, and none of them once it is not.
DWORD ProcessThreadCount() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        ADD_FAILURE() << "cannot enumerate the process's threads";
        return 0;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    DWORD count = 0;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
                ++count;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

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

// A start followed at once by a stop, many times over: the stop's quit
// message must reach a queue that exists, and every thread started must be
// gone by the time the grab says it is inactive - or a hook survives on a
// thread nothing owns.
TEST(Win32InputGrabTest, RapidStartAndStopLeavesNoHookBehind) {
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
    grab.SetGameKeepsFocus(true);

    const DWORD threadsBefore = ProcessThreadCount();
    for (int i = 0; i < 20; ++i) {
        grab.SetActive(true);
        grab.SetActive(false);
    }
    EXPECT_TRUE(Eventually([] { return !QueryRawMouse().present; }, std::chrono::milliseconds(1500)))
        << "a sink registered by a thread that outlived its stop";
    EXPECT_TRUE(Eventually([threadsBefore] { return ProcessThreadCount() == threadsBefore; },
                           std::chrono::milliseconds(1500)))
        << "a stop left its hook thread running: " << ProcessThreadCount() << " threads, " << threadsBefore
        << " before";

    // ...and a start after all that still works.
    grab.SetActive(true);
    EXPECT_TRUE(Eventually([] { return QueryRawMouse().targetIsAWindow; }, std::chrono::milliseconds(1500)));
    grab.SetActive(false);
    grab.Shutdown();
    DestroyWindow(overlay);
}

// AltGr is Ctrl+Alt to Windows - a left Ctrl it makes up, and the right Alt.
// Handed back as the generic keys, the right Alt came back as a left Alt,
// whose key-up never follows, and stayed down on the whole desktop.
TEST(Win32InputGrabTest, HeldModifiersAreHandedBackAsTheVeryKeysThatWereHeld) {
    bool swallowed[256] = {};
    swallowed[VK_LCONTROL] = true;
    swallowed[VK_RMENU] = true;
    swallowed['O'] = true;  // not a modifier: never typed into whatever has focus

    const std::vector<INPUT> keys = Win32InputGrab::ModifierHandBack(swallowed);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0].ki.wVk, VK_LCONTROL);
    EXPECT_EQ(keys[0].ki.dwFlags & KEYEVENTF_EXTENDEDKEY, 0u);
    EXPECT_EQ(keys[1].ki.wVk, VK_RMENU);
    EXPECT_NE(keys[1].ki.dwFlags & KEYEVENTF_EXTENDEDKEY, 0u) << "the right Alt, not the left";
    for (const INPUT& key : keys) {
        EXPECT_EQ(key.type, static_cast<DWORD>(INPUT_KEYBOARD));
        EXPECT_EQ(key.ki.dwFlags & KEYEVENTF_KEYUP, 0u) << "downs: the ups are the user's own";
        EXPECT_NE(key.ki.wScan, 0) << "with a scan code, as a real key has";
    }
}

// The machine's input is only held while the app thread is there to give it
// back: a couple of seconds without a frame and the hooks let everything by.
TEST(Win32InputGrabTest, TheHooksStandDownWhenTheAppThreadStopsBeating) {
    const uint64_t beat = 1'000'000;
    EXPECT_FALSE(Win32InputGrab::IsStalled(beat + 250, beat)) << "an idle frame's gap";
    EXPECT_FALSE(Win32InputGrab::IsStalled(beat + Win32InputGrab::kStalledAfterMs, beat));
    EXPECT_TRUE(Win32InputGrab::IsStalled(beat + Win32InputGrab::kStalledAfterMs + 1, beat));
    EXPECT_FALSE(Win32InputGrab::IsStalled(beat - 1, beat)) << "a beat newer than the clock read before it";
}

// A modifier held since before the grab reached Windows itself, and has
// nothing to be handed back.
TEST(Win32InputGrabTest, NothingSwallowedIsNothingHandedBack) {
    const bool swallowed[256] = {};
    EXPECT_TRUE(Win32InputGrab::ModifierHandBack(swallowed).empty());
}

}  // namespace
}  // namespace sz::platform::win32
