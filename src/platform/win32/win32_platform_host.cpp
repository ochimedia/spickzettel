#include "platform/win32/win32_platform_host.h"

#include <sddl.h>
#include <shellapi.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "platform/win32/resources/resource.h"
#include "platform/win32/win32_displays.h"
#include "platform/win32/win32_input_grab.h"
// The tray menu's two items are words the user reads, so they live in the
// same catalog as every other one - see cmake/UiStrings.cmake. A header
// of constants, so this costs the platform layer no dependency on core.
#include "generated/ui_strings.h"

namespace sz::platform::win32 {

namespace strings = sz::strings;

namespace {
constexpr const char* kWindowClassName = "SpickzettelHostWindowClass";
constexpr UINT kTrayIconMessage = WM_APP + 1;
// A task posted from the app thread - see Post.
constexpr UINT kPostedTaskMessage = WM_APP + 2;

// Whether this message must not go through TranslateMessage.
//
// While the keyboard grab is delivering typing, it posts a synthetic
// WM_KEYDOWN to the overlay *and* synthesizes the matching WM_CHAR itself,
// from the modifier state that only it has (see
// Win32InputGrab::PostCharactersToOverlay). TranslateMessage would then make
// a second WM_CHAR out of that same posted key-down - measured: every letter
// arriving twice - and it would make it from this thread's keyboard state,
// which never saw the swallowed Shift or AltGr. So the duplicate is the wrong
// character as well as a surplus one, and the grab's version is the one to
// keep.
//
// Only key-downs, and only while the grab is delivering: with it off, these
// are real messages for a focused window and TranslateMessage is the only
// thing producing characters at all.
bool SkipCharacterTranslation(const MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) {
        return false;
    }
    return Win32InputGrab::Instance().DeliversTypingToOverlay();
}
constexpr UINT kMenuIdToggle = 1;
constexpr UINT kMenuIdExit = 2;
// See SetBackgroundTimer: a WM_TIMER on the message window, which is
// pumped whether or not the overlay is up.
constexpr UINT_PTR kBackgroundTimerId = 1;
}  // namespace

Win32PlatformHost::~Win32PlatformHost() {
    for (const auto& [id, callback] : hotkeyCallbacks_) {
        UnregisterHotKey(hwnd_, id);
    }
    RemoveTrayIcon();
    overlayWindow_.Destroy();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (instanceMutex_) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
}

bool Win32PlatformHost::Initialize(const std::string& appName) {
    appName_ = appName;
    HINSTANCE instance = GetModuleHandleA(nullptr);

    WNDCLASSEXA windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Win32PlatformHost::WndProcThunk;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClassName;
    // Cosmetic only - this window is never shown, but the icon it
    // registers here is also what ShowTrayIcon() below reuses for the
    // actual tray icon.
    windowClass.hIcon = LoadIconA(instance, MAKEINTRESOURCEA(IDI_APP_ICON));
    RegisterClassExA(&windowClass);

    // A hidden top-level window - not a message-only one (HWND_MESSAGE).
    // WM_QUERYENDSESSION and WM_ENDSESSION are broadcast to every top-level
    // window, and message-only windows are left off that list: the first
    // version of the session-end handling below sat on an HWND_MESSAGE
    // window, where no logoff could ever have reached it. Never shown, and
    // a tool window besides, so that nothing lists it either.
    hwnd_ = CreateWindowExA(WS_EX_TOOLWINDOW, kWindowClassName, appName_.c_str(), WS_OVERLAPPED, 0, 0, 0, 0,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) {
        return false;
    }

    // Broadcast by Explorer once a taskbar is up - after it restarts, with
    // every tray icon it had gone; each program puts its own back. An
    // elevated copy is above Explorer, and the broadcast is only let up
    // to it when asked for.
    taskbarCreatedMessage_ = RegisterWindowMessageA("TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMessage_, MSGFLT_ALLOW, nullptr);

    overlayWindow_.Initialize(instance);
    // A close asked of the overlay is one asked of the app - see WM_CLOSE.
    overlayWindow_.SetCloseRequestedCallback([this] { Exit(); });
    return true;
}

// The session's namespace alone is not one per library: "Run as
// administrator" from a standard account runs the app as the administrator
// account, in the same session, with an %APPDATA% and a library of its
// own. Named for the user as well, the mutex is one per %APPDATA%, and a
// copy it refuses is one that would write this library. Without the SID,
// should the token not answer, it is the session's name alone, as before.
std::wstring InstanceMutexName(const std::string& appName) {
    std::wstring name = L"Local\\" + std::wstring(appName.begin(), appName.end()) + L".Instance";
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return name;
    }
    DWORD size = 0;
    // The documented two-call shape, as in IntegrityRidOf.
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    wchar_t* sid = nullptr;
    if (size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
        name += L".";
        name += sid;
        LocalFree(sid);
    }
    CloseHandle(token);
    return name;
}

bool Win32PlatformHost::AcquireSingleInstance() {
    if (instanceMutex_) {
        return true;
    }
    // A named mutex, one per user and so one per library - see
    // InstanceMutexName. Created owned; ERROR_ALREADY_EXISTS means another
    // process made it first and is still alive - the kernel drops it with
    // its last handle, so a copy that crashed holds nothing.
    //
    // ERROR_ACCESS_DENIED means the same. Asked for a mutex that is
    // already there, CreateMutex opens it with every right, and fails
    // with that when the one who made it did not grant them - which is
    // what the same user sees of a copy run as administrator: its objects
    // are made for the Administrators group and at high integrity, and
    // the user unelevated is neither. The same library, so the same
    // refusal; letting it start was two writers on it.
    const std::wstring name = InstanceMutexName(appName_);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!mutex) {
        // Anything else - the name taken by an object that is not a
        // mutex, say - could not even ask: not a reason to refuse to start.
        return GetLastError() != ERROR_ACCESS_DENIED;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return false;
    }
    instanceMutex_ = mutex;
    return true;
}

bool Win32PlatformHost::ShowTrayIcon() {
    if (trayIconVisible_) {
        return true;
    }

    NOTIFYICONDATAA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayIconMessage;
    // The real app icon (see resource.h/app_icon.rc), not IDI_APPLICATION's
    // generic placeholder - GetModuleHandleA(nullptr) rather than a stored
    // instance handle since this method has no other reason to keep one
    // around.
    nid.hIcon = LoadIconA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(IDI_APP_ICON));
    strncpy_s(nid.szTip, appName_.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconA(NIM_ADD, &nid)) {
        return false;
    }
    trayIconVisible_ = true;
    return true;
}

void Win32PlatformHost::RemoveTrayIcon() {
    if (!trayIconVisible_) {
        return;
    }
    NOTIFYICONDATAA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    Shell_NotifyIconA(NIM_DELETE, &nid);
    trayIconVisible_ = false;
}

void Win32PlatformHost::SetTrayCommandCallback(TrayCommandCallback callback) {
    trayCallback_ = std::move(callback);
}

int Win32PlatformHost::RegisterGlobalHotkey(const KeyCombo& combo, HotkeyCallback callback) {
    if (!hwnd_ || !combo.IsValid()) {
        return 0;
    }

    UINT modifiers = MOD_NOREPEAT;
    if (combo.ctrl) {
        modifiers |= MOD_CONTROL;
    }
    if (combo.alt) {
        modifiers |= MOD_ALT;
    }
    if (combo.shift) {
        modifiers |= MOD_SHIFT;
    }

    // VK_F1..VK_F24 are consecutive in winuser.h, same as KeyCombo's own
    // F1..F24 encoding (see KeyCombo::kFunctionKeyBase) - letters/digits
    // instead pass straight through, since their ASCII value already is
    // the matching VK code.
    const UINT vkCode = combo.IsFunctionKey() ? static_cast<UINT>(VK_F1 + combo.FunctionKeyNumber() - 1)
                                               : static_cast<UINT>(combo.key);

    const int id = nextHotkeyId_++;
    if (!RegisterHotKey(hwnd_, id, modifiers, vkCode)) {
        return 0;
    }

    hotkeyCallbacks_[id] = std::move(callback);
    // Also handed to the input grab: while that is swallowing the keyboard,
    // Windows stops delivering WM_HOTKEY at all (measured - a low-level
    // hook that discards the event suppresses the hotkey with it), so the
    // grab matches the combo itself and posts the identical message back to
    // this same window, where the handler below can't tell the difference.
    // Done for every hotkey unconditionally: it costs nothing while no grab
    // is running, and leaves no registration state to synchronize when one
    // starts.
    Win32InputGrab::Instance().AddHotkey(id, combo, hwnd_);
    return id;
}

void Win32PlatformHost::UnregisterGlobalHotkey(int hotkeyId) {
    if (hotkeyId == 0) {
        return;
    }
    UnregisterHotKey(hwnd_, hotkeyId);
    hotkeyCallbacks_.erase(hotkeyId);
    Win32InputGrab::Instance().RemoveHotkey(hotkeyId);
}

IOverlayWindow& Win32PlatformHost::GetOverlayWindow() { return overlayWindow_; }

std::vector<DisplayInfo> Win32PlatformHost::ListDisplays() const { return EnumerateDisplays(); }

namespace {
// Shared by GetConfigFilePath/GetLibraryPath below.
std::filesystem::path AppDataBase() {
    std::filesystem::path base;
    // GetEnvironmentVariableW rather than std::getenv: the wide form is
    // what %APPDATA% actually is, so a user whose profile folder holds a
    // character outside the system code page gets a path that works rather
    // than one the ANSI copy mangled; it is a plain kernel32 export, so it
    // exists on every toolchain; and MSVC deprecates getenv.
    //
    // Called twice on purpose: with (nullptr, 0) it answers with the size
    // it needs, terminator included, and 0 only when the variable is not
    // set at all.
    std::wstring appData;
    if (const DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0); needed > 0) {
        appData.resize(needed);
        // ...and this time it answers with how much it wrote, terminator
        // excluded, which is where the string really ends.
        appData.resize(GetEnvironmentVariableW(L"APPDATA", appData.data(), needed));
    }
    if (appData.empty()) {
        base = std::filesystem::current_path();
    } else {
        base = appData;
    }
    // Capitalized because %APPDATA% is somewhere people actually browse,
    // and the name is a proper noun there.
    return base / "Spickzettel";
}
}  // namespace

std::filesystem::path Win32PlatformHost::GetConfigFilePath() const { return AppDataBase() / "config.json"; }

std::filesystem::path Win32PlatformHost::GetLibraryPath() const { return AppDataBase() / "library.db"; }

int Win32PlatformHost::RunEventLoop() {
    running_ = true;
    MSG msg;
    while (running_) {
        if (overlayWindow_.IsVisible()) {
            bool dispatched = false;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    running_ = false;
                    break;
                }
                if (!SkipCharacterTranslation(msg)) {
                    TranslateMessage(&msg);
                }
                DispatchMessage(&msg);
                dispatched = true;
            }
            if (running_) {
                // Every refresh while the overlay wants that, and otherwise a
                // frame when one is due or a message may have changed what is
                // shown - sleeping in between, until whichever comes first.
                // See IOverlayWindow::SetFramePacing.
                if (overlayWindow_.WantsFrame(dispatched)) {
                    overlayWindow_.RenderFrame();
                } else {
                    MsgWaitForMultipleObjectsEx(0, nullptr, overlayWindow_.MillisecondsUntilIdleFrame(), QS_ALLINPUT,
                                                MWMO_INPUTAVAILABLE);
                }
            }
        } else {
            const BOOL result = GetMessage(&msg, nullptr, 0, 0);
            if (result <= 0) {
                running_ = false;
                break;
            }
            if (!SkipCharacterTranslation(msg)) {
                TranslateMessage(&msg);
            }
            DispatchMessage(&msg);
        }
    }
    return exitCode_;
}

// A message of its own for each task, to the host window: the loop
// dispatches every waiting message before it draws the next frame, and
// wakes for one while it sleeps.
void Win32PlatformHost::Post(std::function<void()> task) {
    posted_.push_back(std::move(task));
    PostMessageW(hwnd_, kPostedTaskMessage, 0, 0);
}

void Win32PlatformHost::Quit(int exitCode) {
    exitCode_ = exitCode;
    running_ = false;
}

void Win32PlatformHost::Exit() {
    // The tray menu's Exit, which settles first; with nobody listening yet,
    // there is nothing to settle.
    if (trayCallback_) {
        trayCallback_(TrayCommand::Exit);
    } else {
        Quit(0);
    }
}

void Win32PlatformHost::SetSessionEndCallback(std::function<void()> callback) {
    sessionEndCallback_ = std::move(callback);
}

void Win32PlatformHost::SetBackgroundTimer(int intervalMs, std::function<void()> callback) {
    if (intervalMs <= 0 || !callback) {
        if (hwnd_) {
            KillTimer(hwnd_, kBackgroundTimerId);
        }
        backgroundTimerCallback_ = nullptr;
        return;
    }
    backgroundTimerCallback_ = std::move(callback);
    if (hwnd_) {
        SetTimer(hwnd_, kBackgroundTimerId, static_cast<UINT>(intervalMs), nullptr);
    }
}

void Win32PlatformHost::ShowTrayContextMenu() {
    POINT cursor{};
    GetCursorPos(&cursor);

    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, kMenuIdToggle, strings::kTrayToggleOverlay);
    AppendMenuA(menu, MF_STRING, kMenuIdExit, strings::kTrayExit);

    // Required so the popup menu dismisses correctly when it loses focus.
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd_, nullptr);
    PostMessage(hwnd_, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

LRESULT CALLBACK Win32PlatformHost::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32PlatformHost* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTA*>(lParam);
        self = static_cast<Win32PlatformHost*>(createStruct->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Win32PlatformHost*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT Win32PlatformHost::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Explorer restarted (see Initialize). Without this the icon was gone
    // until the app was, and with it the menu's Exit. Taken out first,
    // should this taskbar have kept it, so that the add is of a new icon.
    if (msg == taskbarCreatedMessage_ && msg != 0) {
        if (trayIconVisible_) {
            RemoveTrayIcon();
            ShowTrayIcon();
        }
        return 0;
    }
    switch (msg) {
        case kTrayIconMessage:
            // Left is the icon's primary action, which for this app is the
            // same thing the show hotkey and the menu's own first item do:
            // bring the overlay up, or put it away if it is already there.
            // Without it the only way in from the tray was through a menu
            // to press the one item on it that matters.
            if (LOWORD(lParam) == WM_LBUTTONUP) {
                if (trayCallback_) {
                    trayCallback_(TrayCommand::ToggleOverlay);
                }
            } else if (LOWORD(lParam) == WM_RBUTTONUP) {
                ShowTrayContextMenu();
            }
            return 0;
        case kPostedTaskMessage:
            // One task per message, the oldest: a task that posts another
            // queues it behind whatever has been posted meanwhile.
            if (!posted_.empty()) {
                const std::function<void()> task = std::move(posted_.front());
                posted_.pop_front();
                task();
            }
            return 0;
        case WM_HOTKEY: {
            auto it = hotkeyCallbacks_.find(static_cast<int>(wParam));
            if (it != hotkeyCallbacks_.end() && it->second) {
                // Called on a copy: a callback can unregister its own
                // hotkey - capturing View on the Edit combo does, from
                // inside Edit's - and the one in the map is destroyed then.
                const HotkeyCallback callback = it->second;
                callback();
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kMenuIdToggle && trayCallback_) {
                trayCallback_(TrayCommand::ToggleOverlay);
            } else if (id == kMenuIdExit && trayCallback_) {
                trayCallback_(TrayCommand::Exit);
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == kBackgroundTimerId && backgroundTimerCallback_) {
                backgroundTimerCallback_();
            }
            return 0;
        // Logoff or shutdown. The work is done on the query, which is where
        // Windows waits for an answer - by WM_ENDSESSION the decision is
        // made and the time left is not guaranteed. Done again there
        // anyway, cheaply, in case something changed in between. TRUE
        // means "fine by me": there is no case for holding a shutdown up.
        case WM_QUERYENDSESSION:
            if (sessionEndCallback_) {
                sessionEndCallback_();
            }
            return TRUE;
        case WM_ENDSESSION:
            if (!wParam) {
                return 0;  // called off after all
            }
            // The Restart Manager - an installer or updater making room -
            // closes an application with the same pair of messages, and
            // then waits for it to go: an exit, whose own settling is the
            // one this needs. Both ran, and with the query's that was three
            // runs against the Restart Manager's clock. A logoff ends the
            // process itself.
            if (lParam & ENDSESSION_CLOSEAPP) {
                Exit();
            } else if (sessionEndCallback_) {
                sessionEndCallback_();
            }
            return 0;
        // A request to close - taskkill without /f, or anything else that
        // asks politely - is a request to exit. Left to DefWindowProc it
        // destroyed this window and nothing more: the process ran on
        // without its tray icon or hotkeys, holding the single-instance
        // mutex so that no new copy could start.
        case WM_CLOSE:
            Exit();
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

}  // namespace sz::platform::win32

namespace sz::platform {

std::unique_ptr<IPlatformHost> CreatePlatformHost() { return std::make_unique<win32::Win32PlatformHost>(); }

}  // namespace sz::platform
