#include "platform/win32/win32_platform_host.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <windows.h>

#include <sddl.h>
#include <shellapi.h>

namespace sz::platform::win32 {
namespace {

// Logoff and shutdown are a broadcast to every top-level window, and a
// message-only window (HWND_MESSAGE) is not on that list - so the host has
// to be reachable by the route Windows actually takes. FindWindow lists
// only top-level windows too, which is what makes it the right way to
// look for it here.
TEST(Win32PlatformHostTest, TheHostWindowIsTopLevelAndAnswersASessionEnd) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    int sessionEnds = 0;
    host.SetSessionEndCallback([&sessionEnds] { ++sessionEnds; });

    const HWND hwnd = FindWindowA(nullptr, name.c_str());
    ASSERT_NE(hwnd, nullptr) << "not a top-level window, so not one a session end reaches";
    EXPECT_FALSE(IsWindowVisible(hwnd)) << "top-level, but never on screen";

    EXPECT_EQ(SendMessageA(hwnd, WM_QUERYENDSESSION, 0, 0), TRUE) << "fine by us, once the settle has run";
    EXPECT_EQ(sessionEnds, 1);
    SendMessageA(hwnd, WM_ENDSESSION, TRUE, 0);
    EXPECT_EQ(sessionEnds, 2) << "and again when it is decided";
    SendMessageA(hwnd, WM_ENDSESSION, FALSE, 0);  // called off after all
    EXPECT_EQ(sessionEnds, 2);
}

// A close asked from outside - WM_CLOSE, as taskkill without /f sends, or
// the Restart Manager's close-app - exits the way the tray menu does, and
// leaves the window in place until then rather than destroying it.
TEST(Win32PlatformHostTest, ACloseFromOutsideExitsAsTheTrayMenuDoes) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    int exits = 0;
    host.SetTrayCommandCallback([&exits](TrayCommand command) {
        if (command == TrayCommand::Exit) {
            ++exits;
        }
    });
    int sessionEnds = 0;
    host.SetSessionEndCallback([&sessionEnds] { ++sessionEnds; });
    const HWND hwnd = FindWindowA(nullptr, name.c_str());
    ASSERT_NE(hwnd, nullptr);

    SendMessageA(hwnd, WM_CLOSE, 0, 0);
    EXPECT_EQ(exits, 1);
    EXPECT_TRUE(IsWindow(hwnd)) << "not destroyed with the tray icon and hotkeys still on it";

    SendMessageA(hwnd, WM_ENDSESSION, TRUE, 0);
    EXPECT_EQ(exits, 1) << "a logoff ends the process itself";
    EXPECT_EQ(sessionEnds, 1);
    SendMessageA(hwnd, WM_ENDSESSION, TRUE, ENDSESSION_CLOSEAPP);
    EXPECT_EQ(exits, 2) << "the Restart Manager waits for it to go";
    EXPECT_EQ(sessionEnds, 1) << "the exit's own settle, not another before it";
}

// The same close sent to the overlay, which is the window taskkill finds
// while the overlay is up: the app exits, and the overlay is not destroyed.
TEST(Win32PlatformHostTest, ACloseSentToTheOverlayExitsToo) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    int exits = 0;
    host.SetTrayCommandCallback([&exits](TrayCommand command) {
        if (command == TrayCommand::Exit) {
            ++exits;
        }
    });
    const std::vector<DisplayInfo> displays = host.ListDisplays();
    ASSERT_FALSE(displays.empty());
    ASSERT_TRUE(host.GetOverlayWindow().EnsureCreated(displays[0]));
    HWND overlay = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hwnd, LPARAM found) {
            char title[64] = {};
            GetWindowTextA(hwnd, title, sizeof(title));
            if (std::string(title) == "Spickzettel Overlay") {
                *reinterpret_cast<HWND*>(found) = hwnd;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&overlay));
    ASSERT_NE(overlay, nullptr);

    SendMessageA(overlay, WM_CLOSE, 0, 0);
    EXPECT_EQ(exits, 1);
    EXPECT_TRUE(IsWindow(overlay));
    SendMessageA(overlay, WM_SYSCOMMAND, SC_CLOSE, 0);
    EXPECT_EQ(exits, 1) << "Alt+F4 over the overlay is not a way out";
}

// Explorer restarting takes every tray icon with it, and says so to every
// top-level window once the new taskbar is up: "TaskbarCreated". The icon
// is put back then; it was gone, and the menu's Exit with it, until the
// app was restarted.
TEST(Win32PlatformHostTest, TheTrayIconIsPutBackWhenExplorerRestarts) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    if (!host.ShowTrayIcon()) {
        GTEST_SKIP() << "no taskbar to put an icon on";
    }
    const HWND hwnd = FindWindowA(nullptr, name.c_str());
    ASSERT_NE(hwnd, nullptr);
    NOTIFYICONDATAA icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = hwnd;
    icon.uID = kTrayIconId;
    // A change to no field of it, which the shell takes only for an icon
    // it has.
    const auto shown = [&icon] { return Shell_NotifyIconA(NIM_MODIFY, &icon) != FALSE; };
    ASSERT_TRUE(shown());
    const UINT taskbarCreated = RegisterWindowMessageA("TaskbarCreated");

    // What a restart leaves: the icon gone, not by the host's hand.
    ASSERT_TRUE(Shell_NotifyIconA(NIM_DELETE, &icon));
    ASSERT_FALSE(shown());
    SendMessageA(hwnd, taskbarCreated, 0, 0);
    EXPECT_TRUE(shown()) << "put back";

    // A taskbar that kept it: still one.
    SendMessageA(hwnd, taskbarCreated, 0, 0);
    EXPECT_TRUE(shown());
}

// A tray icon the shell refuses - at log-on, ahead of the taskbar - goes
// up once the taskbar says it is there, or at the next try of a timer,
// for a taskbar that was only slow to answer. The failure ended the start.
TEST(Win32PlatformHostTest, ATrayIconTheShellRefusedGoesUpLater) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    if (!host.ShowTrayIcon()) {
        GTEST_SKIP() << "no taskbar to put an icon on";
    }
    host.RemoveTrayIcon();
    const HWND hwnd = FindWindowA(nullptr, name.c_str());
    ASSERT_NE(hwnd, nullptr);
    NOTIFYICONDATAA icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = hwnd;
    icon.uID = kTrayIconId;
    const auto shown = [&icon] { return Shell_NotifyIconA(NIM_MODIFY, &icon) != FALSE; };
    const UINT taskbarCreated = RegisterWindowMessageA("TaskbarCreated");

    host.FailTrayIconAddsForTesting(1);
    EXPECT_FALSE(host.ShowTrayIcon());
    EXPECT_FALSE(shown());
    SendMessageA(hwnd, taskbarCreated, 0, 0);
    EXPECT_TRUE(shown()) << "when the taskbar comes";

    host.RemoveTrayIcon();
    host.FailTrayIconAddsForTesting(1);
    EXPECT_FALSE(host.ShowTrayIcon());
    SendMessageA(hwnd, WM_TIMER, kTrayRetryTimerId, 0);
    EXPECT_TRUE(shown()) << "at the next try";

    // Taken down, it stays down.
    host.RemoveTrayIcon();
    SendMessageA(hwnd, WM_TIMER, kTrayRetryTimerId, 0);
    SendMessageA(hwnd, taskbarCreated, 0, 0);
    EXPECT_FALSE(shown());
}

// A hotkey's callback may unregister that same hotkey while it runs - a
// capture that moves the combo to another hotkey does - and runs on to
// the end with what it captured intact.
TEST(Win32PlatformHostTest, AHotkeyCallbackCanUnregisterItsOwnHotkey) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    const KeyCombo combo{true, true, true, KeyCombo::kFunctionKeyBase + 24};
    int id = 0;
    std::string ranToTheEnd;
    const std::string word = "all of it, and well past what a small string holds in place";
    id = host.RegisterGlobalHotkey(combo, [&host, &id, &ranToTheEnd, word] {
        host.UnregisterGlobalHotkey(id);
        ranToTheEnd = word;
    });
    if (id == 0) {
        // Taken by something else - a second run of these tests at the same time.
        GTEST_SKIP() << "Ctrl+Alt+Shift+F24 taken by something else";
    }

    SendMessageA(FindWindowA(nullptr, name.c_str()), WM_HOTKEY, static_cast<WPARAM>(id), 0);

    EXPECT_EQ(ranToTheEnd, word);
}

// The background timer's callback may set the timer again while it runs
// - the config retry does, from the save it retries - and runs on to the
// end with what it captured intact.
TEST(Win32PlatformHostTest, ABackgroundTimerCallbackCanSetTheTimerAgain) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    std::string ranToTheEnd;
    const std::string word = "all of it, and well past what a small string holds in place";
    host.SetBackgroundTimer(60000, [&host, &ranToTheEnd, word] {
        host.SetBackgroundTimer(60000, [] {});
        ranToTheEnd = word;
    });

    SendMessageA(FindWindowA(nullptr, name.c_str()), WM_TIMER, kBackgroundTimerId, 0);

    EXPECT_EQ(ranToTheEnd, word);
    host.SetBackgroundTimer(0, nullptr);
}

// The loop hands a wide window its characters as they were posted: an
// emoji is two UTF-16 units, and a WM_CHAR through the ANSI calls went to
// a code-page byte and back one unit at a time - a lone half of the pair
// is in no code page, and arrived as a question mark.
std::vector<WPARAM> g_charsReceived;
LRESULT CALLBACK RecordCharsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CHAR) {
        g_charsReceived.push_back(wParam);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

TEST(Win32PlatformHostTest, TheLoopHandsAWideWindowTheCharactersAsPosted) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = RecordCharsProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"SpickzettelHostTestChars";
    RegisterClassExW(&windowClass);
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                        windowClass.hInstance, nullptr);
    ASSERT_NE(window, nullptr);
    ASSERT_TRUE(IsWindowUnicode(window));
    g_charsReceived.clear();

    // U+1F600, and a u with umlaut after it.
    const std::vector<WPARAM> posted = {0xD83D, 0xDE00, 0x00FC};
    for (const WPARAM unit : posted) {
        PostMessageW(window, WM_CHAR, unit, 1);
    }
    host.Post([&host] { host.Quit(0); });
    host.RunEventLoop();

    EXPECT_EQ(g_charsReceived, posted);
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
}

// A posted task waits for the message loop - it runs after whatever frame
// or message posted it, not inside it - and tasks run in the order they
// were posted, one posted by a task after those already waiting.
TEST(Win32PlatformHostTest, PostedTasksRunFromTheLoopInOrder) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    std::vector<int> ran;
    host.Post([&] { ran.push_back(1); });
    host.Post([&] {
        ran.push_back(2);
        host.Post([&] { ran.push_back(4); });
    });
    host.Post([&] { ran.push_back(3); });
    EXPECT_TRUE(ran.empty()) << "nothing runs where it is posted";

    MSG msg;
    for (int i = 0; i < 100 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
        DispatchMessageW(&msg);
    }

    EXPECT_EQ(ran, (std::vector<int>{1, 2, 3, 4}));
}

// Runs the host's loop while `from` runs on a thread of its own, and
// whether the loop ended by itself: past the `patience`, the thread posts
// a WM_QUIT, which ends any loop, so a loop that would not end fails the
// test rather than hanging it.
bool LoopEndsBy(Win32PlatformHost& host, int& exitCode, const std::function<void()>& from) {
    const DWORD loopThread = GetCurrentThreadId();
    std::atomic<bool> ended = false;
    std::atomic<bool> forced = false;
    std::thread other([&] {
        from();
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ended && std::chrono::steady_clock::now() < giveUp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!ended) {
            forced = true;
            PostThreadMessageW(loopThread, WM_QUIT, 0, 0);
        }
    });
    exitCode = host.RunEventLoop();
    ended = true;
    other.join();
    return !forced;
}

// The Restart Manager's close, and taskkill's, arrive *sent*, while the
// app sits hidden in the tray - the loop waiting in GetMessage, which
// handles a sent message inside itself and returns only for a posted
// one. The exit settled and set the flag, and the loop went on waiting,
// past the Restart Manager's patience.
TEST(Win32PlatformHostTest, ACloseSentWhileHiddenEndsTheLoop) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    for (const auto& [message, lParam] : {std::pair<UINT, LPARAM>{WM_ENDSESSION, ENDSESSION_CLOSEAPP},
                                          std::pair<UINT, LPARAM>{WM_CLOSE, 0}}) {
        Win32PlatformHost host;
        ASSERT_TRUE(host.Initialize(name));
        host.SetTrayCommandCallback([&host](TrayCommand command) {
            if (command == TrayCommand::Exit) {
                host.Quit(3);
            }
        });
        const HWND hwnd = FindWindowA(nullptr, name.c_str());
        ASSERT_NE(hwnd, nullptr);
        int exitCode = -1;
        const WPARAM wParam = message == WM_ENDSESSION ? TRUE : 0;
        EXPECT_TRUE(LoopEndsBy(host, exitCode, [&] { SendMessageA(hwnd, message, wParam, lParam); }))
            << "message " << message;
        EXPECT_EQ(exitCode, 3);
    }
}

// A Quit before the loop starts - a close while a startup message box is
// up - is kept: the loop does not start. It was wiped by the loop setting
// itself running, and the app carried on.
TEST(Win32PlatformHostTest, AQuitBeforeTheLoopIsKept) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    host.Quit(7);
    int exitCode = -1;
    EXPECT_TRUE(LoopEndsBy(host, exitCode, [] {}));
    EXPECT_EQ(exitCode, 7);
}

// The instance is held for as long as the copy holding it is alive, and
// is there to be had once it is gone. The other copy is this test,
// holding the mutex itself.
TEST(Win32PlatformHostTest, TheInstanceIsRefusedWhileAnotherCopyHoldsIt) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    const HANDLE held = CreateMutexW(nullptr, TRUE, InstanceMutexName(name).c_str());
    ASSERT_NE(held, nullptr);
    ASSERT_NE(GetLastError(), static_cast<DWORD>(ERROR_ALREADY_EXISTS));

    EXPECT_FALSE(host.AcquireSingleInstance());
    CloseHandle(held);
    EXPECT_TRUE(host.AcquireSingleInstance()) << "gone with its last handle";
    EXPECT_TRUE(host.AcquireSingleInstance()) << "and asked again, answered from the one now held";
}

// A copy run as administrator makes the mutex for the Administrators group
// at high integrity, and the same user unelevated may not open it:
// CreateMutex fails with ERROR_ACCESS_DENIED instead of answering
// ERROR_ALREADY_EXISTS. That is still a copy holding the instance - the
// same user's, so the same library. Stood in for by a mutex whose DACL
// grants no one anything, which is refused the same way without an
// elevated process to make it.
TEST(Win32PlatformHostTest, AnInstanceThisCopyMayNotOpenIsHeldAllTheSame) {
    const std::string name = "SpickzettelHostTest-" + std::to_string(GetCurrentProcessId());
    Win32PlatformHost host;
    ASSERT_TRUE(host.Initialize(name));
    PSECURITY_DESCRIPTOR grantsNothing = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:", SDDL_REVISION_1, &grantsNothing, nullptr));
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), grantsNothing, FALSE};
    const HANDLE held = CreateMutexW(&attributes, TRUE, InstanceMutexName(name).c_str());
    LocalFree(grantsNothing);
    ASSERT_NE(held, nullptr);
    const HANDLE again = CreateMutexW(nullptr, TRUE, InstanceMutexName(name).c_str());
    ASSERT_EQ(again, nullptr) << "the stand-in is refused as an elevated copy's mutex is";
    ASSERT_EQ(GetLastError(), static_cast<DWORD>(ERROR_ACCESS_DENIED));

    EXPECT_FALSE(host.AcquireSingleInstance());
    CloseHandle(held);
    EXPECT_TRUE(host.AcquireSingleInstance());
}

}  // namespace
}  // namespace sz::platform::win32
