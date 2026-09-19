#include "core/config/display_choice.h"

#include <gtest/gtest.h>

namespace sz::core {
namespace {

using platform::DisplayInfo;

// The two displays on the machine this was written on: a primary on the
// right and a second one to the left of it, at negative coordinates.
std::vector<DisplayInfo> TwoDisplays() {
    return {
        DisplayInfo{"path-left", "MACROSILICON", -1920, 0, 1920, 1080, false, 60, 100},
        DisplayInfo{"path-main", "24G1WG4", 0, 0, 1920, 1080, true, 120, 100},
    };
}

TEST(DisplayChoiceTest, NoChoiceIsThePrimaryDisplay) {
    EXPECT_EQ(ChooseDisplay(TwoDisplays(), "", "").id, "path-main");
}

TEST(DisplayChoiceTest, AChosenDisplayIsFoundByItsId) {
    EXPECT_EQ(ChooseDisplay(TwoDisplays(), "path-left", "MACROSILICON").x, -1920);
}

TEST(DisplayChoiceTest, AMonitorOnAnotherPortIsFoundByItsName) {
    EXPECT_EQ(ChooseDisplay(TwoDisplays(), "path-left-old-port", "MACROSILICON").id, "path-left");
}

TEST(DisplayChoiceTest, TwoMonitorsWithTheSameNameAreNotGuessedBetween) {
    std::vector<DisplayInfo> displays = TwoDisplays();
    displays.push_back(DisplayInfo{"path-right", "MACROSILICON", 1920, 0, 1920, 1080, false, 60, 100});
    EXPECT_EQ(ChooseDisplay(displays, "path-gone", "MACROSILICON").id, "path-main");
}

TEST(DisplayChoiceTest, ADisconnectedDisplayFallsBackToThePrimary) {
    EXPECT_EQ(ChooseDisplay(TwoDisplays(), "path-gone", "Some Monitor").id, "path-main");
}

TEST(DisplayChoiceTest, WithNoPrimaryFlaggedTheFirstDisplayIsTheFallback) {
    std::vector<DisplayInfo> displays = TwoDisplays();
    displays[1].primary = false;
    EXPECT_EQ(ChooseDisplay(displays, "", "").id, "path-left");
}

TEST(DisplayChoiceTest, NothingAttachedStillGivesAWindowSomewhereToGo) {
    const DisplayInfo display = ChooseDisplay({}, "path-main", "24G1WG4");
    EXPECT_EQ(display.x, 0);
    EXPECT_EQ(display.y, 0);
    EXPECT_GT(display.width, 0);
    EXPECT_GT(display.height, 0);
}

}  // namespace
}  // namespace sz::core
