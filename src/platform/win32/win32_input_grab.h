#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "platform/platform_types.h"
#include "platform/pointer_ballistics.h"

namespace sz::platform::win32 {

// Takes physical mouse/keyboard input away from the foreground application
// while the overlay is up in edit mode, without ever asking that
// application to give up focus - the point being that a game which pauses
// on deactivation never notices anything happened.
//
// Implemented with WH_MOUSE_LL / WH_KEYBOARD_LL, the only user-mode
// mechanism that can discard an input event before another process sees
// it. See EditModeInputOptions for what that does and does not reach, and
// docs/ARCHITECTURE.md for the measurements behind those claims.
//
// A process-wide singleton, which is not a stylistic choice: low-level
// hooks are installed per-process and their callbacks are plain function
// pointers with no user-data parameter, so the callback has to find its
// state through a static anyway. Making that explicit is better than a
// file-static pointer to "the" instance pretending not to be one.
class Win32InputGrab {
public:
    static Win32InputGrab& Instance();

    // The hook thread's name, as a debugger, a crash dump and the tests see
    // it: they count it among the process's threads by this.
    static constexpr const wchar_t* kHookThreadName = L"Spickzettel input grab";

    // The window synthesized mouse input is posted to - the overlay
    // itself. Everything below no-ops until this is set.
    void SetOverlayWindow(HWND hwnd);

    // The desktop rectangle of the display the overlay covers. The pointer
    // the grab keeps stays inside it, as the real cursor stays inside the
    // desktop - otherwise it would wander onto a display with nothing of
    // ours on it, where there is nothing to click and no pointer drawn.
    void SetPointerBounds(const RECT& bounds);

    void SetOptions(const EditModeInputOptions& options);

    // Whether the conditions for grabbing currently hold: the overlay is
    // visible, in edit mode rather than click-through view-only mode.
    // Hooks are installed and removed to match; nothing is grabbed while
    // the overlay is hidden.
    void SetActive(bool active);

    // Temporarily hands the keyboard back for real text entry, when a text
    // field takes focus because the grab cannot type into it - see
    // IOverlayWindow::RequestTextInput/ReleaseTextInput. Without this a hook
    // that came in meanwhile would swallow every keystroke the field exists
    // to receive.
    void SetKeyboardSuspended(bool suspended);

    // Whether the game underneath still holds OS focus - i.e. whether
    // dontStealFocus is on. The precondition for grabbing anything
    // at all: with edit mode holding focus the ordinary way, the game has
    // already stopped receiving input (raw input included, since a game
    // registers without RIDEV_INPUTSINK and so only receives it while
    // foreground) and there is nothing left to take. Installing hooks then
    // would buy nothing and cost a system-wide chokepoint on every mouse
    // event - see EnsureHookThread.
    //
    // Deliberately does not gate SoftwarePointerWanted: drawing the pointer
    // is a matter of appearance and stays available either way.
    void SetGameKeepsFocus(bool gameKeepsFocus);

    // The app's global hotkeys, so the grab can dispatch them itself while
    // the keyboard is swallowed. Measured: a swallowing low-level keyboard
    // hook suppresses WM_HOTKEY, so without this, turning the grab on would
    // also disable the hotkey that turns it off.
    void AddHotkey(int id, const KeyCombo& combo, HWND target);
    void RemoveHotkey(int id);
    // While true, the grab matches none of them: a hotkey's chord is a key
    // like any other, for a row in Settings waiting for one (see
    // IPlatformHost::SetHotkeysPaused).
    void SetHotkeysPaused(bool paused) { hotkeysPaused_.store(paused, std::memory_order_relaxed); }

    // True while ImGui must navigate by the position below instead of by
    // the real cursor. Under a mouse grab the real cursor is left entirely
    // to the game - which may pin it, hide it, or set it back to the screen
    // center every frame - so it stops being something the overlay can
    // point with. Trying to share it with a game that does any of those is
    // a fight that cannot be won: a game's SetCursorPos is not an input
    // event and is invisible to any hook.
    bool VirtualCursorActive() const;

    // True while the overlay draws its own pointer and the OS one therefore
    // has to be hidden over this window. A superset of VirtualCursorActive:
    // under a grab the drawn pointer is the only usable one, but the option
    // can also be on by itself, in which case the real cursor is still
    // doing the moving and only the look changes. See
    // EditModeInputOptions::useSoftwarePointer.
    bool SoftwarePointerWanted() const;

    // Whether a text field can be typed into with no focus change at all,
    // because the hook is delivering the keyboard - characters included - to
    // the overlay. See its definition, and IOverlayWindow::RequestTextInput.
    bool DeliversTypingToOverlay() const;

    // Where that pointer is, in screen coordinates. Accumulated from the
    // movements the hook swallows, so it is unaffected by anything the game
    // does to the real cursor.
    POINT VirtualCursor() const;

    // Which mouse buttons are currently held, as MK_* flags - tracked here
    // because the OS no longer knows: the button-downs were swallowed. Read
    // by the render thread once a frame to decide whether to emit a Move.
    UINT HeldButtons() const { return ButtonFlags(); }

    // Which modifier keys the keyboard hook is currently holding on the
    // game's behalf. A swallowed key never reaches GetAsyncKeyState, so
    // while the keyboard is grabbed this is the only record that Ctrl,
    // Shift or Alt is down - the render thread ORs it into the modifier
    // state it hands ImGui each frame.
    void HeldModifiers(bool& ctrl, bool& shift, bool& alt) const {
        ctrl = modifiers_.Ctrl();
        shift = modifiers_.Shift();
        alt = modifiers_.Alt();
    }

    // The side a sideless VK_CONTROL, VK_MENU or VK_SHIFT is, from the scan
    // code and the extended flag the hook reports with it - any other key
    // as it is. The hook reports sides itself; only injected input sends a
    // sideless one, and taken for both sides, its down was undone by
    // neither side's up alone.
    static UINT SidedModifier(UINT vk, DWORD scanCode, bool extended);

    // Ctrl, Alt and Shift as the grab has seen them, each side on its own:
    // with both Shifts held, letting go of one leaves Shift held. One flag
    // per modifier, as it was, read as up the moment either side came up.
    // Atomic: the hook thread writes it, the render thread reads it, and the
    // app thread seeds it as a grab starts. Public to be tested without a
    // hook.
    class ModifierRecord {
    public:
        // Records `vk` going down or up if it is one of the six modifier
        // keys - or the sideless VK_CONTROL, VK_MENU or VK_SHIFT, taken as
        // both sides - and says whether it was.
        bool Track(UINT vk, bool isDown);
        // Sets every side to what `isDown(vk)` says of it - the keyboard as
        // the system has it, when nothing of it has been swallowed.
        template <typename IsDown>
        void Seed(IsDown isDown) {
            for (int side = 0; side < kSides; ++side) {
                held_[side] = isDown(static_cast<int>(kSideKeys[side]));
            }
        }
        void Clear() {
            for (std::atomic<bool>& side : held_) {
                side = false;
            }
        }
        bool Ctrl() const { return held_[0] || held_[1]; }
        bool Alt() const { return held_[2] || held_[3]; }
        bool Shift() const { return held_[4] || held_[5]; }

    private:
        static constexpr int kSides = 6;
        static constexpr UINT kSideKeys[kSides] = {VK_LCONTROL, VK_RCONTROL, VK_LMENU,
                                                   VK_RMENU,    VK_LSHIFT,   VK_RSHIFT};
        std::atomic<bool> held_[kSides] = {};
    };

    // The overlay's left and right button, from the ones raw input reports.
    // Raw input reports the button pressed, before Windows swaps the two
    // for everything else - "Primary mouse button: Right", SM_SWAPBUTTON -
    // so the grab swaps them itself. A button comes up as the one it went
    // down as, should the setting change while it is held. Hook thread
    // only. Public to be tested without a device.
    class ButtonSwap {
    public:
        // Whether the physical left or right button (`physicalRight`),
        // going down or up, is the overlay's right one, with the buttons
        // `swapped` in Windows as they are now.
        bool Right(bool physicalRight, bool down, bool swapped);

    private:
        struct Pressed {
            bool down = false;
            bool asRight = false;
        };
        Pressed pressed_[2];  // the physical left, the physical right
    };

    // Injects the correction banked so far right now, rather than when
    // countering next ends - for the window to call before it stops hiding
    // the game, so that what the game shows next is already the camera put
    // back. True if there was anything to inject, and it was.
    bool SettleCorrection();

    // Debug scaffolding for the debug overlay's input readout - see
    // InputGrabDiagnostics. SampleFrameStep is called once per rendered
    // frame; Diagnostics is read by the readout.
    InputGrabDiagnostics Diagnostics() const;
    void SampleFrameStep();

    // Takes everything down and ends the hook thread - see StopHookThread.
    // Called once at shutdown, and by the tests between them; safe to call
    // with nothing installed, and the grab can be used again after it.
    void Shutdown();

    // Called by the window every frame it renders: the app thread is alive.
    // See AppThreadStalled for what happens when this stops.
    void Heartbeat();
    // How long the app thread may go without a heartbeat before the hooks
    // stop swallowing - see AppThreadStalled. Frames come at least four times
    // a second while the overlay is up, so this is several missed in a row.
    static constexpr uint64_t kStalledAfterMs = 2000;
    // Whether a heartbeat at `lastBeatMs` is too old at `nowMs`. A beat
    // later than `now` - the app thread beat between the hook reading the
    // clock and reading the beat - is as fresh as they come, not a
    // subtraction wrapped around to "stalled forever".
    static bool IsStalled(uint64_t nowMs, uint64_t lastBeatMs) {
        return nowMs > lastBeatMs && nowMs - lastBeatMs > kStalledAfterMs;
    }

    // The key-downs to hand Windows as a grab of the keyboard ends: one per
    // side of Ctrl, Shift and Alt whose down `swallowed` (indexed by
    // virtual key) says the hook took, each as that very key - see
    // HandHeldModifiersToSystem. Public to be tested without injecting.
    // With `up`, the same keys' ups instead.
    static std::vector<INPUT> ModifierHandBack(const bool (&swallowed)[256], bool up = false);

    // Every key the hook took the down of is taken as released: the
    // overlay is told each came up, and the Ctrl/Shift/Alt record goes
    // blank. So is every mouse button the overlay was told went down. For
    // a switch of the active desktop - the Ctrl+Alt+Del screen, the lock
    // screen, a UAC prompt - where the keys and buttons come up with no
    // hook or raw input of this desktop called: the record kept Ctrl+Alt
    // held, a bare S matched Ctrl+Alt+S, and hiding handed Windows the
    // downs, which stayed down system-wide; and a snippet dragged there
    // went on following the pointer, the button held, until the next
    // click. Called on the hook thread for EVENT_SYSTEM_DESKTOPSWITCH;
    // public to be tested without one.
    void InputLeftOnAnotherDesktop();
    // A key going down or up, as the keyboard hook would be handed it -
    // for tests, which cannot press keys. `heldByWindows` is a down of a
    // key Windows has down already: an auto-repeat.
    LRESULT KeyEventForTesting(UINT vk, bool isDown, bool heldByWindows = false);
    // A key as the installed keyboard hook is handed it, the hook's own
    // checks first - see KeyboardProc.
    LRESULT HookedKeyEventForTesting(UINT vk, bool isDown);
    // The keyboard grab's two ends, as Refresh runs them, with no hook
    // installed.
    void GrabKeyboardForTesting(bool grabbed);
    // What the grab's end hands Windows goes to `sink` rather than to
    // SendInput, and `gap` runs between the grab ending and the hand-back.
    // Empty and null for the real thing.
    void CaptureHandBackForTesting(std::vector<INPUT>* sink, std::function<void()> gap);
    // A raw mouse report with these RI_MOUSE_* button flags and no
    // movement, as the raw input sink would be handed it - for tests,
    // which cannot press buttons.
    void RawMouseButtonsForTesting(USHORT buttonFlags);
    // Registrations for raw mouse input fail while `fail`, as
    // RegisterRawInputDevices would - see ReconcileHooks.
    void FailPointerGrabForTesting(bool fail) { failPointerGrabForTesting_.store(fail); }
    // The mouse hook is not installed while `fail`, as if SetWindowsHookEx
    // had failed - with the raw-input sink up already.
    void FailMouseHookForTesting(bool fail) { failMouseHookForTesting_.store(fail); }
    // The keyboard hook is not installed while `fail`, as if
    // SetWindowsHookEx had failed.
    void FailKeyboardHookForTesting(bool fail) { failKeyboardHookForTesting_.store(fail); }
    // The hook thread is not started while `fail`, as if CreateThread had
    // failed - see EnsureHookThread.
    void FailHookThreadStartForTesting(bool fail) { failHookThreadStartForTesting_.store(fail); }
    // The hook thread's next reconcile waits `ms` first, as a thread held up
    // by something would - see AwaitReconciled.
    void StallHookThreadForTesting(DWORD ms) { stallReconcileForTestingMs_.store(ms); }

private:
    Win32InputGrab() = default;

    struct Hotkey {
        int id = 0;
        UINT vk = 0;
        bool ctrl = false;
        bool alt = false;
        bool shift = false;
        HWND target = nullptr;
    };

    static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK RawInputSinkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    // EVENT_SYSTEM_DESKTOPSWITCH, on the hook thread - see
    // InputLeftOnAnotherDesktop.
    static void CALLBACK DesktopSwitchProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child,
                                           DWORD thread, DWORD time);

    LRESULT OnMouse(WPARAM message, const MSLLHOOKSTRUCT& event);
    // Everything the overlay is told about the mouse comes from here, from
    // the raw stream, so movement and button transitions arrive in the order
    // the device produced them. The hook only discards.
    void OnRawMouse(const RAWMOUSE& mouse);
    LRESULT OnKeyboard(WPARAM message, const KBDLLHOOKSTRUCT& event);
    // Swallows a key and records the key-down, so its key-up can be
    // recognized as ours later. See the rule at the top of OnKeyboard.
    LRESULT SwallowKey(UINT vk, bool isDown);
    // Updates the grab's own Ctrl/Shift/Alt record; true if vk was one.
    bool TrackModifier(UINT vk, bool isDown) { return modifiers_.Track(vk, isDown); }
    // Hands still-held modifiers back to Windows when the hook goes away - see
    // its definition for the hotkey that stops working without it.
    void HandHeldModifiersToSystem(const bool (&swallowed)[256]);
    // SendInput, or the sink of CaptureHandBackForTesting.
    void SendKeys(const std::vector<INPUT>& keys);
    // A key the hook lets by once the keyboard is no longer grabbed: see
    // keyboardGrabbed_.
    void KeyPassedAfterTheGrab(WPARAM message, const KBDLLHOOKSTRUCT& event);
    // Posts a key-up to the overlay for every key still marked swallowed, and
    // clears the record. Called as a grab ends - see its definition for the
    // every-other-keypress bug that needs both halves.
    void ReleaseSwallowedKeys();

    // The pointer's start: seeded from the real cursor on the hook thread,
    // as the grab goes in - see ReconcileHooks; its end, the hand-back, is
    // in Refresh.
    void SeedVirtualCursor();
    // The keyboard's two handovers, each run from Refresh on the transition
    // rather than from any one of the several setters that can cause it -
    // see their definitions.
    void BeginGrabbedKeyboard();
    void EndGrabbedKeyboard();

    // Installs/removes hooks and the raw-input sink so they match what
    // options_ and active_ currently ask for, and runs the handovers above on
    // the way. Idempotent; every public setter just updates state and calls
    // this.
    void Refresh();
    // Injects everything banked so far as one correction, putting the
    // game's camera back where it was. Called by the raw-input sink once the
    // bank passes the threshold on either axis, by Refresh as countering
    // ends, and by SettleCorrection - never once per frame, see
    // EditModeInputOptions::counterThreshold. False when there was nothing
    // banked, or Windows refused the injection.
    bool FlushPendingCorrection();
    // False, with no sink left, when it could not be made or registered.
    bool EnsureRawInputSink();
    void DestroyRawInputSink();

    // The hooks live on their own thread, and this is not a nicety. A
    // low-level hook procedure runs on the thread that installed it, and
    // the input stack blocks every mouse event system-wide until that
    // thread services it. Installed on the render thread - which sits in
    // Present(1, 0) for most of a frame - that throttles all mouse input to
    // the frame rate: measured, 300 mouse reports took 10ms with the grab
    // off and 5,318ms with it on, an 18ms wait per event. The pointer
    // arrived in clumps at frame boundaries, which is what a jittery
    // pointer, a stroke made of visible steps, and "everything feels slower
    // with this on" all were. This thread does nothing but pump, so hooks
    // are serviced immediately.
    //
    // One thread for the app's life: started the first time something is
    // wanted, ended only by Shutdown. Hidden, it holds nothing - no hook, no
    // raw-input registration, no timer - and sleeps in GetMessage. The
    // thread used to come and go with every grab, and every show and hide
    // was a race between two threads over its start and its end - see
    // docs/ARCHITECTURE.md, "One hook thread, told what is wanted".
    //
    // What is wanted, as Refresh last decided it: one snapshot, read whole
    // by the hook thread, so that what it installs and what the app thread
    // handed over for are one decision. `generation` counts the decisions.
    struct Wanted {
        bool pointer = false;
        bool keyboard = false;
        uint64_t generation = 0;
    };
    // Hands the hook thread `wanted`, and waits a moment for it to say it is
    // so - see AwaitReconciled. Starts the thread if it is needed and there
    // is none.
    void Reconcile(bool pointer, bool keyboard);
    // Waits for the hook thread to have reconciled `generation`, for at most
    // kReconcileWaitMs. A thread that does not answer in time is taken to
    // have installed nothing (HookThreadAnswering), and is not waited for
    // again until it has caught up: one wait per hang, not one per call.
    void AwaitReconciled(uint64_t generation);
    // Whether the hook thread has answered everything asked of it - so that
    // what it published is what is installed. False with no thread, one
    // that could not start, and one that missed its answer.
    bool HookThreadAnswering() const;
    // Starts the hook thread if there is none, and says whether there is
    // one to post to: running, with its message queue. False when it could
    // not be started, died on the way up, is still on the way up, or is
    // one a Shutdown gave up waiting for: nothing is posted, and nothing is
    // installed by it. `started` says a thread was started by this call.
    bool EnsureHookThread(bool& started);
    // Asks the hook thread to end, and waits a while for it - see
    // Shutdown.
    void StopHookThread();
    // Lets go of the thread's handle, the events and the id, once the
    // thread has been seen to exit - the one place these are closed.
    void CloseHookThreadHandles();
    static DWORD WINAPI HookThreadMain(void* self);
    // Runs on the hook thread: installs/removes hooks to match the wanted_
    // snapshot, publishes what is installed, and answers its generation.
    // Posted to rather than called directly, since only that thread may own
    // them.
    void ReconcileHooks();

    void PostKeyToOverlay(UINT vk, const KBDLLHOOKSTRUCT& event, bool isDown);
    // The characters a swallowed key stands for - the half of typing that
    // normally arrives as WM_CHAR from TranslateMessage, and therefore only
    // to a focused window. See its definition.
    void PostCharactersToOverlay(UINT vk, const KBDLLHOOKSTRUCT& event);
    // Whether a Win key is down, by the grab's record or by Windows'.
    bool WinKeyHeld() const;

    // Each reads state the app thread writes, and each is called
    // from the hook thread, so they take the lock. Never called while it is
    // already held: the setters release before calling Refresh.
    EditModeInputOptions OptionsSnapshot() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return options_;
    }
    // Every Want* below carries gameKeepsFocus_, so that turning off "Don't
    // steal focus" takes the hooks down rather than leaving them installed
    // to take input from a game that is no longer receiving any.
    // Whether the mouse is taken from the game: physical mouse events
    // discarded and the position kept here instead, from the raw stream.
    // What "Use raw mouse input" asks for, and the one thing the hook and
    // the raw-input sink exist for - countering is inside this rather than
    // beside it, because a correction cannot coexist with Windows driving
    // the cursor (see EditModeInputOptions::CounterRawMouseInputCanBeUsed).
    //
    // Says nothing about which pointer is *shown*: see
    // SoftwarePointerWanted and PublishVirtualCursor.
    bool WantPointerGrab() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return active_ && options_.useRawMouseInput && options_.RawMouseInputCanBeUsed(gameKeepsFocus_);
    }
    // The whole keyboard, while edit mode is up over a game that keeps
    // focus. Without gameKeepsFocus_ this window holds focus and receives
    // key messages the ordinary way, with no hook involved.
    bool WantKeyboard() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return WantKeyboardLocked();
    }
    bool WantKeyboardLocked() const { return active_ && !keyboardSuspended_ && gameKeepsFocus_; }
    bool WantCancellation() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return active_ && options_.counterRawMouseInput &&
               options_.CounterRawMouseInputCanBeUsed(gameKeepsFocus_);
    }

    // Current mouse-button state, tracked here because the OS no longer
    // knows it: the button-down events were swallowed, so Windows' own
    // WM_MOUSEMOVE carries no MK_LBUTTON and a drag would never register.
    UINT ButtonFlags() const;
    void PostToOverlay(UINT message, WPARAM wParam, POINT screenPoint);
    bool IsOwnCorrection(const RAWINPUT& input) const;
    // The desktop pointer's own settings - speed slider, and the
    // "enhance pointer precision" acceleration curve out of the registry -
    // so the overlay's pointer moves the way the one it replaces does.
    void LoadPointerBallistics();
    // `reports` is how many mouse reports the movement stands for - see
    // hookMovesSinceReport_.
    void MoveVirtualCursorRaw(LONG rawDx, LONG rawDy, int reports);
    // The integration itself, under pointerMutex_ - what MoveVirtualCursorRaw
    // does before it publishes the result, which it does unlocked.
    void IntegrateRawMovement(LONG rawDx, LONG rawDy, int reports);
    // Puts the position the grab keeps where the user can see it, when it
    // is not being drawn: SetCursorPos to the whole-pixel position, once
    // per report. Generates no raw input, so the game's camera never learns
    // of it, and no hook callback of ours either - which is why the
    // corrections cannot feed back through here.
    //
    // Not the same as letting Windows move the cursor. It moved it from the
    // same physical events, but through an accumulator our corrections were
    // also reaching, which is what ate slow movement - see
    // EditModeInputOptions::CounterRawMouseInputCanBeUsed.
    void PublishVirtualCursor();
    // For devices that report a position rather than a movement - tablets,
    // touchscreens, and the pointer a VM console presents. See OnRawMouse.
    void SetVirtualCursorAbsolute(LONG x, LONG y);
    void ClampVirtualCursor();
    // SetPointerBounds' rectangle, or the primary display's while none has
    // been set. Never empty.
    RECT PointerBounds() const;

    HWND overlay_ = nullptr;
    // See SetPointerBounds. Atomic because the raw-input sink clamps against
    // them on the hook thread while the window sets them from its own; a
    // read torn across a move clamps one report oddly, and the next report
    // puts it right.
    std::atomic<LONG> boundsLeft_{0};
    std::atomic<LONG> boundsTop_{0};
    std::atomic<LONG> boundsRight_{0};
    std::atomic<LONG> boundsBottom_{0};
    // Written by the app thread, read by the hook thread on every event -
    // guarded because EditModeInputOptions is several bools and a torn read
    // would mean acting on half a settings change.
    mutable std::mutex stateMutex_;
    EditModeInputOptions options_;
    bool active_ = false;
    bool keyboardSuspended_ = false;
    // Which keys this hook swallowed the key-down of, so their key-ups can be
    // told apart from the ups of keys pressed before the grab started - see
    // OnKeyboard, where letting those through is what keeps Windows' own idea
    // of which keys are held from going stale. A flat table rather than a set
    // because it is written from the hook callback, where the cost of a lock
    // is paid by every mouse event on the machine; a virtual-key code is one
    // byte, so the whole thing is 256 bools. Written on the hook thread,
    // cleared from the app thread as a grab ends.
    static constexpr UINT kVirtualKeyCount = 256;
    std::atomic<bool> swallowedDown_[kVirtualKeyCount] = {};
    // Between BeginGrabbedKeyboard and EndGrabbedKeyboard. The hook comes
    // down after the end, on its own thread, and a key in between was
    // recorded as one the grab took: a stale record made a later up ours to
    // swallow, and a stale Ctrl was held for a later session that never
    // grabbed the keyboard. Out of the grab, the hook records nothing.
    std::atomic<bool> keyboardGrabbed_{false};
    // The modifiers whose up the hook let by since the grab ended - see
    // HandHeldModifiersToSystem.
    std::atomic<bool> upAfterTheGrab_[kVirtualKeyCount] = {};
    // See CaptureHandBackForTesting.
    std::vector<INPUT>* handBackSinkForTesting_ = nullptr;
    std::function<void()> handBackGapForTesting_;
    // See KeyEventForTesting: the one key it says Windows has down, or 0.
    std::atomic<UINT> heldByWindowsForTesting_{0};
    // See SetGameKeepsFocus. Defaults false so nothing is grabbed until the
    // window has said which way it was shown.
    bool gameKeepsFocus_ = false;
    // What Refresh saw last time, so each handover runs once, on its own
    // transition - see Refresh.
    bool keyboardWasGrabbed_ = false;
    bool counteringWasOn_ = false;
    // The keyboard state ToUnicodeEx is given, kept here because the real one
    // is wrong by construction: every key this hook sees has been swallowed,
    // so GetKeyboardState reports nothing held. Hook-thread only.
    BYTE keyboardState_[256] = {};

    // Owned by the hook thread; never touched from anywhere else.
    HHOOK mouseHook_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
    // EVENT_SYSTEM_DESKTOPSWITCH's, in while either hook is - see
    // InputLeftOnAnotherDesktop.
    HWINEVENTHOOK desktopSwitchHook_ = nullptr;
    // The thread itself is owned by the app thread, from CreateThread to
    // the CloseHandle after it has been seen to exit - see EnsureHookThread,
    // StopHookThread and CloseHookThreadHandles. No thread (hookThread_
    // null); starting (hookThreadReady_ still open - the event the thread
    // sets once its message queue exists, which EnsureHookThread waits on
    // before it lets anyone post to the id, and keeps if that wait times
    // out, for the thread to set when it gets there); running (the event
    // closed); or one a Shutdown gave up waiting for (hookThreadQuitting_:
    // kept, so that no second thread is started over its hooks).
    HANDLE hookThread_ = nullptr;
    DWORD hookThreadId_ = 0;
    HANDLE hookThreadReady_ = nullptr;
    bool hookThreadQuitting_ = false;
    // Set by the hook thread each time it has answered a generation - see
    // AwaitReconciled. Auto-reset; lives as long as the thread.
    HANDLE reconciled_ = nullptr;
    // How long the app thread waits for an answer - well under
    // kStalledAfterMs, and far over the few milliseconds a reconcile takes.
    static constexpr DWORD kReconcileWaitMs = 500;
    // Under stateMutex_: written by Refresh, read whole by the hook thread.
    Wanted wanted_;
    // The latest generation the hook thread has reconciled, and the latest
    // the app thread asked it for. Behind, the thread has not answered:
    // see HookThreadAnswering.
    std::atomic<uint64_t> reconciledGeneration_{0};
    std::atomic<uint64_t> awaitedGeneration_{0};
    // True from the thread's queue existing to its exit, written by the
    // thread itself - so a thread whose start the app thread stopped
    // waiting for counts once it gets there.
    std::atomic<bool> hookThreadUp_{false};
    // The last try to start the thread failed: CreateEvent or CreateThread
    // did, or the thread died on its way up. Tried again at the next
    // Refresh that wants anything.
    std::atomic<bool> hookThreadStartFailed_{false};
    // What is installed, as the hook thread publishes it after each
    // reconcile: not wanted (Off), in (Installed), or wanted and refused
    // (Failed, tried again on the timer). The app thread decides from these
    // - never from what it asked for - so that no hook thread, or one that
    // has not answered, is nothing installed, and every fallback (the real
    // cursor, typing by focus) holds without a word from it.
    enum class HookState : uint8_t { Off, Installed, Failed };
    std::atomic<HookState> pointerGrabState_{HookState::Off};
    std::atomic<HookState> keyboardHookState_{HookState::Off};
    // The pointer grab as the app thread can rely on it: published in, by
    // a thread that has answered.
    bool PointerGrabbed() const {
        return HookThreadAnswering() && pointerGrabState_.load() == HookState::Installed;
    }

    // The overlay's own pointer, in screen coordinates - see
    // VirtualCursorActive. Seeded from the real cursor when a grab starts
    // and moved only by the deltas the hook swallows. Atomic because the
    // hook thread writes it and the render thread reads it every frame.
    std::atomic<LONG> virtualCursorX_{0};
    std::atomic<LONG> virtualCursorY_{0};
    // Everything from here to lastGain_ is the pointer's integration state:
    // the raw-input sink moves it report by report and SeedVirtualCursor
    // seeds it as the grab goes in, both on the hook thread, so the two
    // never overlap. Diagnostics reads it from the app thread, under this
    // lock, which nothing else takes: uncontended but for those reads, and
    // never held across SetCursorPos.
    mutable std::mutex pointerMutex_;
    // The sub-pixel part of the above, and the scale applied to raw device
    // counts - see MoveVirtualCursorRaw.
    // The pointer's real position, unrounded, so that movement smaller than
    // a pixel accumulates instead of vanishing. virtualCursorX_/Y_ above are
    // this floored to whole pixels - the only form anything outside this
    // class ever sees.
    float preciseX_ = 0.0f;
    float preciseY_ = 0.0f;
    float pointerScale_ = 1.0f;
    // The acceleration curve - see LoadPointerBallistics.
    bool ballisticsEnabled_ = false;
    bool curveValid_ = false;
    pointer_ballistics::Curve curve_;
    // Debug scaffolding - see InputGrabDiagnostics. The last of what
    // pointerMutex_ guards.
    float lastGain_ = 0.0f;
    std::atomic<int> stepCounts_[4]{};
    std::atomic<int> frameSteps_[4]{};
    POINT lastFramePoint_{};
    // Mouse moves the hook has seen since the raw-input sink last read one.
    // Windows caps raw input to a background listener at about 125
    // messages a second and merges the rest into one, so a message can
    // stand for several reports - always, from a 1000 Hz mouse - and the
    // curve has to be read per report. The hook is called once for every
    // report regardless, so between two messages it counts them. Written by
    // the hook, read and reset by the sink and by SeedVirtualCursor, all on
    // the hook thread.
    std::atomic<int> hookMovesSinceReport_{0};
    // Which mouse buttons' downs the hook has swallowed, one bit each (left,
    // right, middle, X1, X2) - so that the up of a button pressed before the
    // hook was there reaches Windows. See OnMouse. Cleared as the hook goes
    // in; written on the hook thread only.
    std::atomic<uint8_t> swallowedButtons_{0};
    // GetTickCount64 at the last Heartbeat, or at the grab starting.
    std::atomic<uint64_t> lastHeartbeatMs_{0};
    // Whether the app thread has gone quiet while the hooks are swallowing
    // everything - see its definition.
    bool AppThreadStalled() const;
    // The books on a key let through while the app thread is stalled, kept
    // on the hook thread as it goes past - see its definition.
    void KeyPassedThroughWhileStalled(WPARAM message, const KBDLLHOOKSTRUCT& event);
    // Movement taken from the game since the last correction, banked by the
    // raw-input sink on the hook thread and injected back once it strays too
    // far or countering ends - see FlushPendingCorrection.
    std::atomic<LONG> pendingCorrectionX_{0};
    std::atomic<LONG> pendingCorrectionY_{0};
    // QPC ticks at the moment the oldest still-uncountered report was
    // banked; 0 when nothing is banked. The difference between this and the
    // injection is the whole of what we can measure about countering - see
    // InputGrabDiagnostics::correctionLagMsLast.
    std::atomic<int64_t> pendingCorrectionSince_{0};
    std::atomic<float> correctionLagMsLast_{0.0f};
    std::atomic<float> correctionLagMsMax_{0.0f};
    std::atomic<int> correctionsInjected_{0};
    std::atomic<int> correctionsFailed_{0};
    // See InputGrabDiagnostics::desktopSwitches.
    std::atomic<int> desktopSwitches_{0};

    // What Refresh last decided, for the hot path that would otherwise take
    // the state lock on every mouse event system-wide. Reading a stale
    // value for one event is harmless - it only decides whether that event
    // is countered, and the transition itself is handled by Refresh.
    std::atomic<bool> countering_{false};
    // EditModeInputOptions::useSoftwarePointer, mirrored likewise: it
    // decides, per report, whether the position just computed is written to
    // the real cursor - see PublishVirtualCursor.
    std::atomic<bool> softwarePointerDrawn_{true};
    // EditModeInputOptions::counterThreshold, mirrored likewise for the
    // raw-input sink, which checks the bank against it on every report.
    std::atomic<LONG> counterThreshold_{EditModeInputOptions{}.counterThreshold};

    // Written by the hook thread as buttons go down and up, read by the
    // render thread through HeldButtons - hence atomic.
    std::atomic<bool> leftDown_{false};
    std::atomic<bool> rightDown_{false};
    std::atomic<bool> middleDown_{false};
    std::atomic<bool> x1Down_{false};
    std::atomic<bool> x2Down_{false};
    // Which of those two a physical left or right button is - see
    // ButtonSwap. Not cleared with them as a grab ends: that is the app
    // thread, and a stale entry is replaced by the button's next press.
    ButtonSwap buttonSwap_;

    // Modifier state, tracked for the same reason as the buttons: the
    // modifier key-downs are swallowed too, so GetKeyState can't be asked.
    // Read by the render thread through HeldModifiers.
    ModifierRecord modifiers_;

    std::vector<Hotkey> hotkeys_;
    std::atomic<bool> hotkeysPaused_{false};

    // Message-only window receiving the raw mouse stream - the overlay is told
    // about movement, buttons and the wheel from here, so it exists whenever
    // the mouse is grabbed and not merely while corrections are wanted.
    HWND rawInputSink_ = nullptr;
    // The hook thread's timer for trying again what could not be set up -
    // see ReconcileHooks. 0 when none is running.
    UINT_PTR hookRetryTimer_ = 0;
    // How many times the hook thread has seeded the pointer - see
    // SampleFrameStep, which takes its frame baseline afresh at each, and
    // the count it last saw.
    std::atomic<uint32_t> pointerSeeds_{0};
    uint32_t pointerSeedsSeen_ = 0;
    std::atomic<bool> failPointerGrabForTesting_{false};
    std::atomic<bool> failMouseHookForTesting_{false};
    std::atomic<bool> failKeyboardHookForTesting_{false};
    std::atomic<bool> failHookThreadStartForTesting_{false};
    std::atomic<DWORD> stallReconcileForTestingMs_{0};
};

}  // namespace sz::platform::win32
