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
#include "support/session_test_access.h"

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
        // in ui/view/settings_page.cpp, and assets/ui_strings.json for where
        // the words live now. A test that reached for "Appearance" broke the
        // moment anyone reworded it, which is the coupling this removed.
        for (const char* section : {"sectionappearance", "sectioninteraction", "sectionbehavior", "sectiondefaults",
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
        ctx->ItemClick("**/###sectionbehavior");
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
    EXPECT_NE(AppSettings().Base().freezeScreen, DefaultConfig().profileable.freezeScreen);
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

// What the display latch was for, before the overlay's move came after the
// frame: a frozen screen taken again on the new monitor lets go of the old
// one, and no frame draws the texture that went with it (docs/SETTINGS.md,
// C8). The fake window lets go of a texture at once, so a release in the
// middle of a frame shows here - see the test below.
TEST_F(UiTest, PickingAMonitorWhileFrozenDrawsNoTextureThatIsGone) {
    AppConfig config = DefaultConfig();
    config.profileable.freezeScreen = true;
    StartWith(config);
    host_.displays.insert(host_.displays.begin(),
                          platform::DisplayInfo{"fake-left", "Left Display", -2560, 0, 2560, 1440, false, 60, 100});
    host_.overlayWindow.captureReturnsWidth = static_cast<int>(kDisplayWidth);
    host_.overlayWindow.captureReturnsHeight = static_cast<int>(kDisplayHeight);
    host_.overlayWindow.captureReturnsPixelsRGBA.assign(static_cast<size_t>(kDisplayWidth * kDisplayHeight) * 4, 255);
    host_.overlayWindow.uploadsSucceed = true;
    int undrawable = 0;
    afterRender_ = [&] {
        for (const uint64_t texture : TexturesDrawn()) {
            undrawable += host_.overlayWindow.IsDrawable(texture) ? 0 : 1;
        }
    };
    ShowEditMode();
    StepFrame();
    ASSERT_EQ(host_.overlayWindow.captureCallCount, 1);

    OpenOverviewUi();
    RunUi("pick a monitor while frozen", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionappearance");
        ctx->SetRef(ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        ctx->ItemClick("##overlaydisplay");
        ctx->ItemClick("**/###display0");
    });
    StepFrame();

    EXPECT_EQ(host_.overlayWindow.onDisplay.id, "fake-left");
    EXPECT_EQ(host_.overlayWindow.captureCallCount, 2) << "taken again on the new monitor";
    EXPECT_FALSE(AppSettings().Previewing());
    EXPECT_EQ(undrawable, 0) << "a frame drew the old frozen screen after letting go of it";
    EXPECT_EQ(host_.overlayWindow.badTextureUses, 0);
}

// Switched off in Settings, the frozen screen is let go of after the frame
// (docs/SETTINGS.md, C9) - not from inside the Settings panel's draw, after
// the frame had queued it as its backdrop. On screen the D3D11 renderer
// holds a release made mid-frame until the frame is submitted, so that was
// never seen; the fake window lets go at once, which is what shows it here.
TEST_F(UiTest, SwitchingTheFrozenScreenOffLetsGoOfItAfterTheFrame) {
    AppConfig config = DefaultConfig();
    config.profileable.freezeScreen = true;
    StartWith(config);
    host_.overlayWindow.captureReturnsWidth = static_cast<int>(kDisplayWidth);
    host_.overlayWindow.captureReturnsHeight = static_cast<int>(kDisplayHeight);
    host_.overlayWindow.captureReturnsPixelsRGBA.assign(static_cast<size_t>(kDisplayWidth * kDisplayHeight) * 4, 255);
    host_.overlayWindow.uploadsSucceed = true;
    int undrawable = 0;
    afterRender_ = [&] {
        for (const uint64_t texture : TexturesDrawn()) {
            undrawable += host_.overlayWindow.IsDrawable(texture) ? 0 : 1;
        }
    };
    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(HoldsFrozenScreen(AppSession()));
    ASSERT_NE(controller_->GetSession().FrozenScreenTexture(), 0u);

    OpenOverviewUi();
    RunUi("switch the frozen screen off", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionbehavior");
        ctx->ItemClick("**/###freezescreen");
    });

    EXPECT_FALSE(AppSettings().Live().freezeScreen);
    EXPECT_FALSE(HoldsFrozenScreen(AppSession()));
    EXPECT_EQ(undrawable, 0) << "a frame drew the frozen screen after letting go of it";
    EXPECT_EQ(host_.overlayWindow.badTextureUses, 0);
}

// The interface size is a dropdown like the monitor's, and what is picked
// in it is what the next frame is drawn at - over what Windows says.
TEST_F(UiTest, PickingAnInterfaceSizeDrawsAtIt) {
    host_.overlayWindow.scalePercent = 125;
    ShowEditMode();
    StepFrame();
    ASSERT_FLOAT_EQ(UiScale(), 1.25f);
    OpenOverviewUi();
    RunUi("pick an interface size", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionappearance");
        ctx->SetRef(ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        ctx->ItemClick("##uiscale");
        ctx->ItemClick("**/###uiscale200");
    });
    StepFrame();

    EXPECT_EQ(AppSettings().Stored().uiScalePercent, 200);
    EXPECT_FLOAT_EQ(UiScale(), 2.0f);
}

// Every section, and every tab, reached by clicking at a large scale - so
// what is drawn bigger is also where the clicks land, which a size scaled in
// one place and not the other would break.
TEST_F(UiTest, TheSettingsSectionsAreAllReachableAtALargeScale) {
    host_.overlayWindow.scalePercent = 200;
    ShowEditMode();
    StepFrame();
    ASSERT_FLOAT_EQ(UiScale(), 2.0f);
    OpenOverviewUi();
    RunUi("settings sections at 200%", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        for (const char* section : {"sectionappearance", "sectioninteraction", "sectionbehavior", "sectiondefaults",
                                     "sectionhotkeys", "sectionprofiles", "sectiondebug"}) {
            const std::string path = std::string("**/###") + section;
            ctx->ItemClick(path.c_str());
            ctx->Yield();
        }
        ctx->ItemClick("**/###overviewtababout");
        ctx->ItemClick("**/###overviewtabcanvases");
    });
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
        ctx->ItemClick("**/###sectionbehavior");
        ctx->ItemClick("**/###freezescreen");
    });

    // The defaults are untouched and the profile now states it for itself -
    // the whole point of the override model.
    ASSERT_FALSE(AppSettings().Profiles()[0].overrides.Empty());
    ASSERT_TRUE(AppSettings().Profiles()[0].overrides.freezeScreen.has_value());
    EXPECT_NE(*AppSettings().Profiles()[0].overrides.freezeScreen, DefaultConfig().profileable.freezeScreen);
    EXPECT_EQ(AppSettings().Base().freezeScreen, DefaultConfig().profileable.freezeScreen);
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

// The color tile is a swatch rather than an icon, and was the one tile
// that could not say whether it was on - it wears the same pill as the
// rest now (PillSwatchButton), and it switches like the rest.
TEST_F(UiTest, TheColorTileSwitchesOffLikeEveryOtherButton) {
    ShowEditMode();
    StepFrame();

    OpenOverviewUi();
    RunUi("switch the color off", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninteraction");
        ctx->ItemClick("**/##tilecolor");
    });

    const BarButtonList& bar = AppSettings().Stored().drawingBar;
    const auto color = std::find_if(bar.begin(), bar.end(),
                                      [](const BarButtonSetting& entry) { return entry.button == ChromeButton::Color; });
    ASSERT_NE(color, bar.end());
    EXPECT_FALSE(color->shown);
    EXPECT_EQ(AppSettings().Stored().snippetBar, DefaultSnippetBar()) << "the other row is untouched";
}

// Stroke rendering is a row of the Pen group now, and still chooses.
TEST_F(UiTest, StrokeRenderingIsChosenInThePenGroup) {
    ShowEditMode();
    StepFrame();
    ASSERT_EQ(AppSettings().Stored().strokeRenderMode, StrokeRenderMode::Tessellated);

    OpenOverviewUi();
    RunUi("pick polyline", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninteraction");
        ctx->ItemClick("**/###strokemodepoly");
    });
    EXPECT_EQ(AppSettings().Stored().strokeRenderMode, StrokeRenderMode::Polyline);
}

// A shortcut row takes a mouse button - here the first side button - as
// readily as a key.
TEST_F(UiTest, AShortcutRowTakesAMouseButton) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("open the hotkeys", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionhotkeys");
    });
    controller_->Overlay().ArmShortcutCapture(ShortcutAction::Copy);
    // As the window hands it on: to ImGui, and into the input stream, whose
    // it is - a row waits on the stream (see KeyCapture).
    MouseButtonEvent(3, true);
    StepFrame();
    MouseButtonEvent(3, false);
    StepFrame();
    EXPECT_FALSE(App().IsCapturingShortcut());
    EXPECT_EQ(AppSettings().Stored().profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Copy)],
              (platform::KeyCombo{false, false, false, platform::KeyCombo::kX1Button}));
}

// The retention period's row: the days are there but disabled while the
// switch is off, and once it is on they step a day at a time - both saved
// as settings are.
TEST_F(UiTest, TheRetentionPeriodIsSwitchedOnAndItsDaysSet) {
    controller_->GetSettings().Set(setting::kPurgeDeleted, false);  // on by default
    ShowEditMode();
    StepFrame();
    const int days = AppSettings().Stored().purgeDeletedAfterDays;

    OpenOverviewUi();
    bool disabledWhileOff = false;
    RunUi("switch retention on and add a day", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionbehavior");
        disabledWhileOff = (ctx->ItemInfo("**/##purgedeleteddays").ItemFlags & ImGuiItemFlags_Disabled) != 0;
        ctx->ItemClick("**/###purgedeleted");
        ctx->ItemClick("**/##purgedeleteddays/+");
    });
    EXPECT_TRUE(disabledWhileOff);
    EXPECT_TRUE(AppSettings().Stored().purgeDeleted);
    EXPECT_EQ(AppSettings().Stored().purgeDeletedAfterDays, days + 1);
}

// Ctrl+Click turns a slider into a text field, and ImGui takes what is
// typed there as it is unless told to clamp. A border 5000 px wide covered
// the screen, and a negative text size reached the font code.
TEST_F(UiTest, AValueTypedIntoASliderIsHeldToItsRange) {
    controller_->GetSettings().Set(setting::kShowEditModeBorder, true);
    ShowEditMode();
    StepFrame();

    OpenOverviewUi();
    RunUi("type values past the ends of two sliders", [&](ImGuiTestContext* ctx) {
        // The slider's id as ImGui makes it, in the settings body: the
        // engine hashes a path that starts with ### differently.
        const auto inBody = [ctx](const char* id) -> ImGuiTestRef {
            return ImHashStr(id, 0, ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        };
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionappearance");
        ctx->ItemInputValue(inBody("###editborderwidth"), 5000.0f);
        ctx->ItemClick("**/###sectiondefaults");
        ctx->ItemInputValue(inBody("###defaulttextsize"), -40.0f);
    });
    EXPECT_FLOAT_EQ(AppSettings().Stored().editModeBorderWidthPx, kEditModeBorderWidthMax);
    EXPECT_FLOAT_EQ(AppSettings().Stored().noteTextSizePx, kNoteTextSizeMin);
}

// A slider is committed when ImGui says it was let go of, and that is said
// only in a frame that draws it. Taken out of edit mode in the middle of a
// drag - to view mode here, which closes the panel - the overlay draws the
// slider no more: the dragged value was shown, since it is stored as it
// moves, and never reached the file. The overlay settling commits it (C7).
// Put away to hidden is the same in the app, where a hidden overlay draws
// nothing; this harness goes on drawing frames while hidden, so the mode
// switch is the case it can show.
TEST_F(UiTest, ADragCutShortByLeavingEditModeIsCommitted) {
    controller_->GetSettings().Set(setting::kShowEditModeBorder, true);
    ShowEditMode();
    StepFrame();
    const float before = AppSettings().Stored().editModeBorderOpacity;

    OpenOverviewUi();
    RunUi("start dragging a slider", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionappearance");
        const ImGuiID body = ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID;
        ctx->MouseMove(ImHashStr("###editborderopacity", 0, body));
        ctx->MouseDown(ImGuiMouseButton_Left);
        const ImVec2 at = ImGui::GetIO().MousePos;
        ctx->MouseMoveToPos(ImVec2(at.x - 60.0f, at.y));
        ctx->Yield(2);
        IM_CHECK(AppSettings().Stored().editModeBorderOpacity != before);  // shown as it moves
        IM_CHECK(AppSettings().Previewing());
        // View mode's hotkey, the button still down: edit mode left in place.
        ShowViewMode();
        ctx->Yield(3);
        ctx->MouseUp(ImGuiMouseButton_Left);
        ctx->Yield(2);
    });
    ASSERT_TRUE(App().IsViewOnly());
    ASSERT_FALSE(App().IsOverviewOpen()) << "the slider is not drawn again";
    EXPECT_FALSE(AppSettings().Previewing()) << "committed as the overlay settled";
    EXPECT_NE(AppSettings().Stored().editModeBorderOpacity, before);
}

// A bar's buttons are reordered by dragging them along their own row. One
// dropped on the other bar's row moves nothing there - it carried only its
// place in its own row, and moved whatever sat at that place in the other.
TEST_F(UiTest, ABarButtonDraggedOntoTheOtherBarMovesNothingThere) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("drag bar buttons within and across the rows", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninteraction");
        const ImGuiID body = ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID;
        const auto tile = [body](const char* row, const char* key) -> ImGuiTestRef {
            return ImHashStr(key, 0, ImHashStr(row, 0, body));
        };
        ctx->ItemDragAndDrop(tile("snippetbar", "##tilepin"), tile("drawingbar", "##tiletext"));
        ctx->ItemDragAndDrop(tile("snippetbar", "##tilepin"), tile("snippetbar", "##tileclose"));
    });
    const AppConfig& stored = AppSettings().Stored();
    ASSERT_EQ(stored.drawingBar.size(), 4u);
    EXPECT_EQ(stored.drawingBar[0].button, ChromeButton::Pen) << "the drawing bar as it was";
    EXPECT_EQ(stored.drawingBar[2].button, ChromeButton::Text);
    ASSERT_EQ(stored.snippetBar.size(), 5u);
    EXPECT_EQ(stored.snippetBar[4].button, ChromeButton::Pin) << "and its own row still reorders";
}

// Settings edits one profile at a time, picked by where it is in the list.
// Deleting one above it moves it up a place, and what is edited next still
// goes to it; deleting it leaves nothing picked, not the one after it.
TEST_F(UiTest, DeletingAProfileKeepsTheEditsOnTheProfilePicked) {
    AppConfig config = DefaultConfig();
    for (const char* name : {"A", "B", "C"}) {
        Profile profile;
        profile.name = name;
        config.profiles.push_back(profile);
    }
    StartWith(config);
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();

    const auto body = [](ImGuiTestContext* ctx) {
        return ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID;
    };
    // Deletes the profile at `index` and then switches the freeze on or off
    // for whatever is being edited.
    const auto deleteThenToggle = [&](int index) {
        RunUi("delete a profile, then change a setting", [&](ImGuiTestContext* ctx) {
            ctx->SetRef("//##overview_panel");
            ctx->ItemClick("**/###sectionprofiles");
            const std::string row = "##delprofile" + std::to_string(index);
            ctx->SetRef(body(ctx));
            ctx->ItemClick(("**/" + row).c_str());
            ctx->SetRef("//##overview_panel");
            ctx->ItemClick("**/###sectionbehavior");
            ctx->ItemClick("**/###freezescreen");
        });
    };

    RunUi("edit B", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionbehavior");
        ctx->SetRef(body(ctx));
        ctx->ItemClick("##edittarget");
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("$$1/##target");  // B
    });

    deleteThenToggle(0);
    ASSERT_EQ(AppSettings().Profiles().size(), 2u);
    EXPECT_EQ(AppSettings().Profiles()[0].name, "B");
    EXPECT_FALSE(AppSettings().Profiles()[0].overrides.Empty()) << "B was edited";
    EXPECT_TRUE(AppSettings().Profiles()[1].overrides.Empty()) << "C was not";

    const bool freezeBefore = AppSettings().Base().freezeScreen;
    deleteThenToggle(0);
    ASSERT_EQ(AppSettings().Profiles().size(), 1u);
    EXPECT_TRUE(AppSettings().Profiles()[0].overrides.Empty()) << "C was not edited";
    EXPECT_NE(AppSettings().Base().freezeScreen, freezeBefore) << "the defaults were";
}

// The profile to edit is picked by its place in the list, not by its
// name: two names that end in the same "###" once gave both rows one id,
// and the second could not be picked at all.
TEST_F(UiTest, TwoProfilesWhoseNamesEndAlikeCanEachBePicked) {
    AppConfig config = DefaultConfig();
    for (const char* name : {"First###same", "Second###same"}) {
        Profile profile;
        profile.name = name;
        config.profiles.push_back(profile);
    }
    StartWith(config);
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("edit the second, and change a setting", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionbehavior");
        ctx->SetRef(ctx->WindowInfo("//##overview_panel/##overview_body/##settings_body").ID);
        ctx->ItemClick("##edittarget");
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("$$1/##target");
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###freezescreen");
    });
    EXPECT_TRUE(AppSettings().Profiles()[0].overrides.Empty());
    EXPECT_FALSE(AppSettings().Profiles()[1].overrides.Empty()) << "the second was edited";
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

// The licenses of everything compiled in have to be reachable from the
// app, not only from the repo - that is what MIT, ISC and the OFL each ask
// for. Behind one button rather than in a tab of its own, so this is the
// test that the button is there and that the text actually arrives.
TEST_F(UiTest, TheThirdPartyLicensesAreReachableFromAbout) {
    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("third-party licenses", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtababout");
        ctx->ItemClick("**/###noticesopen");
        ctx->Yield();
        // ...and back out again, so the page isn't a one-way trip.
        ctx->ItemClick("**/###noticesback");
    });

    // Every component that ends up inside the binary names itself in there.
    const std::string_view notices = build::NoticesText();
    for (const char* component : {"Dear ImGui", "SQLite", "nlohmann/json", "QOI", "Manrope", "Lucide"}) {
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
        ctx->ItemClick("**/###sectionbehavior");
        ctx->ItemClick("**/##help_dontstealfocus");

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

// Switching a parent off grays its dependents without clearing them - the
// state the tree draws as a dash rather than a check mark, and the reason those
// rows are grayed rather than hidden: they keep what they were set to.
TEST_F(UiTest, TurningAParentOffLeavesItsDependentsSetButUnavailable) {
    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(AppSettings().Base().dontStealFocus);
    ASSERT_TRUE(AppSettings().Base().rawMouseInput);

    OpenOverviewUi();
    RunUi("dependent rows", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionbehavior");
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
// pointer off grays out nothing below it.
TEST_F(UiTest, TheDrawnPointerIsNotAPreconditionOfTakingTheMouse) {
    ShowEditMode();
    StepFrame();

    OpenOverviewUi();
    RunUi("pointer independence", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionbehavior");
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

// Another profile's name is refused as it is typed, and the profile keeps
// its own: two rows named alike could not be told apart in the picker.
TEST_F(UiTest, AProfileIsNotRenamedToANameAnotherHas) {
    AppConfig config = DefaultConfig();
    for (const char* name : {"Test Game", "Other"}) {
        Profile profile;
        profile.name = name;
        config.profiles.push_back(profile);
    }
    StartWith(config);

    ShowEditMode();
    StepFrame();
    OpenOverviewUi();
    RunUi("rename to a taken name", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionprofiles");
        const ImGuiTestItemInfo row = ctx->ItemInfo("**/Test Game");
        IM_CHECK(row.ID != 0);
        ctx->SetRef(row.ID);
        ctx->ItemOpen(row.ID);
        ctx->ItemInputValue("##name", "Other");
    });

    ASSERT_EQ(AppSettings().Profiles().size(), 2u);
    EXPECT_EQ(AppSettings().Profiles()[0].name, "Test Game");
    EXPECT_EQ(AppSettings().Profiles()[1].name, "Other");
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
