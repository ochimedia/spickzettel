#include "platform/win32/win32_platform_host.h"

#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

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
constexpr const char* kOpenedAgainMessageName = "Spickzettel.OpenedAgain";
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
    // Titled for the instance, which is the user's: a copy started again
    // finds its own user's copy by it, and not another account's in the
    // same session (see PassOpeningToRunningCopy).
    hwnd_ = CreateWindowExA(WS_EX_TOOLWINDOW, kWindowClassName, InstanceWindowTitle(appName_).c_str(), WS_OVERLAPPED,
                            0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        return false;
    }

    // Broadcast by Explorer once a taskbar is up - after it restarts, with
    // every tray icon it had gone; each program puts its own back. An
    // elevated copy is above Explorer, and the broadcast is only let up
    // to it when asked for.
    taskbarCreatedMessage_ = RegisterWindowMessageA("TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMessage_, MSGFLT_ALLOW, nullptr);
    // The same for a copy started again, unelevated, when this one is
    // elevated: the same user, and all it can ask is to come up.
    openedAgainMessage_ = RegisterWindowMessageA(kOpenedAgainMessageName);
    ChangeWindowMessageFilterEx(hwnd_, openedAgainMessage_, MSGFLT_ALLOW, nullptr);

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

std::string InstanceWindowTitle(const std::string& appName) {
    // The name is the app's and a SID, both ASCII.
    const std::wstring name = InstanceMutexName(appName);
    const std::wstring title = name.substr(name.find(L'\\') + 1);
    std::string narrow;
    narrow.reserve(title.size());
    for (const wchar_t c : title) {
        narrow.push_back(static_cast<char>(c));
    }
    return narrow;
}

bool Win32PlatformHost::PassOpeningToRunningCopy() {
    const std::string title = InstanceWindowTitle(appName_);
    const UINT message = openedAgainMessage_ != 0 ? openedAgainMessage_ : RegisterWindowMessageA(kOpenedAgainMessageName);
    // Every host window titled for this user's instance but this copy's
    // own, which Initialize has made already.
    HWND other = nullptr;
    while ((other = FindWindowExA(nullptr, other, kWindowClassName, title.c_str())) != nullptr) {
        if (other == hwnd_) {
            continue;
        }
        // This copy was just started by the user, so the foreground is
        // its to give: without it, the overlay would come up behind
        // whatever the user started it from.
        DWORD process = 0;
        GetWindowThreadProcessId(other, &process);
        AllowSetForegroundWindow(process);
        if (PostMessageA(other, message, 0, 0)) {
            return true;
        }
    }
    return false;
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
    trayIconWanted_ = true;
    return AddTrayIcon();
}

bool Win32PlatformHost::AddTrayIcon() {
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

    // Started at log-on, the app can be ahead of Explorer's notification
    // area, and the add fails. That once ended the start, with a message
    // saying another copy may be running. So the icon waits for the
    // taskbar instead: its TaskbarCreated puts it up (see HandleMessage),
    // and a timer tries again meanwhile, for an Explorer that was only
    // slow to answer. An add that timed out may have landed after all,
    // which the next add would fail on, so any icon of ours is taken out
    // first.
    Shell_NotifyIconA(NIM_DELETE, &nid);
    const bool failForTesting = failTrayIconAdds_ > 0;
    if (failForTesting) {
        --failTrayIconAdds_;
    }
    if (failForTesting || !Shell_NotifyIconA(NIM_ADD, &nid)) {
        SetTimer(hwnd_, kTrayRetryTimerId, kTrayRetryMs, nullptr);
        return false;
    }
    KillTimer(hwnd_, kTrayRetryTimerId);
    trayIconVisible_ = true;
    return true;
}

void Win32PlatformHost::RemoveTrayIcon() {
    trayIconWanted_ = false;
    KillTimer(hwnd_, kTrayRetryTimerId);
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
    hotkeyKeys_[id] = {modifiers, vkCode};
    // Registered to learn whether the combination is free, and left out
    // until the pause ends.
    if (hotkeysPaused_) {
        UnregisterHotKey(hwnd_, id);
    }
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
    hotkeyKeys_.erase(hotkeyId);
    Win32InputGrab::Instance().RemoveHotkey(hotkeyId);
}

// Taken out of Windows' hands for the pause rather than ignored as they
// arrive: Windows keeps a registered hotkey's press from the focused
// window, so the row waiting would never see it. Registered again under
// the same ids. One another application took in the seconds between
// stays out until the app starts again - Windows refuses it - and keeps
// working only under the keyboard grab, which matches it itself.
void Win32PlatformHost::SetHotkeysPaused(bool paused) {
    if (paused == hotkeysPaused_) {
        return;
    }
    hotkeysPaused_ = paused;
    Win32InputGrab::Instance().SetHotkeysPaused(paused);
    for (const auto& [id, keys] : hotkeyKeys_) {
        if (paused) {
            UnregisterHotKey(hwnd_, id);
        } else {
            RegisterHotKey(hwnd_, id, keys.first, keys.second);
        }
    }
}

IOverlayWindow& Win32PlatformHost::GetOverlayWindow() { return overlayWindow_; }

std::vector<DisplayInfo> Win32PlatformHost::ListDisplays() const { return EnumerateDisplays(); }

namespace {
// Shared by GetConfigFilePath/GetLibraryPath below: the app's folder in
// the user's roaming application data (%APPDATA%), which roams with the
// user, or local one (%LOCALAPPDATA%), which stays on this computer.
//
// Asked of the shell for this process's own account, not read from the
// environment variables of those names. Those are only what the parent
// process handed down, and some sandboxes appear to provide unreliable
// ones: another account's folders, which this process may not open, so
// that neither the settings nor the library could be read. The cost is
// that a deliberately changed %APPDATA% is not followed - a test or a
// measurement names its own folder with --data-dir instead (see
// app::CommandLine). The shell's answer does follow Folder Redirection, as
// the variable does.
std::filesystem::path AppDataBase(REFKNOWNFOLDERID folder) {
    std::filesystem::path base;
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &path))) {
        base = path;
    }
    // Freed whether or not the call succeeded, as documented; null is fine.
    CoTaskMemFree(path);
    if (base.empty()) {
        base = std::filesystem::current_path();
    }
    // Capitalized because %APPDATA% is somewhere people actually browse,
    // and the name is a proper noun there.
    return base / "Spickzettel";
}
}  // namespace

// With a data folder, everything is in it - the former library place too,
// which is then the library's own, so that there is nothing to move.
std::filesystem::path Win32PlatformHost::GetConfigFilePath() const {
    return (dataDir_.empty() ? AppDataBase(FOLDERID_RoamingAppData) : dataDir_) / "config.json";
}

// Local, not roaming: a roaming profile copies %APPDATA% at every sign-in
// and sign-out, and Folder Redirection can put it on a server share -
// neither of which a library of screenshots, placed on this computer's
// displays, is for. %LOCALAPPDATA% cannot be redirected. See
// docs/ARCHITECTURE.md, "Persistence".
std::filesystem::path Win32PlatformHost::GetLibraryPath() const {
    return (dataDir_.empty() ? AppDataBase(FOLDERID_LocalAppData) : dataDir_) / "library.db";
}

std::filesystem::path Win32PlatformHost::GetFormerLibraryPath() const {
    return (dataDir_.empty() ? AppDataBase(FOLDERID_RoamingAppData) : dataDir_) / "library.db";
}

// Until Quit, which may come before the loop starts - a close while a
// startup message box is up (see main_win32.cpp) - and is kept.
//
// The wide calls, for the wide overlay window: through the ANSI ones a
// WM_CHAR went to a code-page byte and back, one UTF-16 unit at a time,
// and half of a surrogate pair is no character of any code page - an
// emoji typed arrived as two question marks.
int Win32PlatformHost::RunEventLoop() {
    MSG msg;
    while (!quitting_) {
        if (overlayWindow_.IsVisible()) {
            bool dispatched = false;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    quitting_ = true;
                    break;
                }
                if (!SkipCharacterTranslation(msg)) {
                    TranslateMessage(&msg);
                }
                DispatchMessageW(&msg);
                dispatched = true;
            }
            if (!quitting_) {
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
            const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
            if (result <= 0) {
                quitting_ = true;
                break;
            }
            if (!SkipCharacterTranslation(msg)) {
                TranslateMessage(&msg);
            }
            DispatchMessageW(&msg);
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

// A message besides the flag, to wake the loop: hidden, it waits in
// GetMessage, which handles a *sent* message inside itself and returns
// only for a posted one. A close that arrived sent - WM_CLOSE, or the
// Restart Manager's WM_ENDSESSION - settled and set the flag, and the app
// stayed until something unrelated was posted: past the Restart Manager's
// wait, which then asks for a restart or kills it. Not WM_QUIT, which
// would also end a message box that happens to be up.
void Win32PlatformHost::Quit(int exitCode) {
    exitCode_ = exitCode;
    quitting_ = true;
    PostMessageW(hwnd_, WM_NULL, 0, 0);
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

void Win32PlatformHost::SetOpenedAgainCallback(std::function<void()> callback) {
    openedAgainCallback_ = std::move(callback);
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
        if (trayIconWanted_) {
            RemoveTrayIcon();
            ShowTrayIcon();
        }
        return 0;
    }
    // The app started again - see PassOpeningToRunningCopy.
    if (msg == openedAgainMessage_ && msg != 0) {
        if (openedAgainCallback_) {
            openedAgainCallback_();
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
                // Called on a copy, as a hotkey's is: the callback can set
                // the timer again - the config retry does, when it saves -
                // and the one held here is destroyed then.
                const std::function<void()> callback = backgroundTimerCallback_;
                callback();
            }
            if (wParam == kTrayRetryTimerId && trayIconWanted_) {
                AddTrayIcon();
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

std::unique_ptr<IPlatformHost> CreatePlatformHost(std::filesystem::path dataDir) {
    return std::make_unique<win32::Win32PlatformHost>(std::move(dataDir));
}

}  // namespace sz::platform
