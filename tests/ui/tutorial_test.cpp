#include "ui/tutorial/tutorial.h"

#include <gtest/gtest.h>

#include <vector>

#include "support/fake_tutorial_world.h"

// The runner of docs/TUTORIAL.md, section 5, on a chain of its own - what
// the topics' steps do is chains_test.cpp's.

namespace sz::ui::tutorial {
namespace {

// Read, a gated do step (a snippet made here), a do step that is not
// gated (the subject drawn on), a warning, and a last read step.
const std::vector<Step>& Toy() {
    static const std::vector<Step> chain = [] {
        auto text = [](const World&) { return "text"; };
        std::vector<Step> steps;
        steps.push_back(Step{.id = "read", .title = "Read", .text = text});
        steps.push_back(Step{
            .id = "gated",
            .kind = StepKind::Do,
            .gated = true,
            .title = "Gated",
            .text = text,
            .needs = {Need::InTutorialFolder, Need::CanvasUncovered},
            .goal =
                [](const Look& look) {
                    for (const SnippetFacts* made : look.MadeHere()) {
                        if (made->picture) {
                            return true;
                        }
                    }
                    return false;
                },
            .nearMisses = {{[](const Look& look) { return !look.MadeHere().empty(); }, "miss"}},
        });
        steps.push_back(Step{
            .id = "free",
            .kind = StepKind::Do,
            .title = "Free",
            .text = text,
            .spot = Spot::Subject,
            .needs = {Need::ASubject, Need::SubjectHere, Need::SubjectOnScreen},
            .subject = SubjectRule::Any,
            .goal =
                [](const Look& look) {
                    return look.Subject() != nullptr && look.SubjectAtStart() != nullptr &&
                           look.Subject()->strokes.size() > look.SubjectAtStart()->strokes.size();
                },
        });
        steps.push_back(Step{.id = "warn", .warning = true, .title = "Warn", .text = text});
        steps.push_back(Step{.id = "last", .title = "Last", .text = text});
        return steps;
    }();
    return chain;
}

class TutorialTest : public ::testing::Test {
protected:
    TutorialTest() { tutorial_.Start(FakeWorld::kTutorialFolder); }
    // A frame at `seconds`.
    void Frame(double seconds) { tutorial_.Update(world_, seconds); }
    std::string Id() const { return std::string(tutorial_.CurrentStep().id); }

    FakeWorld world_;
    Tutorial tutorial_{Toy()};
};

TEST_F(TutorialTest, StartsAtTheFirstStepInItsFolder) {
    EXPECT_EQ(tutorial_.GetState(), Tutorial::State::OnStep);
    EXPECT_EQ(tutorial_.Folder(), FakeWorld::kTutorialFolder);
    EXPECT_EQ(Id(), "read");
    EXPECT_EQ(tutorial_.Progress(), "read");
}

TEST_F(TutorialTest, NextAndBackWalkTheChain) {
    tutorial_.Back();
    EXPECT_EQ(Id(), "read");
    tutorial_.Next();
    EXPECT_EQ(Id(), "gated");
    EXPECT_EQ(tutorial_.Progress(), "gated");
    tutorial_.Back();
    EXPECT_EQ(Id(), "read");
}

TEST_F(TutorialTest, AGatedStepWaitsForItsGoalAndMovesOnASecondAfterIt) {
    tutorial_.Next();
    Frame(0.0);
    EXPECT_FALSE(tutorial_.NextEnabled());
    tutorial_.Next();
    EXPECT_EQ(Id(), "gated");

    world_.Make(1);
    Frame(1.0);
    EXPECT_TRUE(tutorial_.GoalMet());
    EXPECT_TRUE(tutorial_.NextEnabled());
    Frame(1.0 + kMoveOnSeconds * 0.9);
    EXPECT_EQ(Id(), "gated");
    Frame(1.0 + kMoveOnSeconds);
    EXPECT_EQ(Id(), "free");
}

TEST_F(TutorialTest, AGoalMetStaysMetSoBackNeverLocksNext) {
    tutorial_.Next();
    Frame(0.0);
    world_.Make(1);
    Frame(0.5);
    Frame(5.0);
    ASSERT_EQ(Id(), "free");
    world_.At(1).deleted = true;  // what met it, taken back
    tutorial_.Back();
    Frame(6.0);
    EXPECT_EQ(Id(), "gated");
    EXPECT_TRUE(tutorial_.NextEnabled());
}

TEST_F(TutorialTest, AStepNothingDependsOnMovesOnWithNextUndone) {
    tutorial_.Resume("free", FakeWorld::kTutorialFolder);
    Frame(0.0);
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_TRUE(tutorial_.NextEnabled());
    tutorial_.Next();
    EXPECT_EQ(Id(), "warn");
}

TEST_F(TutorialTest, ANeedNotMetIsSaidWhileTheGoalIsNot) {
    tutorial_.Next();
    world_.cover = Cover::Overview;
    Frame(0.0);
    EXPECT_FALSE(tutorial_.GoalMet());
    ASSERT_TRUE(tutorial_.CurrentHint().has_value());
    EXPECT_EQ(tutorial_.CurrentHint()->need, Need::CanvasUncovered);
    world_.cover = Cover::None;
    Frame(1.0);
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

// A result is a result: the capture hotkey can make a snippet with the
// Overview up.
TEST_F(TutorialTest, AGoalMetCountsWhateverTheNeedsSay) {
    tutorial_.Next();
    Frame(0.0);
    world_.cover = Cover::Overview;
    world_.Make(1);
    Frame(1.0);
    EXPECT_TRUE(tutorial_.GoalMet());
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(TutorialTest, TheFirstNeedNotMetIsTheHint) {
    tutorial_.Next();
    world_.current = FakeWorld::kOtherCanvas;
    world_.cover = Cover::Popup;
    Frame(0.0);
    ASSERT_TRUE(tutorial_.CurrentHint().has_value());
    EXPECT_EQ(tutorial_.CurrentHint()->need, Need::InTutorialFolder);
    EXPECT_EQ(tutorial_.CurrentHint()->button, HintButton::BackToTutorial);
}

TEST_F(TutorialTest, ANearMissIsSaidWhileTheGoalIsNotMet) {
    tutorial_.Next();
    Frame(0.0);
    world_.Make(1, /*picture=*/false);
    Frame(1.0);
    ASSERT_TRUE(tutorial_.CurrentHint().has_value());
    EXPECT_STREQ(tutorial_.CurrentHint()->text, "miss");
    EXPECT_FALSE(tutorial_.CurrentHint()->need.has_value());
}

TEST_F(TutorialTest, WhatWasThereWhenTheStepBeganDoesNotMeetItsGoal) {
    world_.Make(1);
    tutorial_.Next();
    Frame(0.0);
    EXPECT_FALSE(tutorial_.GoalMet());
}

TEST_F(TutorialTest, MetOnThisVisitAStepMovesOnWhateverItsNeedsSay) {
    tutorial_.Resume("free", FakeWorld::kTutorialFolder);
    world_.Make(1);
    Frame(0.0);
    world_.Draw(1);
    Frame(1.0);
    ASSERT_TRUE(tutorial_.GoalMet());
    world_.At(1).deleted = true;  // no subject left
    Frame(1.5);
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
    Frame(2.0);
    EXPECT_EQ(Id(), "warn");
}

TEST_F(TutorialTest, SkipGoesToTheSkipCardWhichBackLeavesAndDoneLetsGo) {
    tutorial_.Next();
    tutorial_.Skip();
    EXPECT_EQ(tutorial_.GetState(), Tutorial::State::Skipped);
    EXPECT_EQ(tutorial_.Progress(), "skipped");
    tutorial_.Back();
    EXPECT_EQ(tutorial_.GetState(), Tutorial::State::OnStep);
    EXPECT_EQ(Id(), "gated");

    tutorial_.Skip();
    tutorial_.Done();
    EXPECT_EQ(tutorial_.GetState(), Tutorial::State::Off);
    EXPECT_EQ(tutorial_.GetOutcome(), Tutorial::Outcome::Skipped);
    EXPECT_EQ(tutorial_.Progress(), "skipped");
}

TEST_F(TutorialTest, TheSkipCardRepeatsTheWarningsNotReached) {
    tutorial_.Skip();
    ASSERT_EQ(tutorial_.WarningsNotReached().size(), 1u);
    EXPECT_EQ(tutorial_.WarningsNotReached()[0]->id, "warn");

    tutorial_.Resume("last", FakeWorld::kTutorialFolder);
    tutorial_.Skip();
    EXPECT_TRUE(tutorial_.WarningsNotReached().empty());
}

TEST_F(TutorialTest, NextOnTheLastStepFinishes) {
    tutorial_.Resume("last", FakeWorld::kTutorialFolder);
    tutorial_.Next();
    EXPECT_EQ(tutorial_.GetState(), Tutorial::State::Off);
    EXPECT_EQ(tutorial_.GetOutcome(), Tutorial::Outcome::Finished);
    EXPECT_EQ(tutorial_.Progress(), "finished");
    Frame(0.0);  // nothing to do
    EXPECT_FALSE(tutorial_.On());
}

TEST_F(TutorialTest, ResumeFindsTheStepByIdAndAnUnknownIdStartsAgain) {
    tutorial_.Resume("warn", FakeWorld::kOtherFolder);
    EXPECT_EQ(Id(), "warn");
    EXPECT_EQ(tutorial_.Folder(), FakeWorld::kOtherFolder);
    tutorial_.Back();
    tutorial_.Back();
    EXPECT_EQ(Id(), "gated");
    EXPECT_TRUE(tutorial_.NextEnabled());  // passed before, so done

    tutorial_.Resume("gone", FakeWorld::kTutorialFolder);
    EXPECT_EQ(Id(), "read");
}

TEST_F(TutorialTest, TheSubjectIsTheNewestUnlessTheHandPicksAnother) {
    world_.Make(1);
    world_.Make(2);
    tutorial_.Resume("free", FakeWorld::kTutorialFolder);
    Frame(0.0);
    EXPECT_EQ(tutorial_.Subject(), 2u);

    world_.selection = {1};
    Frame(1.0);
    EXPECT_EQ(tutorial_.Subject(), 1u);

    // It stays when the hand lets go, and a newer one does not take over.
    world_.selection.clear();
    world_.Make(3);
    Frame(2.0);
    EXPECT_EQ(tutorial_.Subject(), 1u);

    world_.drawing = 3;
    Frame(3.0);
    EXPECT_EQ(tutorial_.Subject(), 3u);
}

TEST_F(TutorialTest, OnlyTheTutorialFoldersSnippetsAreSubjects) {
    world_.current = FakeWorld::kOtherCanvas;
    world_.Make(9);
    world_.current = FakeWorld::kCanvas;
    world_.selection = {9};
    tutorial_.Resume("free", FakeWorld::kTutorialFolder);
    Frame(0.0);
    EXPECT_FALSE(tutorial_.Subject().has_value());
    ASSERT_TRUE(tutorial_.CurrentHint().has_value());
    EXPECT_EQ(tutorial_.CurrentHint()->need, Need::ASubject);
    EXPECT_EQ(tutorial_.CurrentHint()->button, HintButton::PutOneHere);
}

TEST_F(TutorialTest, ASubjectOnAnotherCanvasOffersTheWayBackThere) {
    world_.current = FakeWorld::kSecondCanvas;
    world_.Make(1);
    world_.current = FakeWorld::kCanvas;
    tutorial_.Resume("free", FakeWorld::kTutorialFolder);
    Frame(0.0);
    ASSERT_TRUE(tutorial_.CurrentHint().has_value());
    EXPECT_EQ(tutorial_.CurrentHint()->need, Need::SubjectHere);
    EXPECT_EQ(tutorial_.CurrentHint()->button, HintButton::BackThere);
    EXPECT_EQ(tutorial_.CurrentHint()->canvas, FakeWorld::kSecondCanvas);

    // One made here will do instead.
    world_.Make(2);
    Frame(1.0);
    EXPECT_EQ(tutorial_.Subject(), 2u);
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(TutorialTest, AMinimizedSubjectMovesTheSpotlightToTheDock) {
    world_.Make(1);
    tutorial_.Resume("free", FakeWorld::kTutorialFolder);
    Frame(0.0);
    EXPECT_EQ(tutorial_.CurrentSpot(), Spot::Subject);
    world_.At(1).minimized = true;
    Frame(1.0);
    EXPECT_EQ(tutorial_.CurrentHint()->need, Need::SubjectOnScreen);
    EXPECT_EQ(tutorial_.CurrentSpot(), Spot::DockChip);
}

TEST(TutorialExpandTest, FillsInKeysTriggersAndTheCanvas) {
    FakeWorld world;
    world.screenshotTrigger = core::CreationTrigger::Alt;
    EXPECT_EQ(Expand("{key:undo} or {key:cheatSheet}", world), "Ctrl+Z or Ctrl+H");
    EXPECT_EQ(Expand("Hold {trigger:screenshot}; {trigger:drawing}+drag", world), "Hold Alt; Ctrl+drag");
    EXPECT_EQ(Expand("On \"{canvas}\".", world, 12), "On \"Canvas 12\".");
}

TEST(TutorialExpandTest, LeavesWhatItDoesNotKnowAndDropsAnUnboundKey) {
    FakeWorld world;
    world.keys.erase(CommandId::Undo);
    EXPECT_EQ(Expand("[{key:undo}] {nope} {key:noSuchCommand} {", world), "[] {nope} {key:noSuchCommand} {");
}

}  // namespace
}  // namespace sz::ui::tutorial
