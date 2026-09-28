// The tutorial card's buttons, clicked by name - docs/TUTORIAL.md, section
// 3: Back, Next, Skip tutorial and Done, Next grayed out on a gated step,
// the buttons under a hint, and the list of topics, from Settings and More
// topics.
#include <string>

#include "fakes/ui_test.h"
#include "generated/ui_strings.h"
#include "ui/tutorial/topics.h"
#include "ui/tutorial/tutorial.h"

namespace sz::test {
namespace {

class TutorialUiTest : public UiTest {
protected:
    OverlayApp& Overlay() { return controller_->Overlay(); }
    const tutorial::Tutorial& Runner() const { return App().TutorialRunner(); }
    std::string StepUp() const {
        return Runner().GetState() == tutorial::Tutorial::State::OnStep ? std::string(Runner().CurrentStep().id)
                                                                         : "(" + Runner().Progress() + ")";
    }

    void StartTheTutorial() {
        ShowEditMode();
        StepFrame();
        Overlay().StartTutorial();
        StepFrames(2);
        ASSERT_TRUE(Runner().On());
    }
    // One of the card's buttons, or the hint's, by its id.
    void ClickOnCard(const char* id) {
        RunUi(id, [id](ImGuiTestContext* ctx) {
            ctx->SetRef("//##tutorial_card");
            ctx->ItemClick(id);
        });
        StepFrames(2);
    }
    bool OnCardDisabled(const char* id) {
        bool disabled = false;
        RunUi("is it grayed out", [&](ImGuiTestContext* ctx) {
            ctx->SetRef("//##tutorial_card");
            disabled = (ctx->ItemInfo(id).ItemFlags & ImGuiItemFlags_Disabled) != 0;
        });
        return disabled;
    }
    // The screenshot step done: a box dragged on the tutorial's canvas,
    // and the second it takes to move on.
    void TakeTheScreenshot() {
        Drag(300.0f, 360.0f, 600.0f, 560.0f);
        StepFrames(75);
        ASSERT_EQ(StepUp(), "move");
    }
};

TEST_F(TutorialUiTest, NextAndBackGoBetweenTheSteps) {
    StartTheTutorial();
    ASSERT_EQ(StepUp(), "welcome");
    ClickOnCard("**/###tutorial_next");
    EXPECT_EQ(StepUp(), "screenshot");
    ClickOnCard("**/###tutorial_back");
    EXPECT_EQ(StepUp(), "welcome");
}

TEST_F(TutorialUiTest, NextIsGrayedOutOnAGatedStepUntilItsGoalIsMet) {
    StartTheTutorial();
    ClickOnCard("**/###tutorial_next");
    ASSERT_EQ(StepUp(), "screenshot");
    EXPECT_TRUE(OnCardDisabled("**/###tutorial_next"));
    ClickOnCard("**/###tutorial_next");
    EXPECT_EQ(StepUp(), "screenshot") << "a grayed-out Next does nothing";

    TakeTheScreenshot();
    ClickOnCard("**/###tutorial_back");
    ASSERT_EQ(StepUp(), "screenshot");
    EXPECT_FALSE(OnCardDisabled("**/###tutorial_next")) << "met once, it stays met";
}

TEST_F(TutorialUiTest, SkipGoesToTheSkipCardAndItsDoneEndsTheTutorial) {
    StartTheTutorial();
    ClickOnCard("**/###tutorial_skip");
    ASSERT_EQ(Runner().GetState(), tutorial::Tutorial::State::Skipped);
    ClickOnCard("**/###tutorial_back");
    EXPECT_EQ(StepUp(), "welcome");
    ClickOnCard("**/###tutorial_skip");
    ClickOnCard("**/###tutorial_done");
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(Runner().GetOutcome(), tutorial::Tutorial::Outcome::Skipped);
}

TEST_F(TutorialUiTest, GoBackToTheTutorialSwitchesToItsFolder) {
    ShowEditMode();
    StepFrame();
    const CanvasId elsewhere = Canvases().CurrentCanvasId();
    StartTheTutorial();
    ClickOnCard("**/###tutorial_next");
    const CanvasId tutorialCanvas = Canvases().CurrentCanvasId();

    controller_->GetSession().SwitchToCanvas(elsewhere);
    StepFrames(2);
    ASSERT_TRUE(Runner().CurrentHint().has_value());
    EXPECT_EQ(Runner().CurrentHint()->need, tutorial::Need::InTutorialFolder);
    ClickOnCard("**/###tutorial_hint");
    EXPECT_EQ(Canvases().CurrentCanvasId(), tutorialCanvas);
    EXPECT_FALSE(Runner().CurrentHint().has_value());
}

TEST_F(TutorialUiTest, PutOneHereMakesASnippetToPracticeOn) {
    StartTheTutorial();
    ClickOnCard("**/###tutorial_next");
    TakeTheScreenshot();
    // The only snippet deleted, from under the move step.
    const ItemId shot = *Runner().Subject();
    controller_->GetSession().DeleteItem(shot);
    StepFrames(2);
    ASSERT_TRUE(Runner().CurrentHint().has_value());
    EXPECT_EQ(Runner().CurrentHint()->need, tutorial::Need::ASubject);

    ClickOnCard("**/###tutorial_hint");
    ASSERT_TRUE(Runner().Subject().has_value());
    EXPECT_NE(*Runner().Subject(), shot);
    const Item* practice = Canvases().FindItemAnywhere(*Runner().Subject());
    ASSERT_NE(practice, nullptr);
    EXPECT_FALSE(practice->hasBackground) << "a drawing";
    EXPECT_GT(practice->picture.opacity, 0.0f) << "with a backing, so it is seen";
    EXPECT_FALSE(Runner().CurrentHint().has_value());

    // Off the history: an undo brings the screenshot back, and leaves it.
    PressCtrlKey(ImGuiKey_Z);
    EXPECT_FALSE(Canvases().IsItemDeleted(*Runner().Subject()));
}

// The skip card's two ways to end: Done, keep the folder; and Done, which
// asks first and then puts the folder in the trash (question 9).
TEST_F(TutorialUiTest, TheSkipCardsDoneTrashesTheFolderAndDoneKeepKeepsIt) {
    StartTheTutorial();
    const FolderId kept = Runner().Folder();
    ClickOnCard("**/###tutorial_skip");
    ClickOnCard("**/###tutorial_donekeep");
    EXPECT_FALSE(Runner().On());
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(kept)));

    Overlay().StartTutorial();
    StepFrames(2);
    const FolderId trashed = Runner().Folder();
    ClickOnCard("**/###tutorial_skip");
    RunUi("done, and confirm", [this, trashed](ImGuiTestContext* ctx) {
        ctx->SetRef("//##tutorial_card");
        ctx->ItemClick("**/###tutorial_done");
        ctx->Yield(3);
        IM_CHECK(!Runner().On());
        IM_CHECK(!Canvases().IsDeleted(*Canvases().FindFolder(trashed)));  // asked first
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("##confirmdelete");
        ctx->Yield(3);
    });
    EXPECT_TRUE(Canvases().IsDeleted(*Canvases().FindFolder(trashed)));
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(kept)));
}

// The folder made at Folders and canvases' `newFolder` goes with the
// tutorial's at Done: one confirmation, and its Delete puts both in the
// trash (docs/TUTORIAL.md, section 17.3).
TEST_F(TutorialUiTest, DoneAsksOnceForTheFolderMadeInTheRunAndTrashesBoth) {
    ShowEditMode();
    StepFrame();
    Overlay().StartTutorial("folders");
    StepFrames(2);
    ASSERT_EQ(StepUp(), "newCanvas");
    RunUi("a new canvas", [](ImGuiTestContext* ctx) {
        ctx->MouseMoveToPos(ImVec2(kDisplayWidth * 0.5f, kDisplayHeight - 1.0f));
        ctx->Yield(30);
        ctx->SetRef("//##canvas_bar");
        ctx->ItemClick("##canvasbar_new");
    });
    StepFrames(75);
    ASSERT_EQ(StepUp(), "moveSnippet");
    ClickOnCard("**/###tutorial_next");
    ClickOnCard("**/###tutorial_next");
    ASSERT_EQ(StepUp(), "newFolder");
    OpenOverviewUi();
    RunUi("a new folder", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/##newfolder");
    });
    StepFrames(75);
    ASSERT_EQ(StepUp(), "rename");
    ASSERT_EQ(Runner().MadeFolders().size(), 1u);
    const FolderId own = Runner().Folder();
    const FolderId made = Runner().MadeFolders().front();
    while (StepUp() != "end") {
        const std::string before = StepUp();
        ClickOnCard("**/###tutorial_next");
        ASSERT_NE(StepUp(), before);
    }

    RunUi("done, and confirm", [this, own, made](ImGuiTestContext* ctx) {
        ctx->SetRef("//##tutorial_card");
        ctx->ItemClick("**/###tutorial_done");
        ctx->Yield(3);
        IM_CHECK(!Canvases().IsDeleted(*Canvases().FindFolder(own)));  // asked first
        IM_CHECK(!Canvases().IsDeleted(*Canvases().FindFolder(made)));
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("##confirmdelete");
        ctx->Yield(3);
    });
    EXPECT_TRUE(Canvases().IsDeleted(*Canvases().FindFolder(own)));
    EXPECT_TRUE(Canvases().IsDeleted(*Canvases().FindFolder(made)));
}

// A press on the card as the window hands it on: into ImGui, and into the
// input stream the machine reads - both, as a real one is.
TEST_F(TutorialUiTest, APressOnTheCardReachesItsButtonThroughTheInputMachine) {
    StartTheTutorial();
    ImRect next;
    RunUi("where Next is", [&](ImGuiTestContext* ctx) {
        ctx->SetRef("//##tutorial_card");
        next = ctx->ItemInfo("**/###tutorial_next").RectFull;
    });
    const ImVec2 at = next.GetCenter();
    MoveTo(at.x, at.y);
    RawMouse(at.x, at.y, platform::MouseEventKind::Move);
    StepFrame();
    RawMouse(at.x, at.y, platform::MouseEventKind::Down);
    MouseButtonEvent(ImGuiMouseButton_Left, true);
    StepFrame();
    RawMouse(at.x, at.y, platform::MouseEventKind::Up);
    MouseButtonEvent(ImGuiMouseButton_Left, false);
    StepFrames(3);
    EXPECT_EQ(StepUp(), "screenshot");
}

// Settings > Interaction: the Overview closes, and the list of topics
// comes up; a topic pressed starts at its first step, in a new folder
// named for it (section 13.4).
TEST_F(TutorialUiTest, OpenTheTutorialInSettingsShowsTheTopicsToPickFrom) {
    ShowEditMode();
    StepFrame();
    const size_t folders = Canvases().Folders().size();
    OpenOverviewUi();
    RunUi("open the tutorial", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectioninteraction");
        ctx->ItemClick("**/###tutorial_open");
    });
    StepFrames(2);
    EXPECT_FALSE(App().IsOverviewOpen());
    EXPECT_TRUE(App().TutorialListed());
    EXPECT_FALSE(Runner().On()) << "nothing started by opening the list";

    ClickOnCard("**/tutorial_topic_drawing");
    EXPECT_FALSE(App().TutorialListed());
    ASSERT_TRUE(Runner().On());
    EXPECT_EQ(App().TutorialTopic().id, "drawing");
    EXPECT_EQ(StepUp(), "drawingMode");
    ASSERT_EQ(Canvases().Folders().size(), folders + 1);
    EXPECT_EQ(Canvases().Folders().back().name, "Tutorial: Drawing and notes");
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, Runner().Folder());
}

TEST_F(TutorialUiTest, TheListClosesWithNothingStarted) {
    ShowEditMode();
    StepFrame();
    Overlay().OpenTutorialList();
    StepFrames(2);
    ASSERT_TRUE(App().TutorialListed());
    ClickOnCard("**/###tutorial_closelist");
    EXPECT_FALSE(App().TutorialListed());
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(ImGui::FindWindowByName("##tutorial_card")->Active, false);
}

// More topics, from the skip card: the list, where Back returns to the
// skip card, the running topic's row to it too, and another row starts
// that topic.
TEST_F(TutorialUiTest, MoreTopicsOpensTheListAndBackReturnsToTheCardItCameFrom) {
    StartTheTutorial();
    ClickOnCard("**/###tutorial_skip");
    ClickOnCard("**/###tutorial_moretopics");
    ASSERT_TRUE(App().TutorialListed());
    EXPECT_EQ(App().TutorialStatus(*tutorial::FindTopic("basics")), TutorialCard::Status::Running);
    EXPECT_EQ(App().TutorialStatus(*tutorial::FindTopic("drawing")), TutorialCard::Status::New);

    ClickOnCard("**/###tutorial_closelist");
    EXPECT_EQ(Runner().GetState(), tutorial::Tutorial::State::Skipped) << "back on the skip card";
    ClickOnCard("**/###tutorial_moretopics");
    ClickOnCard("**/tutorial_topic_basics");
    EXPECT_EQ(Runner().GetState(), tutorial::Tutorial::State::Skipped) << "the running one goes on where it is";

    ClickOnCard("**/###tutorial_moretopics");
    ClickOnCard("**/tutorial_topic_drawing");
    EXPECT_EQ(App().TutorialTopic().id, "drawing");
    EXPECT_EQ(StepUp(), "drawingMode");
    EXPECT_EQ(App().TutorialStatus(*tutorial::FindTopic("basics")), TutorialCard::Status::Started)
        << "skipped, so started";
}

// Profiles by name: Make a profile for this in Settings, and the skip
// card's keep button, which on this topic keeps the profile made
// (docs/TUTORIAL.md, section 18.3).
TEST_F(TutorialUiTest, ProfilesMakesOneInSettingsAndItsKeepButtonKeepsIt) {
    host_.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "Game"};
    ShowEditMode();
    StepFrame();
    Overlay().StartTutorial("profiles");
    StepFrames(2);
    OpenOverviewUi();
    RunUi("open profiles", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###overviewtabsettings");
        ctx->ItemClick("**/###sectionprofiles");
    });
    StepFrames(75);  // the second the card waits before it moves on
    ASSERT_EQ(StepUp(), "makeProfile");
    RunUi("make a profile", [](ImGuiTestContext* ctx) {
        ctx->SetRef("//##overview_panel");
        ctx->ItemClick("**/###makeprofile");
    });
    StepFrames(75);
    EXPECT_EQ(StepUp(), "behavior");
    ASSERT_EQ(AppSettings().Profiles().size(), 1u);

    ClickOnCard("**/###tutorial_skip");
    std::string label;
    RunUi("the keep button", [&label](ImGuiTestContext* ctx) {
        ctx->SetRef("//##tutorial_card");
        label = ctx->ItemInfo("**/###tutorial_donekeep").DebugLabel;
    });
    EXPECT_EQ(label.rfind(strings::kTutorialCardDoneKeepProfile, 0), 0u) << label;
    ClickOnCard("**/###tutorial_donekeep");
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(AppSettings().Profiles().size(), 1u) << "kept";
}

}  // namespace
}  // namespace sz::test
