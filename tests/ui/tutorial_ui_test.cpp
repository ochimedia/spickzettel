// The tutorial card's buttons, clicked by name - docs/TUTORIAL.md, section
// 3: Back, Next, Skip tutorial and Done, Next grayed out on a gated step,
// and the buttons under a hint.
#include <string>

#include "fakes/ui_test.h"
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

}  // namespace
}  // namespace sz::test
