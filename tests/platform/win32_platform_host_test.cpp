#include "platform/win32/win32_platform_host.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <windows.h>

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

    EXPECT_EQ(SendMessageA(hwnd, WM_QUERYENDSESSION, 0, 0), TRUE) << "fine by us, once the flush has run";
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
    EXPECT_EQ(sessionEnds, 1) << "the exit's own flush, not another before it";
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
    ASSERT_NE(id, 0) << "Ctrl+Alt+Shift+F24 taken by something else";

    SendMessageA(FindWindowA(nullptr, name.c_str()), WM_HOTKEY, static_cast<WPARAM>(id), 0);

    EXPECT_EQ(ranToTheEnd, word);
}

}  // namespace
}  // namespace sz::platform::win32
