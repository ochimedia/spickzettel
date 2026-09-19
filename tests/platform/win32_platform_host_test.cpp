#include "platform/win32/win32_platform_host.h"

#include <string>

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

}  // namespace
}  // namespace sz::platform::win32
