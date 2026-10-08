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
#include <cwchar>
#include <functional>
#include <iterator>
#include <cstdlib>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <windows.h>
#include <tlhelp32.h>

namespace sz::platform::win32 {
namespace {

// How many of this process's threads are the grab's hook thread: one from
// the first grab to Shutdown, none after it. Counted by name rather than
// all threads at once: Windows starts threads of its own in a process -
// a thread pool's workers - and on a CI runner one stayed past the wait,
// failing a count of every thread with no hook thread left.
DWORD HookThreadCount() {
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
            if (entry.th32OwnerProcessID != GetCurrentProcessId()) {
                continue;
            }
            const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread == nullptr) {
                continue;  // gone since the snapshot
            }
            PWSTR name = nullptr;
            if (SUCCEEDED(GetThreadDescription(thread, &name))) {
                if (std::wcscmp(name, Win32InputGrab::kHookThreadName) == 0) {
                    ++count;
                }
                LocalFree(name);
            }
            CloseHandle(thread);
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

// A grab that cannot be set up whole is not set up at all: with raw input
// refused, no hook is left swallowing the mouse with nothing to read it,
// and the overlay goes by the real cursor - until a later try gets it.
TEST(Win32InputGrabTest, APointerGrabThatFailsIsNotHalfSetUpAndIsTriedAgain) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);

    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);
    grab.SetGameKeepsFocus(true);
    grab.FailPointerGrabForTesting(true);

    grab.SetActive(true);
    EXPECT_TRUE(Eventually([&grab] { return !grab.VirtualCursorActive(); }, std::chrono::milliseconds(800)))
        << "the failure is published";
    EXPECT_FALSE(QueryRawMouse().present) << "no sink left behind";

    grab.FailPointerGrabForTesting(false);
    EXPECT_TRUE(Eventually([&grab] { return grab.VirtualCursorActive() && QueryRawMouse().targetIsAWindow; },
                           std::chrono::milliseconds(3000)))
        << "tried again";

    grab.SetActive(false);
    grab.Shutdown();
    DestroyWindow(overlay);
}

// A keyboard hook that cannot be installed delivers no typing, and says
// so, so that a text field takes focus instead. A modifier let go of while
// it was failing is not left held once a retry installs it: the record is
// seeded from the system as the hook goes in.
TEST(Win32InputGrabTest, AKeyboardHookThatFailsDeliversNoTypingAndIsSeededOnRetry) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);

    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.useRawMouseInput = false;
    options.useSoftwarePointer = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);
    grab.SetGameKeepsFocus(true);
    grab.FailKeyboardHookForTesting(true);

    grab.SetActive(true);
    EXPECT_FALSE(grab.DeliversTypingToOverlay()) << "the failure is published as the call returns";

    // The thread tried as it was asked to, and failed; the next try is the
    // retry a second later. Waited a little, so that it is the retry that
    // finds the hook allowed.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // Ctrl as the record would have it from a seed while it was down, and
    // let go of with no hook to see it.
    grab.KeyEventForTesting(VK_LCONTROL, true);
    grab.FailKeyboardHookForTesting(false);
    EXPECT_TRUE(Eventually([&grab] { return grab.DeliversTypingToOverlay(); }, std::chrono::milliseconds(3000)))
        << "tried again";
    bool ctrl = true;
    bool shift = true;
    bool alt = true;
    grab.HeldModifiers(ctrl, shift, alt);
    EXPECT_FALSE(ctrl) << "seeded from the system, where nothing is held";

    // Its down was swallowed: let go of here, or the grab's end would hand
    // a held Ctrl to the system.
    grab.KeyEventForTesting(VK_LCONTROL, false);
    std::vector<INPUT> handedBack;
    grab.CaptureHandBackForTesting(&handedBack, nullptr);
    grab.SetActive(false);
    grab.CaptureHandBackForTesting(nullptr, nullptr);
    grab.Shutdown();
    DestroyWindow(overlay);
}

// A grab got on a retry is seeded from where the real cursor went
// meanwhile before it is published - before any frame, and so before a
// click could be stamped with the old position.
TEST(Win32InputGrabTest, APointerGrabGotOnARetryIsSeededBeforeItIsPublished) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    POINT before{};
    GetCursorPos(&before);

    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetPointerBounds(RECT{0, 0, 400, 300});
    grab.SetOptions(options);
    grab.SetGameKeepsFocus(true);
    grab.FailPointerGrabForTesting(true);

    SetCursorPos(50, 50);
    grab.SetActive(true);
    ASSERT_TRUE(Eventually([&grab] { return !grab.VirtualCursorActive(); }, std::chrono::milliseconds(800)));
    SetCursorPos(250, 150);  // the hand goes on, with the real cursor
    grab.FailPointerGrabForTesting(false);
    ASSERT_TRUE(Eventually([&grab] { return grab.VirtualCursorActive(); }, std::chrono::milliseconds(3000)));
    // Where the real cursor is: under the grab it stays where the seed
    // found it. Not (250, 150) exactly, and with a little room, since a
    // hand on this machine's mouse moves both meanwhile.
    const POINT at = grab.VirtualCursor();
    POINT real{};
    GetCursorPos(&real);
    EXPECT_LE(std::labs(at.x - real.x) + std::labs(at.y - real.y), 8)
        << "seeded at (" << at.x << ", " << at.y << "), the real cursor at (" << real.x << ", " << real.y << ")";
    EXPECT_GT(std::labs(at.x - 50) + std::labs(at.y - 50), 20) << "still the seed from before the failure";

    grab.SetActive(false);
    grab.Shutdown();
    grab.SetPointerBounds(RECT{});
    DestroyWindow(overlay);
    SetCursorPos(before.x, before.y);
}

// A grab that could not be set up hands no cursor back when it ends: the
// real cursor was the pointer meanwhile, and stays where the hand left it
// rather than jumping back to where the grab began.
TEST(Win32InputGrabTest, AFailedPointerGrabLeavesTheRealCursorWhereItIs) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    POINT before{};
    GetCursorPos(&before);

    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetPointerBounds(RECT{0, 0, 400, 300});
    grab.SetOptions(options);
    grab.SetGameKeepsFocus(true);
    grab.FailPointerGrabForTesting(true);

    SetCursorPos(50, 50);
    grab.SetActive(true);
    ASSERT_TRUE(Eventually([&grab] { return !grab.VirtualCursorActive(); }, std::chrono::milliseconds(800)));
    SetCursorPos(250, 150);  // the hand goes on, with the real cursor
    grab.SetActive(false);
    // With a little room, since a hand on this machine's mouse moves it
    // meanwhile.
    POINT real{};
    GetCursorPos(&real);
    EXPECT_GT(std::labs(real.x - 50) + std::labs(real.y - 50), 20)
        << "handed back to where the grab began: (" << real.x << ", " << real.y << ")";

    grab.FailPointerGrabForTesting(false);
    grab.Shutdown();
    grab.SetPointerBounds(RECT{});
    DestroyWindow(overlay);
    SetCursorPos(before.x, before.y);
}

// The grab is in when the call that asked for it returns, and out when the
// one that put it away does: the hook thread is waited for. Nothing is
// polled for here.
TEST(Win32InputGrabTest, TheGrabIsInWhenItsCallReturns) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);

    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);
    grab.SetGameKeepsFocus(true);

    grab.SetActive(true);
    EXPECT_TRUE(grab.VirtualCursorActive());
    EXPECT_TRUE(QueryRawMouse().targetIsAWindow);
    EXPECT_TRUE(grab.DeliversTypingToOverlay());

    grab.SetActive(false);
    EXPECT_FALSE(grab.VirtualCursorActive());
    EXPECT_FALSE(QueryRawMouse().present);
    EXPECT_FALSE(grab.DeliversTypingToOverlay());

    grab.Shutdown();
    DestroyWindow(overlay);
}

// One hook thread serves every show: a hide takes down what it installed
// and leaves the thread asleep, holding nothing, and Shutdown ends it.
// Shown and hidden many times over, nothing is left registered, there is
// one thread, and a show after all that still works.
TEST(Win32InputGrabTest, OneHookThreadServesEveryShowAndHiddenItHoldsNothing) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);

    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);
    grab.SetGameKeepsFocus(true);

    for (int i = 0; i < 20; ++i) {
        grab.SetActive(true);
        grab.SetActive(false);
    }
    EXPECT_FALSE(QueryRawMouse().present) << "a sink left registered while hidden";
    EXPECT_EQ(HookThreadCount(), 1u) << "one thread for every show";

    grab.SetActive(true);
    EXPECT_TRUE(QueryRawMouse().targetIsAWindow);
    grab.SetActive(false);
    grab.Shutdown();
    EXPECT_TRUE(Eventually([] { return HookThreadCount() == 0; }, std::chrono::milliseconds(1500)))
        << "Shutdown left the hook thread running";
    DestroyWindow(overlay);
}

// A hook thread that cannot be started installs nothing, and nothing takes
// it for one that did: the overlay goes by the real cursor, and a text field
// takes focus. Asked again by the next change, it starts.
TEST(Win32InputGrabTest, AHookThreadThatCannotStartLeavesTheRealCursorAndFocus) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    Win32InputGrab& grab = Win32InputGrab::Instance();
    grab.Shutdown();  // no thread left from an earlier test
    EditModeInputOptions options;
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);
    grab.SetGameKeepsFocus(true);
    grab.FailHookThreadStartForTesting(true);

    grab.SetActive(true);
    EXPECT_FALSE(grab.VirtualCursorActive());
    EXPECT_FALSE(grab.DeliversTypingToOverlay());
    EXPECT_FALSE(QueryRawMouse().present);
    EXPECT_EQ(HookThreadCount(), 0u);

    grab.FailHookThreadStartForTesting(false);
    options.counterRawMouseInput = true;  // any change asks again
    grab.SetOptions(options);
    EXPECT_TRUE(grab.VirtualCursorActive());
    EXPECT_TRUE(grab.DeliversTypingToOverlay());

    grab.SetActive(false);
    grab.Shutdown();
    DestroyWindow(overlay);
}

// The mouse hook refused with the raw-input sink up already: the sink goes
// too, rather than posting every click to the overlay on top of the one
// Windows delivers.
TEST(Win32InputGrabTest, AMouseHookThatFailsTakesTheSinkDownWithIt) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);
    grab.SetGameKeepsFocus(true);
    grab.FailMouseHookForTesting(true);

    grab.SetActive(true);
    EXPECT_FALSE(grab.VirtualCursorActive());
    EXPECT_FALSE(QueryRawMouse().present) << "no sink without its hook";

    grab.FailMouseHookForTesting(false);
    EXPECT_TRUE(Eventually([&grab] { return grab.VirtualCursorActive(); }, std::chrono::milliseconds(3000)))
        << "tried again";
    grab.SetActive(false);
    grab.Shutdown();
    DestroyWindow(overlay);
}

// A hook thread held up past the wait is not waited for again until it has
// caught up, and until then counts as having installed nothing: the calls
// return in a moment, the overlay goes by the real cursor, and once the
// thread answers the grab is in.
TEST(Win32InputGrabTest, AHookThreadThatDoesNotAnswerIsNotTrustedUntilItDoes) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    options.counterRawMouseInput = false;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);
    grab.SetGameKeepsFocus(true);
    grab.SetActive(true);  // the thread up, and answering
    grab.SetActive(false);

    grab.StallHookThreadForTesting(1500);
    // The keyboard held back at first, so that letting it go asks again.
    grab.SetKeyboardSuspended(true);
    const auto start = std::chrono::steady_clock::now();
    grab.SetActive(true);
    grab.SetKeyboardSuspended(false);  // asked again while it is still held up: no second wait
    const auto waited = std::chrono::steady_clock::now() - start;
    EXPECT_LT(waited, std::chrono::milliseconds(1000)) << "one wait, and a short one";
    EXPECT_FALSE(grab.VirtualCursorActive()) << "not answered, so nothing is in";
    EXPECT_FALSE(grab.DeliversTypingToOverlay());

    EXPECT_TRUE(Eventually([&grab] { return grab.VirtualCursorActive(); }, std::chrono::milliseconds(3000)))
        << "in once it caught up";
    EXPECT_TRUE(grab.DeliversTypingToOverlay());
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

    grab.InputLeftOnAnotherDesktop();
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

// A button held into the Ctrl+Alt+Del screen goes up there, where the
// raw input of this desktop does not hear it: taken as released at the
// switch, and the overlay told. Found by hand: a snippet dragged there
// followed the pointer back on the desktop until the next click.
TEST(Win32InputGrabTest, ButtonsLeftOnAnotherDesktopAreTakenAsReleased) {
    HWND overlay = CreateWindowExW(0, L"STATIC", L"overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    Win32InputGrab& grab = Win32InputGrab::Instance();
    grab.SetOverlayWindow(overlay);
    const auto posted = [overlay] {
        std::vector<UINT> messages;
        MSG msg;
        while (PeekMessageW(&msg, overlay, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {
            messages.push_back(msg.message);
        }
        return messages;
    };

    grab.RawMouseButtonsForTesting(RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN);
    ASSERT_EQ(posted(), (std::vector<UINT>{WM_LBUTTONDOWN, WM_MBUTTONDOWN}));
    grab.InputLeftOnAnotherDesktop();
    EXPECT_EQ(posted(), (std::vector<UINT>{WM_LBUTTONUP, WM_MBUTTONUP}));
    grab.InputLeftOnAnotherDesktop();
    EXPECT_TRUE(posted().empty()) << "the switch back: nothing is held any more";

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

// With every key the overlay's, the repeat of one held since before the
// grab is taken too, and its up left to Windows, which saw it go down.
TEST(Win32InputGrabTest, TheRepeatOfAKeyHeldFromBeforeIsTakenAndItsUpLeft) {
    HWND overlay = CreateWindowExA(0, "STATIC", "overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleA(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);  // not active: no hook, the keys are handed in

    EXPECT_EQ(grab.KeyEventForTesting(VK_BACK, true, /*heldByWindows=*/true), 1);
    EXPECT_EQ(grab.KeyEventForTesting(VK_BACK, false), 0);

    grab.SetOptions(EditModeInputOptions{});
    grab.SetOverlayWindow(nullptr);
    DestroyWindow(overlay);
}

// Alt or a Win key held with a letter is a chord, not text, and the grab
// makes no character of it, as Windows would make none a text field takes.
// The grab made characters of every chord but Ctrl's: Alt+E and Win+E
// typed an "e" into a name.
TEST(Win32InputGrabTest, AnAltOrWinChordTypesNothing) {
    HWND overlay = CreateWindowExW(0, L"STATIC", L"overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);  // not active: no hook, the keys are handed in
    const auto charsPosted = [overlay] {
        std::vector<WPARAM> chars;
        MSG msg;
        while (PeekMessageW(&msg, overlay, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_CHAR) {
                chars.push_back(msg.wParam);
            }
        }
        return chars;
    };
    const auto chord = [&grab](UINT modifier) {
        grab.KeyEventForTesting(modifier, true);
        grab.KeyEventForTesting('E', true);
        grab.KeyEventForTesting('E', false);
        grab.KeyEventForTesting(modifier, false);
    };

    grab.KeyEventForTesting('E', true);
    grab.KeyEventForTesting('E', false);
    ASSERT_EQ(charsPosted(), std::vector<WPARAM>{L'e'}) << "a bare letter types";
    chord(VK_LMENU);
    EXPECT_TRUE(charsPosted().empty()) << "Alt+E";
    chord(VK_LWIN);
    EXPECT_TRUE(charsPosted().empty()) << "Win+E";
    chord(VK_RWIN);
    EXPECT_TRUE(charsPosted().empty()) << "Win+E, the right one";

    grab.SetOptions(EditModeInputOptions{});
    grab.SetOverlayWindow(nullptr);
    DestroyWindow(overlay);
}

// A key pressed with a Win key held is no key of the overlay's: handed over,
// it arrived bare, and Win+1 - the taskbar's first program - switched the
// Behavior panel's first row, Win+E picked the eraser.
TEST(Win32InputGrabTest, AWinChordIsNoKeyOfTheOverlays) {
    HWND overlay = CreateWindowExW(0, L"STATIC", L"overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(overlay, nullptr);
    Win32InputGrab& grab = Win32InputGrab::Instance();
    EditModeInputOptions options;
    grab.SetOverlayWindow(overlay);
    grab.SetOptions(options);  // not active: no hook, the keys are handed in
    const auto keysPosted = [overlay] {
        std::vector<WPARAM> keys;
        MSG msg;
        while (PeekMessageW(&msg, overlay, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP) {
                keys.push_back(msg.wParam);
            }
        }
        return keys;
    };

    for (const UINT win : {VK_LWIN, VK_RWIN}) {
        grab.KeyEventForTesting(win, true);
        EXPECT_EQ(grab.KeyEventForTesting('1', true), 1) << "kept from the shell too";
        EXPECT_EQ(grab.KeyEventForTesting('1', false), 0) << "its down was nobody's";
        grab.KeyEventForTesting(win, false);
        const std::vector<WPARAM> keys = keysPosted();
        EXPECT_EQ(std::count(keys.begin(), keys.end(), static_cast<WPARAM>('1')), 0) << "Win+1 with " << win;
    }

    grab.KeyEventForTesting('1', true);
    grab.KeyEventForTesting('1', false);
    EXPECT_EQ(keysPosted(), (std::vector<WPARAM>{'1', '1'})) << "a bare 1 is the overlay's";

    grab.SetOptions(EditModeInputOptions{});
    grab.SetOverlayWindow(nullptr);
    DestroyWindow(overlay);
}

namespace {
// An overlay stand-in, the keyboard grabbed with every key the overlay's,
// and what the grab hands Windows as it ends caught in `handedBack` - no
// hook installed, and nothing injected.
class GrabbedKeyboard {
public:
    explicit GrabbedKeyboard(std::vector<INPUT>& handedBack, std::function<void()> gap = nullptr)
        : overlay_(CreateWindowExW(0, L"STATIC", L"overlay stand-in", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleW(nullptr), nullptr)) {
        Win32InputGrab& grab = Win32InputGrab::Instance();
        EditModeInputOptions options;
        grab.SetOverlayWindow(overlay_);
        grab.SetOptions(options);
        grab.CaptureHandBackForTesting(&handedBack, std::move(gap));
        grab.Heartbeat();
    }
    ~GrabbedKeyboard() {
        Win32InputGrab& grab = Win32InputGrab::Instance();
        grab.CaptureHandBackForTesting(nullptr, nullptr);
        grab.SetOptions(EditModeInputOptions{});
        grab.SetOverlayWindow(nullptr);
        DestroyWindow(overlay_);
    }
    GrabbedKeyboard(const GrabbedKeyboard&) = delete;
    GrabbedKeyboard& operator=(const GrabbedKeyboard&) = delete;

private:
    HWND overlay_;
};
}  // namespace

// The hook comes down after the grab ends, on its own thread, and a key in
// between is not the grab's: let by, and recorded nowhere. Recorded, S's
// down made its up the next grab's to swallow - S stayed down in the
// game - and a Ctrl was held for a later session that never grabbed the
// keyboard.
TEST(Win32InputGrabTest, AfterTheGrabTheHookRecordsNothing) {
    std::vector<INPUT> handedBack;
    const GrabbedKeyboard keyboard(handedBack);
    Win32InputGrab& grab = Win32InputGrab::Instance();

    grab.GrabKeyboardForTesting(true);
    EXPECT_EQ(grab.HookedKeyEventForTesting('S', true), 1) << "the grab's";
    EXPECT_EQ(grab.HookedKeyEventForTesting('S', false), 1);
    grab.GrabKeyboardForTesting(false);

    EXPECT_EQ(grab.HookedKeyEventForTesting(VK_LCONTROL, true), 0) << "let by";
    EXPECT_EQ(grab.HookedKeyEventForTesting('S', true), 0);
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    grab.HeldModifiers(ctrl, shift, alt);
    EXPECT_FALSE(ctrl) << "held for a session that never grabs the keyboard";

    grab.GrabKeyboardForTesting(true);
    EXPECT_EQ(grab.HookedKeyEventForTesting('S', false), 0) << "its down went to Windows, and so does its up";
    grab.GrabKeyboardForTesting(false);
    grab.HookedKeyEventForTesting(VK_LCONTROL, false);
}

// A modifier let go of just as the grab ends has its up go by to a Windows
// that has not been handed the down yet, and the down handed back after
// it stayed down, system-wide. Its up is handed back too.
TEST(Win32InputGrabTest, AModifierLetGoAsTheGrabEndsIsNotLeftDown) {
    std::vector<INPUT> handedBack;
    Win32InputGrab& grab = Win32InputGrab::Instance();
    {
        const GrabbedKeyboard keyboard(handedBack, [&grab] { grab.HookedKeyEventForTesting(VK_LCONTROL, false); });
        grab.GrabKeyboardForTesting(true);
        ASSERT_EQ(grab.HookedKeyEventForTesting(VK_LCONTROL, true), 1);
        grab.GrabKeyboardForTesting(false);
    }
    ASSERT_EQ(handedBack.size(), 2u);
    EXPECT_EQ(handedBack[0].ki.wVk, VK_LCONTROL);
    EXPECT_EQ(handedBack[0].ki.dwFlags & KEYEVENTF_KEYUP, 0u) << "its down";
    EXPECT_EQ(handedBack[1].ki.wVk, VK_LCONTROL);
    EXPECT_NE(handedBack[1].ki.dwFlags & KEYEVENTF_KEYUP, 0u) << "and then its up";

    // Held on past the end: the down alone.
    handedBack.clear();
    {
        const GrabbedKeyboard keyboard(handedBack);
        grab.GrabKeyboardForTesting(true);
        ASSERT_EQ(grab.HookedKeyEventForTesting(VK_LCONTROL, true), 1);
        grab.GrabKeyboardForTesting(false);
    }
    EXPECT_EQ(handedBack.size(), 1u);
}

// A modifier held since before the grab reached Windows itself, and has
// nothing to be handed back.
TEST(Win32InputGrabTest, NothingSwallowedIsNothingHandedBack) {
    const bool swallowed[256] = {};
    EXPECT_TRUE(Win32InputGrab::ModifierHandBack(swallowed).empty());
}

}  // namespace
}  // namespace sz::platform::win32
