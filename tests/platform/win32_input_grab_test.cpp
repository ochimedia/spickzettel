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

#include <algorithm>
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

// Each side of a modifier is held on its own: with both Shifts down, one
// let go leaves Shift held, and it is up only once both are.
TEST(Win32InputGrabTest, AModifierIsHeldWhileEitherSideIs) {
    Win32InputGrab::ModifierRecord record;
    EXPECT_TRUE(record.Track(VK_LSHIFT, true));
    EXPECT_TRUE(record.Track(VK_RSHIFT, true));
    EXPECT_TRUE(record.Track(VK_LSHIFT, false));
    EXPECT_TRUE(record.Shift()) << "the right Shift is still down";
    record.Track(VK_RSHIFT, false);
    EXPECT_FALSE(record.Shift());

    record.Track(VK_RCONTROL, true);
    EXPECT_TRUE(record.Ctrl());
    EXPECT_FALSE(record.Alt());
    EXPECT_FALSE(record.Track('A', true)) << "not a modifier";

    record.Seed([](int vk) { return vk == VK_LMENU; });
    EXPECT_TRUE(record.Alt());
    EXPECT_FALSE(record.Ctrl()) << "seeded over what was tracked";
}

// A sideless modifier - which only injected input sends - is taken as the
// side its scan code and extended flag say, so that side's up undoes it.
TEST(Win32InputGrabTest, ASidelessModifierIsTakenAsTheSideItIs) {
    EXPECT_EQ(Win32InputGrab::SidedModifier(VK_CONTROL, 0x1D, false), static_cast<UINT>(VK_LCONTROL));
    EXPECT_EQ(Win32InputGrab::SidedModifier(VK_CONTROL, 0x1D, true), static_cast<UINT>(VK_RCONTROL));
    EXPECT_EQ(Win32InputGrab::SidedModifier(VK_MENU, 0x38, false), static_cast<UINT>(VK_LMENU));
    EXPECT_EQ(Win32InputGrab::SidedModifier(VK_MENU, 0x38, true), static_cast<UINT>(VK_RMENU));
    EXPECT_EQ(Win32InputGrab::SidedModifier(VK_SHIFT, 0x2A, false), static_cast<UINT>(VK_LSHIFT));
    EXPECT_EQ(Win32InputGrab::SidedModifier(VK_SHIFT, 0x36, false), static_cast<UINT>(VK_RSHIFT));
    EXPECT_EQ(Win32InputGrab::SidedModifier(VK_RSHIFT, 0x36, false), static_cast<UINT>(VK_RSHIFT));
    EXPECT_EQ(Win32InputGrab::SidedModifier('A', 0x1E, false), static_cast<UINT>('A'));

    Win32InputGrab::ModifierRecord record;
    record.Track(Win32InputGrab::SidedModifier(VK_SHIFT, 0x2A, false), true);
    record.Track(VK_LSHIFT, false);
    EXPECT_FALSE(record.Shift()) << "undone by its own side's up";
}

// Raw input reports the button pressed, not the one Windows makes of it:
// with the primary button set to the right, the right one is the overlay's
// left, as it is everywhere else. A button comes up as the one it went
// down as, whatever the setting says by then.
TEST(Win32InputGrabTest, AButtonIsTheOneWindowsSwapsItTo) {
    Win32InputGrab::ButtonSwap swap;
    EXPECT_FALSE(swap.Right(/*physicalRight=*/false, /*down=*/true, /*swapped=*/false));
    EXPECT_FALSE(swap.Right(false, false, false));
    EXPECT_TRUE(swap.Right(true, true, false));
    EXPECT_TRUE(swap.Right(true, false, false));

    EXPECT_FALSE(swap.Right(true, true, true)) << "swapped, the right is the left";
    EXPECT_TRUE(swap.Right(false, true, true)) << "and the left the right";
    EXPECT_FALSE(swap.Right(true, false, false)) << "up as it went down, though swapped back since";
    EXPECT_TRUE(swap.Right(false, false, false));

    EXPECT_FALSE(swap.Right(true, false, true)) << "an up whose down came before the grab: as the setting is";
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

// Ctrl and Alt held until the Ctrl+Alt+Del screen came up go up there,
// where no hook of this desktop is called. The record kept them held: a
// bare S matched Ctrl+Alt+S and hid the overlay, and the downs handed back
// as it hid stayed down system-wide. A switch of desktop forgets them.
TEST(Win32InputGrabTest, KeysLeftOnAnotherDesktopAreTakenAsReleased) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.dontForwardKeystrokes = true;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);  // not active: no hook, the keys are handed in
    constexpr int kHotkeyId = 77;
    grab.AddHotkey(kHotkeyId, KeyCombo{/*ctrl=*/true, /*alt=*/true, /*shift=*/false, /*key=*/'S'}, overlay);

    EXPECT_EQ(grab.KeyEventForTesting(VK_LCONTROL, true), 1) << "swallowed";
    EXPECT_EQ(grab.KeyEventForTesting(VK_LMENU, true), 1);
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    grab.HeldModifiers(ctrl, shift, alt);
    EXPECT_TRUE(ctrl && alt);

    grab.KeysLeftOnAnotherDesktop();
    grab.HeldModifiers(ctrl, shift, alt);
    EXPECT_FALSE(ctrl || alt || shift);
    grab.KeyEventForTesting('S', true);
    grab.KeyEventForTesting('S', false);

    std::vector<WPARAM> keyUps;
    int hotkeys = 0;
    MSG msg;
    while (PeekMessageA(&msg, overlay, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_KEYUP) {
            keyUps.push_back(msg.wParam);
        } else if (msg.message == WM_HOTKEY) {
            ++hotkeys;
        }
    }
    EXPECT_EQ(hotkeys, 0) << "a bare S is a bare S";
    EXPECT_NE(std::find(keyUps.begin(), keyUps.end(), static_cast<WPARAM>(VK_LCONTROL)), keyUps.end())
        << "the overlay is told Ctrl came up";
    EXPECT_NE(std::find(keyUps.begin(), keyUps.end(), static_cast<WPARAM>(VK_LMENU)), keyUps.end());

    grab.RemoveHotkey(kHotkeyId);
    grab.SetOptions(EditModeInputOptions{});
    grab.SetOverlayWindow(nullptr);
    DestroyWindow(overlay);
}

// The switch reaches the grab: Windows raises EVENT_SYSTEM_DESKTOPSWITCH,
// heard on the hook thread while it runs. Raised here by hand, since a
// test cannot switch desktops.
TEST(Win32InputGrabTest, ASwitchOfDesktopIsHeardWhileTheGrabRuns) {
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
    const auto limit = std::chrono::milliseconds(1500);

    grab.SetActive(true);
    ASSERT_TRUE(Eventually([] { return QueryRawMouse().targetIsAWindow; }, limit)) << "the hook thread is up";
    const int before = grab.Diagnostics().desktopSwitches;
    NotifyWinEvent(EVENT_SYSTEM_DESKTOPSWITCH, GetDesktopWindow(), OBJID_WINDOW, CHILDID_SELF);
    EXPECT_TRUE(Eventually([&] { return grab.Diagnostics().desktopSwitches > before; }, limit));

    grab.SetActive(false);
    grab.Shutdown();
    grab.SetOverlayWindow(nullptr);
    DestroyWindow(overlay);
}

// A modifier held since before the grab reached Windows itself, and has
// nothing to be handed back.
TEST(Win32InputGrabTest, NothingSwallowedIsNothingHandedBack) {
    const bool swallowed[256] = {};
    EXPECT_TRUE(Win32InputGrab::ModifierHandBack(swallowed).empty());
}

}  // namespace
}  // namespace sz::platform::win32
