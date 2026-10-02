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
        tutorial_.StartAt(id, FakeWorld::kTutorialFolder);
        Frame();
        ASSERT_EQ(Id(), id);
    }
    std::string Id() const { return std::string(tutorial_.CurrentStep().id); }
    std::optional<Need> NeedShown() const {
        return tutorial_.CurrentHint() ? tutorial_.CurrentHint()->need : std::nullopt;
    }
    const char* HintText() const { return tutorial_.CurrentHint() ? tutorial_.CurrentHint()->text : ""; }
    // The tutorial's profile, and those it made, by the names they have
    // now.
    std::string NameOf(core::ProfileId id) const {
        for (const ProfileFacts& facts : world_.profiles) {
            if (facts.id == id) {
                return facts.name;
            }
        }
        return "(gone)";
    }
    std::optional<std::string> TutorialsProfile() const {
        const std::optional<core::ProfileId> profile = tutorial_.Profile();
        return profile ? std::optional<std::string>(NameOf(*profile)) : std::nullopt;
    }
    std::vector<std::string> MadeProfiles() const {
        std::vector<std::string> names;
        for (const core::ProfileId id : tutorial_.MadeProfiles()) {
            names.push_back(NameOf(id));
        }
        return names;
    }

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
    static constexpr uint32_t kGreen = 0x00FF00FFu;

    DrawingChainTest() : ChainTest(DrawingChain()) {}

    // Snippet 1 in drawing mode with the pen, a stroke on it, and the step
    // `id` begun.
    void DrawingAt(std::string_view id) {
        world_.Make(1);
        world_.drawing = 1;
        world_.selection = {1};
        world_.hand = core::Tool::Draw;
        world_.Draw(1);
        At(id);
    }
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

class CapturingChainTest : public ChainTest {
protected:
    CapturingChainTest() : ChainTest(CapturingChain()) {}

    const char* TextOf(std::string_view id) {
        for (const Step& step : CapturingChain()) {
            if (step.id == id) {
                return step.text(world_);
            }
        }
        return "";
    }
    // A snippet made on the current canvas, full screen.
    void MakeFullscreen(core::ItemId id, bool picture) {
        SnippetFacts& made = world_.Make(id, picture);
        made.rect = core::Rect{0.0f, 0.0f, 1920.0f, 1080.0f};
        made.fullscreen = true;
    }
};

class FoldersChainTest : public ChainTest {
protected:
    // A folder made in the run, and its canvas.
    static constexpr core::FolderId kMadeFolder = 3;
    static constexpr core::CanvasId kMadeCanvas = 31;

    FoldersChainTest() : ChainTest(FoldersChain()) {}

    const char* TextOf(std::string_view id) {
        for (const Step& step : FoldersChain()) {
            if (step.id == id) {
                return step.text(world_);
            }
        }
        return "";
    }
    // The Overview up, on its Canvases tab.
    void OverviewUp() {
        world_.cover = Cover::Overview;
        world_.overviewShowsCanvases = true;
    }
    // A folder made at `newFolder`, and the steps after it walked to `id`
    // with Next: the folder stays the tutorial's, which a StartAt would
    // forget.
    void MadeAFolderThenAt(std::string_view id) {
        At("newFolder");
        OverviewUp();
        world_.MakeFolder(kMadeFolder, kMadeCanvas);
        Settle();
        while (Id() != id) {
            tutorial_.Next();
            Frame();
        }
    }
};

class ProfilesChainTest : public ChainTest {
protected:
    ProfilesChainTest() : ChainTest(ProfilesChain()) {}

    const char* TextOf(std::string_view id) {
        for (const Step& step : ProfilesChain()) {
            if (step.id == id) {
                return step.text(world_);
            }
        }
        return "";
    }
    std::string Expanded(const char* text) { return Expand(text, world_, 0, TutorialsProfile().value_or("")); }
    // "Game" made for game.exe at `makeProfile`, and the steps after it
    // walked to `id` - `change` by unticking a row, the rest with Next:
    // the profile stays the tutorial's, which a StartAt would forget.
    void MadeAProfileThenAt(std::string_view id) {
        At("makeProfile");
        world_.OpenSettings(SettingsSection::Profiles);
        world_.MakeProfile("Game");
        Settle();
        while (Id() != id) {
            if (Id() == "change") {
                world_.OpenSettings(SettingsSection::Behavior);
                world_.Profile("Game").stated = 1;
                Settle();
                continue;
            }
            tutorial_.Next();
            Frame();
        }
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
    world_.hand = core::Tool::Draw;
    world_.Draw(5);
    Settle();

    ASSERT_EQ(Id(), "color");
    world_.penColor = kGreen;
    world_.Draw(5);
    Settle();

    ASSERT_EQ(Id(), "width");
    world_.penWidth = 6.0f;
    world_.Draw(5);
    Settle();

    ASSERT_EQ(Id(), "line");
    world_.Draw(5, core::DrawShape::Line);
    Settle();

    ASSERT_EQ(Id(), "rectangle");
    world_.Draw(5, core::DrawShape::Rectangle);
    Settle();

    ASSERT_EQ(Id(), "erase");
    world_.hand = core::Tool::Erase;
    world_.At(5).strokes[0].lengthPx -= 30.0f;
    Settle();

    ASSERT_EQ(Id(), "eraseRect");
    world_.erasedWith = core::DrawShape::Rectangle;
    Frame();
    world_.At(5).strokes[1].lengthPx -= 30.0f;
    Settle();

    ASSERT_EQ(Id(), "eraseRight");
    world_.hand = core::Tool::Draw;
    world_.erasedWith = core::DrawShape::Freehand;
    world_.At(5).strokes[2].lengthPx -= 30.0f;
    Settle();

    ASSERT_EQ(Id(), "note");
    world_.hand = core::Tool::Text;
    world_.At(5).note = "Boss at the gate";
    Settle();

    ASSERT_EQ(Id(), "stopDrawing");
    world_.drawing.reset();
    world_.hand = core::Tool::Select;
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

TEST_F(CapturingChainTest, CanBeWalkedTheWayAUserWould) {
    Frame();
    ASSERT_EQ(Id(), "newDrawing");
    EXPECT_FALSE(NeedShown().has_value()) << "an empty canvas is all it needs";
    EXPECT_TRUE(tutorial_.NextEnabled()) << "no step waits";
    world_.Make(1, /*picture=*/false);
    world_.drawing = 1;  // a drawing comes ready to draw on
    world_.hand = core::Tool::Draw;
    Settle();

    ASSERT_EQ(Id(), "fullscreen");
    EXPECT_FALSE(NeedShown().has_value()) << "a double-click leaves drawing mode";
    world_.drawing.reset();
    MakeFullscreen(2, /*picture=*/true);
    Settle();

    // Each capture on a canvas of its own, in the tutorial's folder.
    ASSERT_EQ(Id(), "quickCapture");
    world_.current = FakeWorld::kSecondCanvas;
    world_.Make(3);
    world_.quickCaptures += 1;
    world_.showings += 1;
    Settle();

    ASSERT_EQ(Id(), "end");
    tutorial_.Next();
    EXPECT_EQ(tutorial_.GetOutcome(), Tutorial::Outcome::Finished);
}

TEST_F(CapturingChainTest, TheDrawingStepSaysWhenAScreenshotWasMade) {
    At("newDrawing");
    world_.Make(1);
    Frame();
    EXPECT_EQ(Id(), "newDrawing");
    EXPECT_STREQ(HintText(), strings::kTutorialNewDrawingMissScreenshot);
    world_.Make(2, /*picture=*/false);
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

// Full screen, of either kind, it covers the canvas: the line says to
// delete it first, and a drawing of the whole screen is not the step's.
TEST_F(CapturingChainTest, TheDrawingStepSaysToDeleteOneMadeFullScreen) {
    At("newDrawing");
    MakeFullscreen(1, /*picture=*/false);
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialNewDrawingMissFullscreen);
    world_.At(1).deleted = true;
    Frame();
    EXPECT_STREQ(HintText(), "");
    MakeFullscreen(2, /*picture=*/true);
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialNewDrawingMissFullscreen);
}

TEST_F(CapturingChainTest, TheDrawingStepNeedsDrawingModeOffAndTheTutorialsFolder) {
    world_.Make(1, /*picture=*/false);
    world_.drawing = 1;
    At("newDrawing");
    EXPECT_EQ(NeedShown(), Need::NoDrawingMode);
    world_.drawing.reset();
    world_.current = FakeWorld::kOtherCanvas;
    Frame();
    EXPECT_EQ(NeedShown(), Need::InTutorialFolder);
}

TEST_F(CapturingChainTest, TheFullScreenStepSaysWhenABoxOrADrawingWasMade) {
    At("fullscreen");
    world_.Make(1);
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialFullscreenMissBox);
    world_.At(1).deleted = true;
    world_.Make(2, /*picture=*/false);
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialFullscreenMissDrawing);
    world_.At(2).deleted = true;
    MakeFullscreen(3, /*picture=*/true);
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

// A drawing of the whole screen covers the canvas, in drawing mode, where
// Delete does nothing: out of that first, then deleted.
TEST_F(CapturingChainTest, TheFullScreenStepTakesAFullScreenDrawingAwayInTwoLines) {
    At("fullscreen");
    MakeFullscreen(1, /*picture=*/false);
    world_.drawing = 1;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialFullscreenMissDrawingMode);
    world_.drawing.reset();
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialFullscreenMissDelete);
    world_.At(1).deleted = true;
    Frame();
    EXPECT_STREQ(HintText(), "");
}

TEST_F(CapturingChainTest, TheFullScreenStepTakesTheScreenshotToolButNoOther) {
    world_.tool = core::ItemCreationKind::Drawing;
    At("fullscreen");
    EXPECT_EQ(NeedShown(), Need::NoOtherTool);
    world_.tool = core::ItemCreationKind::Screenshot;
    Frame();
    EXPECT_FALSE(NeedShown().has_value());
}

// A snippet made before the step began is not the step's.
TEST_F(CapturingChainTest, OnlyWhatIsMadeDuringTheStepCounts) {
    MakeFullscreen(1, /*picture=*/true);
    At("fullscreen");
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), "");
}

// The capture step counts either hotkey's captures since it began - the
// quick one it is about, or the silent one its line offers beside it.
TEST_F(CapturingChainTest, TheCaptureStepCountsEitherHotkey) {
    world_.quickCaptures = 3;
    world_.silentCaptures = 2;
    At("quickCapture");
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "captures from before the step";
    world_.quickCaptures += 1;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());

    At("quickCapture");
    world_.silentCaptures += 1;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), "");
}

TEST_F(CapturingChainTest, TheCaptureStepsNeedTheTutorialsFolder) {
    world_.current = FakeWorld::kOtherCanvas;
    At("quickCapture");
    EXPECT_EQ(NeedShown(), Need::InTutorialFolder);
    world_.current = FakeWorld::kSecondCanvas;
    Frame();
    EXPECT_FALSE(NeedShown().has_value()) << "any canvas of the folder";
}

TEST_F(CapturingChainTest, TheTextsFollowTheTriggersAndTheKeys) {
    EXPECT_STREQ(TextOf("newDrawing"), strings::kTutorialNewDrawingTextTrigger);
    world_.drawingTrigger = core::CreationTrigger::Plain;
    EXPECT_STREQ(TextOf("newDrawing"), strings::kTutorialNewDrawingText);
    world_.drawingTrigger = core::CreationTrigger::Off;
    EXPECT_STREQ(TextOf("newDrawing"), strings::kTutorialNewDrawingTextTool);
    world_.keys.erase(CommandId::NewDrawingTool);
    EXPECT_STREQ(TextOf("newDrawing"), strings::kTutorialNewDrawingTextMenu);

    EXPECT_STREQ(TextOf("fullscreen"), strings::kTutorialFullscreenText);
    world_.screenshotTrigger = core::CreationTrigger::Alt;
    EXPECT_STREQ(TextOf("fullscreen"), strings::kTutorialFullscreenTextTrigger);
    world_.screenshotTrigger = core::CreationTrigger::Off;
    EXPECT_STREQ(TextOf("fullscreen"), strings::kTutorialFullscreenTextTool);
    world_.keys.erase(CommandId::NewScreenshotTool);
    EXPECT_STREQ(TextOf("fullscreen"), strings::kTutorialFullscreenTextMenu);

    // The silent capture's word only while it has a key to name.
    EXPECT_STREQ(TextOf("quickCapture"), strings::kTutorialQuickCaptureText);
    world_.keys.erase(CommandId::ToggleEditMode);
    EXPECT_STREQ(TextOf("quickCapture"), strings::kTutorialQuickCaptureTextTray);
    world_.keys.erase(CommandId::SilentCapture);
    EXPECT_STREQ(TextOf("quickCapture"), strings::kTutorialQuickCaptureTextTrayNoSilent);
    world_.keys[CommandId::ToggleEditMode] = "Ctrl+Alt+S";
    EXPECT_STREQ(TextOf("quickCapture"), strings::kTutorialQuickCaptureTextNoSilent);
    world_.keys.erase(CommandId::QuickCapture);
    EXPECT_STREQ(TextOf("quickCapture"), strings::kTutorialQuickCaptureTextNoKey);
}

TEST_F(FoldersChainTest, CanBeWalkedTheWayAUserWould) {
    Frame();
    ASSERT_EQ(Id(), "newCanvas");
    EXPECT_FALSE(tutorial_.NextEnabled()) << "the next step needs another canvas";
    world_.canvases.push_back(CanvasFacts{13, FakeWorld::kTutorialFolder, "Three"});
    world_.current = 13;
    Settle();

    ASSERT_EQ(Id(), "moveSnippet");
    EXPECT_EQ(NeedShown(), Need::ASubject) << "a new folder has no snippet";
    world_.Make(1);  // on canvas 13
    world_.selection = {1};
    Frame();
    EXPECT_FALSE(NeedShown().has_value());
    // Cut, and pasted on the first canvas.
    world_.current = FakeWorld::kCanvas;
    world_.At(1).canvas = FakeWorld::kCanvas;
    Settle();

    ASSERT_EQ(Id(), "overview");
    OverviewUp();
    Settle();

    ASSERT_EQ(Id(), "newFolder");
    EXPECT_FALSE(tutorial_.NextEnabled()) << "the next steps are about the folder it makes";
    world_.MakeFolder(kMadeFolder, kMadeCanvas);
    Settle();
    EXPECT_EQ(tutorial_.Folders(), (std::vector<core::FolderId>{FakeWorld::kTutorialFolder, kMadeFolder}));

    ASSERT_EQ(Id(), "rename");
    EXPECT_FALSE(NeedShown().has_value()) << "its folder is the tutorial's";
    world_.Folder(kMadeFolder).name = "Games";
    Settle();

    ASSERT_EQ(Id(), "switchFolder");
    world_.overviewFolder = FakeWorld::kTutorialFolder;  // its row
    Settle();

    ASSERT_EQ(Id(), "moveCanvas");
    EXPECT_FALSE(NeedShown().has_value()) << "the Overview still up";
    world_.Canvas(13).folder = kMadeFolder;
    Settle();

    ASSERT_EQ(Id(), "deleteCanvas");
    world_.Canvas(FakeWorld::kCanvas).deleted = true;  // the snippet's
    Settle();

    ASSERT_EQ(Id(), "showDeleted");
    world_.overviewShowsDeleted = true;
    Settle();

    ASSERT_EQ(Id(), "restore");
    EXPECT_FALSE(NeedShown().has_value());
    world_.Canvas(FakeWorld::kCanvas).deleted = false;
    Settle();

    ASSERT_EQ(Id(), "openCanvas");
    world_.current = FakeWorld::kCanvas;  // a tile, which closes the Overview
    world_.cover = Cover::None;
    Settle();

    ASSERT_EQ(Id(), "end");
    tutorial_.Next();
    EXPECT_EQ(tutorial_.GetOutcome(), Tutorial::Outcome::Finished);
}

// Any canvas of the tutorial's new since the step began - the Overview's
// New canvas, or a capture's - and none that was there already.
TEST_F(FoldersChainTest, ANewCanvasIsOneThatWasNotThere) {
    At("newCanvas");
    world_.current = FakeWorld::kSecondCanvas;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "there already";
    world_.current = FakeWorld::kOtherCanvas;
    Frame();
    EXPECT_EQ(NeedShown(), Need::InTutorialFolder);
    world_.canvases.push_back(CanvasFacts{13, FakeWorld::kTutorialFolder, "Three"});
    world_.current = 13;
    world_.cover = Cover::Overview;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet()) << "made in the Overview as well";
}

// A copy pasted onto another canvas keeps its place exactly: the first
// one is still where it was, and the line says to cut it instead.
TEST_F(FoldersChainTest, ACopyPastedIsNotAMove) {
    world_.Make(1);
    At("moveSnippet");
    world_.current = FakeWorld::kSecondCanvas;
    world_.Make(2);  // the copy, in the same place
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialMoveSnippetMissCopy);
    world_.At(2).deleted = true;
    world_.At(1).canvas = FakeWorld::kSecondCanvas;  // cut and pasted
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(FoldersChainTest, TheOverviewsStepsNeedItUpOnItsCanvases) {
    At("newFolder");
    EXPECT_EQ(NeedShown(), Need::OverviewUp);
    world_.cover = Cover::Overview;
    world_.overviewShowsCanvases = false;  // on Settings
    Frame();
    EXPECT_EQ(NeedShown(), Need::CanvasesTab);
    world_.overviewShowsCanvases = true;
    Frame();
    EXPECT_FALSE(NeedShown().has_value());
}

TEST_F(FoldersChainTest, NewCanvasPressedForNewFolderSaysWhichButtonItIs) {
    OverviewUp();
    At("newFolder");
    world_.canvases.push_back(CanvasFacts{13, FakeWorld::kTutorialFolder, "Three"});
    world_.current = 13;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_STREQ(HintText(), strings::kTutorialNewFolderMissCanvas);
}

// The folders made while `newFolder` is up are the tutorial's for the
// rest of the run: a step there is in the tutorial folder. A folder made
// at another step stays the user's (section 17.3).
TEST_F(FoldersChainTest, OnlyTheFoldersMadeAtNewFolderAreTheTutorials) {
    MadeAFolderThenAt("moveCanvas");
    ASSERT_EQ(world_.current, kMadeCanvas);
    EXPECT_FALSE(NeedShown().has_value()) << "in a folder of the tutorial's";
    world_.folders.push_back(FolderFacts{4, "Mine too"});
    world_.canvases.push_back(CanvasFacts{41, 4, "Mine too"});
    world_.current = 41;
    Frame();
    EXPECT_EQ(NeedShown(), Need::InTutorialFolder) << "made at another step";
    EXPECT_EQ(tutorial_.Folders(), (std::vector<core::FolderId>{FakeWorld::kTutorialFolder, kMadeFolder}));

    tutorial_.StartAt("moveCanvas", FakeWorld::kTutorialFolder);
    EXPECT_EQ(tutorial_.Folders(), std::vector<core::FolderId>{FakeWorld::kTutorialFolder})
        << "not kept across a start";
}

TEST_F(FoldersChainTest, ARenameCountsForAFolderOrACanvasOfTheTutorials) {
    OverviewUp();
    At("rename");
    world_.Canvas(FakeWorld::kSecondCanvas).name = "Two";  // the same
    world_.Folder(FakeWorld::kOtherFolder).name = "Not the tutorial's";
    world_.Canvas(FakeWorld::kOtherCanvas).name = "Nor this";
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    world_.Canvas(FakeWorld::kSecondCanvas).name = "Maps";
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());

    OverviewUp();
    At("rename");
    world_.Folder(FakeWorld::kTutorialFolder).name = "Practice";
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

// Back from the new folder: the tutorial's own folder shown in the
// Overview, with no canvas picked - the next steps need the Overview up.
TEST_F(FoldersChainTest, TheWayBackIsToTheTutorialsOwnFolder) {
    MadeAFolderThenAt("switchFolder");
    ASSERT_EQ(world_.overviewFolder, kMadeFolder);
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "still in the new folder";
    world_.overviewFolder = FakeWorld::kOtherFolder;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "not the tutorial's";
    world_.overviewFolder = FakeWorld::kTutorialFolder;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
    EXPECT_EQ(world_.current, kMadeCanvas) << "no canvas picked for it";
}

// The last step: on a canvas of the tutorial's with the Overview closed -
// by its tile, or put away over one. Another folder's tile does not count.
TEST_F(FoldersChainTest, OpenACanvasIsOneOfTheTutorialsWithTheOverviewClosed) {
    OverviewUp();
    At("openCanvas");
    EXPECT_FALSE(NeedShown().has_value());
    world_.current = FakeWorld::kOtherCanvas;
    world_.cover = Cover::None;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "not the tutorial's";
    EXPECT_EQ(NeedShown(), Need::OverviewUp);
    OverviewUp();
    world_.current = FakeWorld::kSecondCanvas;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "the Overview still up";
    world_.cover = Cover::None;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

// A canvas moves into a folder of the tutorial's, or out to one of the
// user's.
TEST_F(FoldersChainTest, ACanvasMovedToAnotherFolderCounts) {
    OverviewUp();
    At("moveCanvas");
    world_.Canvas(FakeWorld::kSecondCanvas).folder = FakeWorld::kOtherFolder;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

// The tutorial's own folder deleted counts, and takes the current canvas
// out of its folders: the trash's steps do not need them.
TEST_F(FoldersChainTest, TheTutorialsFolderDeletedIsDeletedAndRestored) {
    OverviewUp();
    At("deleteCanvas");
    world_.Folder(FakeWorld::kTutorialFolder).deleted = true;
    world_.Canvas(FakeWorld::kCanvas).deleted = true;
    world_.Canvas(FakeWorld::kSecondCanvas).deleted = true;
    world_.current = FakeWorld::kOtherCanvas;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());

    world_.overviewShowsDeleted = true;
    At("restore");
    EXPECT_FALSE(NeedShown().has_value()) << "in the trash, and no need for the tutorial's folder";
    world_.Folder(FakeWorld::kTutorialFolder).deleted = false;
    world_.Canvas(FakeWorld::kCanvas).deleted = false;
    world_.Canvas(FakeWorld::kSecondCanvas).deleted = false;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(FoldersChainTest, RestoreNeedsShowDeletedAndSomethingInTheTrash) {
    OverviewUp();
    At("restore");
    EXPECT_EQ(NeedShown(), Need::DeletedShown);
    world_.overviewShowsDeleted = true;
    Frame();
    EXPECT_EQ(NeedShown(), Need::SomethingInTrash);
    world_.Canvas(FakeWorld::kOtherCanvas).deleted = true;
    Frame();
    EXPECT_EQ(NeedShown(), Need::SomethingInTrash) << "not the tutorial's";
    world_.Canvas(FakeWorld::kSecondCanvas).deleted = true;
    Frame();
    EXPECT_FALSE(NeedShown().has_value());
    // Deleted for good: gone, and nothing left in the trash.
    world_.canvases.erase(world_.canvases.begin() + 1);
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_EQ(NeedShown(), Need::SomethingInTrash);
}

TEST_F(FoldersChainTest, TheTextsFollowTheCanvasBarAndTheKeys) {
    EXPECT_STREQ(TextOf("newCanvas"), strings::kTutorialNewCanvasText);
    EXPECT_STREQ(TextOf("moveSnippet"), strings::kTutorialMoveSnippetText);
    EXPECT_STREQ(TextOf("overview"), strings::kTutorialOverviewText);
    world_.canvasBarOn = false;
    EXPECT_STREQ(TextOf("newCanvas"), strings::kTutorialNewCanvasTextOverview);
    world_.keys[CommandId::NewCanvas] = "Ctrl+N";
    EXPECT_STREQ(TextOf("newCanvas"), strings::kTutorialNewCanvasTextKey);
    EXPECT_STREQ(TextOf("moveSnippet"), strings::kTutorialMoveSnippetTextNoBar);
    EXPECT_STREQ(TextOf("overview"), strings::kTutorialOverviewTextMenu);
    world_.keys.erase(CommandId::Paste);
    EXPECT_STREQ(TextOf("moveSnippet"), strings::kTutorialMoveSnippetTextMenuNoBar);
    world_.canvasBarOn = true;
    EXPECT_STREQ(TextOf("moveSnippet"), strings::kTutorialMoveSnippetTextMenu);
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

    world_.Draw(1);  // something drawn: now it shows
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

    tutorial_.StartAt("end", FakeWorld::kTutorialFolder);
    tutorial_.Leave();
    EXPECT_EQ(tutorial_.Progress(), "finished");

    tutorial_.StartAt("draw", FakeWorld::kTutorialFolder);
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

// Only a color that shows, and only once something is drawn with it.
TEST_F(DrawingChainTest, TheColorCountsOnceAStrokeShowsIt) {
    DrawingAt("color");
    world_.penColor = 0xF01010FFu;  // a nudge from red
    world_.Draw(1);
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    EXPECT_FALSE(tutorial_.CurrentHint().has_value());

    world_.penColor = kGreen;
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialColorMissNothingDrawn);
    world_.Draw(1);
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(DrawingChainTest, AStrokeInAnotherColorFromBeforeTheStepDoesNotCount) {
    world_.Make(1);
    world_.drawing = 1;
    world_.hand = core::Tool::Draw;
    world_.Draw(1);  // red, drawn earlier
    world_.penColor = kGreen;
    At("color");
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    world_.penColor = 0xFF0000FFu;
    world_.Draw(1);
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet()) << "red is new against the green in hand at the start";
}

TEST_F(DrawingChainTest, TheColorWidthAndShapesNeedThePenInHand) {
    for (const std::string_view id : {"color", "width", "line", "rectangle"}) {
        SCOPED_TRACE(id);
        world_ = FakeWorld{};
        DrawingAt(id);
        world_.hand = core::Tool::Erase;
        Frame();
        EXPECT_EQ(NeedShown(), Need::PenInHand);
        world_.hand = core::Tool::Draw;
        Frame();
        EXPECT_FALSE(tutorial_.CurrentHint().has_value());
    }
}

TEST_F(DrawingChainTest, TheWidthCountsTwoNotchesDrawnWith) {
    DrawingAt("width");
    world_.penWidth = 4.0f;  // one notch
    world_.Draw(1);
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());

    world_.penWidth = 1.0f;  // two the other way
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialWidthMissNothingDrawn);
    world_.Draw(1);
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(DrawingChainTest, TheWidthSaysWhenTheWheelChangedTheOpacityInstead) {
    DrawingAt("width");
    world_.At(1).pictureOpacity -= 0.15f;
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialWidthMissOpacity);
    EXPECT_FALSE(tutorial_.GoalMet());
}

TEST_F(DrawingChainTest, TheShapesSayWhenAnotherWasDrawn) {
    DrawingAt("line");
    world_.Draw(1);
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialLineMissFreehand);
    world_.Draw(1, core::DrawShape::Rectangle);
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "a rectangle is not a line";
    world_.Draw(1, core::DrawShape::Line);
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());

    At("rectangle");
    world_.Draw(1, core::DrawShape::Line);
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialRectangleMissLine);
    world_.Draw(1, core::DrawShape::Rectangle);
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

// The eraser cuts a stroke into pieces: more strokes, less ink.
TEST_F(DrawingChainTest, TheEraseStepCountsTheInkGoneNotTheStrokes) {
    DrawingAt("erase");
    world_.hand = core::Tool::Erase;
    world_.At(1).strokes[0].lengthPx = 45.0f;
    world_.Draw(1, core::DrawShape::Freehand, 50.0f);  // the other piece
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "5 px is a touch";
    world_.At(1).strokes[1].lengthPx = 30.0f;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

// Begun on a snippet with nothing drawn on it - cleared, undone. Clearing
// it during the step is ink gone, which counts.
TEST_F(DrawingChainTest, TheErasingStepsNeedSomethingDrawn) {
    for (const std::string_view id : {"erase", "eraseRect", "eraseRight"}) {
        SCOPED_TRACE(id);
        world_ = FakeWorld{};
        world_.Make(1);
        world_.drawing = 1;
        world_.hand = core::Tool::Erase;
        At(id);
        EXPECT_EQ(NeedShown(), Need::SubjectDrawnOn);
    }
}

// What the ink went with: the round eraser first, and a rectangle erased
// after it, counts only what the rectangle took.
TEST_F(DrawingChainTest, TheRectangleEraserCountsOnlyWhatItErased) {
    DrawingAt("eraseRect");
    world_.hand = core::Tool::Erase;
    world_.At(1).strokes[0].lengthPx -= 40.0f;
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialEraseRectMissRound);
    world_.erasedWith = core::DrawShape::Rectangle;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    world_.At(1).strokes[0].lengthPx -= 20.0f;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(DrawingChainTest, TheRightButtonCountsWhatWentWithoutTheEraserInHand) {
    DrawingAt("eraseRight");
    world_.hand = core::Tool::Erase;
    world_.erasedWith = core::DrawShape::Rectangle;
    Frame();
    EXPECT_FALSE(tutorial_.CurrentHint().has_value()) << "the eraser in hand is where the step begins";
    world_.At(1).strokes[0].lengthPx -= 40.0f;
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialEraseRightMissEraser);
    world_.hand = core::Tool::Draw;
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet());
    world_.At(1).strokes[0].lengthPx -= 20.0f;
    Frame();
    EXPECT_TRUE(tutorial_.GoalMet());
}

TEST_F(DrawingChainTest, TheNoteCountsOnceTypedAndSaysSoWhileTyping) {
    world_.Make(1);
    world_.At(1).note = "old";
    world_.drawing = 1;
    At("note");
    world_.hand = core::Tool::Text;
    world_.typing = 1;
    world_.At(1).note = "old, and more";
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "still being typed";
    EXPECT_STREQ(HintText(), strings::kTutorialNoteMissTyping);
    world_.At(1).note = "old";
    world_.typing.reset();
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "the same note as before";
    world_.At(1).note.clear();
    Frame();
    EXPECT_FALSE(tutorial_.GoalMet()) << "no note at all";
    world_.At(1).note = "new";
    Frame();
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

TEST_F(ProfilesChainTest, CanBeWalkedTheWayAUserWould) {
    Frame();
    ASSERT_EQ(Id(), "openProfiles");
    world_.OpenSettings(SettingsSection::Profiles);
    Settle();

    ASSERT_EQ(Id(), "makeProfile");
    EXPECT_FALSE(tutorial_.NextEnabled()) << "it waits";
    world_.MakeProfile("Game");
    Settle();

    ASSERT_EQ(Id(), "behavior");
    EXPECT_EQ(TutorialsProfile(), "Game");
    world_.section = SettingsSection::Behavior;
    Settle();

    ASSERT_EQ(Id(), "change");
    EXPECT_FALSE(tutorial_.NextEnabled()) << "it waits";
    EXPECT_EQ(Expanded(TextOf("change")).find("{"), std::string::npos);
    world_.Profile("Game").stated = 1;
    Settle();

    ASSERT_EQ(Id(), "revert");
    EXPECT_EQ(NeedShown(), std::nullopt);
    world_.showing.reset();
    Frame();
    EXPECT_EQ(NeedShown(), Need::ShowingIt);
    world_.showing = "Game";
    Frame();
    EXPECT_EQ(NeedShown(), std::nullopt);
    world_.Profile("Game").stated = 0;
    Settle();

    ASSERT_EQ(Id(), "end");
    tutorial_.Done();
    EXPECT_EQ(tutorial_.GetOutcome(), Tutorial::Outcome::Finished);
    EXPECT_EQ(MadeProfiles(), std::vector<std::string>{"Game"});
}

TEST_F(ProfilesChainTest, ABlankProfileIsNotItAndTheOneForTheProgramIs) {
    At("makeProfile");
    world_.OpenSettings(SettingsSection::Profiles);
    world_.MakeProfile("Profile 1", false);
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialMakeProfileMissBlank);
    EXPECT_EQ(TutorialsProfile(), "Profile 1") << "the only one there is";
    world_.MakeProfile("Game");
    Settle();
    EXPECT_EQ(Id(), "behavior");
    EXPECT_EQ(TutorialsProfile(), "Game");
    EXPECT_EQ(MadeProfiles(), (std::vector<std::string>{"Profile 1", "Game"}));
}

TEST_F(ProfilesChainTest, AProfileMadeAsProfilesOpensIsTheTutorials) {
    Frame();
    world_.OpenSettings(SettingsSection::Profiles);
    Frame();  // met, and the second it waits begun
    world_.MakeProfile("Game");
    Settle();  // on to makeProfile
    Settle();  // met as it began
    EXPECT_EQ(Id(), "behavior") << "makeProfile met by the one made";
    EXPECT_EQ(TutorialsProfile(), "Game");
}

TEST_F(ProfilesChainTest, AProgramWithAProfileAlreadySaysTheNewOneWillNotRun) {
    world_.MakeProfile("Mine");
    world_.showing.reset();
    At("makeProfile");
    world_.OpenSettings(SettingsSection::Profiles);
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialMakeProfileMissTaken);
    EXPECT_NE(Expanded(HintText()).find("Mine"), std::string::npos);
    world_.MakeProfile("Game");
    EXPECT_FALSE(world_.Profile("Game").running);
    Settle();
    EXPECT_EQ(Id(), "behavior") << "it matches, running or not";
    EXPECT_EQ(TutorialsProfile(), "Game");
}

// A profile of the user's renamed while makeProfile is up is not one
// made: it is not the tutorial's, and Done leaves it (section 18.3).
TEST_F(ProfilesChainTest, AProfileOfTheUsersRenamedIsNotOneMade) {
    world_.MakeProfile("Mine");
    world_.showing.reset();
    At("makeProfile");
    world_.OpenSettings(SettingsSection::Profiles);
    world_.Profile("Mine").name = "Mine 2";
    Frame();
    EXPECT_EQ(TutorialsProfile(), std::nullopt);
    EXPECT_TRUE(MadeProfiles().empty());
    world_.MakeProfile("Game");
    Settle();
    EXPECT_EQ(Id(), "behavior");
    EXPECT_EQ(TutorialsProfile(), "Game");
    EXPECT_EQ(MadeProfiles(), std::vector<std::string>{"Game"});
}

// And the tutorial's own, renamed, is still the tutorial's.
TEST_F(ProfilesChainTest, TheTutorialsProfileRenamedIsStillTheTutorials) {
    MadeAProfileThenAt("behavior");
    world_.Profile("Game").name = "My game";
    Frame();
    EXPECT_EQ(TutorialsProfile(), "My game");
    EXPECT_NE(NeedShown(), Need::TutorialsProfile);
    EXPECT_NE(Expanded("{profile}").find("My game"), std::string::npos);
}

TEST_F(ProfilesChainTest, OnlyAProfileMadeAtMakeProfileIsTheTutorials) {
    At("behavior");
    world_.OpenSettings(SettingsSection::Profiles);
    world_.MakeProfile("Game");
    Frame();
    EXPECT_EQ(TutorialsProfile(), std::nullopt);
    EXPECT_EQ(NeedShown(), Need::TutorialsProfile);
}

TEST_F(ProfilesChainTest, TheNeedsSayWhereInSettingsToGo) {
    MadeAProfileThenAt("behavior");
    world_.section = SettingsSection::Behavior;
    Settle();
    ASSERT_EQ(Id(), "change");

    world_.cover = Cover::None;
    Frame();
    EXPECT_EQ(NeedShown(), Need::SettingsUp);
    world_.cover = Cover::Overview;
    world_.overviewShowsSettings = false;
    Frame();
    EXPECT_EQ(NeedShown(), Need::SettingsTab);
    world_.OpenSettings(SettingsSection::Profiles);
    Frame();
    EXPECT_EQ(NeedShown(), Need::BehaviorSection);
    world_.OpenSettings(SettingsSection::Behavior);
    world_.showing.reset();
    Frame();
    EXPECT_EQ(NeedShown(), Need::ShowingIt);
    EXPECT_NE(Expanded(HintText()).find(strings::kProfilesDefaults), std::string::npos);
    EXPECT_EQ(tutorial_.CurrentSpot(), Spot::Showing) << "rung while its line is up";
    world_.showing = "Game";
    Frame();
    EXPECT_EQ(NeedShown(), std::nullopt);
}

TEST_F(ProfilesChainTest, TheTutorialsProfileDeletedIsTheNeedForOne) {
    MadeAProfileThenAt("revert");
    world_.profiles.clear();
    Frame();
    EXPECT_EQ(NeedShown(), Need::TutorialsProfile);
}

TEST_F(ProfilesChainTest, NothingUnderneathIsTheNeedForAProgram) {
    world_.underneath.clear();
    At("makeProfile");
    world_.OpenSettings(SettingsSection::Profiles);
    Frame();
    EXPECT_EQ(NeedShown(), Need::AProgramUnderneath);
    EXPECT_STREQ(HintText(), strings::kTutorialNeedAProgramUnderneath);
    world_.keys.erase(CommandId::ToggleEditMode);
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialNeedAProgramUnderneathTray);
}

TEST_F(ProfilesChainTest, ARowTickedBackIsNotHandedBack) {
    MadeAProfileThenAt("revert");
    world_.Profile("Game").statedAsDefaults = 1;
    Frame();
    EXPECT_STREQ(HintText(), strings::kTutorialRevertMissTickedBack);
    world_.Profile("Game").stated = 0;
    world_.Profile("Game").statedAsDefaults = 0;
    Settle();
    EXPECT_EQ(Id(), "end");
}

TEST_F(ProfilesChainTest, RevertNeedsSomethingSetInTheProfile) {
    MadeAProfileThenAt("revert");
    world_.Profile("Game").stated = 0;
    tutorial_.Back();
    Frame();
    tutorial_.Next();
    Frame();
    ASSERT_EQ(Id(), "revert");
    EXPECT_EQ(NeedShown(), Need::SomethingSetInIt);
}

TEST_F(ProfilesChainTest, TheTextFollowsTheOverview) {
    EXPECT_STREQ(TextOf("openProfiles"), strings::kTutorialOpenProfilesText);
    world_.cover = Cover::Overview;
    EXPECT_STREQ(TextOf("openProfiles"), strings::kTutorialOpenProfilesTextOverview);
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
        strings::kTutorialNewDrawingText,       strings::kTutorialNewDrawingTextTool,
        strings::kTutorialNewDrawingTextMenu,   strings::kTutorialFullscreenText,
        strings::kTutorialFullscreenTextTool,   strings::kTutorialFullscreenTextMenu,
        strings::kTutorialQuickCaptureTextTray, strings::kTutorialQuickCaptureTextNoKey,
        strings::kTutorialQuickCaptureTextNoSilent, strings::kTutorialQuickCaptureTextTrayNoSilent,
        strings::kTutorialNeedOverviewUp,       strings::kTutorialNeedCanvasesTab,
        strings::kTutorialNeedDeletedShown,     strings::kTutorialNeedSomethingInTrash,
        strings::kTutorialNewCanvasTextKey,     strings::kTutorialNewCanvasTextOverview,
        strings::kTutorialMoveSnippetTextNoBar, strings::kTutorialMoveSnippetTextMenu,
        strings::kTutorialMoveSnippetTextMenuNoBar, strings::kTutorialOverviewTextMenu,
        strings::kTutorialNeedSettingsUp,       strings::kTutorialNeedSettingsTab,
        strings::kTutorialNeedProfilesSection,  strings::kTutorialNeedBehaviorSection,
        strings::kTutorialNeedAProgramUnderneath, strings::kTutorialNeedAProgramUnderneathTray,
        strings::kTutorialNeedTutorialsProfile, strings::kTutorialNeedShowingIt,
        strings::kTutorialNeedSomethingSetInIt, strings::kTutorialOpenProfilesTextOverview,
    };
    world_.keys[CommandId::NewCanvas] = "Ctrl+N";
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
            // A read step may have needs: a line for when its words do not
            // hold, as Profiles' `elsewhere` has (docs/TUTORIAL.md, 18.8).
            if (step.kind == StepKind::Read) {
                EXPECT_EQ(step.goal, nullptr) << step.id;
                EXPECT_FALSE(step.gated) << step.id;
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
