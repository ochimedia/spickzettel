#include "platform/win32/win32_displays.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <set>
#include <string>

namespace sz::platform::win32 {
namespace {

// Against whatever is attached to the machine running it, so only what every
// machine with a screen must satisfy. Printed as well, since what Windows
// actually reports for a given monitor is the thing worth looking at.
TEST(Win32DisplaysTest, EveryDisplayIsListedOnceWithOnePrimaryAtTheOrigin) {
    const std::vector<DisplayInfo> displays = EnumerateDisplays();
    ASSERT_FALSE(displays.empty());

    int primaries = 0;
    std::set<std::string> ids;
    for (const DisplayInfo& display : displays) {
        std::printf("  %s '%s' at (%d,%d) %dx%d%s, %d Hz, %d%%\n", display.id.c_str(), display.name.c_str(),
                    display.x, display.y, display.width, display.height, display.primary ? " primary" : "",
                    display.refreshHz, display.scalePercent);
        EXPECT_FALSE(display.id.empty());
        EXPECT_FALSE(display.name.empty());
        EXPECT_GT(display.width, 0);
        EXPECT_GT(display.height, 0);
        ids.insert(display.id);
        if (display.primary) {
            ++primaries;
            EXPECT_EQ(display.x, 0);
            EXPECT_EQ(display.y, 0);
        }
    }
    EXPECT_EQ(primaries, 1);
    EXPECT_EQ(ids.size(), displays.size()) << "two displays share an id";
}

}  // namespace
}  // namespace sz::platform::win32
