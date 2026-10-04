#include "platform/win32/win32_overlay_window.h"

#include <windowsx.h>

#include <dwmapi.h>
#include <tpcshrd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <backends/imgui_impl_win32.h>

#include "platform/pen_glyph.h"
#include "platform/win32/resources/resource.h"
#include "platform/win32/win32_dx11_renderer.h"
#include "platform/win32/win32_input_grab.h"
#include "platform/win32/win32_integrity.h"
#include "platform/win32/win32_screen_capture.h"
#include "platform/win32/win32_text.h"

// imgui_impl_win32.h intentionally wraps its real declaration of this
// function in `#if 0` to avoid dragging <windows.h> into that shared
// header, and instructs callers to copy this exact forward declaration
// into their own .cpp instead.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                              LPARAM lParam);

namespace sz::platform::win32 {

namespace {
// Registered wide, and this matters rather than being a style choice: the
// keyboard grab synthesizes WM_CHAR for the overlay (see
// Win32InputGrab::PostCharactersToOverlay), and a WM_CHAR delivered to an
// ANSI window carries one code-page byte instead of a UTF-16 unit. ImGui's
// backend branches on IsWindowUnicode for exactly that reason, taking
// AddInputCharacterUTF16 for a wide window and MultiByteToWideChar for a
// narrow one. Wide is the lossless half, and it is what makes typing work on
// a layout whose characters have no code-page representation.
constexpr const wchar_t* kWindowClassName = L"SpickzettelOverlayWindowClass";

// What Windows may not make of a touch or a pen on this window. Left to
// itself it turns a press held still into a right click (the "press and
// hold" gesture, with its ring animation) and a pen's barrel button and
// taps into feedback of its own - and a held press is the app's: it
// stands in for a double-click (see Pending, ui/interaction/gestures.h). Told to
// Windows the two ways it documents: as a window property at creation,
// and as the answer to WM_TABLET_QUERYSYSTEMGESTURESTATUS.
//
// Measured on a touch screen, tracing the input at the hook, the raw
// stream and the window: Windows kept sending it anyway - a finger held
// still arrived as a left press, then 650 ms later a right press and
// release with the left still down, then the left release, and none of it
// tagged as touch. So the app does not rely on this being heard: it
// ignores a second button while one is down (see Gesture and Spent,
// ui/interaction/gestures.h), which is what keeps the hold's work.
// This stays because it is the documented request, costs nothing, and may
// be honored for a pen or on another Windows.
constexpr DWORD_PTR kTabletGestureFlags =
    TABLET_DISABLE_PRESSANDHOLD | TABLET_DISABLE_PENTAPFEEDBACK | TABLET_DISABLE_PENBARRELFEEDBACK | TABLET_DISABLE_FLICKS;

// The modifiers the keyboard grab is holding, made visible to GetKeyState for
// exactly as long as ImGui's backend is reading them.
//
// The backend submits Ctrl/Shift/Alt from GetKeyState every time it handles a
// key message, and a key this process swallowed in its own hook updates no key
// state anywhere - so with the keyboard grabbed the backend submitted "Ctrl is
// up" on the very frame a Ctrl chord arrived. The once-a-frame override in
// RenderFrame could not undo that: ImGui's event queue stops applying events
// for a key that has already changed this frame, so the correction was held
// over to the next frame, by which time the other key was no longer *newly*
// pressed - and a shortcut only fires on the frame of the press. Ctrl+A,
// Ctrl+C, Ctrl+V and Shift+arrow selection were all silently dead inside a
// text field, while the same chords worked when the overlay held real focus.
//
// SetKeyboardState writes the calling thread's own key-state table, which is
// what GetKeyState reads back. Lying in it is safe precisely because the
// keyboard is grabbed: no real key message reaches this thread to write that
// table, and the lie is reverted before the message returns. Additive only -
// a modifier the grab isn't holding is left however the OS has it, which is
// what keeps the focused case (where the backend is already right) untouched.
//
// This does not replace RenderFrame's override, which is still the only thing
// that tells ImGui about a swallowed modifier on the frames that carry no key
// message at all - Alt held while dragging with the mouse, say.
class ScopedGrabbedModifiers {
public:
    ScopedGrabbedModifiers(const Win32InputGrab& grab, bool wanted) {
        if (!wanted) {
            return;
        }
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
        grab.HeldModifiers(ctrl, shift, alt);
        if (!ctrl && !shift && !alt) {
            return;
        }
        if (!GetKeyboardState(saved_)) {
            return;
        }
        BYTE faked[kKeyStateSize];
        std::copy(std::begin(saved_), std::end(saved_), std::begin(faked));
        // Both the combined key and one side of it: the combined VK is what
        // the backend reads for the modifier state, and the sided one keeps
        // the table self-consistent for anything that looks closer. Which
        // side is unknowable here - the grab tracks "Ctrl", not "left Ctrl" -
        // and immaterial, because the sided entries are only ever read for a
        // VK_CONTROL/VK_SHIFT/VK_MENU message, which the grab never posts.
        const auto hold = [&faked](int combined, int sided, bool down) {
            if (!down) {
                return;
            }
            faked[combined] |= 0x80;
            faked[sided] |= 0x80;
        };
        hold(VK_CONTROL, VK_LCONTROL, ctrl);
        hold(VK_SHIFT, VK_LSHIFT, shift);
        hold(VK_MENU, VK_LMENU, alt);
        active_ = SetKeyboardState(faked) != FALSE;
    }

    ~ScopedGrabbedModifiers() {
        if (active_) {
            SetKeyboardState(saved_);
        }
    }

    ScopedGrabbedModifiers(const ScopedGrabbedModifiers&) = delete;
    ScopedGrabbedModifiers& operator=(const ScopedGrabbedModifiers&) = delete;

private:
    static constexpr int kKeyStateSize = 256;  // SetKeyboardState's fixed size
    BYTE saved_[kKeyStateSize] = {};
    bool active_ = false;
};

// The buttons a mouse message's MK_ flags - or the input grab's
// HeldButtons, which uses the same - say are held.
uint8_t ButtonsFromKeyState(UINT flags) {
    uint8_t buttons = 0;
    const auto add = [&](UINT flag, MouseButton button) {
        if (flags & flag) {
            buttons |= ButtonBit(button);
        }
    };
    add(MK_LBUTTON, MouseButton::Left);
    add(MK_RBUTTON, MouseButton::Right);
    add(MK_MBUTTON, MouseButton::Middle);
    add(MK_XBUTTON1, MouseButton::X1);
    add(MK_XBUTTON2, MouseButton::X2);
    return buttons;
}

Vec2 ClientPosition(LPARAM lParam) {
    return Vec2{static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))};
}

// The window's own, after a WM_CAPTURECHANGED: whether the capture is
// still lost, and a button still held - see HandleMessage.
constexpr UINT kCaptureLostMessage = WM_APP + 1;

}  // namespace

int KeyForVirtualKey(UINT virtualKey) {
    if ((virtualKey >= 'A' && virtualKey <= 'Z') || (virtualKey >= '0' && virtualKey <= '9')) {
        return static_cast<int>(virtualKey);
    }
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return KeyCombo::kFunctionKeyBase + 1 + static_cast<int>(virtualKey - VK_F1);
    }
    switch (virtualKey) {
        case VK_ESCAPE:
            return KeyCombo::kEscape;
        case VK_DELETE:
            return KeyCombo::kDelete;
        case VK_BACK:
            return KeyCombo::kBackspace;
        case VK_LEFT:
            return KeyCombo::kLeftArrow;
        case VK_RIGHT:
            return KeyCombo::kRightArrow;
        case VK_UP:
            return KeyCombo::kUpArrow;
        case VK_DOWN:
            return KeyCombo::kDownArrow;
        default:
            return 0;
    }
}

Win32OverlayWindow::Win32OverlayWindow() = default;
Win32OverlayWindow::~Win32OverlayWindow() = default;

void Win32OverlayWindow::Initialize(HINSTANCE instance) { instance_ = instance; }

bool Win32OverlayWindow::EnsureCreated(const DisplayInfo& display) {
    if (hwnd_) {
        return true;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Win32OverlayWindow::WndProcThunk;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    // MAKEINTRESOURCE(IDI_APP_ICON) resolves against this module's own
    // resources (app_icon.rc, compiled into the .exe - see
    // src/app_main/CMakeLists.txt) rather than a file path; falls back to
    // null (Windows' own generic default) if that resource is somehow
    // missing rather than failing window creation over a cosmetic detail.
    windowClass.hIcon = LoadIconA(instance_, MAKEINTRESOURCEA(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    RegisterClassExW(&windowClass);

    // Deliberately NOT WS_EX_LAYERED: two legacy layered-window transparency
    // modes were tried and rejected before this one. LWA_COLORKEY (chroma-key
    // transparency) composites correctly per-pixel, but DefWindowProc's
    // default WM_NCHITTEST handling for a layered window returns
    // HTTRANSPARENT over color-keyed pixels - independent of
    // WS_EX_TRANSPARENT - so clicks over any undrawn area of the overlay
    // silently fell through to whatever was underneath (confirmed: even
    // answering WM_NCHITTEST ourselves with HTCLIENT didn't help, since the
    // OS apparently excludes the window from hit-test candidacy for those
    // screen points before a message is ever generated for it). LWA_ALPHA
    // avoids that, but applies one constant alpha to the window's already-
    // rendered RGB as a whole - it ignores the backbuffer's own per-pixel
    // alpha entirely, so the "empty" regions came out fully opaque instead
    // of see-through.
    //
    // Ordinary (non-layered) windows have neither problem: default
    // hit-testing is plain rectangle-based, so the whole window stays
    // clickable. Real per-pixel transparency for one is unlocked via
    // ImGui_ImplWin32_EnableAlphaCompositing() below (DwmEnableBlurBehindWindow
    // with an effectively-infinite blur region), which tells the DWM to
    // composite using this window's own rendered alpha channel - the
    // pre-DirectComposition technique for exactly this scenario, and
    // simpler than a full DirectComposition swapchain/visual tree for v1.
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (noActivate_) {
        // See SetEditModeNoActivate's doc comment - lets the window receive
        // mouse input without Windows ever automatically giving it
        // keyboard focus just for being shown or clicked (ShowWindow's own
        // activation and the usual click-to-activate behavior are both
        // suppressed for a WS_EX_NOACTIVATE window - only an explicit
        // SetForegroundWindow/SetActiveWindow call, e.g. from RequestTextInput,
        // still activates it).
        exStyle |= WS_EX_NOACTIVATE;
    }
    displayRect_ = RECT{display.x, display.y, display.x + display.width, display.y + display.height};
    hwnd_ = CreateWindowExW(exStyle, kWindowClassName, L"Spickzettel Overlay", WS_POPUP, display.x, display.y,
                             display.width, display.height, nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return false;
    }
    // See kTabletGestureFlags: a held pen or finger should stay a held
    // left button.
    SetPropW(hwnd_, L"MicrosoftTabletPenServiceProperty", reinterpret_cast<HANDLE>(kTabletGestureFlags));

    ImGui_ImplWin32_EnableAlphaCompositing(hwnd_);

    // The grab posts synthesized mouse messages here once it starts
    // swallowing the real ones - see Win32InputGrab. Handing it the window
    // now rather than at first use keeps the "is it allowed to run yet"
    // question in one place (RefreshEditModeInput).
    Win32InputGrab::Instance().SetPointerBounds(displayRect_);
    Win32InputGrab::Instance().SetOverlayWindow(hwnd_);

    renderer_ = std::make_unique<Win32Dx11Renderer>();
    if (!renderer_->Initialize(hwnd_)) {
        renderer_.reset();
        // Not left posting to a window about to be gone.
        Win32InputGrab::Instance().SetOverlayWindow(nullptr);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    QueryPerformanceFrequency(&perfFrequency_);
    QueryPerformanceCounter(&lastFrameTime_);
    return true;
}

void Win32OverlayWindow::MoveToDisplay(const DisplayInfo& display) {
    const RECT target{display.x, display.y, display.x + display.width, display.y + display.height};
    if (!hwnd_) {
        return;
    }
    // Where the window really is, not only where it was last put: Windows
    // moves and resizes windows of its own accord - a display unplugged, or
    // its resolution changed - and a window still recorded as placed would
    // be left wherever that put it.
    RECT actual{};
    GetWindowRect(hwnd_, &actual);
    if (EqualRect(&target, &displayRect_) && EqualRect(&target, &actual)) {
        return;
    }
    displayRect_ = target;
    Win32InputGrab::Instance().SetPointerBounds(displayRect_);
    // No SWP_SHOWWINDOW, so a hidden window stays hidden: the tray
    // controller places it before showing it, and before a capture taken
    // while it is hidden. The swap chain follows the size in WM_SIZE.
    SetWindowPos(hwnd_, nullptr, display.x, display.y, display.width, display.height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Win32OverlayWindow::SetDisplaysChangedCallback(std::function<void()> callback) {
    displaysChangedCallback_ = std::move(callback);
}

void Win32OverlayWindow::Present(Presentation presentation) {
    if (!hwnd_) {
        return;
    }
    const Presentation from = !visible_          ? Presentation::Hidden
                              : inputPassthrough_ ? Presentation::ClickThrough
                                                  : Presentation::Interactive;
    // Asked before anything moves: once the window is hidden, whether it
    // held focus can no longer be told.
    const bool heldFocus = GetForegroundWindow() == hwnd_;
    for (const PresentationStep step : PresentationSteps(from, presentation, noActivate_)) {
        Carry(step, heldFocus);
    }
}

void Win32OverlayWindow::Carry(PresentationStep step, bool heldFocus) {
    switch (step) {
        case PresentationStep::SettleCamera:
            SettleCameraBeforeReveal();
            return;
        case PresentationStep::RestoreBorrowedNoActivate:
            // A field open at this moment is being hidden along with the
            // overlay, so whatever it borrowed goes back now rather than on a
            // close that will never come. The grab-side claims are cleared by
            // the grab going off; this is the window's own half, the
            // WS_EX_NOACTIVATE bit.
            if (focusBorrowed_) {
                const LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
                SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
                focusBorrowed_ = false;
            }
            return;
        case PresentationStep::CountAsClickThrough:
            inputPassthrough_ = true;
            return;
        case PresentationStep::CountAsInteractive:
            inputPassthrough_ = false;
            return;
        case PresentationStep::ClickThroughStylesOn:
        case PresentationStep::ClickThroughStylesOff: {
            // The WM_NCHITTEST/HTTRANSPARENT handling turned out not to be
            // enough on its own: that mechanism only re-does hit-testing among
            // windows on the *same thread* ("the message will be sent to
            // underlying windows in the same thread" per its own docs), and
            // the whole point here is routing to a window in a completely
            // different process (the game). WS_EX_TRANSPARENT is what actually
            // makes a window invisible to hit-testing at the OS/window-manager
            // level, regardless of what owns whatever's underneath - the
            // standard technique for a genuine cross-process click-through
            // overlay. WS_EX_TRANSPARENT alone is reported unreliable without
            // WS_EX_LAYERED alongside it; deliberately not calling
            // SetLayeredWindowAttributes/UpdateLayeredWindow for it, though -
            // this window's actual per-pixel transparency still comes entirely
            // from ImGui_ImplWin32_EnableAlphaCompositing's DWM blur-behind
            // (see the translucency section of docs/ARCHITECTURE.md), and
            // those two legacy layered-window APIs are how the *other*,
            // rejected translucency techniques there fed a window's visible
            // alpha - if a set-once color/alpha value from either applied on
            // top of blur-behind, it would fight it. WS_EX_LAYERED's bit is
            // only being borrowed here for what it does to hit-testing, not
            // for what it can do to pixels.
            LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
            if (step == PresentationStep::ClickThroughStylesOn) {
                exStyle |= (WS_EX_LAYERED | WS_EX_TRANSPARENT);
            } else {
                exStyle &= ~(WS_EX_LAYERED | WS_EX_TRANSPARENT);
            }
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);
            return;
        }
        case PresentationStep::Show:
            // SW_SHOWNOACTIVATE: SW_SHOW *activates* the window it shows -
            // that is the whole difference between the two, and it is enough
            // to take focus on its own, with no SetForegroundWindow anywhere
            // near it. Focus is taken by TakeFocus alone, which notes where
            // from.
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            visible_ = true;
            shownSeconds_ = 0.0f;
            QueryPerformanceCounter(&lastFrameTime_);  // avoid a large delta-time spike on the first frame
            return;
        case PresentationStep::Hide:
            ShowWindow(hwnd_, SW_HIDE);
            visible_ = false;
            return;
        case PresentationStep::TakeFocus:
            TakeFocus();
            return;
        case PresentationStep::HandFocusBack:
            // Only if this window held focus when the change began. Once
            // click-through, the user can freely click into and out of other
            // windows through the overlay, and GetForegroundWindow() already
            // tracks wherever that leaves them - handing focus to a noted
            // window then would undo a real focus change the user made.
            // Keyboard focus is independent of the cursor, so without the
            // hand back, a window that held focus would keep the keyboard -
            // eating the game's WASD - for as long as view mode is up.
            if (heldFocus && focusTakenFrom_ && IsWindow(focusTakenFrom_)) {
                SetForegroundWindow(focusTakenFrom_);
            }
            focusTakenFrom_ = nullptr;
            return;
        case PresentationStep::ClaimFront:
            // Claim the top of the topmost band, without taking activation.
            // WS_EX_TOPMOST puts a window in that band but says nothing about
            // its order *within* it, and that order follows activation -
            // which under noActivate_ we deliberately never take, so showing
            // alone leaves us wherever the last activation left us.
            //
            // Necessary but not sufficient on its own: whatever put another
            // topmost window in front is generally still going on after this
            // call. See the recheck in RenderFrame.
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return;
        case PresentationStep::ForgetKeys:
            // No key goes down unseen while away that goes up seen - see
            // EmitKey.
            keysDown_.reset();
            return;
        case PresentationStep::PlacePointer:
            seedPointerFromCursor_ = true;
            return;
        case PresentationStep::ClaimCursor: {
            // Put where it already is, which moves nothing - no input, so
            // nothing for the grab or the game - but has Windows send the
            // window under it a mouse message, and with it WM_SETCURSOR,
            // which hides the OS cursor under the software pointer (see
            // HandleMessage). After the grab is on, which is what decides
            // that. Without it, view mode to edit mode in place left the
            // arrow of whatever was underneath on screen beside the drawn
            // pointer until the first click.
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                SetCursorPos(cursor.x, cursor.y);
            }
            return;
        }
        case PresentationStep::RefreshGrab:
            RefreshEditModeInput();
            return;
    }
}

void Win32OverlayWindow::TakeFocus() {
    const HWND foreground = GetForegroundWindow();
    if (foreground != hwnd_) {
        focusTakenFrom_ = foreground;
    }
    SetForegroundWindow(hwnd_);
}

void Win32OverlayWindow::SetInputOptionsHudDigits(int digitCount) {
    Win32InputGrab::Instance().SetInputOptionsHudDigits(digitCount);
}

void Win32OverlayWindow::SetEditModeInput(const EditModeInputOptions& options) {
    editModeInput_ = options;
    RefreshEditModeInput();
}

// The grab runs only while the overlay is genuinely up and interactive.
// Hidden, it must not be holding the machine's input hostage; in view-only
// mode the overlay is click-through by design and taking input away would
// contradict the entire point of that mode.
void Win32OverlayWindow::RefreshEditModeInput() {
    Win32InputGrab& grab = Win32InputGrab::Instance();
    grab.SetOptions(editModeInput_);
    // The precondition, not an option - see SetGameKeepsFocus. With this
    // window holding focus the ordinary way there is nothing left to take,
    // so the hooks come down rather than costing a system-wide chokepoint
    // for nothing.
    grab.SetGameKeepsFocus(noActivate_);
    grab.SetActive(visible_ && !inputPassthrough_);
}

namespace {
// How long the game is given to draw the camera put back before the frozen
// picture over it goes away. The correction reaches the game's input at
// once, but it shows only after the game's next frame and the compositor's:
// at 60 fps, two to three refreshes. A guess on the generous side - a
// slower game shows its last few frames of wandering all the same.
constexpr DWORD kCameraSettleMs = 80;

// How long a frame with no device to draw with, or with the window
// occluded, waits before the next try.
constexpr DWORD kNoDeviceRetryMs = 100;
}  // namespace

// Settles the camera correction while this window still covers the game,
// and waits for the game to draw it - see kCameraSettleMs. Without this the
// correction went out as the window went away, and the game showed where
// the camera had wandered to for a moment before it snapped back.
void Win32OverlayWindow::SettleCameraBeforeReveal() {
    if (Win32InputGrab::Instance().SettleCorrection()) {
        Sleep(kCameraSettleMs);
    }
}

bool Win32OverlayWindow::IsVisible() const { return visible_; }

int Win32OverlayWindow::ScalePercent() const {
    // The window's DPI is its display's, since the process is per-monitor
    // aware (see WinMain), and Windows updates it when the window moves or
    // the display's scale is changed. 0 for no window.
    const UINT dpi = hwnd_ != nullptr ? GetDpiForWindow(hwnd_) : 0;
    return dpi > 0 ? MulDiv(static_cast<int>(dpi), 100, USER_DEFAULT_SCREEN_DPI) : 100;
}

ForegroundApp Win32OverlayWindow::UnderlyingApplication() const {
    // Whoever holds the foreground, unless that is this window - which it
    // is only in the configuration where edit mode takes focus, and where
    // the answer is therefore the window it took it from. Asking that way
    // round rather than branching on noActivate_ means the two
    // configurations need no separate handling and neither does the
    // hidden case.
    HWND target = GetForegroundWindow();
    if (target == nullptr || target == hwnd_) {
        target = focusTakenFrom_;
    }
    if (target == nullptr || !IsWindow(target)) {
        return {};
    }

    ForegroundApp app;

    wchar_t title[256] = {};
    if (const int length = GetWindowTextW(target, title, static_cast<int>(std::size(title))); length > 0) {
        app.title = Narrow(std::wstring_view(title, static_cast<size_t>(length)));
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(target, &processId);
    if (processId != 0) {
        // QUERY_LIMITED_INFORMATION rather than QUERY_INFORMATION: the
        // limited right is the one a normal-integrity process is granted
        // for most other processes, and it is all of
        // QueryFullProcessImageName, OpenProcessToken and the integrity
        // read below that is needed. It is granted across integrity levels
        // for the same account - an elevated Task Manager answers both
        // questions - and refused for a process owned by another account
        // or shielded by an anti-cheat driver, which is why the title
        // above is read first and unconditionally.
        if (const HANDLE process =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId)) {
            wchar_t path[MAX_PATH] = {};
            DWORD length = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &length) && length > 0) {
                const std::filesystem::path imagePath(std::wstring_view(path, length));
                app.executable = Narrow(imagePath.filename().wstring());
                // Lowercased because Windows filenames are compared that
                // way and a profile matching "EldenRing.exe" must match a
                // process reported as "eldenring.exe".
                std::transform(app.executable.begin(), app.executable.end(), app.executable.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
            app.integrity = IntegrityComparedToOurs(process);
            CloseHandle(process);
        }
    }
    return app;
}

namespace {

// Windows has no stock pen the way it has a crosshair or a hand, so this
// one is drawn - from the same outline the software pointer draws (see
// platform::pen_glyph), rasterized here rather than hand-authored as pixel
// art. The art it replaces was a 45-degree stick with uneven ends, and next
// to the drawn pen it read as a different, crooked tool.
//
// 24 rows of 24 at 100%, which is the size Windows draws its own cursors
// at on a 100% display, and larger in step with the display's scale, as
// Windows' own are; the nib at the lower left so the pen points at the
// pixel it will mark rather than near it.
constexpr int kPenCursorSize = 24;
constexpr int kPenCursorHotspotX = 2;
constexpr int kPenCursorHotspotY = 21;
// Samples per pixel per axis. A pen that is mostly two long diagonals
// stands or falls on its edges, and 16 samples is the difference between
// stepped and smooth at this size.
constexpr int kPenCursorSubsamples = 4;

}  // namespace

HCURSOR Win32OverlayWindow::PenCursor(int scalePercent) {
    if (const auto built = penCursors_.find(scalePercent); built != penCursors_.end()) {
        return built->second;
    }
    HCURSOR& cursor = penCursors_[scalePercent];
    // The glyph is drawn at `scale` times its own size: every pixel below
    // samples it at its own position divided by the scale, the outline
    // included, so the whole pen grows and none of it thins.
    const float scale = static_cast<float>(scalePercent) / 100.0f;
    const int size = static_cast<int>(std::lround(kPenCursorSize * scale));
    const int hotspotX = static_cast<int>(std::lround(kPenCursorHotspotX * scale));
    const int hotspotY = static_cast<int>(std::lround(kPenCursorHotspotY * scale));

    // A 32-bit top-down DIB, so the alpha channel is the mask and no
    // separate AND bitmap has to be built by hand - CreateIconIndirect
    // still wants an hbmMask, but an all-zero one leaves the alpha in
    // charge.
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;  // negative: rows top-down, like the art above
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* pixels = nullptr;
    const HDC screen = GetDC(nullptr);
    const HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
                                            &pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !pixels) {
        return nullptr;
    }

    auto* argb = static_cast<uint32_t*>(pixels);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // The glyph's origin is its nib, and the hotspot is the pixel
            // the nib has to land on - so the origin sits at that pixel's
            // center, half a pixel in from its corner.
            int edgeSamples = 0;
            int fillSamples = 0;
            for (int sy = 0; sy < kPenCursorSubsamples; ++sy) {
                for (int sx = 0; sx < kPenCursorSubsamples; ++sx) {
                    const float px = static_cast<float>(x - hotspotX) +
                                     (static_cast<float>(sx) + 0.5f) / kPenCursorSubsamples - 0.5f;
                    const float py = static_cast<float>(y - hotspotY) +
                                     (static_cast<float>(sy) + 0.5f) / kPenCursorSubsamples - 0.5f;
                    switch (pen_glyph::InkAt(px / scale, py / scale)) {
                        case pen_glyph::Ink::Edge:
                            ++edgeSamples;
                            break;
                        case pen_glyph::Ink::Fill:
                            ++fillSamples;
                            break;
                        case pen_glyph::Ink::None:
                            break;
                    }
                }
            }
            constexpr int kSamplesPerPixel = kPenCursorSubsamples * kPenCursorSubsamples;
            const int covered = edgeSamples + fillSamples;
            if (covered == 0) {
                argb[static_cast<size_t>(y) * size + x] = 0x00000000;
                continue;
            }
            // The same near-black the software pointer outlines with, and
            // the same white it fills with, mixed by how much of the pixel
            // each one covers.
            constexpr float kEdgeR = 0x14, kEdgeG = 0x18, kEdgeB = 0x20;
            const float edgeShare = static_cast<float>(edgeSamples);
            const float fillShare = static_cast<float>(fillSamples);
            const float r = (kEdgeR * edgeShare + 255.0f * fillShare) / static_cast<float>(covered);
            const float g = (kEdgeG * edgeShare + 255.0f * fillShare) / static_cast<float>(covered);
            const float b = (kEdgeB * edgeShare + 255.0f * fillShare) / static_cast<float>(covered);
            const float alpha = static_cast<float>(covered) / kSamplesPerPixel;
            // Premultiplied, which is what an alpha cursor is drawn with -
            // so a half-covered edge pixel is half its color *and* half
            // its alpha rather than a full-strength color showing through.
            const auto channel = [alpha](float value) {
                return static_cast<uint32_t>(std::lround(std::clamp(value * alpha, 0.0f, 255.0f)));
            };
            argb[static_cast<size_t>(y) * size + x] =
                (static_cast<uint32_t>(std::lround(alpha * 255.0f)) << 24) | (channel(r) << 16) |
                (channel(g) << 8) | channel(b);
        }
    }

    // Empty mask: with a 32-bit color bitmap the alpha channel decides, and
    // this only has to exist.
    const HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = FALSE;  // a cursor, so the hotspot fields below are read
    info.xHotspot = static_cast<DWORD>(hotspotX);
    info.yHotspot = static_cast<DWORD>(hotspotY);
    info.hbmMask = mask;
    info.hbmColor = color;
    cursor = static_cast<HCURSOR>(CreateIconIndirect(&info));
    DeleteObject(color);
    DeleteObject(mask);
    return cursor;
}

HCURSOR Win32OverlayWindow::CursorFor(CursorShape shape, int scalePercent) {
    switch (shape) {
        case CursorShape::Crosshair:
            return LoadCursorA(nullptr, IDC_CROSS);
        case CursorShape::Pen:
            // Falls back to the crosshair if the glyph couldn't be built -
            // "the exact point matters here" is the half of the pen's
            // meaning that survives, and it is the more useful half.
            if (HCURSOR pen = PenCursor(scalePercent)) {
                return pen;
            }
            return LoadCursorA(nullptr, IDC_CROSS);
        case CursorShape::Arrow:
        case CursorShape::Default:
            break;
    }
    return LoadCursorA(nullptr, IDC_ARROW);
}

void Win32OverlayWindow::SetCursorShape(CursorShape shape) {
    cursorShape_ = shape;
    // Default is ImGui's frame to own - recorded (so WM_SETCURSOR below
    // falls through to it) and otherwise left alone. Installing an arrow
    // here would land on top of the resize arrow ImGui put up this same
    // frame, which is the flicker this whole hand-off exists to avoid.
    if (shape == CursorShape::Default) {
        return;
    }
    // Applied on the next WM_SETCURSOR (see HandleMessage), which the OS
    // sends on the very next mouse move - but also pushed right now, so a
    // shape set while the pointer is standing still takes effect
    // immediately instead of waiting for a twitch. That matters more than
    // it sounds: the shape is decided at the end of the frame, and the
    // frame that first sees the pointer over a snippet is usually one
    // frame *after* the mouse message that put it there. Stop moving right
    // as you arrive and there is no next message to apply it on.
    //
    // A thread may only set the cursor while it owns it - while the cursor
    // is over one of its own windows - so that, and not focus, is the
    // condition: in edit mode the overlay deliberately never is foreground
    // (see SetEditModeNoActivate), so a foreground check would never pass.
    if (!hwnd_) {
        return;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor) || WindowFromPoint(cursor) != hwnd_) {
        return;
    }
    // Same call the message handler starts with, and for the same reason:
    // with the overlay drawing its own pointer the OS one has to stay
    // hidden, and installing a shape here would put it back on screen.
    SetCursor(Win32InputGrab::Instance().SoftwarePointerWanted() ? nullptr : CursorFor(shape, ScalePercent()));
}

void Win32OverlayWindow::SetEditModeNoActivate(bool enabled) {
    noActivate_ = enabled;
    if (!hwnd_) {
        return;  // not created yet - EnsureCreated bakes noActivate_ into the initial exStyle instead
    }
    // Same live-restyle technique the click-through styles use for
    // WS_EX_LAYERED/WS_EX_TRANSPARENT: WS_EX_NOACTIVATE can be flipped on
    // an already-created window via SetWindowLongPtr, it doesn't have to
    // be set only at CreateWindowEx time. Windows picks this up for the
    // *next* activation attempt (ShowWindow/click-to-activate/etc.) without
    // needing a SetWindowPos/SWP_FRAMECHANGED nudge - unlike a *visual*
    // frame style change, WS_EX_NOACTIVATE has no rendered frame to redraw.
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enabled) {
        exStyle |= WS_EX_NOACTIVATE;
    } else {
        exStyle &= ~WS_EX_NOACTIVATE;
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);

    // Turning it OFF while already visible in edit mode should reclaim
    // focus immediately, matching what coming up interactive would have
    // done had this been set before - otherwise edit mode would silently
    // stay mouse-only until the next hide/show cycle. Turning it ON needs
    // no equivalent action: whatever already has focus (this window, or the
    // game underneath) simply keeps it; this window just won't grab it
    // again on its own from here on. Skipped while click-through - there,
    // focus deliberately stays with whatever's underneath regardless of
    // this setting.
    if (!enabled && visible_ && !inputPassthrough_) {
        TakeFocus();
    }

    // Last, because it is the whole precondition for the grab: turning this
    // off takes the hooks down and turning it on puts them back, live, in the
    // same edit-mode session. Without it the group would keep whatever state
    // it had at the last show.
    RefreshEditModeInput();
}

void Win32OverlayWindow::RequestTextInput() {
    if (!hwnd_) {
        return;
    }
    // A text field needs the keyboard, not focus, and there are two ways to
    // give it one. Borrowing the keyboard costs the game nothing; borrowing
    // focus costs it the focus-loss event this whole mode exists to avoid. So
    // ask for the keyboard whenever the grab is in a position to hand it over
    // - which includes the case where keystroke forwarding is *off*, i.e. the
    // user wants WASD to keep reaching the game while the overlay is up. That
    // want stops at the edge of a text field: someone typing a name is not
    // steering a character, and the field takes the keyboard for exactly as
    // long as it is open.
    textInputRequested_ = true;
    if (Win32InputGrab::Instance().CanDeliverTyping()) {
        Win32InputGrab::Instance().SetTextFieldOpen(true);
        return;
    }
    TakeTextInputFocus();
}

// The other way: focus, for a field the grab cannot type into - none to
// borrow from, the overlay holding focus anyway, or a keyboard hook that
// could not be installed (see Win32InputGrab::kKeyboardUnavailableMessage).
void Win32OverlayWindow::TakeTextInputFocus() {
    // Otherwise the field needs the real thing - no grab to borrow from, or
    // the overlay already holds focus anyway. Hand the keyboard back for as
    // long as the field is open (see Win32InputGrab::SetKeyboardSuspended):
    // without this, a field opened under a grab that is *not* forwarding
    // keystrokes would swallow the very keys it exists to receive.
    Win32InputGrab::Instance().SetKeyboardSuspended(true);

    // And WS_EX_NOACTIVATE has to come off for the duration, or the keys
    // still go somewhere else. That bit tells Windows never to activate this
    // window, and activation - not foreground - is what carries keyboard
    // focus: SetForegroundWindow on a NOACTIVATE window can make it
    // foreground while focus stays with whatever was activated last. The
    // field then opens, looks ready, and every keystroke lands elsewhere,
    // with the system error beep that a key going nowhere produces. Measured
    // over eight renames: two failed exactly that way, one of them with the
    // desktop reported as the foreground window, before the rest worked.
    //
    // Paired with ReleaseTextInput, which puts the bit back. `focusBorrowed_` so
    // that only a RequestTextInput that actually removed it restores it.
    if (noActivate_) {
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle & ~WS_EX_NOACTIVATE);
        focusBorrowed_ = true;
    }
    TakeFocus();
    // Foreground is the window; focus is the keyboard. Ask for both - the
    // second is what an InputText is actually waiting on.
    SetFocus(hwnd_);
}

void Win32OverlayWindow::ReleaseTextInput() {
    textInputRequested_ = false;
    // Both undos are unconditional and idempotent, because a field can be
    // closed by routes that have no idea which way it was opened: the
    // keyboard the field borrowed goes back to whatever the options ask for,
    // and a grab that was never suspended does nothing here. One that *was*
    // must be resumed however the field is closing, or a single rename would
    // disable it for the session.
    Win32InputGrab::Instance().SetTextFieldOpen(false);
    Win32InputGrab::Instance().SetKeyboardSuspended(false);

    // Everything below undoes a RequestTextInput that actually took focus. When
    // the grab was delivering the typing, it never did - so there is no style
    // bit to restore and, more importantly, nobody to hand focus to. Doing it
    // anyway would move the foreground window at the end of a rename that had
    // not touched it, which is the opposite of what this mode is for.
    if (!hwnd_ || !focusBorrowed_) {
        return;
    }
    // Put WS_EX_NOACTIVATE back before handing focus on, so the overlay goes
    // straight back to being a window that never takes activation on its own
    // - see RequestTextInput for why it came off.
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
    focusBorrowed_ = false;

    // Back to the window the field took focus from - only if this window
    // still holds it, as on going click-through or hidden.
    if (GetForegroundWindow() == hwnd_ && focusTakenFrom_ && IsWindow(focusTakenFrom_)) {
        SetForegroundWindow(focusTakenFrom_);
    }
    focusTakenFrom_ = nullptr;
}

void Win32OverlayWindow::SetFrameCallback(FrameCallback callback) { frameCallback_ = std::move(callback); }

void Win32OverlayWindow::SetFramePacing(FramePacing pacing) { framePacing_ = pacing; }

namespace {
// An idle overlay still draws four times a second: the same cadence as the
// check that it is still the front topmost window (see RenderFrame), which
// matters most exactly when it sits over a game - and often enough that
// anything else done per frame, following a change of display size say, is
// never far behind.
constexpr LONGLONG kIdleFrameIntervalMs = 250;
}  // namespace

bool Win32OverlayWindow::WantsFrame(bool messagesDispatched) const {
    return framePacing_ == FramePacing::EveryFrame || messagesDispatched || MillisecondsUntilIdleFrame() == 0;
}

DWORD Win32OverlayWindow::MillisecondsUntilIdleFrame() const {
    if (framePacing_ == FramePacing::EveryFrame || perfFrequency_.QuadPart <= 0) {
        return 0;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const LONGLONG elapsedMs = (now.QuadPart - lastFrameTime_.QuadPart) * 1000 / perfFrequency_.QuadPart;
    return elapsedMs >= kIdleFrameIntervalMs ? 0 : static_cast<DWORD>(kIdleFrameIntervalMs - elapsedMs);
}

void Win32OverlayWindow::SetInputCallback(InputCallback callback) { inputCallback_ = std::move(callback); }

InputGrabDiagnostics Win32OverlayWindow::GetInputGrabDiagnostics() const {
    return Win32InputGrab::Instance().Diagnostics();
}

CaptureResult Win32OverlayWindow::CaptureRegion(const Rect& rect) {
    // Checked as floats, before anything is converted or allocated: a value
    // past int's range makes the conversion undefined, and the size alone
    // decides what is allocated. Nothing larger than all the displays
    // together is on the screen to capture, and nothing further out than
    // that from the window is either. Written so that a NaN fails it.
    const float screenW = static_cast<float>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const float screenH = static_cast<float>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
    const auto within = [](float value, float low, float high) { return value >= low && value <= high; };
    if (!renderer_ || !hwnd_ || !within(rect.w, 1.0f, screenW) || !within(rect.h, 1.0f, screenH) ||
        !within(rect.x, -screenW, screenW) || !within(rect.y, -screenH, screenH)) {
        return CaptureResult{};
    }
    const int width = static_cast<int>(rect.w);
    const int height = static_cast<int>(rect.h);
    // `rect` is in the window's coordinates, which are the desktop's only
    // while the window is on the primary display. Asked of the window rather
    // than worked out from displayRect_, so it is wherever the window
    // really is - hidden or not, which makes no difference to the answer.
    POINT source{static_cast<LONG>(rect.x), static_cast<LONG>(rect.y)};
    ClientToScreen(hwnd_, &source);

    // Without this window in it, so the capture shows what is behind the
    // overlay rather than the overlay's own content - see CaptureScreen,
    // which leaves it out without hiding it.
    std::vector<uint8_t> pixelsBGRA;
    const bool captured = CaptureScreen(visible_ ? hwnd_ : nullptr, source, width, height, pixelsBGRA);

    if (!captured) {
        return CaptureResult{};
    }

    // GDI's 32bpp DIBs are byte-order BGRA; CaptureResult is RGBA, which is
    // also what D3D11's DXGI_FORMAT_R8G8B8A8_UNORM (what
    // CreateTextureFromRGBA uses) wants - swap the B/R bytes of each pixel
    // in place.
    // The fourth byte of a 32bpp DIB is not an alpha channel: GDI leaves it
    // undefined (zero for most of the screen, whatever a layered window
    // wrote for the rest), and a screenshot is opaque by definition, so it
    // is set rather than trusted.
    for (size_t i = 0; i + 3 < pixelsBGRA.size(); i += 4) {
        std::swap(pixelsBGRA[i], pixelsBGRA[i + 2]);
        pixelsBGRA[i + 3] = 255;
    }

    CaptureResult result;
    result.pixelsRGBA = std::move(pixelsBGRA);  // renamed in place above - now actually RGBA
    result.width = width;
    result.height = height;
    return result;
}

uint64_t Win32OverlayWindow::CreateTextureFromPixels(const uint8_t* pixelsRGBA, int width, int height) {
    if (!renderer_ || !pixelsRGBA || width <= 0 || height <= 0) {
        return 0;
    }
    ID3D11ShaderResourceView* srv = renderer_->CreateTextureFromRGBA(pixelsRGBA, width, height);
    return srv ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(srv)) : 0;
}

bool Win32OverlayWindow::UpdateTextureRegion(uint64_t textureHandle, const uint8_t* pixelsRGBA, int sourceWidth,
                                              int x, int y, int w, int h) {
    if (textureHandle == 0 || !renderer_) {
        return false;
    }
    return renderer_->UpdateTextureRegionRGBA(
        reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<uintptr_t>(textureHandle)), pixelsRGBA, sourceWidth,
        x, y, w, h);
}

void Win32OverlayWindow::ReleaseTexture(uint64_t textureHandle) {
    if (textureHandle == 0 || !renderer_) {
        return;
    }
    renderer_->ReleaseTexture(reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<uintptr_t>(textureHandle)));
}

uint64_t Win32OverlayWindow::TextureGeneration() const {
    return pastTextureGenerations_ + (renderer_ ? renderer_->DeviceGeneration() : 0);
}

DrawCallback Win32OverlayWindow::ImageFilterCallback() const { return &Win32Dx11Renderer::ApplyImageFilter; }
DrawCallback Win32OverlayWindow::StrokeDepthCallback() const { return &Win32Dx11Renderer::ApplyStrokeDepth; }
DrawCallback Win32OverlayWindow::StrokeLayerCallback() const { return &Win32Dx11Renderer::ApplyStrokeLayer; }

void Win32OverlayWindow::Destroy() {
    // Before the window goes: the grab posts messages to it, and its hooks
    // are global state that outliving this object would be a real problem.
    Win32InputGrab::Instance().Shutdown();
    if (renderer_) {
        pastTextureGenerations_ += renderer_->DeviceGeneration() + 1;
        renderer_->Shutdown();
        renderer_.reset();
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
}

// Whether another always-on-top window is currently drawn above us and
// actually covers something. Walks only the windows above this one, which is
// a handful even on a busy desktop.
//
// The overlap test is what keeps this from firing constantly: the shell keeps
// tiny topmost helpers around - ThumbnailDeviceHelperWnd is 1x1 at the origin
// - which are permanently above everything and cover nothing. Requiring a
// real overlap distinguishes those from the taskbar's 1280x48 band without
// naming any window in particular.
bool Win32OverlayWindow::CoveredByAnotherTopmostWindow() const {
    RECT mine{};
    if (!hwnd_ || !GetWindowRect(hwnd_, &mine)) {
        return false;
    }
    constexpr LONG kMeaningfulOverlapPx = 8;
    for (HWND above = GetWindow(hwnd_, GW_HWNDPREV); above != nullptr;
         above = GetWindow(above, GW_HWNDPREV)) {
        if (!IsWindowVisible(above)) {
            continue;
        }
        if ((GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
            continue;  // an ordinary window above a topmost one is the OS mid-reshuffle
        }
        RECT other{};
        RECT overlap{};
        if (!GetWindowRect(above, &other) || !IntersectRect(&overlap, &mine, &other)) {
            continue;
        }
        if (overlap.right - overlap.left >= kMeaningfulOverlapPx &&
            overlap.bottom - overlap.top >= kMeaningfulOverlapPx) {
            return true;
        }
    }
    return false;
}

void Win32OverlayWindow::RenderFrame() {
    if (!visible_ || !renderer_) {
        return;
    }
    // Nothing to draw with, or nowhere to be seen: the device is gone and
    // the driver not back yet, or the screen is locked. The frame is
    // skipped, and a short wait stands in for the vsync Present would have
    // waited for, so the loop does not spin. Skipped before the grab's
    // heartbeat, too: an overlay that shows nothing should not keep the
    // input, and the grab lets it through once this thread stops beating.
    LARGE_INTEGER now{};
    const auto secondsSinceLastFrame = [&] {
        QueryPerformanceCounter(&now);
        const float seconds = perfFrequency_.QuadPart > 0
                                  ? static_cast<float>(now.QuadPart - lastFrameTime_.QuadPart) /
                                        static_cast<float>(perfFrequency_.QuadPart)
                                  : 0.0f;
        lastFrameTime_ = now;
        return seconds;
    };
    if (!renderer_->ReadyToRender()) {
        MsgWaitForMultipleObjectsEx(0, nullptr, kNoDeviceRetryMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        // A skipped frame's time is its own, not the next drawn frame's.
        secondsSinceLastFrame();
        return;
    }

    const float deltaSeconds = secondsSinceLastFrame();

    // Reclaiming the front of the topmost band has to be repeated, not done
    // once on show. Reproduced: leave the taskbar as the foreground window -
    // click it, or close the window that was in front of it - and then bring
    // the overlay up. Shell_TrayWnd ends up above us and stays there, so the
    // edit-mode border and the bottom of the context ring are drawn over. A
    // single SetWindowPos in Show is not enough, because whatever put the
    // taskbar in front is still happening after we have shown ourselves.
    //
    // Checked rather than asserted blindly, so this is a few GetWindow
    // calls rather than a SetWindowPos every frame fighting the shell for
    // the front. Every frame for the first half second after a show, four
    // times a second after that. Measured in that repro: the taskbar comes
    // in front on the second frame, 8-16 ms after the show, and once put
    // back it stays back - so checking every frame leaves it in front for
    // one frame, where the throttle alone left it there for 250 ms, long
    // enough to see the canvas bar's first peek pop out from under it.
    topmostCheckSeconds_ += deltaSeconds;
    shownSeconds_ += deltaSeconds;
    if (shownSeconds_ < 0.5f || topmostCheckSeconds_ >= 0.25f) {
        topmostCheckSeconds_ = 0.0f;
        if (CoveredByAnotherTopmostWindow()) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    Win32InputGrab& grab = Win32InputGrab::Instance();

    // ImGui's modifiers, and the input stream's: a modifier pressed or let
    // go without a key message reaching this window - no focus, and no
    // keyboard grab - is only seen here, once a frame.
    const Modifiers held = HeldModifiers();
    renderer_->SetModifierOverride(held.ctrl, held.shift, held.alt);
    EmitModifiersIfChanged(held, NowSeconds());
    grab.SampleFrameStep();
    grab.Heartbeat();
    renderer_->SetSoftwarePointerActive(grab.SoftwarePointerWanted());

    // While the grab owns the mouse, ImGui must navigate by the overlay's
    // own pointer: the real cursor belongs to the game for the duration and
    // may be pinned, hidden or re-centered behind our back.
    if (grab.VirtualCursorActive()) {
        // The whole-pixel position - the same one the grab stamps on the
        // button messages it posts, so what ImGui hovers, what a click
        // hits, and what gets drawn all agree.
        POINT client = grab.VirtualCursor();
        ScreenToClient(hwnd_, &client);
        renderer_->SetMousePositionOverride(true, static_cast<float>(client.x), static_cast<float>(client.y));

        // One Move event per frame, and only if the pointer actually moved -
        // which is what the OS delivers through WM_MOUSEMOVE when it isn't
        // being swallowed, coalesced. Emitted here rather than posted from
        // the input thread: posting one per mouse report flooded the queue
        // and laid down a stroke point per report, and gating that needed a
        // flag the two threads had to share. The render thread already has
        // the position; it just says so.
        const bool moved = client.x != lastEmittedMove_.x || client.y != lastEmittedMove_.y;
        if (moved) {
            EmitPointer(InputEventKind::PointerMove, Vec2{static_cast<float>(client.x), static_cast<float>(client.y)},
                        MouseButton::Left, ButtonsFromKeyState(grab.HeldButtons()));
        }
        lastEmittedMove_ = client;
    } else if (POINT cursor{}; seedPointerFromCursor_ && GetCursorPos(&cursor) && ScreenToClient(hwnd_, &cursor)) {
        // The first frame after a show. ImGui's pointer went with the rest
        // of its input as the overlay came up (see
        // OverlayApp::OnOverlayShown), which puts it nowhere, and a window
        // without focus hears of the cursor only once it moves: a click
        // before that hovered nothing, and fell through a panel to the
        // canvas. Put where the cursor is, once; the backend has it after.
        renderer_->SetMousePositionOverride(true, static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        seedPointerFromCursor_ = false;
    } else {
        renderer_->SetMousePositionOverride(false, 0.0f, 0.0f);
        seedPointerFromCursor_ = false;
    }
    // The frame's time on the stream's own clock, after whatever moved -
    // see InputEventKind::Tick.
    InputEvent tick;
    tick.kind = InputEventKind::Tick;
    Emit(tick);

    renderer_->NewFrame();
    if (frameCallback_) {
        frameCallback_(deltaSeconds);
    }
    renderer_->RenderAndPresent();
}

// Modifier state from two sources that between them cover every case.
// ImGui's backend learns Ctrl/Shift/Alt from key messages, which need
// keyboard focus - and dontStealFocus exists precisely to keep focus with
// the game, which is how Alt-drag stopped working under it.
// GetAsyncKeyState sees a key whoever has focus. What it cannot see is a
// key the keyboard grab swallowed, because a swallowed event updates no key
// state anywhere; the grab tracked those itself all along, so its record
// fills that gap. Whichever source has focus or the hook, the other reads
// false, and the OR is simply the truth. The Windows key has no record in
// the grab: taking the whole keyboard takes it too, and it then reads as
// up for as long as the keyboard is grabbed. See ARCHITECTURE.md, "Taking
// the keyboard takes the Windows key too".
Modifiers Win32OverlayWindow::HeldModifiers() {
    const auto asyncDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool grabCtrl = false;
    bool grabShift = false;
    bool grabAlt = false;
    Win32InputGrab::Instance().HeldModifiers(grabCtrl, grabShift, grabAlt);
    Modifiers held;
    held.ctrl = asyncDown(VK_CONTROL) || grabCtrl;
    held.shift = asyncDown(VK_SHIFT) || grabShift;
    held.alt = asyncDown(VK_MENU) || grabAlt;
    held.super = asyncDown(VK_LWIN) || asyncDown(VK_RWIN);
    return held;
}

double Win32OverlayWindow::NowSeconds() const {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return perfFrequency_.QuadPart > 0
               ? static_cast<double>(now.QuadPart) / static_cast<double>(perfFrequency_.QuadPart)
               : 0.0;
}

void Win32OverlayWindow::EmitModifiersIfChanged(const Modifiers& held, double seconds) {
    if (held == emittedModifiers_ || !visible_) {
        return;
    }
    emittedModifiers_ = held;
    if (inputCallback_) {
        InputEvent event;
        event.kind = InputEventKind::Modifiers;
        event.seconds = seconds;
        event.modifiers = held;
        inputCallback_(event);
    }
}

// Only while visible. Messages still arrive once the window is hidden - the
// input grab hands on the very key of the hotkey that hid it, after the
// hide (see OverlayApp::OnOverlayShown) - and what a hidden window was
// told is nobody's input.
void Win32OverlayWindow::Emit(InputEvent event) {
    if (!visible_) {
        return;
    }
    event.seconds = NowSeconds();
    event.modifiers = HeldModifiers();
    EmitModifiersIfChanged(event.modifiers, event.seconds);
    if (inputCallback_) {
        inputCallback_(event);
    }
}

void Win32OverlayWindow::EmitPointer(InputEventKind kind, const Vec2& position, MouseButton button,
                                     uint8_t buttons) {
    InputEvent event;
    event.kind = kind;
    event.position = position;
    event.button = button;
    event.buttons = buttons;
    lastPointerPosition_ = position;
    Emit(event);
}

void Win32OverlayWindow::EmitButtonUp(MouseButton button, const Vec2& position) {
    const uint8_t bit = ButtonBit(button);
    if ((buttonsHeld_ & bit) == 0) {
        return;
    }
    buttonsHeld_ = static_cast<uint8_t>(buttonsHeld_ & ~bit);
    if (buttonsHeld_ == 0) {
        ReleaseCapture();
    }
    EmitPointer(InputEventKind::PointerUp, position, button);
}

// A key's down or up, by the key's own name - the modifiers alone are not
// keys here but what every event carries. A down for a key already down is
// its repeat: said by the message itself when the OS sends it (bit 30, the
// key's previous state), but not by the input grab, which posts every
// repeat as a fresh down - so the downs delivered are counted here too.
void Win32OverlayWindow::EmitKey(WPARAM virtualKey, LPARAM lParam, bool down) {
    const int key = KeyForVirtualKey(static_cast<UINT>(virtualKey));
    if (virtualKey >= keysDown_.size()) {
        return;
    }
    const bool wasDown = keysDown_.test(virtualKey) || (lParam & (1LL << 30)) != 0;
    keysDown_.set(virtualKey, down);
    if (key == 0) {
        // Nothing to deliver but what it did to the modifiers, if anything.
        EmitModifiersIfChanged(HeldModifiers(), NowSeconds());
        return;
    }
    InputEvent event;
    event.kind = down ? InputEventKind::KeyDown : InputEventKind::KeyUp;
    event.key = key;
    event.repeat = down && wasDown;
    Emit(event);
}

LRESULT CALLBACK Win32OverlayWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32OverlayWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTA*>(lParam);
        self = static_cast<Win32OverlayWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Win32OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Win32OverlayWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Deliberately *before* the ImGui handler below, which is the only
    // reason this works: ImGui answers WM_SETCURSOR itself (mapping
    // ImGuiMouseCursor_ to a Win32 cursor and returning 1, handled), so a
    // shape it has no enum value for - a crosshair - can only be installed
    // by claiming the message first. See CursorShape.
    if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        // While the overlay draws its own pointer the OS one has to go, or
        // there are two pointers on screen. Under a mouse grab the real one
        // is also stranded wherever the game last put it, so it is the one
        // that doesn't respond to the hand on the mouse; with only the
        // software-pointer option on it tracks correctly and is simply
        // redundant. Either way it is hidden - and hidden again per frame
        // via io.MouseDrawCursor, since this message stops arriving once
        // the real cursor stops moving (see Win32Dx11Renderer::NewFrame).
        if (Win32InputGrab::Instance().SoftwarePointerWanted()) {
            SetCursor(nullptr);
            return TRUE;
        }
        if (cursorShape_ != CursorShape::Default) {
            SetCursor(CursorFor(cursorShape_, ScalePercent()));
            return TRUE;
        }
        // Default deliberately falls through to ImGui's own handler below,
        // which installs whatever the frame asked for - the resize band's
        // directional arrow, the dock's hand. Answering this message
        // unconditionally is what made those flash and vanish: ImGui set
        // them in NewFrame and the next mouse move took them straight back
        // off again.
    }

    // Keeps ImGui's IO state (focus, DPI changes, keyboard modifiers, ...)
    // correct even though v1's only ImGui window uses NoInputs; this is what
    // lets a future interactive toolbar be added without revisiting message
    // handling. Safe to call before the renderer exists (it's a no-op then).
    {
        // Only for the key messages: those are the only ones on which the
        // backend submits modifier state, and the only ones that need to see
        // a swallowed modifier as held. See ScopedGrabbedModifiers.
        const ScopedGrabbedModifiers grabbedModifiers(
            Win32InputGrab::Instance(),
            msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP);
        if (renderer_ && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
            return true;
        }
    }

    switch (msg) {
        case WM_NCHITTEST: {
            if (inputPassthrough_) {
                // Belt-and-suspenders alongside the WS_EX_TRANSPARENT style
                // (see PresentationStep::ClickThroughStylesOn in Carry for
                // why that, not this, is what actually makes cross-process
                // click-through work) - answering HTTRANSPARENT here too
                // costs nothing and is the technically "correct" response
                // regardless.
                return HTTRANSPARENT;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_TABLET_QUERYSYSTEMGESTURESTATUS:
            return static_cast<LRESULT>(kTabletGestureFlags);  // see its comment
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            buttonsHeld_ |= ButtonBit(MouseButton::Left);
            EmitPointer(InputEventKind::PointerDown, ClientPosition(lParam), MouseButton::Left);
            return 0;
        case WM_MOUSEMOVE: {
            // While the grab owns the pointer these are our own doing: it
            // writes the position it keeps to the real cursor when that is
            // the pointer being shown (Win32InputGrab::PublishVirtualCursor),
            // and moving the cursor posts a move to whatever is under it -
            // us. Acting on them would lay down a stroke point per mouse
            // report on top of the one the render thread already emits per
            // frame, which is the flood that delivery exists to avoid.
            if (Win32InputGrab::Instance().VirtualCursorActive()) {
                return 0;
            }
            EmitPointer(InputEventKind::PointerMove, ClientPosition(lParam), MouseButton::Left,
                        ButtonsFromKeyState(static_cast<UINT>(wParam)));
            return 0;
        }
        case WM_LBUTTONUP:
            EmitButtonUp(MouseButton::Left, ClientPosition(lParam));
            return 0;
        // Right button: the app's own right-button gestures (a resize from
        // a snippet's nearest edge, the eraser in drawing mode, framing a
        // drawing on empty canvas - see ui/interaction/recognizer.cpp). Shares
        // SetCapture/ReleaseCapture with the left button above (Win32
        // mouse capture is per-window, not per-button): harmless to call
        // again if already captured, and let go of once neither is held.
        case WM_RBUTTONDOWN:
            SetCapture(hwnd);
            buttonsHeld_ |= ButtonBit(MouseButton::Right);
            EmitPointer(InputEventKind::PointerDown, ClientPosition(lParam), MouseButton::Right);
            return 0;
        case WM_RBUTTONUP:
            EmitButtonUp(MouseButton::Right, ClientPosition(lParam));
            return 0;
        // The capture taken from under a held button - Alt+Tab mid-drag,
        // the other window taking the pointer - and the button's up goes
        // to that window. Unheard, the drag went on: the snippet followed
        // the pointer until the next click. So the button goes up here,
        // where the pointer was last. Asked again once this message is
        // done: ImGui's backend lets go of the capture itself on an up,
        // ahead of the up above, and that is no capture lost.
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != hwnd && buttonsHeld_ != 0) {
                PostMessageW(hwnd, kCaptureLostMessage, 0, 0);
            }
            return 0;
        case Win32InputGrab::kKeyboardUnavailableMessage:
            // Only for the field still open, and not twice.
            if (textInputRequested_ && !focusBorrowed_) {
                Win32InputGrab::Instance().SetTextFieldOpen(false);
                TakeTextInputFocus();
            }
            return 0;
        case kCaptureLostMessage:
            if (GetCapture() != hwnd) {
                EmitButtonUp(MouseButton::Left, lastPointerPosition_);
                EmitButtonUp(MouseButton::Right, lastPointerPosition_);
            }
            return 0;
        // The middle and side buttons, the wheel and the keys: into the
        // stream as well, and then on to the default handling they had
        // before there was one - Alt+F4 is a WM_SYSKEYDOWN before it is the
        // WM_SYSCOMMAND below.
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            EmitPointer(msg == WM_MBUTTONDOWN ? InputEventKind::PointerDown : InputEventKind::PointerUp,
                        ClientPosition(lParam), MouseButton::Middle);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            EmitPointer(msg == WM_XBUTTONDOWN ? InputEventKind::PointerDown : InputEventKind::PointerUp,
                        ClientPosition(lParam),
                        GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_MOUSEWHEEL: {
            // In screen coordinates, unlike the button messages.
            POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &at);
            InputEvent event;
            event.kind = InputEventKind::Wheel;
            event.position = Vec2{static_cast<float>(at.x), static_cast<float>(at.y)};
            event.wheel = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
            Emit(event);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            EmitKey(wParam, lParam, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        // Whatever was held goes up somewhere else now, unheard: a key
        // pressed again after is a press, not a repeat - see EmitKey.
        case WM_KILLFOCUS:
            keysDown_.reset();
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        // This window has no title bar or system menu of its own, but
        // Alt+F4 (and, in principle, a WM_CLOSE from anywhere else) still
        // reaches it as long as it holds real keyboard focus - Windows
        // translates Alt+F4 into WM_SYSCOMMAND/SC_CLOSE for whichever
        // top-level window currently has focus, independent of whether
        // that window is decorated. DefWindowProcW's default handling for
        // both would DestroyWindow() this hwnd_ - but nothing else ever
        // resets the hwnd_ member back to null (that only happens in
        // Destroy(), which this doesn't go through), so EnsureCreated()'s
        // own `if (hwnd_) return true;` would then keep reporting success
        // against an already-destroyed handle forever, with no way for
        // the tray/hotkeys to ever bring the overlay back. This window's
        // lifecycle is owned entirely by Present()/Destroy() (driven
        // by the tray icon and hotkeys), not by its own default close
        // affordances - so neither falls through to the default handling:
        // Alt+F4 is swallowed, and a WM_CLOSE is an exit (below).
        case WM_SYSCOMMAND: {
            // The low 4 bits of an SC_* command are reserved by Windows
            // for its own internal use - mask them off before comparing,
            // per WM_SYSCOMMAND's own documented contract.
            if ((wParam & 0xFFF0) == SC_CLOSE) {
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        // Not Alt+F4, which is the SC_CLOSE above and stays swallowed, but a
        // close asked from outside: taskkill without /f sends it to the
        // windows it can see, which while the overlay is up is this one and
        // not the hidden host window. The window is still not destroyed;
        // the app exits, as the tray menu's Exit does.
        case WM_CLOSE:
            if (closeRequestedCallback_) {
                closeRequestedCallback_();
            }
            return 0;
        case WM_DISPLAYCHANGE: {
            // Which display the overlay belongs on is not this window's
            // decision, and the answer may now be a different display, or
            // the same one at another size. It comes back as MoveToDisplay.
            if (displaysChangedCallback_) {
                displaysChangedCallback_();
            }
            return 0;
        }
        case WM_SIZE:
            // Whoever resized the window - MoveToDisplay, or Windows itself
            // - the swap chain is resized with it, or the frame is drawn at
            // the old size and stretched.
            if (renderer_ && wParam != SIZE_MINIMIZED) {
                renderer_->HandleResize();
            }
            return 0;
        case WM_DPICHANGED:
            // Sent when the window lands on a display with a different
            // scale, suggesting the old size scaled to match. The window is
            // exactly the size of its display in physical pixels and has to
            // stay that way, so the suggestion is declined by not acting on
            // it.
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace sz::platform::win32
