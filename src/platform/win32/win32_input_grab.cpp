#include "platform/win32/win32_input_grab.h"

#include <windowsx.h>

#include <algorithm>
#include <iterator>
#include <cmath>

namespace sz::platform::win32 {

namespace {
constexpr const char* kRawInputSinkClassName = "SpickzettelRawInputSink";

// Stamped into dwExtraInfo on everything this process injects - the camera
// corrections, and the modifiers handed back when the keyboard grab ends - so
// the hooks can recognize their own work coming back around and let it past.
// Deliberately narrower than testing LLMHF_INJECTED: that would also wave
// through synthetic input from anything else on the machine, which is
// precisely what "the overlay has taken the input" should not do.
constexpr ULONG_PTR kOwnInjectionMarker = 0x5A4B5053;  // 'SPKZ'

// Asks the hook thread to install or remove hooks to match the current
// options. Only that thread may own them - see StartHookThread.
constexpr UINT kReconcileHooksMessage = WM_USER + 1;

// Windows' pointer-speed slider (1..20, 10 default) as the multipliers it
// stands for WITH "enhance pointer precision" OFF. This is the documented
// table, and measured: 6 -> 0.5, 10 -> 1.0, 14 -> 2.0, 20 -> 3.5.
//
// With the acceleration curve ON - the default, and the common case - the
// OS does not use this table at all. Measured across sliders 6..20 at three
// speeds: the ratio to the middle notch is slider/10, linear, every time
// (6 -> 0.59, 12 -> 1.18, 14 -> 1.35, 20 -> 2.0). Applying this table in
// that mode was this code's second attempt and its worst bug: at a slider
// of 15 it ran the pointer at x2.25 where the OS runs x1.5, which lifted
// the slow-speed gain past one pixel per count and made single mouse
// reports step two pixels. The first attempt, slider/10, had been right
// for the curve-on case all along. See SliderMultiplier.
constexpr float kSliderMultiplierCurveOff[21] = {1.0f,   0.03125f, 0.0625f, 0.125f, 0.25f, 0.375f, 0.5f,
                                                  0.625f, 0.75f,    0.875f,  1.0f,   1.25f, 1.5f,   1.75f,
                                                  2.0f,   2.25f,    2.5f,    2.75f,  3.0f,  3.25f,  3.5f};

float SliderMultiplier(int slider, bool curveOn) {
    slider = std::clamp(slider, 1, 20);
    return curveOn ? static_cast<float>(slider) / 10.0f : kSliderMultiplierCurveOff[slider];
}

// Ties the SmoothMouseCurve's units to ours. The curve's shape comes from
// the registry, but the absolute scale in the documented algorithm depends
// on screen DPI and refresh in ways not worth reproducing from memory, so
// this one number is calibrated by measurement instead: it is chosen so that
// a given hand movement travels the same distance under the grab as the
// desktop pointer does without it. See docs/ARCHITECTURE.md.
constexpr float kBallisticScale = 0.2278f;

// The performance counter, for the one thing here that is timed in
// fractions of a millisecond - see InputGrabDiagnostics::correctionLagMsLast.
int64_t NowTicks() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// KeyCombo's platform-agnostic encoding -> Win32 virtual-key code. Same
// translation Win32PlatformHost::RegisterGlobalHotkey does for
// RegisterHotKey; duplicated rather than shared because that one builds
// RegisterHotKey's separate modifier bitmask at the same time and there is
// no shared shape worth extracting for two lines.
UINT VirtualKeyFor(const KeyCombo& combo) {
    if (combo.IsFunctionKey()) {
        return static_cast<UINT>(VK_F1 + combo.FunctionKeyNumber() - 1);
    }
    return static_cast<UINT>(combo.key);
}
}  // namespace

Win32InputGrab& Win32InputGrab::Instance() {
    static Win32InputGrab instance;
    return instance;
}

void Win32InputGrab::SetOverlayWindow(HWND hwnd) {
    overlay_ = hwnd;
    Refresh();
}

void Win32InputGrab::SetPointerBounds(const RECT& bounds) {
    boundsLeft_.store(bounds.left, std::memory_order_relaxed);
    boundsTop_.store(bounds.top, std::memory_order_relaxed);
    boundsRight_.store(bounds.right, std::memory_order_relaxed);
    boundsBottom_.store(bounds.bottom, std::memory_order_relaxed);
}

RECT Win32InputGrab::PointerBounds() const {
    RECT bounds{boundsLeft_.load(std::memory_order_relaxed), boundsTop_.load(std::memory_order_relaxed),
                boundsRight_.load(std::memory_order_relaxed), boundsBottom_.load(std::memory_order_relaxed)};
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        bounds = RECT{0, 0, std::max(GetSystemMetrics(SM_CXSCREEN), 1), std::max(GetSystemMetrics(SM_CYSCREEN), 1)};
    }
    return bounds;
}

void Win32InputGrab::SetOptions(const EditModeInputOptions& options) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        options_ = options;
    }
    Refresh();
}

void Win32InputGrab::SetActive(bool active) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (active_ == active) {
            return;
        }
        active_ = active;
    }
    if (!active) {
        // Whatever button was held when the grab ended stays "held" forever
        // otherwise: the mouse-up that would have cleared it is exactly the
        // event that stopped being swallowed. The keyboard's equivalent is
        // not here - Refresh owns both ends of that, because activating is
        // only one of several ways the keyboard grab comes and goes.
        leftDown_ = rightDown_ = middleDown_ = false;

        // A text field cannot be open when the overlay is not shown, and
        // neither claim on the keyboard may outlive the grab. Both are set by
        // a field opening and cleared by it closing - and a field can stop
        // existing without closing, because hiding the overlay takes it with
        // it. Measured: hide with a rename open, show again, and the keyboard
        // was still being swallowed with no field anywhere, so the WASD that
        // keystroke forwarding exists to preserve silently stopped reaching
        // the game.
        std::lock_guard<std::mutex> lock(stateMutex_);
        textFieldOpen_ = false;
        keyboardSuspended_ = false;
    }
    Refresh();
}

void Win32InputGrab::SetGameKeepsFocus(bool gameKeepsFocus) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (gameKeepsFocus_ == gameKeepsFocus) {
            return;
        }
        gameKeepsFocus_ = gameKeepsFocus;
    }
    Refresh();
}

void Win32InputGrab::SetInputOptionsHudDigits(int digitCount) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (hudDigits_ == digitCount) {
            return;
        }
        hudDigits_ = digitCount;
    }
    Refresh();
}

void Win32InputGrab::SetKeyboardSuspended(bool suspended) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (keyboardSuspended_ == suspended) {
            return;
        }
        keyboardSuspended_ = suspended;
    }
    // The modifier record is not cleared here: suspending changes whether the
    // keyboard is grabbed, and Refresh does the whole handover on that
    // transition - hand what is held back to Windows, tell the overlay the
    // keys are up, and re-seed on the way back in.
    Refresh();
}

void Win32InputGrab::AddHotkey(int id, const KeyCombo& combo, HWND target) {
    if (id == 0 || !combo.IsValid() || !target) {
        return;
    }
    RemoveHotkey(id);
    std::lock_guard<std::mutex> lock(stateMutex_);
    hotkeys_.push_back(Hotkey{id, VirtualKeyFor(combo), combo.ctrl, combo.alt, combo.shift, target});
}

void Win32InputGrab::RemoveHotkey(int id) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    for (auto it = hotkeys_.begin(); it != hotkeys_.end(); ++it) {
        if (it->id == id) {
            hotkeys_.erase(it);
            return;
        }
    }
}

void Win32InputGrab::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        active_ = false;
    }
    Refresh();
}

// Takes the keyboard over: start from what is physically held rather than
// from "nothing".
//
// A grab always starts in the middle of whatever chord started it. The hotkey
// that turns edit mode on is the clearest case - Ctrl and Alt go down before
// any hook exists, so the hook never sees them - and holding the modifiers and
// pressing O again would find ctrl=false, alt=false in the record the grab
// matches hotkeys against: the overlay toggles on and refuses to toggle off.
// GetAsyncKeyState is trustworthy at exactly this moment: nothing has
// been swallowed yet in this grab, and OnKeyboard's key-up rule is what stops
// it having gone stale in an earlier one.
void Win32InputGrab::BeginGrabbedKeyboard() {
    const auto asyncDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    ctrlDown_ = asyncDown(VK_CONTROL);
    altDown_ = asyncDown(VK_MENU);
    shiftDown_ = asyncDown(VK_SHIFT);
}

// Gives the keyboard back, to both parties that were being lied to while it
// was held: the overlay gets a key-up for everything the hook swallowed the
// down of, and Windows gets a key-down for the modifiers still held, which it
// never saw go down. Without the first, ImGui keeps believing a key is held
// and eats the next press of it; without the second, the OS has no chord to
// match and the hotkey stops working.
void Win32InputGrab::EndGrabbedKeyboard() {
    const bool ctrl = ctrlDown_;
    const bool shift = shiftDown_;
    const bool alt = altDown_;
    ctrlDown_ = altDown_ = shiftDown_ = false;
    ReleaseSwallowedKeys();
    HandHeldModifiersToSystem(ctrl, shift, alt);
}

// Takes the pointer over from the real cursor: start where it is, and reset
// everything that describes movement since there is none yet.
//
// Called on the transition into driving rather than when the grab activates,
// because those are not the same moment. Switching raw input on part way
// through an edit-mode session leaves the grab already active, so seeding in
// SetActive alone would never run again - and the drawn pointer would jump
// back to wherever the real cursor had been when edit mode opened. The mirror of the
// hand-back in Refresh, and it has to be the same one place, or the two ends
// of the same handover drift apart.
void Win32InputGrab::BeginVirtualCursor() {
    POINT seed{};
    GetCursorPos(&seed);
    // The real cursor need not be on the overlay's display: the hotkey that
    // brought the overlay up works from anywhere. Clamped in from another
    // display, the pointer would start stuck to this one's edge, far from
    // where anyone would look for it; the middle is where they will.
    const RECT bounds = PointerBounds();
    if (!PtInRect(&bounds, seed)) {
        seed = POINT{bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
    }
    // Under the pointer lock throughout, the registry reads included: the
    // raw-input sink may still be up on the hook thread and integrating
    // reports into the very fields being seeded (see pointerMutex_). A
    // report waits here for a moment once per grab; one integrated halfway
    // through a seed would put the pointer somewhere neither meant.
    std::lock_guard<std::mutex> lock(pointerMutex_);
    virtualCursorX_.store(seed.x);
    virtualCursorY_.store(seed.y);
    preciseX_ = static_cast<float>(seed.x);
    preciseY_ = static_cast<float>(seed.y);
    ClampVirtualCursor();

    for (int i = 0; i < 4; ++i) {
        stepCounts_[i].store(0);
        frameSteps_[i].store(0);
    }
    lastFramePoint_ = POINT{virtualCursorX_.load(), virtualCursorY_.load()};
    QueryPerformanceFrequency(&reportFrequency_);
    lastReportTime_.QuadPart = 0;
    lastGain_ = 0.0f;
    LoadPointerBallistics();
}

void Win32InputGrab::Refresh() {
    // Hand the real cursor back where the drawn one was left, the moment the
    // drawn one stops being what the user is pointing with.
    //
    // Under a mouse grab the real cursor is parked wherever it was when the
    // grab started - it is deliberately left to the game, and every movement
    // since has gone into the virtual position instead. So switching the
    // pointer off, or leaving edit mode, would teleport the cursor back to
    // that starting point: a drawn pointer at (582,491) and a real one still
    // at (400,400), measured. The gap is not a bug in either pointer, but the
    // jump when one replaces the other is a real one.
    //
    // SetCursorPos generates no raw input, so this cannot nudge a game's
    // camera; a game that pins the cursor will simply move it again.
    const bool virtualCursorDriving = WantPointerGrab() && overlay_ != nullptr;
    // The hot-path mirrors, written here and nowhere else - see their
    // declarations for why the hook reads these rather than the options.
    pointerGrabbing_.store(virtualCursorDriving, std::memory_order_relaxed);
    const bool countering = WantCancellation() && overlay_ != nullptr;
    countering_.store(countering, std::memory_order_relaxed);
    const EditModeInputOptions options = OptionsSnapshot();
    softwarePointerDrawn_.store(options.SoftwarePointerDrawn(), std::memory_order_relaxed);
    if (countering && !counteringWasOn_) {
        // Start each run of countering with an empty ledger and empty
        // measurements: a delta banked before it was switched on has no
        // correction owing, and a lag figure from the last game says
        // nothing about this one.
        pendingCorrectionX_.store(0, std::memory_order_relaxed);
        pendingCorrectionY_.store(0, std::memory_order_relaxed);
        pendingCorrectionSince_.store(0, std::memory_order_relaxed);
        correctionLagMsLast_.store(0.0f, std::memory_order_relaxed);
        correctionLagMsMax_.store(0.0f, std::memory_order_relaxed);
        correctionsInjected_.store(0, std::memory_order_relaxed);
    } else if (!countering && counteringWasOn_) {
        // Settling the account while the hook is still installed - it is
        // only taken down at the end of this - so the correction is
        // swallowed like every other and does not fling the real cursor.
        FlushPendingCorrection();
    }
    counteringWasOn_ = countering;
    if (virtualCursorWasDriving_ && !virtualCursorDriving) {
        const POINT at = VirtualCursor();
        SetCursorPos(at.x, at.y);
    } else if (!virtualCursorWasDriving_ && virtualCursorDriving) {
        BeginVirtualCursor();
    }
    virtualCursorWasDriving_ = virtualCursorDriving;

    // The keyboard's handover, on the same shape and for the same reason: it
    // belongs to the transition, not to any one of the things that can cause
    // it. WantKeyboard depends on five pieces of state, so the hook comes and
    // goes through SetActive, SetKeyboardSuspended, SetGameKeepsFocus,
    // SetOptions and SetInputOptionsHudDigits alike. In SetActive alone,
    // opening a rename field (which suspends the keyboard) or switching
    // keystroke forwarding off from the Settings tab would skip all of it -
    // leaving ImGui with a latched key, or the OS with no
    // idea a modifier was held.
    const bool keyboardGrabbed = WantKeyboard();
    if (!keyboardWasGrabbed_ && keyboardGrabbed) {
        BeginGrabbedKeyboard();
    } else if (keyboardWasGrabbed_ && !keyboardGrabbed) {
        EndGrabbedKeyboard();
    }
    keyboardWasGrabbed_ = keyboardGrabbed;

    const bool wantHooks = (WantPointerGrab() && overlay_ != nullptr) || keyboardGrabbed;
    if (wantHooks) {
        // The thread owns the hooks and the raw-input sink alike, so it has
        // to be the one to create or destroy them; this only asks - and
        // only of a thread whose queue exists. A post to one still on its
        // way up, or on its way out, is lost; a thread on its way up
        // reconciles once by itself when it gets there, and for the rest
        // the next transition asks again.
        if (StartHookThread()) {
            PostThreadMessageA(hookThreadId_, kReconcileHooksMessage, 0, 0);
        }
    } else {
        StopHookThread();
    }
}

bool Win32InputGrab::StartHookThread() {
    if (hookThread_) {
        // There is a thread. Finished since it was last looked at - a Stop
        // that gave up waiting, or a start that died late - and it is
        // closed here and a fresh one started. Still there and on its way
        // out (see StopHookThread), waited for once more: a post would
        // land behind its quit, and a second thread over the same hooks
        // is not an option. Still there and on its way up, ready only if
        // its queue has appeared meanwhile.
        const DWORD wait = WaitForSingleObject(hookThread_, hookThreadQuitting_ ? 2000 : 0);
        if (wait != WAIT_OBJECT_0) {
            if (hookThreadQuitting_) {
                return false;
            }
            if (hookThreadReady_) {
                if (WaitForSingleObject(hookThreadReady_, 0) != WAIT_OBJECT_0) {
                    return false;
                }
                CloseHandle(hookThreadReady_);
                hookThreadReady_ = nullptr;
            }
            return true;
        }
        CloseHookThreadHandles();
    }
    // A thread has no message queue until it first asks for one, and
    // PostThreadMessage to a thread without a queue fails - so the first
    // reconcile request, posted the moment this returns, could land on
    // nothing. The thread signals once its queue exists, and this waits for
    // that (or for the thread to die trying) before handing the id out.
    // Without the event there is no handshake, so no thread either.
    hookThreadReady_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!hookThreadReady_) {
        return false;
    }
    hookThread_ = CreateThread(nullptr, 0, &Win32InputGrab::HookThreadMain, this, 0, &hookThreadId_);
    if (!hookThread_) {
        CloseHookThreadHandles();
        return false;
    }
    const HANDLE readyOrDead[] = {hookThreadReady_, hookThread_};
    const DWORD result = WaitForMultipleObjects(2, readyOrDead, FALSE, 5000);
    if (result == WAIT_OBJECT_0) {
        CloseHandle(hookThreadReady_);  // running: the handshake is over
        hookThreadReady_ = nullptr;
        return true;
    }
    if (result == WAIT_OBJECT_0 + 1) {
        CloseHookThreadHandles();  // died before its queue existed
        return false;
    }
    // Timed out: still starting, on a machine that is very busy. The event
    // stays open for the thread to set when it gets there, and the next
    // Start looks at it; nothing is posted until then.
    return false;
}

void Win32InputGrab::StopHookThread() {
    if (!hookThread_) {
        return;
    }
    if (!PostThreadMessageA(hookThreadId_, WM_QUIT, 0, 0) && hookThreadReady_) {
        // No queue to post to yet: the thread is still on its way up. A
        // quit that is not delivered leaves a thread that never stops, so
        // this waits for the queue (or the thread's death) and asks again.
        const HANDLE readyOrDead[] = {hookThreadReady_, hookThread_};
        WaitForMultipleObjects(2, readyOrDead, FALSE, 5000);
        PostThreadMessageA(hookThreadId_, WM_QUIT, 0, 0);
    }
    if (WaitForSingleObject(hookThread_, 2000) != WAIT_OBJECT_0) {
        // Still running - a hook callback stuck behind something, say. The
        // handle and id are kept, so that it stays this object's thread:
        // the next Start finds it rather than starting a second thread over
        // the same hooks and raw-input sink, and knows from the flag that
        // it is not one to post to; the next Stop asks again.
        hookThreadQuitting_ = true;
        return;
    }
    CloseHookThreadHandles();
}

void Win32InputGrab::CloseHookThreadHandles() {
    if (hookThread_) {
        CloseHandle(hookThread_);
        hookThread_ = nullptr;
    }
    if (hookThreadReady_) {
        CloseHandle(hookThreadReady_);
        hookThreadReady_ = nullptr;
    }
    hookThreadId_ = 0;
    hookThreadQuitting_ = false;
}

DWORD WINAPI Win32InputGrab::HookThreadMain(void* self) {
    auto& grab = *static_cast<Win32InputGrab*>(self);
    // The message queue exists from this call on - and StartHookThread is
    // waiting to hear so before it lets anyone post here.
    MSG msg;
    PeekMessageA(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    if (grab.hookThreadReady_) {
        SetEvent(grab.hookThreadReady_);
    }
    grab.ReconcileHooks();

    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kReconcileHooksMessage) {
            grab.ReconcileHooks();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // The sink is this thread's window, and Windows destroys it with the
    // thread whether or not anyone asks - so it has to be let go of here,
    // by the code that would otherwise go on believing it exists. Measured
    // without this: the first show registered for raw mouse input and the
    // second did not, because EnsureRawInputSink saw a handle and returned
    // early, and the handle was a window that had died with the previous
    // thread. Under a grab that is a dead mouse: the hook goes on swallowing
    // every event, and nothing is left to read them.
    grab.DestroyRawInputSink();
    if (grab.mouseHook_) {
        UnhookWindowsHookEx(grab.mouseHook_);
        grab.mouseHook_ = nullptr;
    }
    if (grab.keyboardHook_) {
        UnhookWindowsHookEx(grab.keyboardHook_);
        grab.keyboardHook_ = nullptr;
    }
    return 0;
}

void Win32InputGrab::ReconcileHooks() {
    const bool wantMouse = WantPointerGrab() && overlay_ != nullptr;
    const bool wantKeyboard = WantKeyboard();

    if (wantMouse && !mouseHook_) {
        mouseHook_ = SetWindowsHookExA(WH_MOUSE_LL, &Win32InputGrab::MouseProc, GetModuleHandleA(nullptr), 0);
    } else if (!wantMouse && mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }

    if (wantKeyboard && !keyboardHook_) {
        keyboardHook_ =
            SetWindowsHookExA(WH_KEYBOARD_LL, &Win32InputGrab::KeyboardProc, GetModuleHandleA(nullptr), 0);
    } else if (!wantKeyboard && keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }

    // The raw-input sink lives here too, for the same reason the hooks do:
    // it drives the pointer, and a pointer whose position waits for the
    // render thread to pump arrives a frame behind the hand and out of
    // order with the clicks, which come from this thread. Needed whenever
    // the hook is - countering reads its device deltas from here, and in
    // that mode the buttons the hook discards are read from here too.
    if (wantMouse) {
        EnsureRawInputSink();
    } else {
        DestroyRawInputSink();
    }

}

void Win32InputGrab::EnsureRawInputSink() {
    if (rawInputSink_) {
        return;
    }
    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = &Win32InputGrab::RawInputSinkWndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kRawInputSinkClassName;
    RegisterClassA(&windowClass);  // harmless if already registered

    rawInputSink_ = CreateWindowExA(0, kRawInputSinkClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance,
                                   nullptr);
    if (!rawInputSink_) {
        return;
    }
    // RIDEV_INPUTSINK is what makes this work at all: ordinary raw-input
    // registration only delivers to the foreground window, and the whole
    // premise here is that the game is the foreground window. This reads
    // the very same device deltas the game is reading.
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;  // mouse
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = rawInputSink_;
    RegisterRawInputDevices(&device, 1, sizeof(device));
}

void Win32InputGrab::DestroyRawInputSink() {
    if (!rawInputSink_) {
        return;
    }
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;  // must be null for RIDEV_REMOVE
    RegisterRawInputDevices(&device, 1, sizeof(device));
    DestroyWindow(rawInputSink_);
    rawInputSink_ = nullptr;
}

void Win32InputGrab::FlushPendingCorrection() {
    const LONG dx = pendingCorrectionX_.exchange(0);
    const LONG dy = pendingCorrectionY_.exchange(0);
    const int64_t since = pendingCorrectionSince_.exchange(0);
    if (dx == 0 && dy == 0) {
        return;
    }
    // How long the game had this movement to itself. Recorded before the
    // injection rather than after, so the number is the wait rather than
    // the wait plus SendInput's own cost.
    if (since != 0) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart > 0) {
            const float lagMs =
                static_cast<float>(NowTicks() - since) * 1000.0f / static_cast<float>(frequency.QuadPart);
            correctionLagMsLast_.store(lagMs, std::memory_order_relaxed);
            if (lagMs > correctionLagMsMax_.load(std::memory_order_relaxed)) {
                correctionLagMsMax_.store(lagMs, std::memory_order_relaxed);
            }
        }
    }
    correctionsInjected_.fetch_add(1, std::memory_order_relaxed);

    // One correction carrying everything banked, rather than one per report
    // or per frame. The total the game integrates is identical, and it is a
    // good deal less synthetic input for it to look at.
    INPUT correction{};
    correction.type = INPUT_MOUSE;
    correction.mi.dx = -dx;
    correction.mi.dy = -dy;
    correction.mi.dwFlags = MOUSEEVENTF_MOVE;
    correction.mi.dwExtraInfo = kOwnInjectionMarker;
    SendInput(1, &correction, sizeof(correction));
}

// The absolute counterpart of MoveVirtualCursorRaw: the device has named a
// position rather than a movement, so there is nothing to scale, accelerate
// or accumulate - only to clamp and record. Still feeds the same step
// histogram, so the HUD reads the same way on either kind of device.
void Win32InputGrab::SetVirtualCursorAbsolute(LONG x, LONG y) {
    const RECT bounds = PointerBounds();
    {
        std::lock_guard<std::mutex> lock(pointerMutex_);
        preciseX_ = std::clamp(static_cast<float>(x), static_cast<float>(bounds.left),
                               static_cast<float>(bounds.right - 1));
        preciseY_ = std::clamp(static_cast<float>(y), static_cast<float>(bounds.top),
                               static_cast<float>(bounds.bottom - 1));

        const LONG beforeX =
            virtualCursorX_.exchange(static_cast<LONG>(std::floor(preciseX_)), std::memory_order_relaxed);
        const LONG beforeY =
            virtualCursorY_.exchange(static_cast<LONG>(std::floor(preciseY_)), std::memory_order_relaxed);
        const LONG stepX = std::labs(virtualCursorX_.load(std::memory_order_relaxed) - beforeX);
        const LONG stepY = std::labs(virtualCursorY_.load(std::memory_order_relaxed) - beforeY);
        stepCounts_[std::clamp<LONG>(std::max(stepX, stepY), 0, 3)].fetch_add(1, std::memory_order_relaxed);
        // Gain is meaningless here and the HUD says so by reading zero rather
        // than showing the last relative report's figure forever.
        lastGain_ = 0.0f;
    }

    PublishVirtualCursor();
}

void Win32InputGrab::ClampVirtualCursor() {
    const RECT bounds = PointerBounds();
    virtualCursorX_.store(std::clamp<LONG>(virtualCursorX_.load(), bounds.left, bounds.right - 1));
    virtualCursorY_.store(std::clamp<LONG>(virtualCursorY_.load(), bounds.top, bounds.bottom - 1));
}

// See the declaration for why this exists at all. Called from both places
// that move the position, after the clamp, so the cursor is only ever put
// somewhere the position is allowed to be.
void Win32InputGrab::PublishVirtualCursor() {
    if (softwarePointerDrawn_.load(std::memory_order_relaxed)) {
        return;  // it is being drawn; the real cursor is hidden and idle
    }
    const POINT at = VirtualCursor();
    SetCursorPos(at.x, at.y);
}

// Reads the same pointer settings the desktop cursor obeys: the speed
// slider, whether "enhance pointer precision" is on, and if it is, the
// acceleration curve itself out of the registry. Called each time the grab
// activates, so changing any of them in Settings takes effect on the next
// show rather than needing a restart.
void Win32InputGrab::LoadPointerBallistics() {
    int slider = 10;
    if (!SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &slider, 0)) {
        slider = 10;
    }
    int mouseParams[3]{};
    ballisticsEnabled_ =
        SystemParametersInfoA(SPI_GETMOUSE, 0, mouseParams, 0) != FALSE && mouseParams[2] != 0;
    // Which multiplier the slider means depends on whether the curve is on -
    // see SliderMultiplier - so it can only be resolved once that is known.
    pointerScale_ = SliderMultiplier(slider, ballisticsEnabled_);
    curveValid_ = false;
    if (!ballisticsEnabled_) {
        return;
    }

    // Five points each, one per 8-byte entry, 16.16 fixed point in the low
    // four bytes: X is speed in mickeys per millisecond, Y the pointer
    // travel that speed should produce.
    const auto readCurve = [](const char* name, float (&out)[5]) {
        HKEY key{};
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Control Panel\\Mouse", 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return false;
        }
        BYTE data[40]{};
        DWORD size = sizeof(data);
        const LSTATUS status = RegQueryValueExA(key, name, nullptr, nullptr, data, &size);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS || size < sizeof(data)) {
            return false;
        }
        for (int i = 0; i < 5; ++i) {
            const BYTE* entry = data + i * 8;
            const DWORD fixed = static_cast<DWORD>(entry[0]) | (static_cast<DWORD>(entry[1]) << 8) |
                                (static_cast<DWORD>(entry[2]) << 16) | (static_cast<DWORD>(entry[3]) << 24);
            out[i] = static_cast<float>(fixed) / 65536.0f;
        }
        return true;
    };
    curveValid_ = readCurve("SmoothMouseXCurve", curveSpeed_) && readCurve("SmoothMouseYCurve", curveOutput_);
}

// The acceleration curve's gain at a given speed: how many pixels a mickey
// is worth when the hand is moving this fast. Below the first point the
// curve is flat, which is what keeps slow movement gentle; the segments
// above it rise, which is what makes a fast flick cross the screen.
float Win32InputGrab::BallisticGain(float mickeysPerMs) const {
    if (!curveValid_ || mickeysPerMs <= 0.0f) {
        return 1.0f;
    }
    for (int i = 1; i < 5; ++i) {
        if (mickeysPerMs <= curveSpeed_[i]) {
            const float span = curveSpeed_[i] - curveSpeed_[i - 1];
            const float t = span > 0.0f ? (mickeysPerMs - curveSpeed_[i - 1]) / span : 0.0f;
            const float output = curveOutput_[i - 1] + t * (curveOutput_[i] - curveOutput_[i - 1]);
            return output / mickeysPerMs;
        }
    }
    return curveOutput_[4] / curveSpeed_[4] > 0.0f ? curveOutput_[4] / mickeysPerMs : 1.0f;
}

// Raw device counts in, whole pixels out, with the fraction kept for next
// time. Keeping the remainder is the entire point: a slow hand produces
// one-count reports whose scaled contribution is well under a pixel, and
// rounding each one on its own turns steady slow movement into no movement
// at all. Accumulating instead means the pointer always eventually goes
// where the hand went - and it is why the position kept here survives
// countering, where Windows' own accumulator does not (see
// EditModeInputOptions::CounterRawMouseInputCanBeUsed).
//
// The scale is Windows' own pointer-speed slider, plus its "enhance pointer
// precision" curve when that is on, so the pointer travels roughly the
// distance the desktop one would from the same hand movement.
void Win32InputGrab::MoveVirtualCursorRaw(LONG rawDx, LONG rawDy) {
    IntegrateRawMovement(rawDx, rawDy);
    PublishVirtualCursor();
}

void Win32InputGrab::IntegrateRawMovement(LONG rawDx, LONG rawDy) {
    std::lock_guard<std::mutex> lock(pointerMutex_);
    float gain = pointerScale_;
    if (ballisticsEnabled_ && curveValid_) {
        // Speed needs the interval between reports, which is why this keeps
        // its own clock: a mickey means nothing without knowing how long it
        // took. Clamped because the first report after a pause would
        // otherwise read as infinitely slow, and a stalled queue as
        // infinitely fast.
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        float deltaMs = 8.0f;
        if (reportFrequency_.QuadPart > 0 && lastReportTime_.QuadPart != 0) {
            deltaMs = static_cast<float>(now.QuadPart - lastReportTime_.QuadPart) * 1000.0f /
                      static_cast<float>(reportFrequency_.QuadPart);
        }
        lastReportTime_ = now;
        deltaMs = std::clamp(deltaMs, 0.5f, 100.0f);

        const float magnitude =
            std::sqrt(static_cast<float>(rawDx) * rawDx + static_cast<float>(rawDy) * rawDy);
        gain *= kBallisticScale * BallisticGain(magnitude / deltaMs);
    }

    lastGain_ = gain;

    // The position is a float, rounded only where whole pixels are actually
    // required: the messages posted to the overlay carry integer client
    // coordinates. What gets *drawn* uses the unrounded value, and that is
    // what stops the pointer stepping.
    //
    // Rounding here instead cost nothing per report and showed up per
    // frame. Measured over a slow sweep: no single report ever moved the
    // pointer two pixels, yet 9% of frames did, because two reports landed
    // between two draws. Windows never shows this - its cursor is a
    // hardware one moved at report rate, not drawn at frame rate - and
    // keeping the fraction is the software equivalent.
    const RECT bounds = PointerBounds();
    preciseX_ = std::clamp(preciseX_ + static_cast<float>(rawDx) * gain, static_cast<float>(bounds.left),
                            static_cast<float>(bounds.right - 1));
    preciseY_ = std::clamp(preciseY_ + static_cast<float>(rawDy) * gain, static_cast<float>(bounds.top),
                            static_cast<float>(bounds.bottom - 1));
    // Floor, matching what ImGui does to its own mouse position, so the
    // integer everything else sees is the same integer ImGui hit-tests
    // against.
    const LONG beforeX =
        virtualCursorX_.exchange(static_cast<LONG>(std::floor(preciseX_)), std::memory_order_relaxed);
    const LONG beforeY =
        virtualCursorY_.exchange(static_cast<LONG>(std::floor(preciseY_)), std::memory_order_relaxed);

    // How far this one report moved the pointer, bucketed for the HUD - the
    // difference between coarse arithmetic here and a smooth pointer being
    // sampled coarsely further down.
    const LONG stepX = std::labs(virtualCursorX_.load(std::memory_order_relaxed) - beforeX);
    const LONG stepY = std::labs(virtualCursorY_.load(std::memory_order_relaxed) - beforeY);
    stepCounts_[std::clamp<LONG>(std::max(stepX, stepY), 0, 3)].fetch_add(1, std::memory_order_relaxed);
}

bool Win32InputGrab::VirtualCursorActive() const { return WantPointerGrab() && overlay_ != nullptr; }

// Whether a text field can be typed into without this window taking focus:
// the hook is swallowing the whole keyboard and handing it to the overlay,
// characters included (see PostCharactersToOverlay).
//
// Deliberately not the same question as WantKeyboard, which is also true for
// the input options HUD alone - that case swallows the digits and lets
// everything else through to whoever has focus, which is no use to a text
// field.
bool Win32InputGrab::DeliversTypingToOverlay() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return CanDeliverTypingLocked() && WantsAllKeystrokesLocked();
}

bool Win32InputGrab::CanDeliverTyping() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return CanDeliverTypingLocked();
}

bool Win32InputGrab::WantsAllKeystrokes() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return WantsAllKeystrokesLocked();
}

void Win32InputGrab::SetTextFieldOpen(bool open) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (textFieldOpen_ == open) {
            return;
        }
        textFieldOpen_ = open;
    }
    // Refresh does the rest: this changes WantKeyboard, and the handover on
    // that transition is already written - hook installed and modifiers
    // seeded on the way in, key-ups and modifiers handed back on the way out.
    Refresh();
}

bool Win32InputGrab::SoftwarePointerWanted() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    // Not gated on overlay_ or on the grab: the drawing happens in core,
    // which asks this only to decide whether to hide the OS cursor over a
    // window it already has.
    return active_ && options_.SoftwarePointerDrawn();
}

InputGrabDiagnostics Win32InputGrab::Diagnostics() const {
    InputGrabDiagnostics out;
    {
        std::lock_guard<std::mutex> lock(pointerMutex_);
        out.pointerGain = lastGain_;
        out.ballisticsEnabled = ballisticsEnabled_ && curveValid_;
    }
    for (int i = 0; i < 4; ++i) {
        out.stepCounts[i] = stepCounts_[i].load(std::memory_order_relaxed);
        out.frameSteps[i] = frameSteps_[i].load(std::memory_order_relaxed);
    }
    out.correctionLagMsLast = correctionLagMsLast_.load(std::memory_order_relaxed);
    out.correctionLagMsMax = correctionLagMsMax_.load(std::memory_order_relaxed);
    out.correctionsInjected = correctionsInjected_.load(std::memory_order_relaxed);
    return out;
}

// Called once per rendered frame: how far the pointer moved since the last
// frame, bucketed the same way. If the per-report numbers are all ones and
// the per-frame numbers are twos, the arithmetic is fine and the pointer is
// simply being drawn once a frame while the hand moves continuously - which
// is a different problem with a different fix.
void Win32InputGrab::SampleFrameStep() {
    const POINT now = VirtualCursor();
    const LONG dx = now.x - lastFramePoint_.x;
    const LONG dy = now.y - lastFramePoint_.y;
    lastFramePoint_ = now;
    const int step = static_cast<int>(std::max(std::labs(dx), std::labs(dy)));
    if (step > 0) {
        frameSteps_[std::clamp(step, 0, 3)].fetch_add(1, std::memory_order_relaxed);
    }
}

POINT Win32InputGrab::VirtualCursor() const {
    return POINT{virtualCursorX_.load(std::memory_order_relaxed), virtualCursorY_.load(std::memory_order_relaxed)};
}

UINT Win32InputGrab::ButtonFlags() const {
    UINT flags = 0;
    if (leftDown_) {
        flags |= MK_LBUTTON;
    }
    if (rightDown_) {
        flags |= MK_RBUTTON;
    }
    if (middleDown_) {
        flags |= MK_MBUTTON;
    }
    return flags;
}

void Win32InputGrab::PostToOverlay(UINT message, WPARAM wParam, POINT screenPoint) {
    if (!overlay_) {
        return;
    }
    POINT client = screenPoint;
    ScreenToClient(overlay_, &client);
    PostMessageA(overlay_, message, wParam, MAKELPARAM(client.x, client.y));
}

LRESULT CALLBACK Win32InputGrab::MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    Win32InputGrab& self = Instance();
    if (code != HC_ACTION) {
        return CallNextHookEx(self.mouseHook_, code, wParam, lParam);
    }
    const auto& event = *reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    // Our own cancellation corrections. Swallowed like everything else, and
    // they still reach the game: raw input is generated no matter what this
    // hook returns (measured - 30 swallowed synthetic moves left the cursor
    // exactly where it started, while a raw-input sink in another process
    // saw all 30), so the camera gets the correction either way.
    //
    // Swallowing is what keeps a correction from dragging the real cursor,
    // and that matters because the real cursor is the reference the virtual
    // one is measured against (see OnMouse). Passing these through meant any
    // correction landing between a movement and this hook reading the cursor
    // corrupted that movement's delta by roughly its own size - which is
    // what a jittery pointer under "hold the camera still" actually was.
    //
    // Still needs its own branch rather than falling through: treated as
    // ordinary input below, a correction would move the virtual cursor too,
    // and the pointer would drift against the hand.
    if (event.dwExtraInfo == kOwnInjectionMarker) {
        return 1;
    }
    const LRESULT result = self.OnMouse(wParam, event);
    if (result != 0) {
        return result;
    }
    return CallNextHookEx(self.mouseHook_, code, wParam, lParam);
}

// The hook's whole remaining job is to discard. Everything the overlay is
// told about the mouse - movement, buttons, wheel - is read from the raw
// input stream in OnRawMouse instead, so that position and clicks come from
// one ordered source rather than two that can overtake each other.
//
// The hook still has to exist: raw input is read-only, and this is the only
// user-mode mechanism that can stop an event reaching the game's legacy
// input path or moving the real cursor.
//
// Movement included, and that stays true whichever pointer is on screen.
// Letting movement through so that Windows could keep the real cursor was
// tried, for the mode where the hardware cursor is the pointer, and it
// broke slow movement outright: the corrections go through the same pointer
// ballistics on their way to the game, and canceled the physical
// movement's sub-pixel fraction before the cursor could accumulate it (22px
// of slow travel became 1px). The position is kept here now and written to
// the real cursor instead - see PublishVirtualCursor.
//
// Raw button events reach the game regardless of any of this: nothing in
// user mode can stop those, which is why a game that reads its clicks that
// way still needs the overlay to take focus outright.
LRESULT Win32InputGrab::OnMouse(WPARAM /*message*/, const MSLLHOOKSTRUCT& /*event*/) {
    return 1;  // every mouse event is swallowed; nothing here is interpreted
}

// Everything the overlay learns about the mouse, from the one stream that
// carries movement and button transitions together and therefore cannot
// deliver them out of order. A single report can hold both, so movement is
// applied first: a click then lands at the position that report moved to,
// rather than the previous one.
void Win32InputGrab::OnRawMouse(const RAWMOUSE& mouse) {
    // An absolute device reports where the pointer *is*, not how far it
    // moved: tablets, touchscreens, and the virtual pointer every VM console
    // presents (a QEMU/SPICE guest gets a USB tablet, not a mouse). This used
    // to be skipped outright, which meant the pointer simply never moved for
    // anyone on such a device - and since the mouse events are swallowed all
    // the same, that left the overlay unusable rather than merely degraded.
    //
    // The position is normalized to 0..65535 over the primary screen, or over
    // the whole virtual desktop when MOUSE_VIRTUAL_DESKTOP says so. No
    // ballistics: acceleration curves describe how far a *delta* should carry
    // the pointer, and there is no delta here - the device has already said
    // where to be.
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        const bool virtualDesktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int originX = virtualDesktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
        const int originY = virtualDesktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
        const int width = GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int height = GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        if (width > 0 && height > 0) {
            SetVirtualCursorAbsolute(
                originX + static_cast<LONG>((static_cast<int64_t>(mouse.lLastX) * width) / 65535),
                originY + static_cast<LONG>((static_cast<int64_t>(mouse.lLastY) * height) / 65535));
        }
        // Deliberately no camera correction on this path. The correction has
        // to be the negation of a device *delta*, and an absolute device
        // produces none; deriving one from consecutive positions and
        // injecting it as relative motion mixes two coordinate systems and
        // has not been measured against a real game. So "Counter raw mouse
        // input" does nothing on an absolute device, which is the honest
        // outcome rather than a guess - and a game doing relative mouse-look
        // is not one you drive with a tablet anyway.
    } else if (mouse.lLastX != 0 || mouse.lLastY != 0) {
        MoveVirtualCursorRaw(mouse.lLastX, mouse.lLastY);
        if (countering_.load(std::memory_order_relaxed)) {
            // The negation has to be of the raw *device* delta, not of
            // anything derived from cursor positions: those are
            // post-acceleration and clamped at the screen edges, and
            // canceling one with the other leaves most of the motion behind
            // (measured: ~27% removed, versus ~98% this way).
            pendingCorrectionX_.fetch_add(mouse.lLastX);
            pendingCorrectionY_.fetch_add(mouse.lLastY);
            // The clock starts on the oldest report still uncountered, and
            // only on that one: what the game gets to integrate is measured
            // from when the *first* uncorrected movement reached it.
            int64_t none = 0;
            pendingCorrectionSince_.compare_exchange_strong(none, NowTicks(), std::memory_order_relaxed);

        }

        // Movement is deliberately not posted anywhere. The render thread
        // reads the pointer every frame anyway and emits one Move event
        // itself while a button is held - see Win32OverlayWindow::RenderFrame
        // - which is the OS's own one-per-frame delivery by construction.
        // Posting a move per report here turned a 1000Hz mouse into a
        // message flood and a stroke point per report.
    }

    const USHORT flags = mouse.usButtonFlags;
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) {
        leftDown_ = true;
        PostToOverlay(WM_LBUTTONDOWN, ButtonFlags(), VirtualCursor());
    }
    if (flags & RI_MOUSE_LEFT_BUTTON_UP) {
        leftDown_ = false;
        PostToOverlay(WM_LBUTTONUP, ButtonFlags(), VirtualCursor());
    }
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
        rightDown_ = true;
        PostToOverlay(WM_RBUTTONDOWN, ButtonFlags(), VirtualCursor());
    }
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP) {
        rightDown_ = false;
        PostToOverlay(WM_RBUTTONUP, ButtonFlags(), VirtualCursor());
    }
    if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
        middleDown_ = true;
        PostToOverlay(WM_MBUTTONDOWN, ButtonFlags(), VirtualCursor());
    }
    if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) {
        middleDown_ = false;
        PostToOverlay(WM_MBUTTONUP, ButtonFlags(), VirtualCursor());
    }
    if ((flags & RI_MOUSE_WHEEL) && overlay_ != nullptr) {
        // usButtonData carries the wheel delta as a signed value in an
        // unsigned field. WM_MOUSEWHEEL's lParam is in *screen* coordinates,
        // unlike every button message above, so this one doesn't go through
        // PostToOverlay's ScreenToClient conversion.
        const auto delta = static_cast<SHORT>(mouse.usButtonData);
        const POINT at = VirtualCursor();
        PostMessageA(overlay_, WM_MOUSEWHEEL, MAKEWPARAM(ButtonFlags(), delta),
                      MAKELPARAM(at.x, at.y));
    }
}

LRESULT CALLBACK Win32InputGrab::KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    Win32InputGrab& self = Instance();
    if (code != HC_ACTION) {
        return CallNextHookEx(self.keyboardHook_, code, wParam, lParam);
    }
    // No injected-input exception here, unlike the mouse: nothing in this
    // app injects keystrokes, so there is no own-work case to let past, and
    // "the overlay has the keyboard" should mean it regardless of where a
    // keystroke came from.
    const auto& event = *reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    const LRESULT result = self.OnKeyboard(wParam, event);
    if (result != 0) {
        return result;
    }
    return CallNextHookEx(self.keyboardHook_, code, wParam, lParam);
}

// Turns a swallowed key-down into the character(s) it stands for and posts
// them as WM_CHAR, which is the half of typing the OS normally does and this
// hook has to do instead.
//
// ImGui implements text *editing* itself - caret movement, shift-selection,
// word jumps, backspace - entirely from key events, which PostKeyToOverlay
// already delivers. What it cannot do is turn a virtual key into a character:
// that is the keyboard layout's job, it arrives as WM_CHAR from
// TranslateMessage, and TranslateMessage only runs for the *focused* window.
// That single gap is the whole reason a text field would need real focus.
//
// The keyboard state handed to ToUnicodeEx is our own, not the thread's:
// every key that reaches here has been swallowed, so GetKeyboardState would
// report none of it held and every letter would come out unshifted.
//
// The layout is the foreground window's, not ours. The user is looking at
// their game's language, and that is the layout their muscle memory is on.
void Win32InputGrab::PostCharactersToOverlay(UINT vk, const KBDLLHOOKSTRUCT& event) {
    if (!overlay_) {
        return;
    }
    // Ctrl without Alt is a shortcut, not text - Ctrl+A would otherwise
    // produce U+0001. AltGr arrives as Ctrl+Alt together and *is* text on
    // layouts that use it (the German @ and \ live there), so only the
    // Ctrl-alone case is excluded.
    if (ctrlDown_ && !altDown_) {
        return;
    }

    keyboardState_[VK_SHIFT] = shiftDown_ ? 0x80 : 0;
    keyboardState_[VK_CONTROL] = ctrlDown_ ? 0x80 : 0;
    keyboardState_[VK_MENU] = altDown_ ? 0x80 : 0;
    // Toggles, not held states: Caps Lock survives the hook untouched, so the
    // OS still has the truth about it.
    keyboardState_[VK_CAPITAL] = static_cast<BYTE>(GetKeyState(VK_CAPITAL) & 0x0001);

    const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    wchar_t chars[8] = {};
    const int count = ToUnicodeEx(vk, event.scanCode, keyboardState_, chars,
                                   static_cast<int>(std::size(chars)), 0, layout);
    // A dead key (accent awaiting its vowel) returns -1 and leaves itself
    // pending in the layout's per-thread state, so the next call composes.
    // Nothing to post yet, and nothing to fix up: this runs on the grab's own
    // hook thread, so that pending state belongs to us alone and cannot
    // disturb whatever the foreground application is in the middle of.
    if (count <= 0) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        // PostMessageW to a window registered wide (see kWindowClassName), so
        // this arrives as the UTF-16 unit it is rather than a code-page byte.
        PostMessageW(overlay_, WM_CHAR, static_cast<WPARAM>(chars[i]), 1);
    }
}

void Win32InputGrab::PostKeyToOverlay(UINT vk, const KBDLLHOOKSTRUCT& event, bool isDown) {
    if (!overlay_) {
        return;
    }
    LPARAM lParam = 1;  // repeat count
    lParam |= static_cast<LPARAM>(event.scanCode & 0xFF) << 16;
    if (event.flags & LLKHF_EXTENDED) {
        lParam |= 1LL << 24;
    }
    if (!isDown) {
        lParam |= (1LL << 30) | (1LL << 31);  // previous state, transition
    }
    PostMessageA(overlay_, isDown ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>(vk), lParam);
}

// Ends every key the hook is still holding down on the overlay's behalf, and
// forgets them, so a grab never begins or ends with a key latched.
//
// Both halves matter. Forgetting alone was the first attempt and it produced
// a HUD whose number keys worked every *other* press: toggling an option
// restarts edit mode, the restart deactivates and reactivates the grab
// between the key's down and its up, and the up then arrived with nothing
// recorded - so it was passed to the OS and never handed to the overlay.
// ImGui went on believing the digit was held, and the next press was not a
// press at all. Posting the key-up first is what keeps the app's own idea of
// the keyboard honest across a restart; clearing the record is what keeps a
// stale entry from authorizing the swallow of an up whose down the OS *did*
// see, which is the whole point of the rule in OnKeyboard.
//
// The physical up that arrives afterwards is then passed through to the OS.
// That leaves the OS an up for a key it never saw go down, which it ignores.
void Win32InputGrab::ReleaseSwallowedKeys() {
    for (UINT vk = 0; vk < kVirtualKeyCount; ++vk) {
        if (!swallowedDown_[vk].exchange(false, std::memory_order_relaxed)) {
            continue;
        }
        if (overlay_) {
            // Synthesized rather than forwarded - there is no real event
            // here - so the scan code is 0 and only the transition bits that
            // make it a release are set.
            PostMessageA(overlay_, WM_KEYUP, static_cast<WPARAM>(vk), (1LL << 30) | (1LL << 31) | 1);
        }
    }
}

// Tells the OS about modifiers the hook swallowed and the user is still
// holding, at the moment the hook stops being there to swallow them.
//
// The mirror of the seeding in SetActive, and the same fact from the other
// end: a grab that starts mid-chord leaves *us* not knowing what is held, and
// a grab that ends mid-chord leaves *Windows* not knowing. Hold Ctrl and Alt
// while the overlay is already up and press O: the grab dispatches that hotkey
// itself and hides the overlay, which takes the hook down - and every
// subsequent O reaches an OS that never saw Ctrl or Alt go down, so
// RegisterHotKey has no chord to match and the overlay refuses to come back.
// Measured: with the modifiers held, GetAsyncKeyState reported both up.
//
// Only the modifiers, deliberately. Handing back every swallowed key would
// mean typing the letters into whatever has focus; a modifier the user is
// genuinely holding is true information, and the physical key-up that follows
// reaches an OS with no hook in the way and squares the books.
void Win32InputGrab::HandHeldModifiersToSystem(bool ctrl, bool shift, bool alt) {
    INPUT keys[3]{};
    UINT count = 0;
    const auto add = [&](WORD vk) {
        keys[count].type = INPUT_KEYBOARD;
        keys[count].ki.wVk = vk;
        // Marked as ours: this runs on the transition, which is a moment
        // before the hook is actually taken down, so without the mark our own
        // hook would swallow these straight back and the OS would learn
        // nothing. See OnKeyboard's first line.
        keys[count].ki.dwExtraInfo = kOwnInjectionMarker;
        ++count;
    };
    if (ctrl) {
        add(VK_CONTROL);
    }
    if (shift) {
        add(VK_SHIFT);
    }
    if (alt) {
        add(VK_MENU);
    }
    if (count > 0) {
        SendInput(count, keys, sizeof(INPUT));
    }
}

// Records Ctrl/Shift/Alt as the grab's own view of what is held, and answers
// whether `vk` was one of them. This record is what the grab matches hotkeys
// against and what the render thread ORs into the modifier state it hands
// ImGui - a swallowed key updates nothing the OS can be asked about, so this
// is the only place that knows.
bool Win32InputGrab::TrackModifier(UINT vk, bool isDown) {
    switch (vk) {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
            ctrlDown_ = isDown;
            return true;
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
            altDown_ = isDown;
            return true;
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
            shiftDown_ = isDown;
            return true;
        default:
            return false;
    }
}

// Swallows a key and remembers that it did, so the matching key-up can be
// told apart from one whose key-down was never ours. Always returns 1, the
// hook's "this event goes no further".
LRESULT Win32InputGrab::SwallowKey(UINT vk, bool isDown) {
    if (isDown && vk < kVirtualKeyCount) {
        swallowedDown_[vk].store(true, std::memory_order_relaxed);
    }
    return 1;
}

LRESULT Win32InputGrab::OnKeyboard(WPARAM message, const KBDLLHOOKSTRUCT& event) {
    const bool isDown = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
    const UINT vk = event.vkCode;

    // Our own hand-back on the way out (see HandHeldModifiersToSystem), whose
    // entire purpose is to reach the OS. Swallowing it would defeat it.
    if (event.dwExtraInfo == kOwnInjectionMarker) {
        return 0;
    }

    // A key-up is only ours to swallow if we swallowed its key-down.
    //
    // The hotkey that turns edit mode on is the case that matters, and it is
    // unavoidable rather than unlikely: the overlay is hidden when its hotkey
    // is pressed, so nothing is installed yet and those key-downs reach the
    // OS normally. The hotkey then shows the overlay, which installs this
    // hook - and the key-*ups* a moment later were being swallowed. Windows
    // therefore never learned the keys came up, and GetAsyncKeyState went on
    // reporting Ctrl and Alt held for the rest of the session.
    //
    // What that looked like: Win32OverlayWindow::RenderFrame feeds ImGui
    // `asyncDown(VK_MENU) || grabAlt`, so ImGui saw Alt permanently down.
    // Items could be dragged with no Alt held, and a right-click on an item's
    // body was taken as an Alt+right-drag resize instead of opening the
    // context ring - the ring only survived over an item's chrome, where
    // WantCaptureMouse blocks the Alt-drag. The stuck state also outlived the
    // process, and it was never only ours: the game underneath got the
    // key-down and no key-up either.
    //
    // Letting the up through is the whole fix. The overlay is not told about
    // it - it never saw the corresponding down. The modifier record still has
    // to be told, though: SetActive seeds it from whatever is physically held
    // when a grab starts, so a modifier released during the grab reaches this
    // path with the record saying it is down, and nothing else would clear
    // it.
    if (!isDown && vk < kVirtualKeyCount && !swallowedDown_[vk].exchange(false, std::memory_order_relaxed)) {
        TrackModifier(vk, false);
        return 0;
    }

    // Debug scaffolding, and the reason this hook is installed for the whole
    // of edit mode rather than only while grabbing: the input options HUD
    // offers a number key per option, and the situation those options exist
    // for is precisely the one where the overlay has no keyboard focus to
    // receive them - dontStealFocus leaves focus with the game on
    // purpose. So the bare digits the HUD advertises are taken here and
    // handed to the overlay, and every other key passes through untouched.
    // Remove this branch (and restore WantKeyboard to requiring
    // options_.dontForwardKeystrokes) when the HUD goes.
    if (!WantsAllKeystrokes()) {
        const bool isHudDigit = vk >= '1' && vk < '1' + static_cast<UINT>(HudDigits());
        const bool bare = (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
                          (GetKeyState(VK_MENU) & 0x8000) == 0 && (GetKeyState(VK_SHIFT) & 0x8000) == 0;
        if (!isHudDigit || !bare) {
            return 0;  // everyone else's key, left alone
        }
        PostKeyToOverlay(vk, event, isDown);
        return SwallowKey(vk, isDown);
    }

    if (TrackModifier(vk, isDown)) {
        return SwallowKey(vk, isDown);
    }

    if (isDown) {
        // Copied under the lock rather than iterated in place: the app
        // thread can add or remove hotkeys (a rebind in Settings) while
        // this runs on the hook thread.
        std::vector<Hotkey> hotkeys;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            hotkeys = hotkeys_;
        }
        for (const Hotkey& hotkey : hotkeys) {
            if (hotkey.vk == vk && hotkey.ctrl == ctrlDown_ && hotkey.alt == altDown_ &&
                hotkey.shift == shiftDown_) {
                // Exactly the message RegisterHotKey would have produced,
                // delivered to exactly the window that registered it - so
                // Win32PlatformHost's existing WM_HOTKEY handler dispatches
                // it without knowing this didn't come from the OS.
                const UINT modifiers = (ctrlDown_ ? MOD_CONTROL : 0u) | (altDown_ ? MOD_ALT : 0u) |
                                       (shiftDown_ ? MOD_SHIFT : 0u);
                PostMessageA(hotkey.target, WM_HOTKEY, static_cast<WPARAM>(hotkey.id),
                             MAKELPARAM(modifiers, vk));
                // Deliberately still falls through to the overlay below: a
                // hotkey is a global chord, not a reason for the focused
                // surface to go deaf.
                break;
            }
        }
    }

    // Hand the key to the overlay. "Take over the keyboard" has to mean the
    // overlay gets it, not that nobody does - without this the app's own
    // plain-key shortcuts are dead for exactly as long as the grab is on,
    // which is the whole time it matters. Safe against double delivery: the
    // hook returns 1 below, so Windows routes this key nowhere else.
    //
    // Modifier chords still don't reach the app: ImGui's Win32 backend
    // reads Ctrl/Shift/Alt with GetKeyState, and a key this hook swallowed
    // never reaches the state GetKeyState reports. Plain keys work, chords
    // don't, and fixing that would mean synthesizing modifier state rather
    // than reading it.
    PostKeyToOverlay(vk, event, isDown);
    // And the characters those keys stand for, which is what a text field is
    // actually waiting on - see PostCharactersToOverlay.
    if (isDown) {
        PostCharactersToOverlay(vk, event);
    }
    // Swallowed either way: the game sees no keyboard at all.
    return SwallowKey(vk, isDown);
}

// Our own cancellation correction arriving back through raw input. It must
// drive neither the pointer nor another correction.
//
// The dwExtraInfo marker is the whole test. A second one - treat any null
// device handle as ours while cancellation is running, as a fallback in case
// RAWMOUSE didn't carry dwExtraInfo through - costs far more than it is
// worth: a null device handle is what *all* injected input
// has, so with cancellation on, every event from remote desktop, an
// accessibility tool or any automation was discarded. Worse, the whole
// report went, buttons and wheel included (see RawInputSinkWndProc), so on
// such a setup the overlay could not be clicked at all - no pointer
// movement, no context ring, nothing. That was invisible while cancellation
// was opt-in and became the default experience the moment it wasn't.
//
// If dwExtraInfo ever does fail to survive, the symptom is a pointer that
// drifts against the hand while cancellation is on, which is a great deal
// easier to notice than input that silently disappears.
bool Win32InputGrab::IsOwnCorrection(const RAWINPUT& input) const {
    return input.data.mouse.ulExtraInformation == kOwnInjectionMarker;
}

LRESULT CALLBACK Win32InputGrab::RawInputSinkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg != WM_INPUT) {
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    UINT size = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    BYTE buffer[sizeof(RAWINPUT) + 32];
    if (size <= sizeof(buffer) && GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer,
                                                   &size, sizeof(RAWINPUTHEADER)) == size) {
        const auto* input = reinterpret_cast<RAWINPUT*>(buffer);
        if (input->header.dwType == RIM_TYPEMOUSE && !Instance().IsOwnCorrection(*input)) {
            Instance().OnRawMouse(input->data.mouse);
        }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

}  // namespace sz::platform::win32
