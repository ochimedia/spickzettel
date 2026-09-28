#include "ui/tutorial/chains.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>

#include "generated/ui_strings.h"
#include "support/fake_tutorial_world.h"
#include "ui/tutorial/topics.h"
#include "ui/tutorial/tutorial.h"

// The chains of docs/TUTORIAL.md, sections 4 and 13, run against a fake
// world: done the way a user would, off the path the ways section 6.1
// lists, and checked for the shape the runner relies on.

namespace sz::ui::tutorial {
namespace {

class ChainTest : public ::testing::Test {
protected:
    explicit ChainTest(const std::vector<Step>& chain) : tutorial_(chain) {
        tutorial_.Start(FakeWorld::kTutorialFolder);
    }

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
    Tutorial tutorial_;
    double seconds_ = 0.0;
};

class BasicsChainTest : public ChainTest {
protected:
    BasicsChainTest() : ChainTest(BasicsChain()) {}
};

class DrawingChainTest : public ChainTest {
protected:
    DrawingChainTest() : ChainTest(DrawingChain()) {}
};

class PinningChainTest : public ChainTest {
protected:
    PinningChainTest() : ChainTest(PinningChain()) {}

    const char* TextOf(std::string_view id) {
        for (const Step& step : PinningChain()) {
            if (step.id == id) {
                return step.text(world_);
            }
        }
        return "";
    }
};

TEST_F(BasicsChainTest, CanBeWalkedTheWayAUserWould) {
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

// In a new folder: nothing to draw on, until the practice snippet is put
// there.
TEST_F(DrawingChainTest, CanBeWalkedTheWayAUserWould) {
    Frame();
    ASSERT_EQ(Id(), "drawingMode");
    EXPECT_EQ(NeedShown(), Need::ASubject);
    EXPECT_EQ(tutorial_.CurrentHint()->button, HintButton::PutOneHere);
    EXPECT_FALSE(tutorial_.NextEnabled());
    world_.Make(5, /*picture=*/false);  // what Put one here makes
    Frame();
    EXPECT_EQ(tutorial_.Subject(), 5u);
    world_.drawing = 5;
    Settle();

    ASSERT_EQ(Id(), "draw");
    world_.At(5).strokes = 1;
    Settle();

    ASSERT_EQ(Id(), "stopDrawing");
    world_.drawing.reset();
    Settle();

    ASSERT_EQ(Id(), "end");
    tutorial_.Next();
    EXPECT_EQ(tutorial_.GetOutcome(), Tutorial::Outcome::Finished);
}

// In a new folder, as Drawing: the practice snippet first.
TEST_F(PinningChainTest, CanBeWalkedTheWayAUserWould) {
    Frame();
    ASSERT_EQ(Id(), "pin");
    EXPECT_EQ(NeedShown(), Need::ASubject);
    EXPECT_FALSE(tutorial_.NextEnabled()) << "the steps after it need a pin";
    world_.Make(5, /*picture=*/false);
    Frame();
    EXPECT_EQ(NeedShown(), Need::SubjectSelected);
    world_.selection = {5};
    world_.At(5).pinned = true;
    Settle();

    ASSERT_EQ(Id(), "pinnedAway");
    world_.pinnedViews += 1;
    world_.showings += 1;
    Settle();

    ASSERT_EQ(Id(), "opacity");
    world_.At(5).pictureOpacity -= 0.10f;
    Settle();

    ASSERT_EQ(Id(), "viewMode");
    world_.viewModes += 1;
    Settle();

    ASSERT_EQ(Id(), "unpin");
    world_.At(5).pinned = false;
    Settle();

    ASSERT_EQ(Id(), "end");
    tutorial_.Next();
    EXPECT_EQ(tutorial_.GetOutcome(), Tutorial::Outcome::Finished);
}

// The bar pins the whole selection: any of the tutorial's pinned will do.
TEST_F(PinningChainTest, APinOnAnotherOfTheTutorialsSnippetsCounts) {
    world_.Make(1);
    world_.Make(2);
    world_.selection = {2};
    At("pin");
    world_.At(1).pinned = true;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

// Put away and back, a pinned one is the subject - and one unpinned since
// gets its line.
TEST_F(PinningChainTest, PuttingItAwayNeedsItPinnedAndPrefersAPinnedOne) {
    world_.Make(1).pinned = true;
    world_.Make(2);
    world_.selection = {2};  // the hand on the one not pinned
    At("pinnedAway");
    EXPECT_EQ(tutorial_.Subject(), 1u);
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());

    world_.At(1).pinned = false;
    Frame();
    EXPECT_EQ(NeedShown(), Need::SubjectPinned);
    EXPECT_STREQ(HintText(), strings::kTutorialNeedSubjectPinned);
    world_.At(2).pinned = true;
    Frame();
    EXPECT_EQ(tutorial_.Subject(), 2u) << "the hand's, now that it is pinned";
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(PinningChainTest, PuttingItAwaySaysWhenViewModeCameUpInstead) {
    world_.Make(1).pinned = true;
    At("pinnedAway");
    world_.viewModes += 1;
    world_.showings += 1;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialPinnedAwayMissViewMode);
}

TEST_F(PinningChainTest, SeeThroughCountsOnlyWhatShows) {
    world_.Make(1);
    world_.selection = {1};
    At("opacity");
    world_.At(1).drawingOpacity = 0.7f;  // nothing drawn to fade
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialOpacityMissNothingDrawn);

    world_.At(1).strokes = 1;  // something drawn: now it shows
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(PinningChainTest, SeeThroughTakesThePictureEitherWayAndNotOneNotch) {
    world_.Make(1).pictureOpacity = 0.5f;
    world_.selection = {1};
    At("opacity");
    world_.At(1).pictureOpacity = 0.55f;  // one notch
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    world_.At(1).pictureOpacity = 0.6f;  // two, up
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(PinningChainTest, SeeThroughSaysAResizeIsNotItAndNeedsTheSelection) {
    world_.Make(1);
    At("opacity");
    EXPECT_EQ(NeedShown(), Need::SubjectSelected);
    world_.selection = {1};
    world_.At(1).rect.w *= 1.3f;
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialOpacityMissResized);
}

TEST_F(PinningChainTest, ViewModeSaysWhenTheOverlayWasPutAwayInstead) {
    At("viewMode");
    world_.showings += 1;
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialViewModeMissAway);

    // With no key to name, no line to say it with: Next goes on.
    world_.keys.erase(CommandId::ToggleViewMode);
    Frame();
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
    EXPECT_TRUE(tutorial_.NextEnabled());
}

TEST_F(PinningChainTest, TheTextsFollowTheKeys) {
    EXPECT_STREQ(TextOf("pinnedAway"), strings::kTutorialPinnedAwayText);
    EXPECT_STREQ(TextOf("viewMode"), strings::kTutorialViewModeText);
    world_.keys.erase(CommandId::ToggleEditMode);
    EXPECT_STREQ(TextOf("pinnedAway"), strings::kTutorialPinnedAwayTextTray);
    EXPECT_STREQ(TextOf("viewMode"), strings::kTutorialViewModeTextTray);
    world_.keys.erase(CommandId::ToggleViewMode);
    EXPECT_STREQ(TextOf("viewMode"), strings::kTutorialViewModeTextNoKey);
}

// Begun with nothing pinned: a pin first, and the unpin after it counts.
TEST_F(PinningChainTest, UnpinAsksForAPinFirstWhenNothingIsPinned) {
    world_.Make(1);
    world_.selection = {1};
    At("unpin");
    EXPECT_EQ(NeedShown(), Need::SubjectPinned);
    world_.At(1).pinned = true;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    world_.At(1).pinned = false;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(PinningChainTest, UnpinIsNotDoneByDeletingThePinnedOne) {
    world_.Make(1).pinned = true;
    world_.Make(2);
    world_.selection = {1};
    At("unpin");
    world_.At(1).deleted = true;
    world_.selection.clear();
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
}

// Let go of for another topic: finished from its end, skipped from the
// skip card, and otherwise nothing said, so its step is what stays kept.
TEST_F(DrawingChainTest, LeftForAnotherTopicItSaysOnlyHowItEnded) {
    Frame();
    tutorial_.Leave();
    EXPECT_FALSE(tutorial_.On());
    EXPECT_EQ(tutorial_.Progress(), "");

    tutorial_.Resume("end", FakeWorld::kTutorialFolder);
    tutorial_.Leave();
    EXPECT_EQ(tutorial_.Progress(), "finished");

    tutorial_.Resume("draw", FakeWorld::kTutorialFolder);
    tutorial_.Skip();
    tutorial_.Leave();
    EXPECT_EQ(tutorial_.Progress(), "skipped");
}

TEST_F(BasicsChainTest, TheScreenshotStepSaysWhenAFullscreenOneWasMade) {
    At("screenshot");
    world_.Make(1).fullscreen = true;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialScreenshotMissFullscreen);
}

TEST_F(BasicsChainTest, TheScreenshotStepSaysWhenADrawingWasMade) {
    At("screenshot");
    world_.Make(1, /*picture=*/false);
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialScreenshotMissDrawing);
}

TEST_F(BasicsChainTest, TheScreenshotStepTakesTheScreenshotToolButNoOther) {
    At("screenshot");
    world_.tool = core::ItemCreationKind::Drawing;
    Frame();
    EXPECT_EQ(NeedShown(), Need::NoOtherTool);
    world_.tool = core::ItemCreationKind::Screenshot;
    Frame();
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(BasicsChainTest, TheScreenshotStepWaitsForAScreenshot) {
    At("screenshot");
    EXPECT_FALSE(tutorial_.NextEnabled());
    world_.Make(1);
    Frame();
    EXPECT_TRUE(tutorial_.NextEnabled());
}

TEST_F(BasicsChainTest, LeavingTheTutorialFolderOffersTheWayBack) {
    world_.Make(1);
    At("move");
    world_.current = FakeWorld::kOtherCanvas;
    Frame();
    EXPECT_EQ(NeedShown(), Need::InTutorialFolder);
    EXPECT_EQ(tutorial_.CurrentHint()->button, HintButton::BackToTutorial);
}

TEST_F(BasicsChainTest, TheMoveStepSaysAResizeIsNotAMove) {
    world_.Make(1);
    At("move");
    world_.At(1).rect.w *= 2.0f;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialMoveMissResized);
}

TEST_F(BasicsChainTest, FullscreenIsNeitherAMoveNorAResize) {
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

TEST_F(BasicsChainTest, TheMoveStepNeedsASnippetThatCanMoveOutOfDrawingMode) {
    world_.Make(1).fullscreen = true;
    At("move");
    EXPECT_EQ(NeedShown(), Need::SubjectCanMove);
    world_.At(1).fullscreen = false;
    world_.drawing = 1;
    Frame();
    EXPECT_EQ(NeedShown(), Need::NoDrawingMode);
}

TEST_F(BasicsChainTest, WithNothingToPracticeOnAPracticeSnippetBecomesTheSubject) {
    At("move");
    EXPECT_EQ(NeedShown(), Need::ASubject);
    EXPECT_EQ(tutorial_.CurrentHint()->button, HintButton::PutOneHere);
    world_.Make(5, /*picture=*/false);  // what Put one here makes
    Frame();
    EXPECT_EQ(tutorial_.Subject(), 5u);
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(DrawingChainTest, DrawingOnAnotherOfTheTutorialsSnippetsCounts) {
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

TEST_F(DrawingChainTest, TheDrawStepAsksToDrawOnItAgainOutOfDrawingMode) {
    world_.Make(1);
    At("draw");
    EXPECT_EQ(NeedShown(), Need::DrawingOnSubject);
    world_.drawing = 1;
    Frame();
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(BasicsChainTest, DeleteDoesNothingInDrawingModeAndTheStepSaysSo) {
    world_.Make(1);
    world_.drawing = 1;
    At("delete");
    EXPECT_EQ(NeedShown(), Need::NoDrawingMode);
    EXPECT_FALSE(tutorial_.NextEnabled());
}

TEST_F(BasicsChainTest, DeletingTheOnlySnippetStillMovesOn) {
    world_.Make(1);
    At("delete");
    world_.At(1).deleted = true;
    Settle();
    EXPECT_EQ(Id(), "undo");
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());
}

TEST_F(BasicsChainTest, UndoneBeforeItsStepTheUndoStepAsksForADeleteAgain) {
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

TEST_F(BasicsChainTest, TheUndoStepNeedsTheCanvasItWasDeletedOnAndTheKeys) {
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

TEST_F(BasicsChainTest, TheTextsFollowTheTriggersAndTheKeys) {
    const auto& chain = BasicsChain();
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

TEST_F(BasicsChainTest, EveryTextHasItsPlaceholdersFilledIn) {
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
        strings::kTutorialNeedSubjectPinned,    strings::kTutorialPinnedAwayTextTray,
        strings::kTutorialViewModeTextTray,     strings::kTutorialViewModeTextNoKey,
    };
    for (const Topic& topic : Topics()) {
        texts.push_back(topic.title);
        texts.push_back(topic.gist);
        for (const Step& step : topic.chain()) {
            texts.push_back(step.title);
            texts.push_back(step.text(world_));
            for (const NearMiss& miss : step.nearMisses) {
                texts.push_back(miss.text);
            }
        }
    }
    world_.screenshotTrigger = core::CreationTrigger::Ctrl;
    for (const char* text : texts) {
        const std::string expanded = Expand(text, world_, FakeWorld::kCanvas);
        EXPECT_EQ(expanded.find('{'), std::string::npos) << text;
        EXPECT_FALSE(expanded.empty());
    }
}

// What the runner relies on, and docs/TUTORIAL.md, sections 4 and 13
// say, for every topic: ids unique, and every need a step cannot meet by
// itself - one with no button to meet it - met by an earlier step of the
// chain that waits for it. A topic may start with a do step, which a
// need's button makes possible; it ends on a read step, the end card.
TEST(TopicsTest, HaveTheShapeTheRunnerReliesOn) {
    std::set<std::string_view> topicIds;
    for (const Topic& topic : Topics()) {
        EXPECT_TRUE(topicIds.insert(topic.id).second) << topic.id;
        EXPECT_EQ(FindTopic(topic.id), &topic);
        const std::vector<Step>& chain = topic.chain();
        ASSERT_FALSE(chain.empty()) << topic.id;
        std::set<std::string_view> ids;
        std::set<std::string_view> gatedSoFar;
        for (const Step& step : chain) {
            EXPECT_TRUE(ids.insert(step.id).second) << topic.id << "/" << step.id;
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
                if (need == Need::DrawingOnSubject) {
                    EXPECT_TRUE(gatedSoFar.contains("drawingMode")) << topic.id << "/" << step.id;
                }
                if (need == Need::DeletedSubject) {
                    EXPECT_TRUE(gatedSoFar.contains("delete")) << topic.id << "/" << step.id;
                }
                if (need == Need::SubjectPinned) {
                    EXPECT_TRUE(gatedSoFar.contains("pin")) << topic.id << "/" << step.id;
                }
            }
            if (step.gated) {
                gatedSoFar.insert(step.id);
            }
        }
        EXPECT_EQ(chain.back().kind, StepKind::Read) << topic.id;
    }
    EXPECT_EQ(Topics().front().id, kBasicsTopic) << "the first in the list";
    EXPECT_EQ(FindTopic("gone"), nullptr);
}

}  // namespace
}  // namespace sz::ui::tutorial
