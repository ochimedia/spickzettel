#include "ui/tutorial/welcome_chain.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>

#include "generated/ui_strings.h"
#include "support/fake_tutorial_world.h"
#include "ui/tutorial/tutorial.h"

// The welcome chain of docs/TUTORIAL.md, section 4, run against a fake
// world: done the way a user would, off the path the ways section 6.1
// lists, and checked for the shape the runner relies on.

namespace sz::ui::tutorial {
namespace {

class WelcomeChainTest : public ::testing::Test {
protected:
    WelcomeChainTest() { tutorial_.Start(FakeWorld::kTutorialFolder); }

    void Frame() {
        seconds_ += 0.1;
        tutorial_.Update(world_, seconds_);
    }
    // Frames past the second a done step waits, and a few more, so that
    // the next step has begun: what changes after this is the next step's.
    void Settle() {
        for (int i = 0; i < 15; ++i) {
            Frame();
        }
    }
    // At step `id`, begun.
    void At(std::string_view id) {
        tutorial_.Resume(id, FakeWorld::kTutorialFolder);
        Frame();
        ASSERT_EQ(Id(), id);
    }
    std::string Id() const { return std::string(tutorial_.CurrentStep().id); }
    std::optional<Need> NeedShown() const {
        return tutorial_.CurrentHint() ? tutorial_.CurrentHint()->need : std::nullopt;
    }
    const char* HintText() const { return tutorial_.CurrentHint() ? tutorial_.CurrentHint()->text : ""; }

    FakeWorld world_;
    Tutorial tutorial_{WelcomeChain()};
    double seconds_ = 0.0;
};

TEST_F(WelcomeChainTest, CanBeWalkedTheWayAUserWould) {
    Frame();
    EXPECT_EQ(Id(), "welcome");
    tutorial_.Next();
    Frame();

    ASSERT_EQ(Id(), "screenshot");
    world_.Make(1);
    Settle();

    ASSERT_EQ(Id(), "move");
    EXPECT_EQ(tutorial_.Subject(), 1u);
    world_.At(1).rect.x += 50.0f;
    Settle();

    ASSERT_EQ(Id(), "resize");
    EXPECT_EQ(NeedShown(), Need::SubjectSelected);
    world_.selection = {1};
    world_.At(1).rect.w *= 1.5f;
    Settle();

    ASSERT_EQ(Id(), "drawingMode");
    world_.drawing = 1;
    Settle();

    ASSERT_EQ(Id(), "draw");
    world_.At(1).strokes = 1;
    Settle();

    ASSERT_EQ(Id(), "stopDrawing");
    world_.drawing.reset();
    Settle();

    ASSERT_EQ(Id(), "delete");
    world_.At(1).deleted = true;
    world_.selection.clear();
    Settle();

    ASSERT_EQ(Id(), "undo");
    EXPECT_EQ(tutorial_.Subject(), 1u);
    world_.At(1).deleted = false;
    Settle();

    ASSERT_EQ(Id(), "programs");
    tutorial_.Next();
    Frame();
    ASSERT_EQ(Id(), "antiCheat");
    tutorial_.Next();
    Frame();

    ASSERT_EQ(Id(), "away");
    world_.showings += 1;
    Settle();

    ASSERT_EQ(Id(), "end");
    tutorial_.Next();
    EXPECT_EQ(tutorial_.GetOutcome(), Tutorial::Outcome::Finished);
}

TEST_F(WelcomeChainTest, TheScreenshotStepSaysWhenAFullscreenOneWasMade) {
    At("screenshot");
    world_.Make(1).fullscreen = true;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialScreenshotMissFullscreen);
}

TEST_F(WelcomeChainTest, TheScreenshotStepSaysWhenADrawingWasMade) {
    At("screenshot");
    world_.Make(1, /*picture=*/false);
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialScreenshotMissDrawing);
}

TEST_F(WelcomeChainTest, TheScreenshotStepTakesTheScreenshotToolButNoOther) {
    At("screenshot");
    world_.tool = core::ItemCreationKind::Drawing;
    Frame();
    EXPECT_EQ(NeedShown(), Need::NoOtherTool);
    world_.tool = core::ItemCreationKind::Screenshot;
    Frame();
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(WelcomeChainTest, TheScreenshotStepWaitsForAScreenshot) {
    At("screenshot");
    EXPECT_FALSE(tutorial_.NextEnabled());
    world_.Make(1);
    Frame();
    EXPECT_TRUE(tutorial_.NextEnabled());
}

TEST_F(WelcomeChainTest, LeavingTheTutorialFolderOffersTheWayBack) {
    world_.Make(1);
    At("move");
    world_.current = FakeWorld::kOtherCanvas;
    Frame();
    EXPECT_EQ(NeedShown(), Need::InTutorialFolder);
    EXPECT_EQ(tutorial_.CurrentHint()->button, HintButton::BackToTutorial);
}

TEST_F(WelcomeChainTest, TheMoveStepSaysAResizeIsNotAMove) {
    world_.Make(1);
    At("move");
    world_.At(1).rect.w *= 2.0f;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialMoveMissResized);
}

TEST_F(WelcomeChainTest, FullscreenIsNeitherAMoveNorAResize) {
    world_.Make(1);
    At("move");
    world_.At(1).fullscreen = true;
    world_.At(1).rect = core::Rect{0.0f, 0.0f, 1280.0f, 768.0f};
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_EQ(NeedShown(), Need::SubjectCanMove);

    world_.At(1).fullscreen = false;
    world_.At(1).rect = core::Rect{100.0f, 100.0f, 200.0f, 150.0f};
    At("resize");
    world_.At(1).fullscreen = true;
    world_.At(1).rect = core::Rect{0.0f, 0.0f, 1280.0f, 768.0f};
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_EQ(NeedShown(), Need::SubjectCanMove);
}

TEST_F(WelcomeChainTest, TheMoveStepNeedsASnippetThatCanMoveOutOfDrawingMode) {
    world_.Make(1).fullscreen = true;
    At("move");
    EXPECT_EQ(NeedShown(), Need::SubjectCanMove);
    world_.At(1).fullscreen = false;
    world_.drawing = 1;
    Frame();
    EXPECT_EQ(NeedShown(), Need::NoDrawingMode);
}

TEST_F(WelcomeChainTest, WithNothingToPracticeOnAPracticeSnippetBecomesTheSubject) {
    At("move");
    EXPECT_EQ(NeedShown(), Need::ASubject);
    EXPECT_EQ(tutorial_.CurrentHint()->button, HintButton::PutOneHere);
    world_.Make(5, /*picture=*/false);  // what Put one here makes
    Frame();
    EXPECT_EQ(tutorial_.Subject(), 5u);
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(WelcomeChainTest, DrawingOnAnotherOfTheTutorialsSnippetsCounts) {
    world_.Make(1);
    world_.Make(2);
    world_.selection = {2};
    At("drawingMode");
    EXPECT_EQ(tutorial_.Subject(), 2u);
    world_.drawing = 1;
    Frame();
    EXPECT_EQ(tutorial_.Subject(), 1u);
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(WelcomeChainTest, TheDrawStepAsksToDrawOnItAgainOutOfDrawingMode) {
    world_.Make(1);
    At("draw");
    EXPECT_EQ(NeedShown(), Need::DrawingOnSubject);
    world_.drawing = 1;
    Frame();
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(WelcomeChainTest, DeleteDoesNothingInDrawingModeAndTheStepSaysSo) {
    world_.Make(1);
    world_.drawing = 1;
    At("delete");
    EXPECT_EQ(NeedShown(), Need::NoDrawingMode);
    EXPECT_FALSE(tutorial_.NextEnabled());
}

TEST_F(WelcomeChainTest, DeletingTheOnlySnippetStillMovesOn) {
    world_.Make(1);
    At("delete");
    world_.At(1).deleted = true;
    Settle();
    EXPECT_EQ(Id(), "undo");
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(WelcomeChainTest, UndoneBeforeItsStepTheUndoStepAsksForADeleteAgain) {
    world_.Make(1);
    At("delete");
    world_.At(1).deleted = true;
    Frame();
    world_.At(1).deleted = false;  // Ctrl+Z within the second
    Settle();
    ASSERT_EQ(Id(), "undo");
    EXPECT_EQ(NeedShown(), Need::DeletedSubject);

    world_.At(1).deleted = true;
    Frame();
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
    world_.At(1).deleted = false;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(WelcomeChainTest, TheUndoStepNeedsTheCanvasItWasDeletedOnAndTheKeys) {
    world_.Make(1);
    At("delete");
    world_.At(1).deleted = true;
    Settle();
    ASSERT_EQ(Id(), "undo");

    // Undo takes back the canvas's own steps: elsewhere, it cannot.
    world_.current = FakeWorld::kSecondCanvas;
    Frame();
    EXPECT_EQ(NeedShown(), Need::SubjectHere);
    EXPECT_EQ(tutorial_.CurrentHint()->button, HintButton::BackThere);
    world_.current = FakeWorld::kCanvas;
    world_.cover = Cover::Overview;  // which has the keys
    Frame();
    EXPECT_EQ(NeedShown(), Need::CanvasUncovered);
    world_.cover = Cover::None;
    Frame();
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(WelcomeChainTest, TheTextsFollowTheTriggersAndTheKeys) {
    const auto& chain = WelcomeChain();
    auto textOf = [&](std::string_view id) {
        for (const Step& step : chain) {
            if (step.id == id) {
                return step.text(world_);
            }
        }
        return "";
    };
    EXPECT_STREQ(textOf("screenshot"), strings::kTutorialScreenshotText);
    world_.screenshotTrigger = core::CreationTrigger::Ctrl;
    EXPECT_STREQ(textOf("screenshot"), strings::kTutorialScreenshotTextTrigger);
    world_.screenshotTrigger = core::CreationTrigger::Off;
    EXPECT_STREQ(textOf("screenshot"), strings::kTutorialScreenshotTextTool);
    world_.keys.erase(CommandId::NewScreenshotTool);
    EXPECT_STREQ(textOf("screenshot"), strings::kTutorialScreenshotTextMenu);

    EXPECT_STREQ(textOf("away"), strings::kTutorialAwayText);
    world_.keys.erase(CommandId::ToggleEditMode);
    EXPECT_STREQ(textOf("away"), strings::kTutorialAwayTextTray);

    EXPECT_STREQ(textOf("end"), strings::kTutorialEndText);
    world_.keys.erase(CommandId::CheatSheet);
    EXPECT_STREQ(textOf("end"), strings::kTutorialEndTextNoCheatSheetKey);
}

TEST_F(WelcomeChainTest, EveryTextHasItsPlaceholdersFilledIn) {
    std::vector<const char*> texts = {
        strings::kTutorialNeedInTutorialFolder, strings::kTutorialNeedCloseOverview,
        strings::kTutorialNeedCloseCheatSheet,  strings::kTutorialNeedClosePopup,
        strings::kTutorialNeedNoDrawingMode,    strings::kTutorialNeedNoOtherTool,
        strings::kTutorialNeedASubject,         strings::kTutorialNeedSubjectHere,
        strings::kTutorialNeedSubjectOnScreen,  strings::kTutorialNeedSubjectCanMove,
        strings::kTutorialNeedSubjectSelected,  strings::kTutorialNeedDrawingOnSubject,
        strings::kTutorialNeedDeletedSubject,   strings::kTutorialScreenshotTextTrigger,
        strings::kTutorialScreenshotTextTool,   strings::kTutorialScreenshotTextMenu,
        strings::kTutorialAwayTextTray,         strings::kTutorialEndTextNoCheatSheetKey,
    };
    for (const Step& step : WelcomeChain()) {
        texts.push_back(step.title);
        texts.push_back(step.text(world_));
        for (const NearMiss& miss : step.nearMisses) {
            texts.push_back(miss.text);
        }
    }
    world_.screenshotTrigger = core::CreationTrigger::Ctrl;
    for (const char* text : texts) {
        const std::string expanded = Expand(text, world_, FakeWorld::kCanvas);
        EXPECT_EQ(expanded.find('{'), std::string::npos) << text;
        EXPECT_FALSE(expanded.empty());
    }
}

// What the runner relies on, and docs/TUTORIAL.md, section 4 says: every
// need a step cannot meet by itself is met by an earlier step that waits
// for it.
TEST_F(WelcomeChainTest, HasTheShapeTheRunnerReliesOn) {
    const auto& chain = WelcomeChain();
    std::set<std::string_view> ids;
    std::set<std::string_view> gatedSoFar;
    for (const Step& step : chain) {
        EXPECT_TRUE(ids.insert(step.id).second) << step.id;
        EXPECT_NE(step.text, nullptr) << step.id;
        if (step.kind == StepKind::Read) {
            EXPECT_EQ(step.goal, nullptr) << step.id;
            EXPECT_FALSE(step.gated) << step.id;
            EXPECT_TRUE(step.needs.empty()) << step.id;
        } else {
            EXPECT_NE(step.goal, nullptr) << step.id;
            EXPECT_FALSE(step.warning) << step.id;
        }
        for (const Need need : step.needs) {
            if (need == Need::ASubject) {
                EXPECT_TRUE(gatedSoFar.contains("screenshot")) << step.id;
            }
            if (need == Need::DrawingOnSubject) {
                EXPECT_TRUE(gatedSoFar.contains("drawingMode")) << step.id;
            }
            if (need == Need::DeletedSubject) {
                EXPECT_TRUE(gatedSoFar.contains("delete")) << step.id;
            }
        }
        if (step.gated) {
            gatedSoFar.insert(step.id);
        }
    }
    EXPECT_EQ(chain.front().kind, StepKind::Read);
    EXPECT_EQ(chain.back().kind, StepKind::Read);
}

}  // namespace
}  // namespace sz::ui::tutorial
