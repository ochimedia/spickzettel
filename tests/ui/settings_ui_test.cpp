// The Settings panel, driven by widget name.
//
// Without the test engine everything here has to be checked by launching
// the app, clicking at coordinates worked out from a screenshot, and looking
// at another screenshot.
#include <algorithm>
#include <string>
#include <string_view>

#include "core/build_info/build_info.h"

#include "fakes/ui_test.h"

namespace sz::test {
namespace {


// Where a window sits in ImGui's own display order - the list is front-to-
// back with the last entry on top, so "in front of" is ">".
int DisplayIndexOf(const ImGuiWindow* window) {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    for (int i = 0; i < g.Windows.Size; ++i) {
        if (g.Windows[i] == window) {
            return i;
        }
    }
    return -1;
}

TEST_F(UiTest, TheSettingsSectionsAreAllReachable) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("settings sections", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        // By id, not by the words on the button - see the SectionRow table
        // in overlay_app_overview.cpp, and assets/ui_strings.json for where
        // the words live now. A test that reached for "Appearance" broke the
        // moment anyone reworded it, which is the coupling this removed.
        for (const char* section : {"sectionappearance", "sectioninteraction", "sectioninput",
                                     "sectionhotkeys", "sectionprofiles", "sectiondebug"}) {
            const std::string path = std::string("**/###") + section;
            ctx->ItemClick(path.c_str());
            ctx->Yield();
        }
    });
}

// The regression this harness was built for: a dropdown nested inside the
// Overview panel opened *behind* it and read as a control that did
// nothing. Verified by putting the bug back (dropping the picker's own
// KeepPopoverInFront call) and watching this fail: the click lands on the
// panel covering the popup, so the pick silently doesn't take and the
// setting below goes to the wrong target - which is exactly what it looked
// like from the outside, and took a screenshot and a guess to find.
TEST_F(UiTest, TheEditTargetDropdownOpensInFrontOfThePanel) {
    AppConfig config = DefaultConfig();
    Profile profile;
    profile.name = "Test Game";
    profile.match.executables.push_back("game.exe");
    config.profiles.push_back(profile);
    StartWith(config);
    host_.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "Test Game"};

    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("edit target dropdown", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninput");
        // By path rather than by "**/" like the rows above: a combo
        // registers no label with the test engine (ImGui::BeginCombo makes
        // no IMGUI_TEST_ENGINE_ITEM_INFO call), so a label search cannot
        // see it at all. And the path has to go through WindowInfo,
        // because a child window's real id is a hash of the mangled name
        // ImGui gives it ("##overview_panel/##settings_body_1A2B3C4D")
        // rather than of the path that reads it - which is exactly what
        // WindowInfo exists to unmangle.
        //
        // Opening it is half the test; picking something out of it is the
        // half that proves the popup is on top and hit-testable rather
        // than merely present.
        ctx->SetRef(ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        ctx->ItemClick("##edittarget");
        ctx->ItemClick("**/###targetdefaults");
        // And what it says it is now editing is what it edits.
        ctx->ItemClick("**/###freezescreen");
    });

    // Which is the defaults, not the profile that is otherwise current.
    EXPECT_NE(AppSettings().Base().freezeScreen, DefaultConfig().freezeScreenInEditMode);
    EXPECT_TRUE(AppSettings().Profiles()[0].overrides.Empty());
}

// Same kind of dropdown as the one above, and so the same way to break -
// plus the one thing a model test cannot see: that what the list offers is
// the host's displays, and that picking one moves the overlay there.
TEST_F(UiTest, PickingAMonitorPutsTheOverlayOnIt) {
    host_.displays.insert(host_.displays.begin(),
                          platform::DisplayInfo{"fake-left", "Left Display", -2560, 0, 2560, 1440, false, 60, 100});

    ShowEditMode();
    StepFrame();
    ASSERT_EQ(host_.overlayWindow.onDisplay.id, "fake-primary");
    OpenOverviewUi();
    RunUi("pick a monitor", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionappearance");
        // By path, for the reason given in the test above.
        ctx->SetRef(ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        ctx->ItemClick("##overlaydisplay");
        // Listed left to right, so the display on the left comes first.
        ctx->ItemClick("**/###display0");
    });

    EXPECT_EQ(AppSettings().Stored().overlayDisplayId, "fake-left");
    EXPECT_EQ(AppSettings().Stored().overlayDisplayName, "Left Display");
    EXPECT_EQ(host_.overlayWindow.onDisplay.id, "fake-left");
}

TEST_F(UiTest, TogglingAnInputSettingWhileAProfileIsActiveLandsInTheProfile) {
    AppConfig config = DefaultConfig();
    Profile profile;
    profile.name = "Test Game";
    profile.match.executables.push_back("game.exe");
    config.profiles.push_back(profile);
    StartWith(config);
    host_.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "Test Game"};

    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(AppSettings().ActiveProfile().has_value());
    ASSERT_TRUE(AppSettings().Profiles()[0].overrides.Empty());

    OpenOverviewUi();
    RunUi("toggle freeze", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninput");
        ctx->ItemClick("**/###freezescreen");
    });

    // The defaults are untouched and the profile now states it for itself -
    // the whole point of the override model.
    ASSERT_FALSE(AppSettings().Profiles()[0].overrides.Empty());
    ASSERT_TRUE(AppSettings().Profiles()[0].overrides.freezeScreen.has_value());
    EXPECT_NE(*AppSettings().Profiles()[0].overrides.freezeScreen, DefaultConfig().freezeScreenInEditMode);
    EXPECT_EQ(AppSettings().Base().freezeScreen, DefaultConfig().freezeScreenInEditMode);
}

// The row of tiles in Settings > Interaction is the bar: clicking one
// switches that button off, and the bar over a snippet stops carrying it.
TEST_F(UiTest, ClickingAButtonInTheInteractionRowTakesItOffTheBar) {
    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(AppSettings().Stored().snippetBar.front().shown);

    OpenOverviewUi();
    RunUi("switch a bar button off", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninteraction");
        ctx->ItemClick("**/##tilepin");
    });

    const BarButtonList& bar = AppSettings().Stored().snippetBar;
    const auto pin = std::find_if(bar.begin(), bar.end(),
                                   [](const BarButtonSetting& entry) { return entry.button == ChromeButton::Pin; });
    ASSERT_NE(pin, bar.end()) << "switched off, not removed from the row";
    EXPECT_FALSE(pin->shown);
    // Every other one is untouched, and so is the drawing bar.
    EXPECT_EQ(std::count_if(bar.begin(), bar.end(), [](const BarButtonSetting& e) { return e.shown; }),
               static_cast<long>(kSnippetBarButtons.size()) - 1);
    EXPECT_EQ(AppSettings().Stored().drawingBar, DefaultDrawingBar());
}

// The colour tile is a swatch rather than an icon, and was the one tile
// that could not say whether it was on - it wears the same pill as the
// rest now (PillSwatchButton), and it switches like the rest.
TEST_F(UiTest, TheColourTileSwitchesOffLikeEveryOtherButton) {
    ShowEditMode();
    StepFrame();

    OpenOverviewUi();
    RunUi("switch the colour off", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninteraction");
        ctx->ItemClick("**/##tilecolour");
    });

    const BarButtonList& bar = AppSettings().Stored().drawingBar;
    const auto colour = std::find_if(bar.begin(), bar.end(),
                                      [](const BarButtonSetting& entry) { return entry.button == ChromeButton::Colour; });
    ASSERT_NE(colour, bar.end());
    EXPECT_FALSE(colour->shown);
    EXPECT_EQ(AppSettings().Stored().snippetBar, DefaultSnippetBar()) << "the other row is untouched";
}

TEST_F(UiTest, MakingAProfileForWhatIsUnderneathTakesOneClick) {
    host_.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "Test Game"};
    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(AppSettings().Profiles().empty());

    OpenOverviewUi();
    RunUi("make a profile", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionprofiles");
        ctx->ItemClick("**/###makeprofile");
    });

    ASSERT_EQ(AppSettings().Profiles().size(), 1u);
    EXPECT_EQ(AppSettings().Profiles()[0].name, "Test Game");
    ASSERT_EQ(AppSettings().Profiles()[0].match.executables.size(), 1u);
    EXPECT_EQ(AppSettings().Profiles()[0].match.executables[0], "game.exe");
    // And it matches at once, so the next thing changed lands in it.
    EXPECT_TRUE(AppSettings().ActiveProfile().has_value());
}

// The licences of everything compiled in have to be reachable from the
// app, not only from the repo - that is what MIT, ISC and the OFL each ask
// for. Behind one button rather than in a tab of its own, so this is the
// test that the button is there and that the text actually arrives.
TEST_F(UiTest, TheThirdPartyLicencesAreReachableFromAbout) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("third-party licences", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtababout");
        ctx->ItemClick("**/###noticesopen");
        ctx->Yield();
        // ...and back out again, so the page isn't a one-way trip.
        ctx->ItemClick("**/###noticesback");
    });

    // Every component that ends up inside the binary names itself in there.
    const std::string_view notices = build::NoticesText();
    for (const char* component : {"Dear ImGui", "nlohmann/json", "stb_image", "QOI", "Manrope", "Lucide"}) {
        EXPECT_NE(notices.find(component), std::string_view::npos)
            << component << " is compiled in but is not in THIRD-PARTY-NOTICES.md";
    }
}

// The explanations moved out of the panel and behind a "?" per row, which
// only works if the "?" is reachable and its popover lands in front - the
// same stacking the dropdown above depends on, and the same failure if it
// breaks (a button that visibly does nothing).
TEST_F(UiTest, AHelpMarkerOpensItsExplanationInFront) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("help popover", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninput");
        ctx->ItemClick("**/##help_freezescreen");

        ImGuiWindow* popover = ctx->WindowInfo("//$FOCUSED").Window;
        IM_CHECK(popover != nullptr);
        // In front of the panel that opened it, said in the terms ImGui
        // itself uses: g.Windows is in display order, and the last entry is
        // the one drawn on top - which is where BringWindowToDisplayFront
        // (KeepPopoverInFront) puts its target.
        IM_CHECK_GT(DisplayIndexOf(popover), DisplayIndexOf(ctx->WindowInfo("//##overview_panel").Window));
        ctx->KeyPress(ImGuiKey_Escape);
    });

    // Escape closed the popover, not the whole Overview behind it.
    EXPECT_TRUE(App().IsOverviewOpen());
}

// Switching a parent off greys its dependents without clearing them - the
// state the tree draws as a dash rather than a tick, and the reason those
// rows are greyed rather than hidden: they keep what they were set to.
TEST_F(UiTest, TurningAParentOffLeavesItsDependentsSetButUnavailable) {
    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(AppSettings().Base().dontStealFocus);
    ASSERT_TRUE(AppSettings().Base().rawMouseInput);

    OpenOverviewUi();
    RunUi("dependent rows", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninput");
        // Everything below it needs this one, directly or through the row
        // between them.
        ctx->ItemClick("**/###dontstealfocus");
        IM_CHECK((ctx->ItemInfo("**/###rawmouse").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        // The elevated exception included: with focus taken already there
        // is no case left for it to be the exception to.
        IM_CHECK((ctx->ItemInfo("**/###takefocusoverelevated").ItemFlags &
                   ImGuiItemFlags_Disabled) != 0);
        IM_CHECK((ctx->ItemInfo("**/###counterrawmouse").ItemFlags &
                   ImGuiItemFlags_Disabled) != 0);
        // Not these two: they are in the same list for the eye, not in
        // the tree, and they go on working with nothing taken from the game.
        IM_CHECK((ctx->ItemInfo("**/###softwarepointer").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((ctx->ItemInfo("**/###freezescreen").ItemFlags & ImGuiItemFlags_Disabled) ==
                  0);
    });

    EXPECT_FALSE(AppSettings().Base().dontStealFocus);
    // Kept, not cleared - which is what the dash on those rows means.
    EXPECT_TRUE(AppSettings().Base().rawMouseInput);
    EXPECT_TRUE(AppSettings().Base().takeFocusOverElevated);
}

// Which pointer is shown and whether the mouse is taken are separate
// questions: the grab keeps the position either way, so switching the drawn
// pointer off greys out nothing below it.
TEST_F(UiTest, TheDrawnPointerIsNotAPreconditionOfTakingTheMouse) {
    ShowEditMode();
    StepFrame();

    OpenOverviewUi();
    RunUi("pointer independence", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninput");
        ctx->ItemClick("**/###softwarepointer");
        IM_CHECK((ctx->ItemInfo("**/###rawmouse").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        IM_CHECK((ctx->ItemInfo("**/###counterrawmouse").ItemFlags &
                   ImGuiItemFlags_Disabled) == 0);

        // Countering does still need the mouse taken - see
        // EditModeInputOptions::CounterRawMouseInputCanBeUsed.
        ctx->ItemClick("**/###rawmouse");
        IM_CHECK((ctx->ItemInfo("**/###counterrawmouse").ItemFlags &
                   ImGuiItemFlags_Disabled) != 0);
    });

    EXPECT_FALSE(AppSettings().Base().softwarePointer);
    EXPECT_FALSE(AppSettings().Base().rawMouseInput);
}

// The Profiles list is names first and everything else behind one click,
// so the two have to be actually separate: nothing editable on a closed
// row, and the fields there and working on an open one.
TEST_F(UiTest, AProfileRowKeepsItsFieldsUntilItIsOpened) {
    AppConfig config = DefaultConfig();
    Profile profile;
    profile.name = "Test Game";
    profile.match.executables.push_back("game.exe");
    config.profiles.push_back(profile);
    StartWith(config);

    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("profile row", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionprofiles");
        // Found by the name it shows, which is the whole point of the list.
        // Its ID is not that name - a name-derived ID would change under
        // the very field that edits it - so everything below is addressed
        // relative to the row rather than by a path spelled out here.
        const ImGuiTestItemInfo row = ctx->ItemInfo("**/Test Game");
        IM_CHECK(row.ID != 0);
        ctx->SetRef(row.ID);
        IM_CHECK(ctx->ItemInfo("##name", ImGuiTestOpFlags_NoError).ID == 0);
        ctx->ItemOpen(row.ID);
        IM_CHECK(ctx->ItemInfo("##name").ID != 0);
        ctx->ItemInputValue("##name", "Renamed");
    });

    ASSERT_EQ(AppSettings().Profiles().size(), 1u);
    EXPECT_EQ(AppSettings().Profiles()[0].name, "Renamed");
    // Renaming is all that happened: what it matches is untouched, which is
    // the thing that would break if the row edited a copy at the wrong index.
    ASSERT_EQ(AppSettings().Profiles()[0].match.executables.size(), 1u);
    EXPECT_EQ(AppSettings().Profiles()[0].match.executables[0], "game.exe");
}

TEST_F(UiTest, TheOverviewMakesACanvasAndSwitchesToIt) {
    ShowEditMode();
    StepFrame();
    const CanvasId before = Canvases().CurrentCanvasId();

    OpenOverviewUi();
    RunUi("new canvas", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabcanvases");
        ctx->ItemClick("**/##newcanvas");
    });

    EXPECT_NE(Canvases().CurrentCanvasId(), before);
    EXPECT_TRUE(App().IsOverviewOpen());  // it stays up
}

}  // namespace
}  // namespace sz::test
