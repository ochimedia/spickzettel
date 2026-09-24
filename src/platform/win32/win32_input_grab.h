#pragma once

#include <windows.h>

#include <atomic>
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

    // Temporarily hands the keyboard back for real text entry - see
    // IOverlayWindow::RequestTextInput/ReleaseTextInput, which bracket exactly the
    // situations that need it. Without this a rename field would silently
    // swallow every keystroke instead of receiving it.
    void SetKeyboardSuspended(bool suspended);

    // Whether the game underneath still holds OS focus - i.e. whether
    // dontStealFocus is on. The precondition for grabbing anything
    // at all: with edit mode holding focus the ordinary way, the game has
    // already stopped receiving input (raw input included, since a game
    // registers without RIDEV_INPUTSINK and so only receives it while
    // foreground) and there is nothing left to take. Installing hooks then
    // would buy nothing and cost a system-wide chokepoint on every mouse
    // event - see StartHookThread.
    //
    // Deliberately does not gate SoftwarePointerWanted: drawing the pointer
    // is a matter of appearance and stays available either way.
    void SetGameKeepsFocus(bool gameKeepsFocus);

    // How many number keys the input options HUD is claiming, starting at
    // '1'; 0 when it is not up. It reads its own toggles from those keys, and
    // an overlay with no focus can only be handed them by the keyboard hook -
    // so a non-zero count keeps the hook installed for the HUD's sake even
    // when nothing asked for the keyboard. See WantKeyboard.
    //
    // The count comes across rather than being a constant on this side: it
    // has to match the HUD's row count, and a constant here only matched it
    // by comment.
    void SetInputOptionsHudDigits(int digitCount);

    // The app's global hotkeys, so the grab can dispatch them itself while
    // the keyboard is swallowed. Measured: a swallowing low-level keyboard
    // hook suppresses WM_HOTKEY, so without this, turning the grab on would
    // also disable the hotkey that turns it off.
    void AddHotkey(int id, const KeyCombo& combo, HWND target);
    void RemoveHotkey(int id);

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

    // Whether it *would* deliver typing if asked to take the keyboard - i.e.
    // whether SetTextFieldOpen(true) is enough on its own. Everything except
    // the decision itself.
    bool CanDeliverTyping() const;

    // A text field is open and wants the keyboard for as long as it is.
    //
    // This is the difference between the two ways to give a field what it
    // needs. With keystroke forwarding on - the user wanting WASD to keep
    // reaching the game while the overlay is up - serving a field by taking
    // real OS focus stops the stray WASD but costs the game exactly the
    // focus-loss event the whole mode exists to avoid. Taking the keyboard
    // for the duration instead does the same job for nothing: the
    // game sees no keystrokes while the field is open, gets them back when it
    // closes, and never learns it lost anything.
    //
    // Turning this on flips WantKeyboard, so Refresh installs the hook and
    // seeds the modifier record through BeginGrabbedKeyboard, and turning it
    // off runs EndGrabbedKeyboard - the boundary handling is already there and
    // this is simply another thing that moves the boundary.
    void SetTextFieldOpen(bool open);

    // Whether the whole keyboard is currently ours rather than only the input
    // options HUD's digits - see WantsAllKeystrokesLocked.
    bool WantsAllKeystrokes() const;

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
        ctrl = ctrlDown_;
        shift = shiftDown_;
        alt = altDown_;
    }

    // Injects the correction banked so far right now, rather than when
    // countering next ends - for the window to call before it stops hiding
    // the game, so that what the game shows next is already the camera put
    // back. True if there was anything to inject.
    bool SettleCorrection();

    // Debug scaffolding for the input options HUD - see
    // InputGrabDiagnostics. SampleFrameStep is called once per rendered
    // frame; Diagnostics is read by the HUD.
    InputGrabDiagnostics Diagnostics() const;
    void SampleFrameStep();

    // Called once at shutdown; also safe to call when nothing is installed.
    void Shutdown();

    // The key-downs to hand Windows as a grab of the keyboard ends: one per
    // side of Ctrl, Shift and Alt whose down `swallowed` (indexed by
    // virtual key) says the hook took, each as that very key - see
    // HandHeldModifiersToSystem. Public to be tested without injecting.
    static std::vector<INPUT> ModifierHandBack(const bool (&swallowed)[256]);

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
    bool TrackModifier(UINT vk, bool isDown);
    // Hands still-held modifiers back to Windows when the hook goes away - see
    // its definition for the hotkey that stops working without it.
    static void HandHeldModifiersToSystem(const bool (&swallowed)[256]);
    // Posts a key-up to the overlay for every key still marked swallowed, and
    // clears the record. Called whenever a grab starts or ends - see its
    // definition for the every-other-keypress bug that needs both halves.
    void ReleaseSwallowedKeys();

    // The two handovers, each run from Refresh on the transition rather than
    // from any one of the several setters that can cause it - see their
    // definitions, and the matching hand-backs in Refresh itself.
    void BeginVirtualCursor();
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
    // banked.
    bool FlushPendingCorrection();
    void EnsureRawInputSink();
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
    // Starts the hook thread if there is none, and says whether there is
    // now one whose message queue exists - the only kind a post reaches.
    // False when the thread could not be started, died on the way up, is
    // still on the way up, or is on its way out after a Stop that gave up
    // waiting: the caller posts nothing, and the next transition asks
    // again. See the .cpp for the states.
    bool StartHookThread();
    void StopHookThread();
    // Lets go of the thread's handle, the ready event and the id, once the
    // thread has been seen to exit - the one place these are closed.
    void CloseHookThreadHandles();
    static DWORD WINAPI HookThreadMain(void* self);
    // Runs on the hook thread: installs/removes hooks to match the state
    // below. Posted to rather than called directly, since only that thread
    // may own them.
    void ReconcileHooks();

    void PostKeyToOverlay(UINT vk, const KBDLLHOOKSTRUCT& event, bool isDown);
    // The characters a swallowed key stands for - the half of typing that
    // normally arrives as WM_CHAR from TranslateMessage, and therefore only
    // to a focused window. See its definition.
    void PostCharactersToOverlay(UINT vk, const KBDLLHOOKSTRUCT& event);

    // All four read state the app thread writes, and all four are called
    // from the hook thread, so they take the lock. Never called while it is
    // already held: the setters release before calling Refresh.
    EditModeInputOptions OptionsSnapshot() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return options_;
    }
    int HudDigits() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return hudDigits_;
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
    // The hook is installed for either of two reasons: the option asks for
    // it, or the input options HUD is up and its number keys have to reach
    // an overlay that deliberately has no focus. In the second case it
    // swallows nothing but those digits - see OnKeyboard - which is still
    // a keystroke the game doesn't get, and is why the HUD is opt-in.
    //
    // Both reasons need gameKeepsFocus_. Without it this window holds focus
    // and receives key messages the ordinary way, so the HUD's digits arrive
    // through ImGui with no hook involved.
    bool WantKeyboard() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return active_ && !keyboardSuspended_ &&
               EditModeInputOptions::KeystrokesCanBeHeld(gameKeepsFocus_) &&
               (WantsAllKeystrokesLocked() || hudDigits_ > 0);
    }
    // Whether the whole keyboard is ours, as opposed to only the HUD's
    // digits. The option asks for it permanently; an open text field asks for
    // it while it is open, which is the difference between a field costing
    // the game its focus and costing it nothing at all.
    bool WantsAllKeystrokesLocked() const { return options_.dontForwardKeystrokes || textFieldOpen_; }
    // Everything typing needs except the decision to take the keyboard - what
    // SetTextFieldOpen turns into a yes.
    bool CanDeliverTypingLocked() const {
        return active_ && !keyboardSuspended_ && overlay_ != nullptr &&
               EditModeInputOptions::KeystrokesCanBeHeld(gameKeepsFocus_);
    }
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
    // How many number keys the input options HUD is currently claiming, 0 when
    // it is not up. Carried across the interface rather than duplicated as a
    // constant here: a mismatch with the row count in the UI leaves a row
    // unreachable or swallows a digit that does nothing.
    int hudDigits_ = 0;
    // Which keys this hook swallowed the key-down of, so their key-ups can be
    // told apart from the ups of keys pressed before the grab started - see
    // OnKeyboard, where letting those through is what keeps Windows' own idea
    // of which keys are held from going stale. A flat table rather than a set
    // because it is written from the hook callback, where the cost of a lock
    // is paid by every mouse event on the machine; a virtual-key code is one
    // byte, so the whole thing is 256 bools. Written on the hook thread,
    // cleared from the app thread when a grab starts or ends.
    static constexpr UINT kVirtualKeyCount = 256;
    std::atomic<bool> swallowedDown_[kVirtualKeyCount] = {};
    // See SetGameKeepsFocus. Defaults false so nothing is grabbed until the
    // window has said which way it was shown.
    bool gameKeepsFocus_ = false;
    // See SetTextFieldOpen.
    bool textFieldOpen_ = false;
    // What Refresh saw last time, so each handover runs once, on its own
    // transition - see Refresh.
    bool virtualCursorWasDriving_ = false;
    bool keyboardWasGrabbed_ = false;
    bool counteringWasOn_ = false;
    // The keyboard state ToUnicodeEx is given, kept here because the real one
    // is wrong by construction: every key this hook sees has been swallowed,
    // so GetKeyboardState reports nothing held. Hook-thread only.
    BYTE keyboardState_[256] = {};

    // Owned by the hook thread; never touched from anywhere else.
    HHOOK mouseHook_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
    // The thread itself is owned by the app thread, from CreateThread to
    // the CloseHandle after it has been seen to exit - see StartHookThread,
    // StopHookThread and CloseHookThreadHandles. Three states, told apart
    // by these: no thread (hookThread_ null); starting (hookThreadReady_
    // still open - the event the thread sets once its message queue
    // exists, which Start waits on before it lets anyone post to the id,
    // and keeps if that wait times out, for the thread to set when it
    // gets there); running (the event closed); stopping
    // (hookThreadQuitting_: a Stop posted WM_QUIT and gave up waiting, so
    // whatever is posted now lands behind the quit and is lost).
    HANDLE hookThread_ = nullptr;
    DWORD hookThreadId_ = 0;
    HANDLE hookThreadReady_ = nullptr;
    bool hookThreadQuitting_ = false;

    // The overlay's own pointer, in screen coordinates - see
    // VirtualCursorActive. Seeded from the real cursor when a grab starts
    // and moved only by the deltas the hook swallows. Atomic because the
    // hook thread writes it and the render thread reads it every frame.
    std::atomic<LONG> virtualCursorX_{0};
    std::atomic<LONG> virtualCursorY_{0};
    // Everything from here to lastGain_ is the pointer's integration state:
    // the raw-input sink moves it on the hook thread, report by report, and
    // BeginVirtualCursor seeds it on the app thread whenever the virtual
    // pointer starts driving - which can happen while the sink is still up,
    // since raw mouse input can go off and on again before the hook thread
    // has reconciled and taken the sink down. Diagnostics reads it from the
    // app thread too. So all of it is under this lock, which nothing else
    // takes: uncontended but for those moments, and never held across
    // SetCursorPos.
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
    // the hook, read and reset by the sink, both on the hook thread; atomic
    // because BeginVirtualCursor resets it from the app thread.
    std::atomic<int> hookMovesSinceReport_{0};
    // Which mouse buttons' downs the hook has swallowed, one bit each (left,
    // right, middle, X1, X2) - so that the up of a button pressed before the
    // hook was there reaches Windows. See OnMouse. Cleared as the hook goes
    // in; written on the hook thread only.
    std::atomic<uint8_t> swallowedButtons_{0};
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

    // What Refresh last decided, for the two hot paths that would otherwise
    // take the state lock on every mouse event system-wide. Reading a stale
    // value for one event is harmless - each of these only decides whether
    // that event is swallowed or countered, and the transition itself is
    // handled by Refresh.
    std::atomic<bool> pointerGrabbing_{false};
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

    // Modifier state, tracked for the same reason as the buttons: the
    // modifier key-downs are swallowed too, so GetKeyState can't be asked.
    // Atomic because the render thread reads them through HeldModifiers.
    std::atomic<bool> ctrlDown_{false};
    std::atomic<bool> altDown_{false};
    std::atomic<bool> shiftDown_{false};

    std::vector<Hotkey> hotkeys_;

    // Message-only window receiving the raw mouse stream - the overlay is told
    // about movement, buttons and the wheel from here, so it exists whenever
    // the mouse is grabbed and not merely while corrections are wanted.
    HWND rawInputSink_ = nullptr;
};

}  // namespace sz::platform::win32
