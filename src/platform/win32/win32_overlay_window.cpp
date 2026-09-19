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
// keyboard grab synthesises WM_CHAR for the overlay (see
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
// stands in for a double-click (see OverlayApp::MatureHeldPress). Told to
// Windows the two ways it documents: as a window property at creation,
// and as the answer to WM_TABLET_QUERYSYSTEMGESTURESTATUS.
//
// Measured on a touch screen, tracing the input at the hook, the raw
// stream and the window: Windows kept sending it anyway - a finger held
// still arrived as a left press, then 650 ms later a right press and
// release with the left still down, then the left release, and none of it
// tagged as touch. So the app does not rely on this being
// heard: it ignores a second button while one is down (see
// OverlayApp::pressedButton_), which is what keeps the hold's work. This
// stays because it is the documented request, costs nothing, and may be
// honoured for a pen or on another Windows.
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
}  // namespace

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

    // The grab posts synthesised mouse messages here once it starts
    // swallowing the real ones - see Win32InputGrab. Handing it the window
    // now rather than at first use keeps the "is it allowed to run yet"
    // question in one place (RefreshEditModeInput).
    Win32InputGrab::Instance().SetPointerBounds(displayRect_);
    Win32InputGrab::Instance().SetOverlayWindow(hwnd_);

    renderer_ = std::make_unique<Win32Dx11Renderer>();
    if (!renderer_->Initialize(hwnd_)) {
        renderer_.reset();
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
    if (!hwnd_ || EqualRect(&target, &displayRect_)) {
        return;
    }
    displayRect_ = target;
    Win32InputGrab::Instance().SetPointerBounds(displayRect_);
    // No SWP_SHOWWINDOW, so a hidden window stays hidden: the tray
    // controller places it before showing it, and before a capture taken
    // while it is hidden.
    SetWindowPos(hwnd_, nullptr, display.x, display.y, display.width, display.height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (renderer_) {
        renderer_->HandleResize();
    }
}

void Win32OverlayWindow::SetDisplaysChangedCallback(std::function<void()> callback) {
    displaysChangedCallback_ = std::move(callback);
}

void Win32OverlayWindow::Show() { ShowInternal(/*activate=*/true); }

void Win32OverlayWindow::ShowWithoutActivating() { ShowInternal(/*activate=*/false); }

void Win32OverlayWindow::ShowInternal(bool activate) {
    if (!hwnd_ || visible_) {
        return;
    }
    // Captured regardless of noActivate_: it's the restore target for
    // Hide() and, under noActivate_, for ReleaseTextInput() too - whether or
    // not this Show() call itself ends up taking focus.
    previousForegroundWindow_ = GetForegroundWindow();
    // SW_SHOW *activates* the window it shows - that is the whole
    // difference between it and SW_SHOWNOACTIVATE, and it is enough to take
    // focus on its own, with no SetForegroundWindow anywhere near it.
    // Ordinarily that is wanted; where it isn't, WS_EX_NOACTIVATE (see
    // SetEditModeNoActivate) makes it impossible anyway - and where a
    // caller wants no focus regardless of that setting, it says so (see
    // ShowWithoutActivating).
    ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate && !noActivate_) {
        SetForegroundWindow(hwnd_);
    }
    // Claim the top of the topmost band, without taking activation.
    // WS_EX_TOPMOST puts a window in that band but says nothing about its
    // order *within* it, and that order follows activation - which under
    // noActivate_ we deliberately never take, so ShowWindow alone leaves us
    // wherever the last activation left us.
    //
    // Necessary but not sufficient on its own: whatever put another topmost
    // window in front is generally still going on after this call. See the
    // recheck in RenderFrame.
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    visible_ = true;
    RefreshEditModeInput();
    QueryPerformanceCounter(&lastFrameTime_);  // avoid a large delta-time spike on the first frame
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

void Win32OverlayWindow::Hide() {
    if (!hwnd_ || !visible_) {
        return;
    }
    // Only reclaim/restore focus if this window itself currently holds
    // it. `previousForegroundWindow_` is a one-time snapshot from Show()
    // - accurate for as long as this window keeps holding real OS focus
    // itself (ordinary edit mode), but not once input-passthrough view
    // mode hands focus off to whatever's underneath (see
    // SetInputPassthrough): from that point on, the user can freely
    // click into and out of *other* windows through the click-through
    // overlay, and GetForegroundWindow() already correctly tracks
    // wherever that leaves them - overriding it with our own stale
    // memory here would silently undo a real focus change the user made
    // while the overlay was up (e.g. switching to a window that wasn't
    // even foreground yet at Show() time).
    const bool weHoldFocus = (GetForegroundWindow() == hwnd_);
    // A field open at this moment is being hidden along with the overlay, so
    // whatever it borrowed goes back now rather than on a close that will
    // never come. The grab-side claims are cleared by SetActive below; this
    // is the window's own half, the WS_EX_NOACTIVATE bit.
    if (focusBorrowed_) {
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
        focusBorrowed_ = false;
    }
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    RefreshEditModeInput();  // never keep the machine's input while invisible
    if (weHoldFocus && previousForegroundWindow_ && IsWindow(previousForegroundWindow_)) {
        SetForegroundWindow(previousForegroundWindow_);
    }
    previousForegroundWindow_ = nullptr;
}

bool Win32OverlayWindow::IsVisible() const { return visible_; }

ForegroundApp Win32OverlayWindow::UnderlyingApplication() const {
    // Whoever holds the foreground, unless that is this window - which it
    // is only in the configuration where edit mode takes focus, and where
    // the answer is therefore the window it took it from. Asking that way
    // round rather than branching on noActivate_ means the two
    // configurations need no separate handling and neither does the
    // hidden case.
    HWND target = GetForegroundWindow();
    if (target == nullptr || target == hwnd_) {
        target = previousForegroundWindow_;
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
        // for most other processes, and it is all QueryFullProcessImageName
        // needs. It still fails for a process at a higher integrity level -
        // a game started as administrator - which is why the title above is
        // read first and unconditionally.
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
            CloseHandle(process);
        }
    }
    return app;
}

void Win32OverlayWindow::SetInputPassthrough(bool enabled) {
    inputPassthrough_ = enabled;
    if (!hwnd_) {
        return;
    }

    // The WM_NCHITTEST/HTTRANSPARENT handling below turned out not to be
    // enough on its own: that mechanism only re-does hit-testing among
    // windows on the *same thread* ("the message will be sent to
    // underlying windows in the same thread" per its own docs), and the
    // whole point here is routing to a window in a completely different
    // process (the game). WS_EX_TRANSPARENT is what actually makes a
    // window invisible to hit-testing at the OS/window-manager level,
    // regardless of what owns whatever's underneath - the standard
    // technique for a genuine cross-process click-through overlay.
    // WS_EX_TRANSPARENT alone is reported unreliable without
    // WS_EX_LAYERED alongside it; deliberately not calling
    // SetLayeredWindowAttributes/UpdateLayeredWindow for it, though -
    // this window's actual per-pixel transparency still comes entirely
    // from ImGui_ImplWin32_EnableAlphaCompositing's DWM blur-behind (see
    // the translucency section of docs/ARCHITECTURE.md), and those two
    // legacy layered-window APIs are how the *other*, rejected
    // translucency techniques there fed a window's visible alpha - if a
    // set-once color/alpha value from either applied on top of blur-behind,
    // it would fight it. WS_EX_LAYERED's bit is only being borrowed here
    // for what it does to hit-testing, not for what it can do to pixels.
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enabled) {
        exStyle |= (WS_EX_LAYERED | WS_EX_TRANSPARENT);
    } else {
        exStyle &= ~(WS_EX_LAYERED | WS_EX_TRANSPARENT);
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);
    // View-only mode is click-through by design, so the grab stands down
    // for its duration and comes back when edit mode does.
    RefreshEditModeInput();

    if (!visible_) {
        return;
    }
    // Click-through alone only covers mouse routing - keyboard focus is
    // independent of cursor position in Win32, so without this, whichever
    // window was focused when view mode was entered (typically this
    // overlay itself, from Show()) would keep "owning" the keyboard the
    // entire time view mode is up, silently eating the game's WASD/etc.
    // input. Handing focus back to whatever had it before this overlay
    // ever took over - the same target Hide() itself restores to - is
    // what actually makes view mode not interfere with gameplay, not just
    // "not swallow clicks."
    if (enabled) {
        if (previousForegroundWindow_ && IsWindow(previousForegroundWindow_)) {
            SetForegroundWindow(previousForegroundWindow_);
        }
    } else if (!noActivate_) {
        // Back to edit mode: reclaim focus so drawing/the toolbar work
        // immediately, without needing an extra click on the overlay
        // first. Skipped under noActivate_ - there, edit mode is meant to
        // stay mouse-only and leave the game focused; RequestTextInput() grabs
        // real focus later only if something (e.g. a rename field)
        // actually needs it.
        SetForegroundWindow(hwnd_);
    }
}

namespace {

// Windows has no stock pen the way it has a crosshair or a hand, so this
// one is drawn - from the same outline the software pointer draws (see
// platform::pen_glyph), rasterised here rather than hand-authored as pixel
// art. The art it replaces was a 45-degree stick with uneven ends, and next
// to the drawn pen it read as a different, crooked tool.
//
// 24 rows of 24, which is the size Windows scales its own cursors at on a
// 100% display, with the nib at the lower left so the pen points at the
// pixel it will mark rather than near it.
constexpr int kPenCursorSize = 24;
constexpr int kPenCursorHotspotX = 2;
constexpr int kPenCursorHotspotY = 21;
// Samples per pixel per axis. A pen that is mostly two long diagonals
// stands or falls on its edges, and 16 samples is the difference between
// stepped and smooth at this size.
constexpr int kPenCursorSubsamples = 4;

}  // namespace

HCURSOR Win32OverlayWindow::PenCursor() {
    if (penCursorBuilt_) {
        return penCursor_;
    }
    penCursorBuilt_ = true;

    // A 32-bit top-down DIB, so the alpha channel is the mask and no
    // separate AND bitmap has to be built by hand - CreateIconIndirect
    // still wants an hbmMask, but an all-zero one leaves the alpha in
    // charge.
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = kPenCursorSize;
    header.bV5Height = -kPenCursorSize;  // negative: rows top-down, like the art above
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
    for (int y = 0; y < kPenCursorSize; ++y) {
        for (int x = 0; x < kPenCursorSize; ++x) {
            // The glyph's origin is its nib, and the hotspot is the pixel
            // the nib has to land on - so the origin sits at that pixel's
            // centre, half a pixel in from its corner.
            int edgeSamples = 0;
            int fillSamples = 0;
            for (int sy = 0; sy < kPenCursorSubsamples; ++sy) {
                for (int sx = 0; sx < kPenCursorSubsamples; ++sx) {
                    const float px = static_cast<float>(x - kPenCursorHotspotX) +
                                     (static_cast<float>(sx) + 0.5f) / kPenCursorSubsamples - 0.5f;
                    const float py = static_cast<float>(y - kPenCursorHotspotY) +
                                     (static_cast<float>(sy) + 0.5f) / kPenCursorSubsamples - 0.5f;
                    switch (pen_glyph::InkAt(px, py)) {
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
                argb[static_cast<size_t>(y) * kPenCursorSize + x] = 0x00000000;
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
            // so a half-covered edge pixel is half its colour *and* half
            // its alpha rather than a full-strength colour showing through.
            const auto channel = [alpha](float value) {
                return static_cast<uint32_t>(std::lround(std::clamp(value * alpha, 0.0f, 255.0f)));
            };
            argb[static_cast<size_t>(y) * kPenCursorSize + x] =
                (static_cast<uint32_t>(std::lround(alpha * 255.0f)) << 24) | (channel(r) << 16) |
                (channel(g) << 8) | channel(b);
        }
    }

    // Empty mask: with a 32-bit colour bitmap the alpha channel decides, and
    // this only has to exist.
    const HBITMAP mask = CreateBitmap(kPenCursorSize, kPenCursorSize, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = FALSE;  // a cursor, so the hotspot fields below are read
    info.xHotspot = kPenCursorHotspotX;
    info.yHotspot = kPenCursorHotspotY;
    info.hbmMask = mask;
    info.hbmColor = color;
    penCursor_ = static_cast<HCURSOR>(CreateIconIndirect(&info));
    DeleteObject(color);
    DeleteObject(mask);
    return penCursor_;
}

HCURSOR Win32OverlayWindow::CursorFor(CursorShape shape) {
    switch (shape) {
        case CursorShape::Crosshair:
            return LoadCursorA(nullptr, IDC_CROSS);
        case CursorShape::Pen:
            // Falls back to the crosshair if the glyph couldn't be built -
            // "the exact point matters here" is the half of the pen's
            // meaning that survives, and it is the more useful half.
            if (HCURSOR pen = PenCursor()) {
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
    SetCursor(Win32InputGrab::Instance().SoftwarePointerWanted() ? nullptr : CursorFor(shape));
}

void Win32OverlayWindow::SetEditModeNoActivate(bool enabled) {
    noActivate_ = enabled;
    if (!hwnd_) {
        return;  // not created yet - EnsureCreated bakes noActivate_ into the initial exStyle instead
    }
    // Same live-restyle technique SetInputPassthrough already uses for
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
    // focus immediately, matching what Show() would have done had this
    // been set before the window was ever shown - otherwise edit mode
    // would silently stay mouse-only until the next hide/show cycle.
    // Turning it ON needs no equivalent action: whatever already has focus
    // (this window, or the game underneath) simply keeps it; this window
    // just won't grab it again on its own from here on. Skipped while in
    // view-only/input-passthrough - there, focus deliberately stays with
    // whatever's underneath regardless of this setting (see
    // SetInputPassthrough).
    if (!enabled && visible_ && !inputPassthrough_) {
        SetForegroundWindow(hwnd_);
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
    if (Win32InputGrab::Instance().CanDeliverTyping()) {
        Win32InputGrab::Instance().SetTextFieldOpen(true);
        return;
    }

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
    SetForegroundWindow(hwnd_);
    // Foreground is the window; focus is the keyboard. Ask for both - the
    // second is what an InputText is actually waiting on.
    SetFocus(hwnd_);
}

void Win32OverlayWindow::ReleaseTextInput() {
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

    if (previousForegroundWindow_ && IsWindow(previousForegroundWindow_)) {
        SetForegroundWindow(previousForegroundWindow_);
    }
}

void Win32OverlayWindow::SetFrameCallback(FrameCallback callback) { frameCallback_ = std::move(callback); }

void Win32OverlayWindow::SetFramePacing(FramePacing pacing) { framePacing_ = pacing; }

namespace {
// An idle overlay still draws four times a second: the same cadence as the
// check that it is still the front topmost window (see RenderFrame), which
// matters most exactly when it sits over a game - and often enough that
// anything else done per frame, the autosave's debounce or following a
// change of display size, is never far behind.
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

void Win32OverlayWindow::SetMouseCallback(MouseCallback callback) { mouseCallback_ = std::move(callback); }

InputGrabDiagnostics Win32OverlayWindow::GetInputGrabDiagnostics() const {
    return Win32InputGrab::Instance().Diagnostics();
}

CaptureResult Win32OverlayWindow::CaptureRegionAsTexture(const Rect& rect) {
    const int width = static_cast<int>(rect.w);
    const int height = static_cast<int>(rect.h);
    if (!renderer_ || !hwnd_ || width <= 0 || height <= 0) {
        return CaptureResult{};
    }
    // `rect` is in the window's coordinates, which are the desktop's only
    // while the window is on the primary display. Asked of the window rather
    // than worked out from displayRect_, so it is wherever the window
    // really is - hidden or not, which makes no difference to the answer.
    POINT source{static_cast<LONG>(rect.x), static_cast<LONG>(rect.y)};
    ClientToScreen(hwnd_, &source);

    // Hide this window before grabbing pixels, so the capture shows what's
    // actually behind the overlay rather than the overlay's own content -
    // BitBlt from the desktop DC reads the fully composited image, which
    // would otherwise include this (topmost, alpha-composited) window.
    // DwmFlush() blocks until the next composition pass has happened,
    // which is what makes the hide actually take effect in what BitBlt
    // sees before the capture below runs - without it, this is a race.
    const bool wasVisible = visible_;
    if (wasVisible) {
        ShowWindow(hwnd_, SW_HIDE);
        DwmFlush();
    }

    std::vector<uint8_t> pixelsBGRA(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    bool captured = false;

    if (HDC screenDC = GetDC(nullptr)) {
        if (HDC memDC = CreateCompatibleDC(screenDC)) {
            if (HBITMAP bitmap = CreateCompatibleBitmap(screenDC, width, height)) {
                HGDIOBJ oldObj = SelectObject(memDC, bitmap);
                // CAPTUREBLT includes layered windows (other apps' own
                // translucent UI) in the capture, matching what's visually
                // on screen rather than just the opaque desktop.
                const BOOL blitted =
                    BitBlt(memDC, 0, 0, width, height, screenDC, source.x, source.y, SRCCOPY | CAPTUREBLT);
                // Deselected *before* it is read: GetDIBits documents that
                // the bitmap must not be selected into a DC when it is
                // called. It happened to work while selected, on the
                // drivers tried, which is not the same as being allowed.
                SelectObject(memDC, oldObj);
                if (blitted) {
                    BITMAPINFOHEADER bi{};
                    bi.biSize = sizeof(bi);
                    bi.biWidth = width;
                    bi.biHeight = -height;  // negative = top-down DIB, matching our RGBA row order
                    bi.biPlanes = 1;
                    bi.biBitCount = 32;
                    bi.biCompression = BI_RGB;
                    BITMAPINFO bmi{};
                    bmi.bmiHeader = bi;
                    // Every row, or nothing: a short read is a picture
                    // with garbage along its bottom, not a capture.
                    captured = GetDIBits(memDC, bitmap, 0, static_cast<UINT>(height), pixelsBGRA.data(), &bmi,
                                          DIB_RGB_COLORS) == height;
                }
                DeleteObject(bitmap);
            }
            DeleteDC(memDC);
        }
        ReleaseDC(nullptr, screenDC);
    }

    if (wasVisible) {
        ShowWindow(hwnd_, SW_SHOW);
    }

    if (!captured) {
        return CaptureResult{};
    }

    // GDI's 32bpp DIBs are byte-order BGRA; D3D11's DXGI_FORMAT_R8G8B8A8_UNORM
    // (what CreateTextureFromRGBA uses, matching imgui_impl_dx11.cpp's own
    // texture format) wants RGBA - swap the B/R bytes of each pixel in place.
    // The fourth byte of a 32bpp DIB is not an alpha channel: GDI leaves it
    // undefined (zero for most of the screen, whatever a layered window
    // wrote for the rest), and a screenshot is opaque by definition, so it
    // is set rather than trusted.
    for (size_t i = 0; i + 3 < pixelsBGRA.size(); i += 4) {
        std::swap(pixelsBGRA[i], pixelsBGRA[i + 2]);
        pixelsBGRA[i + 3] = 255;
    }

    ID3D11ShaderResourceView* srv = renderer_->CreateTextureFromRGBA(pixelsBGRA.data(), width, height);
    if (!srv) {
        return CaptureResult{};
    }
    CaptureResult result;
    result.textureHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(srv));
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

void Win32OverlayWindow::Destroy() {
    // Before the window goes: the grab posts messages to it, and its hooks
    // are global state that outliving this object would be a real problem.
    Win32InputGrab::Instance().Shutdown();
    if (renderer_) {
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

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const float deltaSeconds = perfFrequency_.QuadPart > 0
                                    ? static_cast<float>(now.QuadPart - lastFrameTime_.QuadPart) /
                                          static_cast<float>(perfFrequency_.QuadPart)
                                    : 0.0f;
    lastFrameTime_ = now;

    // Reclaiming the front of the topmost band has to be repeated, not done
    // once on show. Reproduced: leave the taskbar as the foreground window -
    // click it, or close the window that was in front of it - and then bring
    // the overlay up. Shell_TrayWnd ends up above us and stays there, so the
    // edit-mode border and the bottom of the context ring are drawn over. A
    // single SetWindowPos in Show is not enough, because whatever put the
    // taskbar in front is still happening after we have shown ourselves.
    //
    // Checked rather than asserted blindly, and throttled, so this is a few
    // GetWindow calls four times a second rather than a SetWindowPos every
    // frame fighting the shell for the front.
    topmostCheckSeconds_ += deltaSeconds;
    if (topmostCheckSeconds_ >= 0.25f) {
        topmostCheckSeconds_ = 0.0f;
        if (CoveredByAnotherTopmostWindow()) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    Win32InputGrab& grab = Win32InputGrab::Instance();
    grab.FlushPendingCorrection();

    // Modifier state from two sources that between them cover every case.
    // ImGui's backend learns Ctrl/Shift/Alt from key messages, which need
    // keyboard focus - and edit_mode_no_activate exists precisely to keep
    // focus with the game, which is how Alt-drag stopped working under it.
    // GetAsyncKeyState sees a key whoever has focus. What it cannot see is
    // a key the keyboard grab swallowed, because a swallowed event updates
    // no key state anywhere; the grab tracked those itself all along, so
    // its record fills that gap. Whichever source has focus or the hook,
    // the other reads false, and the OR is simply the truth.
    const auto asyncDown = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool grabCtrl = false;
    bool grabShift = false;
    bool grabAlt = false;
    grab.HeldModifiers(grabCtrl, grabShift, grabAlt);
    renderer_->SetModifierOverride(asyncDown(VK_CONTROL) || grabCtrl, asyncDown(VK_SHIFT) || grabShift,
                                   asyncDown(VK_MENU) || grabAlt);
    grab.SampleFrameStep();
    renderer_->SetSoftwarePointerActive(grab.SoftwarePointerWanted());

    // While the grab owns the mouse, ImGui must navigate by the overlay's
    // own pointer: the real cursor belongs to the game for the duration and
    // may be pinned, hidden or re-centred behind our back.
    if (grab.VirtualCursorActive()) {
        // The whole-pixel position - the same one the grab stamps on the
        // button messages it posts, so what ImGui hovers, what a click
        // hits, and what gets drawn all agree.
        POINT client = grab.VirtualCursor();
        ScreenToClient(hwnd_, &client);
        renderer_->SetMousePositionOverride(true, static_cast<float>(client.x), static_cast<float>(client.y));

        // While a button is held, one Move event per frame, and only if the
        // pointer actually moved - which is exactly what the OS delivers
        // through WM_MOUSEMOVE when it isn't being swallowed. Emitted here
        // rather than posted from the input thread: posting one per mouse
        // report flooded the queue and laid down a stroke point per report,
        // and gating that needed a flag the two threads had to share. The
        // render thread already has the position; it just says so.
        const UINT held = grab.HeldButtons();
        const bool moved = client.x != lastEmittedMove_.x || client.y != lastEmittedMove_.y;
        if (held != 0 && moved) {
            const Vec2 pos{static_cast<float>(client.x), static_cast<float>(client.y)};
            if (held & MK_LBUTTON) {
                EmitMouseEvent(pos, MouseButton::Left, MouseEventKind::Move);
            }
            if (held & MK_RBUTTON) {
                EmitMouseEvent(pos, MouseButton::Right, MouseEventKind::Move);
            }
        }
        lastEmittedMove_ = client;
    } else {
        renderer_->SetMousePositionOverride(false, 0.0f, 0.0f);
    }

    renderer_->NewFrame();
    if (frameCallback_) {
        frameCallback_(deltaSeconds);
    }
    renderer_->RenderAndPresent();
}

void Win32OverlayWindow::EmitMouseEvent(const Vec2& position, MouseButton button, MouseEventKind kind) {
    if (mouseCallback_) {
        mouseCallback_(MouseEvent{position, button, kind});
    }
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
            SetCursor(CursorFor(cursorShape_));
            return TRUE;
        }
        // Default deliberately falls through to ImGui's own handler below,
        // which installs whatever the frame asked for - a resize handle's
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
                // Belt-and-suspenders alongside SetInputPassthrough's own
                // WS_EX_TRANSPARENT toggle (see its comment for why that,
                // not this, is what actually makes cross-process
                // click-through work) - answering HTTRANSPARENT here too
                // costs nothing and is the technically "correct" response
                // regardless.
                return HTTRANSPARENT;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_TABLET_QUERYSYSTEMGESTURESTATUS:
            return static_cast<LRESULT>(kTabletGestureFlags);  // see its comment
        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            const Vec2 pos{static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))};
            EmitMouseEvent(pos, MouseButton::Left, MouseEventKind::Down);
            return 0;
        }
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
            const Vec2 pos{static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))};
            if (wParam & MK_LBUTTON) {
                EmitMouseEvent(pos, MouseButton::Left, MouseEventKind::Move);
            }
            // Both buttons can be down at once (e.g. a right-drag started
            // while a left drag/stroke is still in flight) - each gets its
            // own Move event off the same wParam bitmask rather than an
            // else-if, so neither one silently stops tracking mid-gesture.
            if (wParam & MK_RBUTTON) {
                EmitMouseEvent(pos, MouseButton::Right, MouseEventKind::Move);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const Vec2 pos{static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))};
            EmitMouseEvent(pos, MouseButton::Left, MouseEventKind::Up);
            return 0;
        }
        // Right button: the app's own right-button gestures (a resize from
        // a snippet's nearest edge, the eraser in drawing mode, framing a
        // drawing on empty canvas - see OverlayApp::OnMouse). Shares
        // SetCapture/ReleaseCapture with the left button above (Win32
        // mouse capture is per-window, not per-button); harmless to call
        // again if already captured, and ReleaseCapture here is safe even
        // if a left drag is still in progress since a genuine simultaneous
        // L+R drag is not a gesture this app gives any meaning to.
        case WM_RBUTTONDOWN: {
            SetCapture(hwnd);
            const Vec2 pos{static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))};
            EmitMouseEvent(pos, MouseButton::Right, MouseEventKind::Down);
            return 0;
        }
        case WM_RBUTTONUP: {
            ReleaseCapture();
            const Vec2 pos{static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))};
            EmitMouseEvent(pos, MouseButton::Right, MouseEventKind::Up);
            return 0;
        }
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
        // lifecycle is owned entirely by Show()/Hide()/Destroy() (driven
        // by the tray icon and hotkeys), not by its own default close
        // affordances - swallow both instead of falling through to the
        // default handling.
        case WM_SYSCOMMAND: {
            // The low 4 bits of an SC_* command are reserved by Windows
            // for its own internal use - mask them off before comparing,
            // per WM_SYSCOMMAND's own documented contract.
            if ((wParam & 0xFFF0) == SC_CLOSE) {
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CLOSE:
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
