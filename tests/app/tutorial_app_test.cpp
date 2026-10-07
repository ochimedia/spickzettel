// The tutorial in the running app - docs/TUTORIAL.md, section 9: the
// anchors the owners mark as they draw, what the app answers the tutorial,
// the card and the spotlight, and the welcome chain walked through with
// real gestures.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/config/settings_catalog.h"
#include "fakes/headless_app.h"
#include "support/failing_writes.h"
#include "generated/ui_strings.h"
#include "support/view_stack.h"
#include "ui/tutorial/topics.h"

namespace sz::test {
namespace {

class TutorialAppTest : public HeadlessAppTest {
protected:
    OverlayApp& Overlay() { return controller_->Overlay(); }

    // A screenshot framed on empty canvas, left selected.
    ItemId MakeASnippet(float fromX, float fromY, float toX, float toY) {
        Drag(fromX, fromY, toX, toY);
        return Canvases().CurrentOrNull()->items.back().id;
    }

    // A spot left empty by the steps that make snippets there, and clear
    // of the card.
    static constexpr ImVec2 kEmptySpot{900.0f, 450.0f};

    static ImVec2 Center(const AnchorRect& rect) {
        return ImVec2((rect.min.x + rect.max.x) * 0.5f, (rect.min.y + rect.max.y) * 0.5f);
    }

    // ===== The tutorial =====

    const tutorial::Tutorial& Runner() const { return App().TutorialRunner(); }
    std::string StepUp() const {
        return Runner().GetState() == tutorial::Tutorial::State::OnStep ? std::string(Runner().CurrentStep().id)
                                                                         : "(" + Runner().Progress() + ")";
    }
    // The hint under the step's text, or empty.
    std::string HintUp() const {
        const std::optional<tutorial::Hint>& hint = Runner().CurrentHint();
        return hint.has_value() ? std::string(hint->text) : std::string();
    }
    std::optional<tutorial::Need> NeedUp() const {
        const std::optional<tutorial::Hint>& hint = Runner().CurrentHint();
        return hint.has_value() ? hint->need : std::nullopt;
    }

    // Edit mode, and a topic at its first step in a folder of its own -
    // or, for Profiles, in none, over a program it can make a profile for;
    // or, for Basics, none yet, until its welcome is read (section 13.5).
    void StartTheTutorial(std::string_view topic = tutorial::kBasicsTopic) {
        if (topic == "profiles" && !host_.overlayWindow.underlyingApp.Known()) {
            host_.overlayWindow.underlyingApp = kGame;
        }
        ShowEditMode();
        StepFrame();
        Overlay().StartTutorial(topic);
        StepFrames(2);
        ASSERT_TRUE(Runner().On());
        if (tutorial::FindTopic(topic)->folder && Runner().DoStepReached()) {
            ASSERT_EQ(App().TutorialWorld().FolderOf(Canvases().CurrentCanvasId()), Runner().Folder());
        } else {
            ASSERT_EQ(Runner().Folder(), 0u);
        }
    }
    // The program the overlay comes up over in Profiles' steps.
    inline static const platform::ForegroundApp kGame{"game.exe", "Game"};
    // The overlay put away, `app` clicked, and the overlay brought back
    // over it.
    void ComeBackOver(const platform::ForegroundApp& app) {
        ShowEditMode();  // away
        StepFrame();
        host_.overlayWindow.underlyingApp = app;
        ShowEditMode();  // and back
        StepFrame();
    }
    // A profile's place in the list, by name.
    size_t ProfileIndex(const std::string& name) const {
        const std::vector<Profile>& profiles = AppSettings().Profiles();
        for (size_t i = 0; i < profiles.size(); ++i) {
            if (profiles[i].name == name) {
                return i;
            }
        }
        ADD_FAILURE() << "no profile " << name;
        return 0;
    }
    // One of the Settings panel's sections, pressed in its list.
    void PickSection(SettingsPage::SettingsSection section) {
        ClickAnchor(Anchor{AnchorId::SettingsSection, static_cast<uint64_t>(section)});
    }
    // The tutorial's profile, and those it made, by the names they have
    // now.
    std::string NameOf(core::ProfileId id) const {
        for (const Profile& profile : AppSettings().Profiles()) {
            if (profile.id == id) {
                return profile.name;
            }
        }
        return "(gone)";
    }
    std::optional<std::string> TutorialsProfile() const {
        const std::optional<core::ProfileId> profile = Runner().Profile();
        return profile ? std::optional<std::string>(NameOf(*profile)) : std::nullopt;
    }
    std::vector<std::string> MadeProfiles() const {
        std::vector<std::string> names;
        for (const core::ProfileId id : Runner().MadeProfiles()) {
            names.push_back(NameOf(id));
        }
        return names;
    }
    // Showing's list opened, and the entry of profile `name` picked - or
    // the defaults, for none.
    void PickShowing(std::optional<std::string> name) {
        ClickAnchor(Anchor{AnchorId::SettingsShowing});
        StepFrames(2);
        ClickAnchor(Anchor{AnchorId::SettingsShowingEntry, name ? ProfileIndex(*name) + 1 : 0});
        StepFrames(2);
    }
    // The card dragged by its top edge, as a hand moves a window.
    void DragCard(float dx, float dy) {
        const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
        ASSERT_NE(card, nullptr);
        const ImVec2 from(card->Pos.x + card->Size.x - 12.0f, card->Pos.y + 4.0f);
        MoveTo(from.x, from.y);
        StepFrame();
        MouseButtonEvent(ImGuiMouseButton_Left, true);
        StepFrame();
        for (int i = 1; i <= 4; ++i) {
            MoveTo(from.x + dx * static_cast<float>(i) / 4.0f, from.y + dy * static_cast<float>(i) / 4.0f);
            StepFrame();
        }
        MouseButtonEvent(ImGuiMouseButton_Left, false);
        StepFrames(2);
    }
    // Done, with the end or skip card's Keep checkbox ticked.
    void DoneKeeping() {
        Press(TutorialButton::Keep);
        Press(TutorialButton::Done);
    }
    // Long enough for a step whose goal is met to move on by itself.
    void Settle() { StepFrames(75); }
    void Press(TutorialButton button) {
        Overlay().PressTutorial(button);
        StepFrames(2);
    }

    // What is kept of `topic`, or empty for a topic never started.
    std::string Kept(std::string_view topic = tutorial::kBasicsTopic) const {
        const auto& progress = AppSettings().Stored().tutorialProgress;
        const auto it = progress.find(std::string(topic));
        return it == progress.end() ? std::string() : it->second;
    }

    // The subject as the model has it now.
    const Item& Subject() const { return *Canvases().FindItemAnywhere(*Runner().Subject()); }
    ImVec2 SubjectMiddle() const {
        const Rect& r = Subject().rect;
        return ImVec2(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    }

    // A subject, selected: the practice snippet put here if there is none.
    void SelectTheSubject() {
        if (!Runner().Subject().has_value()) {
            Overlay().PressTutorialHint();  // Put one here
            StepFrames(3);
        }
        if (!App().IsSelected(Subject().id)) {
            RawClick(SubjectMiddle().x, SubjectMiddle().y);
        }
    }
    void PressBarPin() { PressBar(ChromeButton::Pin); }
    // A button of the selection bar.
    void PressBar(ChromeButton button) {
        const std::optional<ImVec2> center = App().SelectionBarButtonCenter(button);
        ASSERT_TRUE(center.has_value());
        RawClick(center->x, center->y);
    }
    // Row `row` of the `rows` in the menu of the pen's or the eraser's
    // button, opened with a right click on it. The menu is anonymous, so
    // the row is clicked where it stands: rows of one height, top to bottom.
    // First the card is let come to rest, as a hand waits for it: a line
    // just shown under it can send it sliding across the bar.
    void PickFromShapeMenu(ChromeButton button, int row, int rows) {
        Settle();
        const std::optional<ImVec2> center = App().SelectionBarButtonCenter(button);
        ASSERT_TRUE(center.has_value());
        RightClick(center->x, center->y);
        StepFrames(2);
        ASSERT_TRUE(App().IsShapeMenuOpen());
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        ASSERT_FALSE(g.OpenPopupStack.empty());
        const ImRect inner = g.OpenPopupStack.back().Window->InnerRect;
        const float rowH = inner.GetHeight() / static_cast<float>(rows);
        Click(inner.GetCenter().x, inner.Min.y + rowH * (static_cast<float>(row) + 0.5f));
        StepFrames(2);
    }
    // A stroke across the subject, `dy` below its middle.
    void DrawAcross(float dy = 0.0f) {
        const ImVec2 middle = SubjectMiddle();
        Drag(middle.x - 60.0f, middle.y + dy, middle.x + 60.0f, middle.y + dy);
    }
    // A color picked in the chooser: near white, by the top left of its
    // square - to ImGui alone, as a widget is pressed.
    void PickAColor() {
        PressBar(ChromeButton::Color);
        StepFrames(2);
        ASSERT_TRUE(App().IsColorChooserOpen());
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        ASSERT_FALSE(g.OpenPopupStack.empty());
        const ImRect square = g.OpenPopupStack.back().Window->InnerRect;
        Click(square.Min.x + 30.0f, square.Min.y + 30.0f);
    }

    // A widget the tutorial may point at, clicked where its owner marked
    // it this frame.
    void ClickAnchor(Anchor anchor) {
        const std::optional<AnchorRect> rect = App().AnchorAt(anchor);
        ASSERT_TRUE(rect.has_value()) << "not on screen: " << static_cast<int>(anchor.id);
        Click(Center(*rect).x, Center(*rect).y);
    }
    // The canvas bar, out once the pointer is at the bottom edge, and one
    // of its buttons pressed.
    void PressOnCanvasBar(AnchorId button) {
        MoveTo(kDisplayWidth * 0.5f, kDisplayHeight - 1.0f);
        for (int i = 0; i < 60 && App().CanvasBarReveal() < 1.0f; ++i) {
            StepFrame();
        }
        StepFrame();
        ClickAnchor(Anchor{button});
    }
    // Held Alt and the wheel, to the canvas next to this one in its
    // folder: back, or on where there is none before it.
    void ToTheOtherCanvas() {
        const CanvasId before = Canvases().CurrentCanvasId();
        MoveTo(kEmptySpot.x, kEmptySpot.y);
        With(ImGuiMod_Alt, [&] { Wheel(1.0f); });
        StepFrames(2);
        if (Canvases().CurrentCanvasId() == before) {
            With(ImGuiMod_Alt, [&] { Wheel(-1.0f); });
            StepFrames(2);
        }
    }
    // A widget dragged onto another, as ImGui's drag and drop takes it:
    // pressed, moved over in steps, and let go of there.
    void DragWidget(ImVec2 from, ImVec2 to) {
        MoveTo(from.x, from.y);
        StepFrame();
        MouseButtonEvent(ImGuiMouseButton_Left, true);
        StepFrame();
        for (int i = 1; i <= 8; ++i) {
            const float t = static_cast<float>(i) / 8.0f;
            MoveTo(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
            StepFrame();
        }
        StepFrame();
        MouseButtonEvent(ImGuiMouseButton_Left, false);
        StepFrames(3);
    }
    // A name typed into the field that has the keyboard, and Enter.
    void TypeAName(const std::string& name) {
        for (const char letter : name) {
            ImGui::GetIO().AddInputCharacter(letter);
            StepFrame();
        }
        PressKey(ImGuiKey_Enter);
        StepFrames(2);
    }
    // The canvases of a folder not in the trash, and whether a snippet not
    // deleted is on a canvas.
    std::vector<CanvasId> LiveCanvasesIn(FolderId folder) const {
        std::vector<CanvasId> ids;
        for (const Canvas& canvas : Canvases().Canvases()) {
            if (canvas.folderId == folder && !Canvases().IsDeleted(canvas)) {
                ids.push_back(canvas.id);
            }
        }
        return ids;
    }
    bool HoldsASnippet(CanvasId id) const {
        const Canvas* canvas = Canvases().FindCanvas(id);
        return canvas != nullptr && std::any_of(canvas->items.begin(), canvas->items.end(), [&](const Item& item) {
                   return !Canvases().IsDeleted(*canvas, item);
               });
    }

    // What a hand does for each step of every topic, as the step's text
    // says it: real gestures, keys and hotkeys.
    void DoStep(const std::string& id) {
        if (id == "welcome" || id == "programs" || id == "antiCheat" || id == "exclusiveFullscreen") {
            Press(TutorialButton::Next);
        } else if (id == "screenshot") {
            Drag(300.0f, 360.0f, 600.0f, 560.0f);
        } else if (id == "move") {
            const ImVec2 from = SubjectMiddle();
            Drag(from.x, from.y, from.x + 120.0f, from.y - 60.0f);
        } else if (id == "resize") {
            if (!App().IsSelected(Subject().id)) {
                RawClick(SubjectMiddle().x, SubjectMiddle().y);
            }
            const Rect r = Subject().rect;
            // On the bottom-right corner's handle.
            const float x = std::round(r.x + r.w);
            const float y = std::round(r.y + r.h);
            Drag(x, y, x + 80.0f, y + 60.0f);
        } else if (id == "drawingMode") {
            if (!Runner().Subject().has_value()) {
                Overlay().PressTutorialHint();  // Put one here
                StepFrames(3);
            }
            DoubleClick(SubjectMiddle().x, SubjectMiddle().y);
        } else if (id == "draw") {
            const ImVec2 middle = SubjectMiddle();
            Drag(middle.x - 60.0f, middle.y - 20.0f, middle.x + 60.0f, middle.y + 20.0f);
        } else if (id == "color") {
            PickAColor();
            PressKey(ImGuiKey_Escape);  // the chooser closed
            DrawAcross(30.0f);
        } else if (id == "width") {
            MoveTo(SubjectMiddle().x, SubjectMiddle().y);
            Wheel(1.0f);
            Wheel(1.0f);
            Wheel(1.0f);
            DrawAcross(-30.0f);
        } else if (id == "line") {
            PickFromShapeMenu(ChromeButton::Pen, 1, 3);  // Pen, Line, Rectangle
            DrawAcross(50.0f);
        } else if (id == "rectangle") {
            PickFromShapeMenu(ChromeButton::Pen, 2, 3);
            const ImVec2 middle = SubjectMiddle();
            Drag(middle.x - 80.0f, middle.y - 60.0f, middle.x - 20.0f, middle.y + 60.0f);
        } else if (id == "erase") {
            PressBar(ChromeButton::Eraser);
            const ImVec2 middle = SubjectMiddle();
            Drag(middle.x, middle.y - 80.0f, middle.x, middle.y + 80.0f);
        } else if (id == "eraseRect") {
            PickFromShapeMenu(ChromeButton::Eraser, 1, 2);  // Eraser, Rectangle eraser
            const ImVec2 middle = SubjectMiddle();
            Drag(middle.x + 20.0f, middle.y - 80.0f, middle.x + 50.0f, middle.y + 80.0f);
        } else if (id == "eraseRight") {
            PressBar(ChromeButton::Pen);
            const ImVec2 middle = SubjectMiddle();
            Drag(middle.x - 40.0f, middle.y - 80.0f, middle.x - 40.0f, middle.y + 80.0f, 4,
                 platform::MouseButton::Right);
        } else if (id == "note") {
            PressBar(ChromeButton::Text);
            RawClick(SubjectMiddle().x, SubjectMiddle().y);
            for (const char letter : std::string("gate")) {
                ImGui::GetIO().AddInputCharacter(letter);
                StepFrame();
            }
            PressKey(ImGuiKey_Escape);  // the typing ended
        } else if (id == "stopDrawing") {
            RawClick(1200.0f, 120.0f);  // outside it
        } else if (id == "delete") {
            RawClick(SubjectMiddle().x, SubjectMiddle().y);
            PressKey(ImGuiKey_Delete);
        } else if (id == "undo") {
            PressCtrlKey(ImGuiKey_Z);
        } else if (id == "away") {
            ShowEditMode();  // away
            StepFrame();
            ShowEditMode();  // and back
            StepFrame();
        } else if (id == "pin" || id == "unpin") {
            SelectTheSubject();
            PressBarPin();
        } else if (id == "pinnedAway") {
            ShowEditMode();  // away, to the pinned view
            StepFrame();
            ShowEditMode();  // and back
            StepFrame();
        } else if (id == "opacity") {
            // Properties up holds the wheel, which its own sliders stand in
            // for: put away, the wheel is the canvas's again.
            if (App().InputStack().find("ItemProperties") != std::string::npos) {
                PressKey(ImGuiKey_Escape);
            }
            SelectTheSubject();
            MoveTo(SubjectMiddle().x, SubjectMiddle().y);
            KeyEvent(ImGuiMod_Ctrl, true);
            Wheel(-1.0f);
            Wheel(-1.0f);
            Wheel(-1.0f);
            KeyEvent(ImGuiMod_Ctrl, false);
            StepFrame();
        } else if (id == "viewMode") {
            ShowViewMode();
            StepFrame();
            ShowEditMode();  // back
            StepFrame();
        } else if (id == "newDrawing") {
            MakeADrawing(300.0f, 360.0f, 600.0f, 560.0f);
        } else if (id == "fullscreen") {
            DoubleClick(kEmptySpot.x, kEmptySpot.y);
        } else if (id == "quickCapture") {
            ShowEditMode();  // away
            StepFrame();
            TriggerHotkey(config_.hotkeyQuickCapture);  // and back with it
            StepFrame();
        } else if (id == "newCanvas") {
            PressOnCanvasBar(AnchorId::CanvasBarNew);
        } else if (id == "moveSnippet") {
            if (!Runner().Subject().has_value()) {
                Drag(300.0f, 360.0f, 600.0f, 560.0f);  // a screenshot to take along
            }
            const std::vector<Item>& here = Canvases().CurrentOrNull()->items;
            if (std::none_of(here.begin(), here.end(), [&](const Item& item) { return item.id == Subject().id; })) {
                ToTheOtherCanvas();  // to where it is
            }
            if (!App().IsSelected(Subject().id)) {
                RawClick(SubjectMiddle().x, SubjectMiddle().y);
            }
            PressCtrlKey(ImGuiKey_X);
            ToTheOtherCanvas();
            PressCtrlKey(ImGuiKey_V);
        } else if (id == "overview") {
            PressOnCanvasBar(AnchorId::CanvasBarOverview);
        } else if (id == "newFolder") {
            ClickAnchor(Anchor{AnchorId::OverviewNewFolder});
        } else if (id == "rename") {
            ASSERT_FALSE(Runner().MadeFolders().empty());
            const Anchor row{AnchorId::OverviewFolderRow, Runner().MadeFolders().front()};
            ClickAnchor(row);  // a double-click
            ClickAnchor(row);
            TypeAName("Games");
        } else if (id == "switchFolder") {
            ClickAnchor(Anchor{AnchorId::OverviewFolderRow, Runner().Folder()});
        } else if (id == "moveCanvas") {
            if (!App().IsOverviewOpen()) {
                PressOnCanvasBar(AnchorId::CanvasBarOverview);
            }
            ClickAnchor(Anchor{AnchorId::OverviewFolderRow, Runner().Folder()});
            // One without the snippet, which the steps after it delete and
            // restore.
            const std::vector<CanvasId> canvases = LiveCanvasesIn(Runner().Folder());
            ASSERT_FALSE(canvases.empty());
            CanvasId dragged = canvases.front();
            for (const CanvasId canvas : canvases) {
                if (!HoldsASnippet(canvas)) {
                    dragged = canvas;
                }
            }
            const std::optional<AnchorRect> tile = App().AnchorAt(Anchor{AnchorId::OverviewCanvasTile, dragged});
            const std::optional<AnchorRect> row =
                App().AnchorAt(Anchor{AnchorId::OverviewFolderRow, Runner().MadeFolders().front()});
            ASSERT_TRUE(tile.has_value());
            ASSERT_TRUE(row.has_value());
            DragWidget(Center(*tile), Center(*row));
        } else if (id == "deleteCanvas") {
            if (!App().TutorialSpot().has_value()) {
                ClickAnchor(Anchor{AnchorId::OverviewFolderRow, Runner().Folder()});  // its tiles shown
            }
            const std::optional<AnchorRect> trash = App().TutorialSpot();
            ASSERT_TRUE(trash.has_value());
            // The confirmation is the Overview's own (overview_ui_test), and
            // left out here: the hand has no name to click it by.
            const bool asks = AppSettings().Stored().confirmDelete;
            controller_->GetSettings().Set(setting::kConfirmDelete, false);
            Click(Center(*trash).x, Center(*trash).y);
            controller_->GetSettings().Set(setting::kConfirmDelete, asks);
        } else if (id == "showDeleted") {
            ClickAnchor(Anchor{AnchorId::OverviewShowDeleted});
        } else if (id == "restore") {
            const std::optional<AnchorRect> restore = App().TutorialSpot();
            ASSERT_TRUE(restore.has_value());
            Click(Center(*restore).x, Center(*restore).y);
        } else if (id == "openCanvas") {
            ClickAnchor(Anchor{AnchorId::OverviewFolderRow, Runner().Folder()});
            const std::vector<CanvasId> canvases = LiveCanvasesIn(Runner().Folder());
            ASSERT_FALSE(canvases.empty());
            ClickAnchor(Anchor{AnchorId::OverviewCanvasTile, canvases.front()});
        } else if (id == "openProfiles") {
            // "Right-click an empty spot, choose Settings": the menu's item
            // has no handle here, as for the Overview's.
            ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Settings}));
            StepFrames(2);
            PickSection(SettingsPage::SettingsSection::Profiles);
        } else if (id == "makeProfile") {
            ClickAnchor(Anchor{AnchorId::SettingsMakeProfile});
        } else if (id == "behavior") {
            PickSection(SettingsPage::SettingsSection::Behavior);
        } else if (id == "change") {
            ClickAnchor(Anchor{AnchorId::SettingsDontStealFocus});
        } else if (id == "revert") {
            if (NeedUp() == tutorial::Need::ShowingIt) {
                PickShowing(TutorialsProfile());  // as its line says
            }
            const std::optional<AnchorRect> arrow = App().TutorialSpot();
            ASSERT_TRUE(arrow.has_value());
            Click(Center(*arrow).x, Center(*arrow).y);
        } else if (id == "end") {
            Press(TutorialButton::Done);
        } else {
            FAIL() << "no hand for step " << id;
        }
        Settle();
    }
    // Every step of `topic`, from the one up, done and moved on from.
    void WalkThrough(const tutorial::Topic& topic);
    // The steps of `topic` done, one after the other, until `id` is up.
    void WalkTo(const std::string& id, std::string_view topic = tutorial::kBasicsTopic) {
        StartTheTutorial(topic);
        for (int guard = 0; guard < 20 && StepUp() != id; ++guard) {
            const std::string before = StepUp();
            DoStep(before);
            ASSERT_NE(StepUp(), before) << "the step " << before << " did not move on: " << HintUp();
        }
        ASSERT_EQ(StepUp(), id);
    }
};

// ===== Anchors (section 7.2) =====

TEST_F(TutorialAppTest, TheSelectionBarsCloseButtonIsMarkedWhereItIsDrawn) {
    ShowEditMode();
    StepFrame();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    StepFrame();

    const std::optional<AnchorRect> close = App().AnchorAt(Anchor{AnchorId::SelectionBarClose});
    const std::optional<ImVec2> center = App().SelectionBarButtonCenter(ChromeButton::Close);
    ASSERT_TRUE(close.has_value());
    ASSERT_TRUE(center.has_value());
    EXPECT_FLOAT_EQ(Center(*close).x, center->x);
    EXPECT_FLOAT_EQ(Center(*close).y, center->y);
}

TEST_F(TutorialAppTest, TheSelectionBarsPinIsMarkedWhereItIsDrawn) {
    ShowEditMode();
    StepFrame();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    StepFrame();

    const std::optional<AnchorRect> pin = App().AnchorAt(Anchor{AnchorId::SelectionBarPin});
    const std::optional<ImVec2> center = App().SelectionBarButtonCenter(ChromeButton::Pin);
    ASSERT_TRUE(pin.has_value());
    ASSERT_TRUE(center.has_value());
    EXPECT_FLOAT_EQ(Center(*pin).x, center->x);
    EXPECT_FLOAT_EQ(Center(*pin).y, center->y);
}

// The canvas bar's two buttons, marked where they are drawn: pressed
// there, each does what it is for.
TEST_F(TutorialAppTest, TheCanvasBarsButtonsAreMarkedWhereTheyAreDrawn) {
    ShowEditMode();
    StepFrame();
    const CanvasId before = Canvases().CurrentCanvasId();
    const size_t canvases = Canvases().Canvases().size();
    PressOnCanvasBar(AnchorId::CanvasBarNew);
    EXPECT_EQ(Canvases().Canvases().size(), canvases + 1);
    EXPECT_NE(Canvases().CurrentCanvasId(), before);
    PressOnCanvasBar(AnchorId::CanvasBarOverview);
    EXPECT_TRUE(App().IsOverviewOpen());
}

// The Overview's widgets a step points at, marked where they are drawn:
// pressed there, each does what it is for.
TEST_F(TutorialAppTest, TheOverviewsWidgetsAreMarkedWhereTheyAreDrawn) {
    AppConfig config = DefaultConfig();
    config.confirmDelete = false;
    StartWith(config);
    ShowEditMode();
    StepFrame();
    const FolderId first = Canvases().CurrentFolderId();
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrames(2);

    ClickAnchor(Anchor{AnchorId::OverviewNewFolder});
    const FolderId made = Canvases().CurrentFolderId();
    ASSERT_NE(made, first);
    const CanvasId canvas = Canvases().CurrentCanvasId();
    ASSERT_TRUE(App().AnchorAt(Anchor{AnchorId::OverviewFolderRow, made}).has_value());
    ASSERT_TRUE(App().AnchorAt(Anchor{AnchorId::OverviewCanvasTile, canvas}).has_value());

    ClickAnchor(Anchor{AnchorId::OverviewCanvasDelete, canvas});
    EXPECT_TRUE(Canvases().IsDeleted(*Canvases().FindCanvas(canvas)));
    EXPECT_FALSE(App().AnchorAt(Anchor{AnchorId::OverviewRestore, canvas}).has_value()) << "not shown yet";
    ClickAnchor(Anchor{AnchorId::OverviewShowDeleted});
    EXPECT_TRUE(App().TutorialWorld().OverviewShowsDeleted());
    ClickAnchor(Anchor{AnchorId::OverviewRestore, canvas});
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindCanvas(canvas)));

    ClickAnchor(Anchor{AnchorId::OverviewShowDeleted});  // off again
    const CanvasId current = Canvases().CurrentCanvasId();
    ClickAnchor(Anchor{AnchorId::OverviewFolderRow, first});
    EXPECT_EQ(Canvases().CurrentFolderId(), first) << "browsed";
    EXPECT_EQ(Canvases().CurrentCanvasId(), current) << "and not gone to";
    const CanvasId there = LiveCanvasesIn(first).front();
    ClickAnchor(Anchor{AnchorId::OverviewCanvasTile, there});
    EXPECT_EQ(Canvases().CurrentCanvasId(), there);
    EXPECT_FALSE(App().IsOverviewOpen());
}

TEST_F(TutorialAppTest, AnAnchorNotDrawnThisFrameIsNotOnTheBoard) {
    ShowEditMode();
    StepFrame();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    StepFrame();
    ASSERT_TRUE(App().AnchorAt(Anchor{AnchorId::SelectionBarClose}).has_value());

    PressKey(ImGuiKey_Escape);  // deselected: no bar
    EXPECT_TRUE(App().Selection().empty());
    EXPECT_FALSE(App().AnchorAt(Anchor{AnchorId::SelectionBarClose}).has_value());
}

TEST_F(TutorialAppTest, TheSelectionBarsPenIsMarkedWhereItIsDrawn) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    ASSERT_TRUE(App().InDrawingMode());
    StepFrame();

    const std::optional<AnchorRect> pen = App().AnchorAt(Anchor{AnchorId::SelectionBarPen});
    const std::optional<ImVec2> center = App().SelectionBarButtonCenter(ChromeButton::Pen);
    ASSERT_TRUE(pen.has_value());
    ASSERT_TRUE(center.has_value());
    EXPECT_FLOAT_EQ(Center(*pen).x, center->x);
    EXPECT_FLOAT_EQ(Center(*pen).y, center->y);
    EXPECT_TRUE(App().AnchorAt(Anchor{AnchorId::SelectionBarClose}).has_value()) << "one bar, in drawing mode too";
}

TEST_F(TutorialAppTest, TheSelectionBarsOtherDrawingButtonsAreMarkedWhereTheyAreDrawn) {
    ShowEditMode();
    StepFrame();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    StepFrame();
    for (const auto [id, button] : {std::pair{AnchorId::SelectionBarEraser, ChromeButton::Eraser},
                                    std::pair{AnchorId::SelectionBarText, ChromeButton::Text},
                                    std::pair{AnchorId::SelectionBarColor, ChromeButton::Color}}) {
        const std::optional<AnchorRect> anchor = App().AnchorAt(Anchor{id});
        const std::optional<ImVec2> center = App().SelectionBarButtonCenter(button);
        ASSERT_TRUE(anchor.has_value());
        ASSERT_TRUE(center.has_value());
        EXPECT_FLOAT_EQ(Center(*anchor).x, center->x);
        EXPECT_FLOAT_EQ(Center(*anchor).y, center->y);
    }
}

TEST_F(TutorialAppTest, ADockChipIsMarkedForItsSnippet) {
    ShowEditMode();
    StepFrame();
    const ItemId first = MakeASnippet(100.0f, 100.0f, 400.0f, 300.0f);
    const ItemId second = MakeASnippet(500.0f, 100.0f, 800.0f, 300.0f);
    controller_->GetSession().SetMinimized({first, second}, true);
    StepFrame();

    const std::optional<AnchorRect> firstChip = App().AnchorAt(Anchor{AnchorId::DockChip, first});
    const std::optional<AnchorRect> secondChip = App().AnchorAt(Anchor{AnchorId::DockChip, second});
    ASSERT_TRUE(firstChip.has_value());
    ASSERT_TRUE(secondChip.has_value());
    EXPECT_LT(firstChip->max.x, secondChip->min.x) << "side by side, in the canvas's order";
    EXPECT_GT(firstChip->min.y, kDisplayHeight * 0.5f) << "along the bottom";

    // A click on the chip where it is marked brings the snippet back.
    Click(Center(*firstChip).x, Center(*firstChip).y);
    EXPECT_FALSE(Canvases().FindItemAnywhere(first)->minimized);
    EXPECT_FALSE(App().AnchorAt(Anchor{AnchorId::DockChip, first}).has_value());
}

// ===== The world (section 7.1) =====

TEST_F(TutorialAppTest, TheWorldSaysWhatCoversTheCanvas) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::None);

    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::CheatSheet}));
    StepFrame();
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::CheatSheet);
    PressKey(ImGuiKey_Escape);
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::None);

    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrame();
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::Overview);
    PressKey(ImGuiKey_Escape);
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::None);

    RightClick(900.0f, 400.0f);
    ASSERT_TRUE(App().IsEmptyCanvasMenuOpen());
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::Popup);
}

// A snippet's own popups, opened from its bar, cover nothing: a step may
// be using them (docs/TUTORIAL.md, section 15.3).
TEST_F(TutorialAppTest, TheColorChooserAndPropertiesCoverNothing) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    MakeASnippet(300.0f, 300.0f, 700.0f, 550.0f);
    PressBar(ChromeButton::More);
    StepFrames(2);
    ASSERT_NE(App().InputStack().find("ItemProperties"), std::string::npos) << App().InputStack();
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::None);
    PressKey(ImGuiKey_Escape);

    DoubleClick(500.0f, 420.0f);
    ASSERT_TRUE(App().InDrawingMode());
    PressBar(ChromeButton::Color);
    StepFrames(2);
    ASSERT_TRUE(App().IsColorChooserOpen());
    EXPECT_EQ(world.CanvasCover(), tutorial::Cover::None);
}

TEST_F(TutorialAppTest, TheWorldSaysWhatIsInTheHand) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    const ItemId shot = MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    EXPECT_EQ(world.Selection(), std::vector<ItemId>{shot});
    EXPECT_TRUE(world.DrawingItems().empty());
    EXPECT_FALSE(world.CreationToolInHand().has_value());

    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::NewScreenshotTool}));
    EXPECT_EQ(world.CreationToolInHand(), ItemCreationKind::Screenshot);
    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(world.CreationToolInHand().has_value());

    DoubleClick(450.0f, 400.0f);
    EXPECT_EQ(world.DrawingItems(), std::vector<ItemId>{shot});
}

TEST_F(TutorialAppTest, TheWorldReadsTheSnippetsOfAFolderAsFacts) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    const ItemId shot = MakeASnippet(100.0f, 100.0f, 400.0f, 300.0f);
    MakeADrawing(500.0f, 100.0f, 800.0f, 300.0f);
    const ItemId drawing = Canvases().CurrentOrNull()->items.back().id;
    PressKey(ImGuiKey_Escape);  // drawing mode ended
    const CanvasId canvas = Canvases().CurrentCanvasId();
    const FolderId folder = world.FolderOf(canvas);
    ASSERT_NE(folder, 0u);
    EXPECT_EQ(world.CurrentCanvas(), canvas);
    EXPECT_EQ(world.CanvasName(canvas), Canvases().CurrentOrNull()->name);

    controller_->GetSession().SetMinimized({drawing}, true);
    RawClick(250.0f, 200.0f);
    PressKey(ImGuiKey_Delete);

    std::vector<tutorial::SnippetFacts> facts = world.SnippetsIn(folder);
    const auto find = [&](ItemId id) -> const tutorial::SnippetFacts* {
        for (const tutorial::SnippetFacts& f : facts) {
            if (f.id == id) {
                return &f;
            }
        }
        return nullptr;
    };
    const tutorial::SnippetFacts* shotFacts = find(shot);
    const tutorial::SnippetFacts* drawingFacts = find(drawing);
    ASSERT_NE(shotFacts, nullptr) << "a deleted snippet is still a fact";
    ASSERT_NE(drawingFacts, nullptr);
    EXPECT_TRUE(shotFacts->picture);
    EXPECT_TRUE(shotFacts->deleted);
    EXPECT_FALSE(shotFacts->minimized);
    EXPECT_EQ(shotFacts->canvas, canvas);
    EXPECT_FLOAT_EQ(shotFacts->rect.w, Canvases().FindItemAnywhere(shot)->rect.w);
    EXPECT_FALSE(drawingFacts->picture);
    EXPECT_FALSE(drawingFacts->deleted);
    EXPECT_TRUE(drawingFacts->minimized);
}

TEST_F(TutorialAppTest, TheWorldCountsTheStrokesAndSeesFullscreen) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    const ItemId drawing = Canvases().CurrentOrNull()->items.back().id;
    const FolderId folder = world.FolderOf(world.CurrentCanvas());
    Drag(350.0f, 350.0f, 600.0f, 500.0f);  // drawn with the pen in hand
    const auto factsOf = [&](ItemId id) {
        for (const tutorial::SnippetFacts& f : world.SnippetsIn(folder)) {
            if (f.id == id) {
                return f;
            }
        }
        return tutorial::SnippetFacts{};
    };
    EXPECT_EQ(factsOf(drawing).strokes.size(), 1u);
    EXPECT_FALSE(factsOf(drawing).fullscreen);

    PressKey(ImGuiKey_Escape);  // out of drawing mode
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::ToggleFullscreen, drawing}));
    StepFrame();
    EXPECT_TRUE(factsOf(drawing).fullscreen);
}

// Each stroke as it looks on screen - its color, its width at the
// snippet's size now, a line or a rectangle - and the ink, and the note.
TEST_F(TutorialAppTest, TheWorldReadsTheStrokesAsTheyLookAndTheNote) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    MakeADrawing(300.0f, 300.0f, 700.0f, 550.0f);
    const ItemId drawing = Canvases().CurrentOrNull()->items.back().id;
    const FolderId folder = world.FolderOf(world.CurrentCanvas());
    const auto factsOf = [&]() {
        for (const tutorial::SnippetFacts& f : world.SnippetsIn(folder)) {
            if (f.id == drawing) {
                return f;
            }
        }
        return tutorial::SnippetFacts{};
    };
    EXPECT_EQ(world.ToolInHand(), Tool::Draw);
    Drag(350.0f, 350.0f, 600.0f, 500.0f, 8);
    DragWith(ImGuiMod_Shift, 350.0f, 400.0f, 650.0f, 400.0f);
    DragWith(ImGuiMod_Ctrl, 400.0f, 350.0f, 500.0f, 450.0f);

    tutorial::SnippetFacts facts = factsOf();
    ASSERT_EQ(facts.strokes.size(), 3u);
    EXPECT_EQ(facts.strokes[0].shape, DrawShape::Freehand);
    EXPECT_EQ(facts.strokes[1].shape, DrawShape::Line);
    EXPECT_EQ(facts.strokes[2].shape, DrawShape::Rectangle);
    EXPECT_EQ(facts.strokes[0].colorRGBA, world.PenColor());
    EXPECT_NEAR(facts.strokes[0].widthPx, world.PenWidth(), 0.01f);
    EXPECT_NEAR(facts.strokes[1].lengthPx, 300.0f, 0.5f);
    EXPECT_NEAR(facts.strokes[2].lengthPx, 400.0f, 0.5f);

    // Bigger by the wheel: as much wider and longer on screen.
    const float ink = facts.InkPx();
    const float widthBefore = Canvases().FindItemAnywhere(drawing)->rect.w;
    PressKey(ImGuiKey_Escape);  // out of drawing mode, still selected
    MoveTo(500.0f, 420.0f);
    Wheel(2.0f);
    const float scale = Canvases().FindItemAnywhere(drawing)->rect.w / widthBefore;
    ASSERT_GT(scale, 1.05f) << "the wheel resized nothing";
    facts = factsOf();
    EXPECT_NEAR(facts.strokes[0].widthPx, scale * world.PenWidth(), 0.05f);
    EXPECT_NEAR(facts.InkPx(), scale * ink, 1.0f);
    const Rect grown = Canvases().FindItemAnywhere(drawing)->rect;
    DoubleClick(grown.x + grown.w * 0.5f, grown.y + grown.h * 0.5f);
    ASSERT_EQ(App().DrawingItems(), std::vector<ItemId>{drawing});

    // Erased across: less ink.
    const std::optional<ImVec2> eraser = App().SelectionBarButtonCenter(ChromeButton::Eraser);
    ASSERT_TRUE(eraser.has_value());
    RawClick(eraser->x, eraser->y);
    EXPECT_EQ(world.ToolInHand(), Tool::Erase);
    const Rect r = Canvases().FindItemAnywhere(drawing)->rect;
    Drag(r.x + r.w * 0.5f, r.y + 10.0f, r.x + r.w * 0.5f, r.y + r.h - 10.0f);
    EXPECT_LT(factsOf().InkPx(), facts.InkPx() - 16.0f);
    EXPECT_EQ(world.ErasedWith(), DrawShape::Freehand);
    // Ctrl held, the same eraser erases a rectangle, and says so.
    DragWith(ImGuiMod_Ctrl, r.x + 10.0f, r.y + 10.0f, r.x + 40.0f, r.y + r.h - 10.0f);
    EXPECT_EQ(world.ErasedWith(), DrawShape::Rectangle);

    // A note: being typed, and then on the snippet.
    const std::optional<ImVec2> text = App().SelectionBarButtonCenter(ChromeButton::Text);
    ASSERT_TRUE(text.has_value());
    RawClick(text->x, text->y);
    RawClick(r.x + 40.0f, r.y + 40.0f);
    EXPECT_EQ(world.NoteBeingTyped(), drawing);
    ImGui::GetIO().AddInputCharacter('h');
    StepFrame();
    ImGui::GetIO().AddInputCharacter('i');
    StepFrame();
    EXPECT_EQ(factsOf().note, "hi") << "on the snippet as it is typed";
    PressKey(ImGuiKey_Escape);
    EXPECT_FALSE(world.NoteBeingTyped().has_value());
    EXPECT_EQ(factsOf().note, "hi");
}

TEST_F(TutorialAppTest, ADeletedCanvasIsInNoFolderAndADeletedFolderHoldsNothing) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    const CanvasId canvas = world.CurrentCanvas();
    const FolderId folder = world.FolderOf(canvas);
    ASSERT_FALSE(world.SnippetsIn(folder).empty());

    Session& session = controller_->GetSession();
    ASSERT_TRUE(session.Delete(folder));
    StepFrame();
    EXPECT_EQ(world.FolderOf(canvas), 0u);
    EXPECT_TRUE(world.SnippetsIn(folder).empty());
    EXPECT_EQ(world.FolderOf(987654321u), 0u) << "no such canvas";
}

TEST_F(TutorialAppTest, TheWorldNamesKeysAsTheyAreBound) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    EXPECT_EQ(world.KeyLabel(CommandId::Undo), std::optional<std::string>("Ctrl+Z"));
    EXPECT_EQ(world.KeyLabel(CommandId::ToggleEditMode),
              std::optional<std::string>(FormatKeyComboLabel(config_.hotkeyEditMode)));
    EXPECT_FALSE(world.KeyLabel(CommandId::Properties).has_value()) << "only a menu reaches it";
    EXPECT_EQ(world.ScreenshotTrigger(), config_.screenshotTrigger);
    EXPECT_EQ(world.DrawingTrigger(), config_.drawingTrigger);
}

// A hotkey another program holds does nothing when pressed, so no card
// names it (docs/TUTORIAL.md, section 16.3) - until it is picked again and
// registers.
TEST_F(TutorialAppTest, TheWorldNamesNoHotkeyThatDidNotRegister) {
    host_.registerHotkeySucceeds = false;
    StartWith(DefaultConfig());
    host_.registerHotkeySucceeds = true;
    const tutorial::World& world = App().TutorialWorld();
    EXPECT_FALSE(world.KeyLabel(CommandId::QuickCapture).has_value());
    EXPECT_FALSE(world.KeyLabel(CommandId::ToggleEditMode).has_value());
    EXPECT_EQ(world.KeyLabel(CommandId::Undo), std::optional<std::string>("Ctrl+Z")) << "not a hotkey";

    controller_->Overlay().ArmHotkeyCapture(HotkeySlot::QuickCapture);
    controller_->Overlay().CompleteHotkeyCapture(config_.hotkeyQuickCapture);
    EXPECT_EQ(world.KeyLabel(CommandId::QuickCapture),
              std::optional<std::string>(FormatKeyComboLabel(config_.hotkeyQuickCapture)));
}

// Each capture hotkey's screenshots, pressed with the overlay away or up.
TEST_F(TutorialAppTest, TheWorldCountsEachCaptureHotkeysScreenshots) {
    const tutorial::World& world = App().TutorialWorld();
    TriggerHotkey(config_.hotkeyQuickCapture);  // away: up with it
    StepFrame();
    EXPECT_EQ(world.Captures(HotkeySlot::QuickCapture), 1u);
    EXPECT_EQ(world.Captures(HotkeySlot::SilentCapture), 0u);
    TriggerHotkey(config_.hotkeySilentCapture);  // up
    StepFrame();
    EXPECT_EQ(world.Captures(HotkeySlot::SilentCapture), 1u);
    ShowEditMode();  // away
    StepFrame();
    TriggerHotkey(config_.hotkeySilentCapture);
    StepFrame();
    EXPECT_EQ(world.Captures(HotkeySlot::SilentCapture), 2u);
    EXPECT_EQ(world.Captures(HotkeySlot::QuickCapture), 1u);
}

// A capture whose canvas could not be written is not made, and not counted.
TEST_F(TutorialAppTest, ACaptureNotWrittenIsNotCounted) {
    const std::filesystem::path library = StartWithLibrary();
    test::FailingWrites failing(library);
    failing.FailAll();
    TriggerHotkey(config_.hotkeyQuickCapture);
    StepFrame();
    EXPECT_EQ(App().TutorialWorld().Captures(HotkeySlot::QuickCapture), 0u);
}

// The folders and canvases, those in the trash among them, the Overview's
// tab and Show deleted, and the canvas bar's setting.
TEST_F(TutorialAppTest, TheWorldTellsTheFoldersCanvasesAndTheOverview) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    const FolderId folder = Canvases().CurrentFolderId();
    const CanvasId canvas = Canvases().CurrentCanvasId();
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::NewCanvas}));
    const CanvasId second = Canvases().CurrentCanvasId();
    controller_->GetSession().RenameCanvas(second, "Maps");
    ASSERT_TRUE(controller_->GetSession().Delete(canvas));

    const std::vector<tutorial::CanvasFacts> canvases = world.CanvasesIn(folder);
    ASSERT_EQ(canvases.size(), 2u);
    EXPECT_EQ(canvases[0].id, canvas);
    EXPECT_TRUE(canvases[0].deleted) << "in the trash, and still told";
    EXPECT_EQ(canvases[1].name, "Maps");
    EXPECT_FALSE(canvases[1].deleted);
    const std::vector<tutorial::FolderFacts> folders = world.Folders();
    ASSERT_FALSE(folders.empty());
    EXPECT_EQ(folders.front().id, folder);
    EXPECT_FALSE(folders.front().deleted);

    EXPECT_FALSE(world.OverviewShowsCanvases()) << "not up";
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrames(2);
    EXPECT_TRUE(world.OverviewShowsCanvases());
    EXPECT_FALSE(world.OverviewShowsDeleted());
    ClickAnchor(Anchor{AnchorId::OverviewShowDeleted});
    EXPECT_TRUE(world.OverviewShowsDeleted());
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Settings}));
    StepFrames(2);
    EXPECT_FALSE(world.OverviewShowsCanvases()) << "on Settings";

    EXPECT_TRUE(world.CanvasBarOn());
    controller_->GetSettings().Set(setting::kShowCanvasBar, false);
    EXPECT_FALSE(world.CanvasBarOn());
}

TEST_F(TutorialAppTest, TheWorldCountsTheOverlayComingUp) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    const uint64_t before = world.Showings();
    ShowEditMode();  // away
    StepFrame();
    EXPECT_EQ(world.Showings(), before);
    ShowEditMode();  // and back
    StepFrame();
    EXPECT_EQ(world.Showings(), before + 1);
}

// Each transition into the pinned view, or view mode, once - not every
// time the mode is set: Hidden keeps the mode it came down from.
TEST_F(TutorialAppTest, TheWorldCountsThePinnedViewAndViewModeAsTheyAreEntered) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    const uint64_t pinnedBefore = world.PinnedViews();
    const uint64_t viewBefore = world.ViewModes();

    ShowViewMode();
    StepFrame();
    EXPECT_EQ(world.ViewModes(), viewBefore + 1);
    ShowViewMode();  // away: hidden, nothing pinned
    StepFrame();
    ShowViewMode();  // and view mode again
    StepFrame();
    EXPECT_EQ(world.ViewModes(), viewBefore + 2);
    EXPECT_EQ(world.PinnedViews(), pinnedBefore);

    ShowEditMode();  // edit mode, in place
    StepFrame();
    const ItemId shot = MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    controller_->GetSession().SetPinned({shot}, true);
    ShowEditMode();  // away: the pinned view
    StepFrame();
    EXPECT_EQ(world.PinnedViews(), pinnedBefore + 1);
    ShowViewMode();  // view mode, in place
    StepFrame();
    EXPECT_EQ(world.ViewModes(), viewBefore + 3);
    ShowViewMode();  // away again: the pinned view
    StepFrame();
    EXPECT_EQ(world.PinnedViews(), pinnedBefore + 2);
}

TEST_F(TutorialAppTest, TheWorldReadsAPinAndBothOpacities) {
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();
    const ItemId shot = MakeASnippet(300.0f, 300.0f, 600.0f, 500.0f);
    const FolderId folder = world.FolderOf(world.CurrentCanvas());
    const auto facts = [&] {
        for (const tutorial::SnippetFacts& f : world.SnippetsIn(folder)) {
            if (f.id == shot) {
                return f;
            }
        }
        return tutorial::SnippetFacts{};
    };
    EXPECT_FALSE(facts().pinned);
    const float picture = facts().pictureOpacity;
    const float drawing = facts().drawingOpacity;
    EXPECT_FLOAT_EQ(picture, Canvases().FindItemAnywhere(shot)->picture.opacity);

    PressBarPin();
    EXPECT_TRUE(facts().pinned);

    MoveTo(450.0f, 400.0f);
    KeyEvent(ImGuiMod_Ctrl, true);
    Wheel(-1.0f);
    KeyEvent(ImGuiMod_Ctrl, false);
    StepFrame();
    EXPECT_NEAR(facts().pictureOpacity, picture - 0.05f, 0.001f) << "Ctrl: the picture's";
    EXPECT_FLOAT_EQ(facts().drawingOpacity, drawing);

    KeyEvent(ImGuiMod_Shift, true);
    Wheel(-1.0f);
    KeyEvent(ImGuiMod_Shift, false);
    StepFrame();
    EXPECT_NEAR(facts().drawingOpacity, drawing - 0.05f, 0.001f) << "Shift: the strokes'";
}

// ===== The card, the spotlight and the walk-through (section 9) =====

TEST_F(TutorialAppTest, AStartMakesAFolderOfItsOwnAndSwitchesToIt) {
    ShowEditMode();
    StepFrame();
    const CanvasId before = Canvases().CurrentCanvasId();
    const size_t folders = Canvases().Folders().size();
    Overlay().StartTutorial();
    StepFrames(2);

    ASSERT_TRUE(Runner().On());
    EXPECT_EQ(StepUp(), "welcome");
    EXPECT_EQ(Canvases().Folders().size(), folders) << "not while the welcome is read (section 13.5)";
    EXPECT_EQ(Canvases().CurrentCanvasId(), before);
    Press(TutorialButton::Next);
    StepFrame();
    ASSERT_EQ(Canvases().Folders().size(), folders + 1);
    const Folder& made = Canvases().Folders().back();
    EXPECT_EQ(made.id, Runner().Folder());
    EXPECT_EQ(made.name, "Tutorial: Basics") << "named for its topic";
    EXPECT_NE(Canvases().CurrentCanvasId(), before);
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, made.id);
    EXPECT_EQ(ItemCountOnCurrentCanvas(), 0u) << "an empty canvas to start on";
}

// Every topic, done the way its cards say, with real gestures.
TEST_F(TutorialAppTest, EveryTopicIsWalkedThroughWithRealGestures) {
    for (const tutorial::Topic& topic : tutorial::Topics()) {
        SCOPED_TRACE(std::string(topic.id));
        StartTheTutorial(topic.id);
        WalkThrough(topic);
        ShowEditMode();  // put away, for the next topic's start
        StepFrame();
    }
}

void TutorialAppTest::WalkThrough(const tutorial::Topic& topic) {
    std::vector<std::string> seen;
    for (int guard = 0; guard < 20 && Runner().On(); ++guard) {
        const std::string step = StepUp();
        seen.push_back(step);
        DoStep(step);
        if (Runner().On()) {
            ASSERT_NE(StepUp(), step) << "the step " << step << " did not move on: " << HintUp();
        }
    }
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(Runner().GetOutcome(), tutorial::Tutorial::Outcome::Finished);
    std::vector<std::string> chain;
    for (const tutorial::Step& step : topic.chain()) {
        chain.emplace_back(step.id);
    }
    EXPECT_EQ(seen, chain) << "every step, in order, none skipped";
}

TEST_F(TutorialAppTest, ADoStepShowsItsCheckAndMovesOnASecondLater) {
    WalkTo("screenshot");
    Drag(300.0f, 360.0f, 600.0f, 560.0f);
    EXPECT_EQ(StepUp(), "screenshot");
    EXPECT_TRUE(Runner().GoalMet());
    StepFrames(30);
    EXPECT_EQ(StepUp(), "screenshot") << "half a second on, still showing its check";
    StepFrames(40);
    EXPECT_EQ(StepUp(), "move");
}

TEST_F(TutorialAppTest, TheCardIsDrawnWhileTheTutorialIsOnInEditModeOnly) {
    ShowEditMode();
    StepFrame();
    EXPECT_EQ(ImGui::FindWindowByName("##tutorial_card"), nullptr) << "no tutorial, no card";
    Overlay().StartTutorial();
    StepFrames(2);
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    EXPECT_TRUE(card->Active);

    ShowViewMode();
    StepFrames(2);
    EXPECT_FALSE(card->Active) << "view only is click-through";
    ShowEditMode();
    StepFrames(2);
    EXPECT_TRUE(card->Active);
    EXPECT_EQ(StepUp(), "welcome") << "back where it was";
}

TEST_F(TutorialAppTest, TheCardSitsAboveThePanelsAndBelowTheDeleteConfirmation) {
    StartTheTutorial();
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::CheatSheet}));
    StepFrames(2);
    const std::vector<std::string> overSheet = {"canvas", "items", "chrome", "cheat sheet backdrop",
                                                "cheat sheet", "tutorial card"};
    ASSERT_TRUE(InStackOrder(overSheet));
    EXPECT_EQ(Describe(SurfacesBackToFront()), Describe(overSheet));
    PressKey(ImGuiKey_Escape);

    // The tutorial's own canvas deleted: asked first. Which comes out
    // from under the canvas bar for a moment - in the table's order all
    // the same.
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::DeleteCanvas, 0, Canvases().CurrentCanvasId()}));
    StepFrames(3);
    const std::vector<std::string> stack = SurfacesBackToFront("delete confirmation");
    EXPECT_TRUE(InStackOrder(stack)) << Describe(stack);
    ASSERT_GE(stack.size(), 2u);
    EXPECT_EQ(stack[stack.size() - 2], "tutorial card") << Describe(stack);
    EXPECT_EQ(stack.back(), "delete confirmation") << Describe(stack);
}

// The bar's Pin once the subject is selected, and the snippet until then.
TEST_F(TutorialAppTest, ThePinStepRingsTheSnippetUntilItsBarShowsThenThePin) {
    StartTheTutorial("pinning");
    ASSERT_EQ(StepUp(), "pin");
    Overlay().PressTutorialHint();  // Put one here
    StepFrames(3);
    EXPECT_EQ(NeedUp(), tutorial::Need::SubjectSelected);
    std::optional<AnchorRect> spot = App().TutorialSpot();
    ASSERT_TRUE(spot.has_value());
    EXPECT_FLOAT_EQ(spot->min.x, Subject().rect.x);

    RawClick(SubjectMiddle().x, SubjectMiddle().y);
    spot = App().TutorialSpot();
    const std::optional<AnchorRect> pin = App().AnchorAt(Anchor{AnchorId::SelectionBarPin});
    ASSERT_TRUE(spot.has_value());
    ASSERT_TRUE(pin.has_value());
    EXPECT_FLOAT_EQ(spot->min.x, pin->min.x);
    EXPECT_FLOAT_EQ(spot->min.y, pin->min.y);
}

// The line may name the bar's Pin with no ring on it, so the card keeps
// off the bar as well as the snippet - here, with neither the top nor the
// bottom clear of both, in a top corner.
TEST_F(TutorialAppTest, TheCardKeepsClearOfTheSubjectAndItsBar) {
    WalkTo("pinnedAway", "pinning");
    StepFrames(2);
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    const std::optional<AnchorRect> pin = App().AnchorAt(Anchor{AnchorId::SelectionBarPin});
    ASSERT_TRUE(pin.has_value()) << "selected, with its bar";
    const Rect& r = Subject().rect;
    const auto clear = [&](ImVec2 min, ImVec2 max) {
        return card->Pos.x + card->Size.x <= min.x || max.x <= card->Pos.x || card->Pos.y + card->Size.y <= min.y ||
               max.y <= card->Pos.y;
    };
    EXPECT_TRUE(clear(pin->min, pin->max));
    EXPECT_TRUE(clear(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h)));
}

// Shift and the wheel on a snippet with nothing drawn on it change a
// value and nothing on screen: no check, and a line that says so.
TEST_F(TutorialAppTest, TheOpacityStepCountsOnlyAChangeThatShows) {
    WalkTo("opacity", "pinning");
    ASSERT_EQ(Subject().strokes.size(), 0u);
    SelectTheSubject();
    MoveTo(SubjectMiddle().x, SubjectMiddle().y);
    KeyEvent(ImGuiMod_Shift, true);
    Wheel(-1.0f);
    Wheel(-1.0f);
    Wheel(-1.0f);
    KeyEvent(ImGuiMod_Shift, false);
    StepFrames(3);
    EXPECT_FALSE(Runner().GoalMet());
    EXPECT_EQ(HintUp(), ::sz::strings::kTutorialOpacityMissNothingDrawn);

    DoStep("opacity");  // Ctrl, as the line says
    EXPECT_EQ(StepUp(), "viewMode");
}

// The rectangle eraser's step, done the other way its line gives: Ctrl
// held with the round eraser still picked. It counted as round, since the
// tally asked the shape picked rather than the one the drag erased with.
TEST_F(TutorialAppTest, TheRectangleEraserStepCountsACtrlDragWithTheRoundEraser) {
    WalkTo("eraseRect", "drawing");
    ASSERT_EQ(App().EraserShape(), DrawShape::Freehand);
    const ImVec2 middle = SubjectMiddle();
    DragWith(ImGuiMod_Ctrl, middle.x + 20.0f, middle.y - 80.0f, middle.x + 50.0f, middle.y + 80.0f);
    Settle();
    EXPECT_EQ(StepUp(), "eraseRight") << HintUp();
}

TEST_F(TutorialAppTest, TheSpotlightRingsTheSubjectAndTheCardStaysClearOfIt) {
    WalkTo("move");
    const std::optional<AnchorRect> spot = App().TutorialSpot();
    ASSERT_TRUE(spot.has_value());
    const Rect& r = Subject().rect;
    EXPECT_FLOAT_EQ(spot->min.x, r.x);
    EXPECT_FLOAT_EQ(spot->max.y, r.y + r.h);

    // Moved up under the card, the card goes to the bottom.
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    EXPECT_LT(card->Pos.y, kDisplayHeight * 0.5f) << "at the top to begin with";
    const ImVec2 from = SubjectMiddle();
    const ImVec2 to(card->Pos.x + card->Size.x * 0.5f, card->Pos.y + card->Size.y * 0.5f);
    Drag(from.x, from.y, to.x, to.y);
    StepFrames(2);
    EXPECT_GT(card->Pos.y, kDisplayHeight * 0.5f);
}

// The card leaves only what the user is asked to click: the snippet
// moved under it on a step that rings nothing leaves it where it is, and
// so does the next step (docs/TUTORIAL.md, section 19).
// The card leaves only for what the user is asked to click: the snippet
// moved under it, on a step that rings nothing, leaves it where it is
// (docs/TUTORIAL.md, section 19).
TEST_F(TutorialAppTest, TheCardStaysOverTheSubjectAlone) {
    WalkTo("pinnedAway", "pinning");
    StepFrames(2);
    ASSERT_FALSE(App().TutorialSpot().has_value()) << "rings nothing";
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    PressKey(ImGuiKey_Escape);  // put down: no bar to keep clear of
    StepFrames(15);
    ASSERT_FALSE(App().AnchorAt(Anchor{AnchorId::SelectionBarPin}).has_value());
    ASSERT_FALSE(App().TutorialSpot().has_value());
    const ImVec2 before = card->Pos;
    // Its top edge under the card's lower edge - moved as a drag ends, with
    // no bar over it.
    Rect r = Subject().rect;
    r.x = card->Pos.x + (card->Size.x - r.w) * 0.5f;
    r.y = card->Pos.y + card->Size.y - 20.0f;
    ASSERT_TRUE(controller_->GetSession().SetRects({{Subject().id, r}}));
    StepFrames(15);
    const Rect& moved = Subject().rect;
    ASSERT_TRUE(card->Pos.x < moved.x + moved.w && moved.x < card->Pos.x + card->Size.x &&
                card->Pos.y < moved.y + moved.h && moved.y < card->Pos.y + card->Size.y)
        << "the snippet under it";
    EXPECT_FLOAT_EQ(card->Pos.x, before.x);
    EXPECT_FLOAT_EQ(card->Pos.y, before.y);
}

// Once moved, the card stays where it went, across steps too, although
// the place it left is clear again.
TEST_F(TutorialAppTest, TheCardStaysWhereItWentWhenTheStepMovesOn) {
    WalkTo("move");
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    const ImVec2 from = SubjectMiddle();
    Drag(from.x, from.y, card->Pos.x + card->Size.x * 0.5f, card->Pos.y + card->Size.y * 0.5f);
    StepFrames(15);
    ASSERT_GT(card->Pos.y, kDisplayHeight * 0.5f) << "out of the way, at the bottom";
    const ImVec2 went = card->Pos;
    const float wentBottom = card->Pos.y + card->Size.y;
    // In the top left, clear of the top and the bottom both.
    Rect r = Subject().rect;
    r.x = 24.0f;
    r.y = 100.0f;
    ASSERT_TRUE(controller_->GetSession().SetRects({{Subject().id, r}}));
    Settle();
    ASSERT_EQ(StepUp(), "resize");
    StepFrames(15);
    EXPECT_FLOAT_EQ(card->Pos.x, went.x);
    EXPECT_FLOAT_EQ(card->Pos.y + card->Size.y, wentBottom) << "at the bottom, grown upward";
}

// A move slides: part of the way on the frame after, all of it once the
// slide is over. The Overview opened places the card anew, in its lower
// right.
TEST_F(TutorialAppTest, TheCardSlidesToANewPlace) {
    StartTheTutorial();
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    const ImVec2 before = card->Pos;
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrames(3);
    const ImVec2 midway = card->Pos;
    StepFrames(15);
    const ImVec2 after = card->Pos;
    EXPECT_GT(midway.x, before.x) << "on its way";
    EXPECT_LT(midway.x, after.x);
    EXPECT_GT(after.x, kDisplayWidth * 0.5f) << "on the right";
    EXPECT_GT(after.y, kDisplayHeight * 0.5f) << "at the bottom";
    StepFrames(3);
    EXPECT_FLOAT_EQ(card->Pos.x, after.x) << "there";
    EXPECT_FLOAT_EQ(card->Pos.y, after.y);
}

TEST_F(TutorialAppTest, TheSpotlightRingsTheBarsCloseOnTheDeleteStep) {
    WalkTo("delete");
    RawClick(SubjectMiddle().x, SubjectMiddle().y);
    StepFrame();
    const std::optional<AnchorRect> spot = App().TutorialSpot();
    const std::optional<AnchorRect> close = App().AnchorAt(Anchor{AnchorId::SelectionBarClose});
    ASSERT_TRUE(spot.has_value());
    ASSERT_TRUE(close.has_value());
    EXPECT_FLOAT_EQ(spot->min.x, close->min.x);
    EXPECT_FLOAT_EQ(spot->min.y, close->min.y);

    // Pressed where the ring is, it is the step done.
    RawClick(Center(*spot).x, Center(*spot).y);
    Settle();
    EXPECT_EQ(StepUp(), "undo");
}

TEST_F(TutorialAppTest, TheSpotlightRingsTheDrawingBarsPen) {
    WalkTo("draw", "drawing");
    const std::optional<AnchorRect> spot = App().TutorialSpot();
    const std::optional<AnchorRect> pen = App().AnchorAt(Anchor{AnchorId::SelectionBarPen});
    ASSERT_TRUE(spot.has_value());
    ASSERT_TRUE(pen.has_value());
    EXPECT_FLOAT_EQ(spot->min.x, pen->min.x);
}

TEST_F(TutorialAppTest, SkipShowsTheWarningsNotReachedAndDoneLetsGo) {
    WalkTo("move");
    Press(TutorialButton::Skip);
    EXPECT_EQ(Runner().GetState(), tutorial::Tutorial::State::Skipped);
    EXPECT_EQ(Runner().WarningsNotReached().size(), 3u);
    EXPECT_EQ(App().TutorialSkipWarnings().size(), 3u);
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    EXPECT_TRUE(card->Active) << "the skip card";

    Press(TutorialButton::Back);
    EXPECT_EQ(StepUp(), "move") << "a misclick costs nothing";
    Press(TutorialButton::Skip);
    Press(TutorialButton::Done);
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(Runner().GetOutcome(), tutorial::Tutorial::Outcome::Skipped);
    EXPECT_FALSE(card->Active);
}

// Basics' three warnings, on the skip card of any topic, until Basics has
// been finished once (section 13.6).
TEST_F(TutorialAppTest, AnyTopicsSkipCardShowsTheWarningsUntilBasicsIsFinished) {
    StartTheTutorial("drawing");
    Press(TutorialButton::Skip);
    const std::vector<const tutorial::Step*> warnings = App().TutorialSkipWarnings();
    ASSERT_EQ(warnings.size(), 3u);
    EXPECT_EQ(warnings[0]->id, "programs");
    EXPECT_EQ(warnings[1]->id, "antiCheat");
    EXPECT_EQ(warnings[2]->id, "exclusiveFullscreen");
    DoneKeeping();

    AppConfig config = DefaultConfig();
    config.tutorialProgress = {{"basics", "finished"}};
    StartWithLibrary(config);
    StartTheTutorial("drawing");
    Press(TutorialButton::Skip);
    EXPECT_TRUE(App().TutorialSkipWarnings().empty()) << "read to the end of Basics already";
}

// A snippet made full screen by mistake covers the canvas, and a drag on
// it does not frame: the near miss says to delete it first, and then the
// drag the step asks for does it (question 31).
TEST_F(TutorialAppTest, AFullScreenSnippetMadeByMistakeIsDeletedAsTheLinesSay) {
    for (const auto& [topic, step] : {std::pair{"basics", "screenshot"}, std::pair{"capturing", "newDrawing"}}) {
        SCOPED_TRACE(topic);
        WalkTo(step, topic);
        if (std::string_view(topic) == "basics") {
            DoubleClick(kEmptySpot.x, kEmptySpot.y);
        } else {
            With(ImGuiMod_Ctrl, [&] { DoubleClick(kEmptySpot.x, kEmptySpot.y); });
        }
        StepFrames(3);
        ASSERT_TRUE(Canvases().CurrentOrNull()->items.back().isFullscreen);
        if (App().InDrawingMode()) {
            // A drawing comes in drawing mode, where Delete does nothing.
            EXPECT_EQ(NeedUp(), tutorial::Need::NoDrawingMode);
            PressKey(ImGuiKey_Escape);
            StepFrames(3);
        }
        EXPECT_NE(tutorial::Expand(HintUp(), App().TutorialWorld()).find("Press Delete to delete it"), std::string::npos)
            << HintUp();

        PressKey(ImGuiKey_Delete);
        StepFrames(3);
        EXPECT_EQ(HintUp(), "");
        DoStep(step);
        EXPECT_NE(StepUp(), step) << HintUp();
        ShowEditMode();  // put away, for the next topic's start
        StepFrame();
    }
}

// In the step for a screenshot of the whole screen, a drawing of it: Esc,
// then Delete, as the lines say, and the double-click then does it.
TEST_F(TutorialAppTest, AFullScreenDrawingInTheFullScreenStepGoesAsTheLinesSay) {
    WalkTo("fullscreen", "capturing");
    With(ImGuiMod_Ctrl, [&] { DoubleClick(kEmptySpot.x, kEmptySpot.y); });
    StepFrames(3);
    ASSERT_TRUE(App().InDrawingMode());
    EXPECT_EQ(HintUp(), std::string(strings::kTutorialFullscreenMissDrawingMode));
    PressKey(ImGuiKey_Escape);
    StepFrames(3);
    EXPECT_EQ(HintUp(), std::string(strings::kTutorialFullscreenMissDelete));
    PressKey(ImGuiKey_Delete);
    StepFrames(3);
    EXPECT_EQ(HintUp(), "");
    DoStep("fullscreen");
    EXPECT_EQ(StepUp(), "quickCapture") << HintUp();
}

// The silent capture its line offers beside the quick one does the step
// too: away, pressed, and the check once the overlay is back.
TEST_F(TutorialAppTest, TheSilentCaptureDoesTheCaptureStepToo) {
    WalkTo("quickCapture", "capturing");
    ShowEditMode();  // away
    StepFrame();
    TriggerHotkey(config_.hotkeySilentCapture);
    StepFrame();
    ShowEditMode();  // and back to see it
    Settle();
    EXPECT_EQ(StepUp(), "end");
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, Runner().Folder());
}

// Pressed with the overlay up, a capture hotkey does the step too: the
// goal reads the capture, not where it was pressed (question 28).
TEST_F(TutorialAppTest, ACaptureHotkeyPressedWithTheOverlayUpDoesItsStep) {
    WalkTo("quickCapture", "capturing");
    TriggerHotkey(config_.hotkeyQuickCapture);
    Settle();
    EXPECT_EQ(StepUp(), "end");
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, Runner().Folder());
}

// Inside the Overview the spotlight rings the Overview's own widgets, and
// not through the delete confirmation over it; the canvas's spots still
// go under it.
TEST_F(TutorialAppTest, TheSpotlightRingsInsideTheOverviewButNotThroughIt) {
    WalkTo("newFolder", "folders");
    const std::optional<AnchorRect> ring = App().TutorialSpotlight();
    const std::optional<AnchorRect> button = App().AnchorAt(Anchor{AnchorId::OverviewNewFolder});
    ASSERT_TRUE(ring.has_value());
    ASSERT_TRUE(button.has_value());
    EXPECT_FLOAT_EQ(ring->min.x, button->min.x);
    EXPECT_FLOAT_EQ(ring->min.y, button->min.y);
    // The card keeps clear of it.
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    EXPECT_FALSE(card->Pos.x < button->max.x && button->min.x < card->Pos.x + card->Size.x &&
                 card->Pos.y < button->max.y && button->min.y < card->Pos.y + card->Size.y);

    const CanvasId canvas = Canvases().CurrentCanvasId();
    ClickAnchor(Anchor{AnchorId::OverviewCanvasDelete, canvas});  // the confirmation up
    ASSERT_NE(App().InputStack().find("ConfirmDelete"), std::string::npos);
    EXPECT_FALSE(App().TutorialSpotlight().has_value());
    PressKey(ImGuiKey_Escape);
    StepFrames(2);

    ShowEditMode();  // put away, for the next start
    StepFrame();
    WalkTo("moveSnippet", "folders");
    Drag(300.0f, 360.0f, 600.0f, 560.0f);  // a subject
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrames(2);
    EXPECT_TRUE(App().TutorialSpot().has_value()) << "the subject is there";
    EXPECT_FALSE(App().TutorialSpotlight().has_value()) << "under the Overview";
}

// A copy pasted rather than the snippet cut: the line says so, and a cut
// then moves it.
TEST_F(TutorialAppTest, ACopyPastedOnTheOtherCanvasIsNotTheMove) {
    WalkTo("moveSnippet", "folders");
    Drag(300.0f, 360.0f, 600.0f, 560.0f);
    PressCtrlKey(ImGuiKey_C);
    ToTheOtherCanvas();
    PressCtrlKey(ImGuiKey_V);
    StepFrames(3);
    EXPECT_EQ(StepUp(), "moveSnippet");
    EXPECT_EQ(HintUp(), std::string(strings::kTutorialMoveSnippetMissCopy));
    DoStep("moveSnippet");
    EXPECT_EQ(StepUp(), "overview") << HintUp();
}

// The folder made at `newFolder` is the tutorial's: Done asks once, for
// both, and puts both in the trash; with Keep ticked, it keeps both.
TEST_F(TutorialAppTest, DoneTakesTheFolderMadeInTheRunWithTheTutorials) {
    WalkTo("end", "folders");
    ASSERT_EQ(Runner().MadeFolders().size(), 1u);
    const FolderId own = Runner().Folder();
    const FolderId made = Runner().MadeFolders().front();
    EXPECT_EQ(Canvases().FindFolder(made)->name, "Games");
    Press(TutorialButton::Done);
    EXPECT_NE(App().InputStack().find("ConfirmDelete"), std::string::npos) << "asked first, once";
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(made)));
    PressKey(ImGuiKey_Escape);
    StepFrames(2);

    controller_->GetSettings().Set(setting::kConfirmDelete, false);
    ShowEditMode();  // put away, for the next start
    StepFrame();
    WalkTo("end", "folders");
    const FolderId own2 = Runner().Folder();
    const FolderId made2 = Runner().MadeFolders().front();
    Press(TutorialButton::Done);
    EXPECT_TRUE(Canvases().IsDeleted(*Canvases().FindFolder(own2)));
    EXPECT_TRUE(Canvases().IsDeleted(*Canvases().FindFolder(made2)));
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(made))) << "only this run's";

    ShowEditMode();
    StepFrame();
    WalkTo("end", "folders");
    const FolderId own3 = Runner().Folder();
    const FolderId made3 = Runner().MadeFolders().front();
    DoneKeeping();
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(own3)));
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(made3)));
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(own)));
}

// ===== Profiles (section 18) =====

TEST_F(TutorialAppTest, TheSettingsPagesWidgetsAreMarkedWhereTheyAreDrawn) {
    host_.overlayWindow.underlyingApp = kGame;
    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Settings}));
    StepFrames(2);
    PickSection(SettingsPage::SettingsSection::Profiles);
    ASSERT_TRUE(App().AnchorAt(Anchor{AnchorId::SettingsNewProfile}).has_value());

    ClickAnchor(Anchor{AnchorId::SettingsMakeProfile});
    ASSERT_EQ(AppSettings().Profiles().size(), 1u);
    EXPECT_EQ(AppSettings().Profiles()[0].match.executables, std::vector<std::string>{"game.exe"});
    EXPECT_EQ(AppSettings().ActiveProfile(), 0u) << "running at once";
    EXPECT_TRUE(App().AnchorAt(Anchor{AnchorId::SettingsDeleteProfile, 0}).has_value());

    PickSection(SettingsPage::SettingsSection::Behavior);
    EXPECT_EQ(App().TutorialWorld().SettingsShowing(), "Game") << "Showing follows the profile made";
    const std::optional<AnchorRect> showing = App().AnchorAt(Anchor{AnchorId::SettingsShowing});
    ASSERT_TRUE(showing.has_value());
    EXPECT_GT(showing->max.x - showing->min.x, 200.0f) << "the whole box, not its text";
    EXPECT_FALSE(App().AnchorAt(Anchor{AnchorId::SettingsRevert, 0}).has_value()) << "nothing stated yet";
    ClickAnchor(Anchor{AnchorId::SettingsDontStealFocus});
    ASSERT_TRUE(AppSettings().Profiles()[0].overrides.dontStealFocus.has_value());
    EXPECT_FALSE(*AppSettings().Profiles()[0].overrides.dontStealFocus);
    EXPECT_TRUE(AppSettings().Base().dontStealFocus) << "the defaults as they were";

    ClickAnchor(Anchor{AnchorId::SettingsRevert, 0});
    EXPECT_FALSE(AppSettings().Profiles()[0].overrides.dontStealFocus.has_value()) << "handed back";

    PickShowing(std::nullopt);
    EXPECT_EQ(App().TutorialWorld().SettingsShowing(), std::nullopt);
    PickShowing("Game");
    EXPECT_EQ(App().TutorialWorld().SettingsShowing(), "Game");
}

TEST_F(TutorialAppTest, NewProfilePointsShowingAtTheProfileItMakes) {
    ShowEditMode();
    StepFrame();
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Settings}));
    StepFrames(2);
    PickSection(SettingsPage::SettingsSection::Profiles);
    ClickAnchor(Anchor{AnchorId::SettingsNewProfile});
    ASSERT_EQ(AppSettings().Profiles().size(), 1u);
    EXPECT_EQ(App().TutorialWorld().SettingsShowing(), AppSettings().Profiles()[0].name);
}

TEST_F(TutorialAppTest, TheWorldTellsTheProgramTheProfilesAndSettings) {
    AppConfig config = DefaultConfig();
    Profile mine;
    mine.name = "Mine";
    mine.match.executables.push_back("game.exe");
    mine.overrides.freezeScreen = true;           // not the default
    mine.overrides.softwarePointer = true;        // the default's value, stated
    Profile blank;
    blank.name = "Blank";
    config.profiles = {mine, blank};
    host_.overlayWindow.underlyingApp = kGame;
    StartWith(config);
    ShowEditMode();
    StepFrame();
    const tutorial::World& world = App().TutorialWorld();

    EXPECT_EQ(world.Underneath(), "game.exe");
    const std::vector<tutorial::ProfileFacts> profiles = world.Profiles();
    ASSERT_EQ(profiles.size(), 2u);
    EXPECT_EQ(profiles[0].name, "Mine");
    EXPECT_EQ(profiles[0].program, "game.exe");
    EXPECT_TRUE(profiles[0].matchesUnderneath);
    EXPECT_TRUE(profiles[0].running);
    EXPECT_EQ(profiles[0].stated, 2u);
    EXPECT_EQ(profiles[0].statedAsDefaults, 1u);
    EXPECT_EQ(profiles[1].program, "");
    EXPECT_FALSE(profiles[1].matchesUnderneath);
    EXPECT_FALSE(profiles[1].running);

    EXPECT_FALSE(world.OverviewShowsSettings());
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Settings}));
    StepFrames(2);
    EXPECT_TRUE(world.OverviewShowsSettings());
    PickSection(SettingsPage::SettingsSection::Profiles);
    EXPECT_EQ(world.SettingsSectionShown(), tutorial::SettingsSection::Profiles);
    PickSection(SettingsPage::SettingsSection::Appearance);
    EXPECT_EQ(world.SettingsSectionShown(), tutorial::SettingsSection::Other);
    PickSection(SettingsPage::SettingsSection::Behavior);
    EXPECT_EQ(world.SettingsSectionShown(), tutorial::SettingsSection::Behavior);
    EXPECT_EQ(world.SettingsShowing(), "Mine") << "the profile that runs, as the overlay came up";

    ComeBackOver(platform::ForegroundApp{});
    EXPECT_EQ(world.Underneath(), "");
    ComeBackOver(platform::ForegroundApp{"", "Some Window"});
    EXPECT_EQ(world.Underneath(), "Some Window") << "the title, where the file can't be read";
    EXPECT_EQ(world.SettingsShowing(), std::nullopt) << "the defaults, over another program";
}

TEST_F(TutorialAppTest, ProfilesMakesNoFolderAndLeavesTheCanvasUp) {
    ShowEditMode();
    StepFrame();
    const CanvasId before = Canvases().CurrentCanvasId();
    const size_t folders = Canvases().Folders().size();
    host_.overlayWindow.underlyingApp = kGame;
    Overlay().StartTutorial("profiles");
    StepFrames(2);
    ASSERT_TRUE(Runner().On());
    EXPECT_EQ(Runner().Folder(), 0u);
    EXPECT_EQ(Canvases().Folders().size(), folders);
    EXPECT_EQ(Canvases().CurrentCanvasId(), before);
}

// Done deletes the profile made in the run, and only that one, without
// asking; with Keep ticked, it keeps it.
TEST_F(TutorialAppTest, DoneDeletesTheTutorialsProfileAndKeepKeepsIt) {
    AppConfig config = DefaultConfig();
    Profile mine;
    mine.name = "Mine";
    mine.match.executables.push_back("other.exe");
    config.profiles = {mine};
    StartWith(config);
    WalkTo("end", "profiles");
    ASSERT_EQ(MadeProfiles(), std::vector<std::string>{"Game"});
    const size_t folders = Canvases().Folders().size();
    Press(TutorialButton::Done);
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(App().InputStack().find("ConfirmDelete"), std::string::npos) << "not asked";
    ASSERT_EQ(AppSettings().Profiles().size(), 1u);
    EXPECT_EQ(AppSettings().Profiles()[0].name, "Mine");
    EXPECT_EQ(Canvases().Folders().size(), folders) << "no folder to trash";
    EXPECT_EQ(Kept("profiles"), "finished");

    ShowEditMode();  // put away, for the next start
    StepFrame();
    host_.overlayWindow.underlyingApp = kGame;
    WalkTo("end", "profiles");
    DoneKeeping();
    ASSERT_EQ(AppSettings().Profiles().size(), 2u);
    EXPECT_EQ(AppSettings().Profiles()[1].name, "Game");
}

// A profile of the user's renamed as makeProfile is up is not the
// tutorial's: Done leaves it, under its new name.
TEST_F(TutorialAppTest, DoneLeavesAProfileOfTheUsersRenamedOnTheWay) {
    AppConfig config = DefaultConfig();
    Profile mine;
    mine.name = "Mine";
    mine.match.executables.push_back("game.exe");
    config.profiles = {mine};
    StartWith(config);
    WalkTo("makeProfile", "profiles");
    ASSERT_TRUE(controller_->GetSettings().RenameProfile(0, "Mine 2"));  // as its name field does
    StepFrames(2);
    EXPECT_TRUE(MadeProfiles().empty());
    for (int guard = 0; guard < 20 && StepUp() != "end"; ++guard) {
        DoStep(StepUp());
    }
    ASSERT_EQ(StepUp(), "end");
    EXPECT_EQ(TutorialsProfile(), "Game");
    Press(TutorialButton::Done);
    ASSERT_EQ(AppSettings().Profiles().size(), 1u);
    EXPECT_EQ(AppSettings().Profiles()[0].name, "Mine 2");
}

// Showing follows the list when Done takes a profile out of it, as when a
// row's trash button does.
TEST_F(TutorialAppTest, DoneMovesShowingWithTheListAsARowsTrashButtonDoes) {
    WalkTo("end", "profiles");
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Settings}));
    StepFrames(2);
    PickSection(SettingsPage::SettingsSection::Profiles);
    ClickAnchor(Anchor{AnchorId::SettingsNewProfile});  // after the tutorial's, and shown
    const std::string shown = AppSettings().Profiles().back().name;
    ASSERT_EQ(App().TutorialWorld().SettingsShowing(), shown);
    Press(TutorialButton::Done);
    ASSERT_EQ(AppSettings().Profiles().size(), 1u);
    EXPECT_EQ(App().TutorialWorld().SettingsShowing(), shown) << "still the one it showed";
}

// A spot in Settings not on screen: the ring is on the way to it - the
// Overview's Settings tab, then the section it is in (section 18.3).
TEST_F(TutorialAppTest, TheSpotlightRingsTheWayToASpotInSettings) {
    StartTheTutorial("profiles");
    ASSERT_EQ(StepUp(), "openProfiles");
    const auto ringsOn = [this](const Anchor& anchor) {
        const std::optional<AnchorRect> ring = App().TutorialSpotlight();
        const std::optional<AnchorRect> at = App().AnchorAt(anchor);
        return ring.has_value() && at.has_value() && ring->min.x == at->min.x && ring->min.y == at->min.y;
    };
    const Anchor profilesRow{AnchorId::SettingsSection,
                             static_cast<uint64_t>(SettingsPage::SettingsSection::Profiles)};

    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrames(2);
    ASSERT_FALSE(App().TutorialWorld().OverviewShowsSettings());
    EXPECT_TRUE(ringsOn(Anchor{AnchorId::OverviewSettingsTab})) << "the Canvases tab up";
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Settings}));
    StepFrames(2);
    EXPECT_TRUE(ringsOn(profilesRow));

    PickSection(SettingsPage::SettingsSection::Profiles);
    Settle();
    ASSERT_EQ(StepUp(), "makeProfile");
    EXPECT_TRUE(ringsOn(Anchor{AnchorId::SettingsMakeProfile}));
    PickSection(SettingsPage::SettingsSection::Appearance);
    EXPECT_TRUE(ringsOn(profilesRow)) << "another section picked";
}

TEST_F(TutorialAppTest, TheSpotlightRingsInsideSettings) {
    WalkTo("makeProfile", "profiles");
    const std::optional<AnchorRect> ring = App().TutorialSpotlight();
    const std::optional<AnchorRect> button = App().AnchorAt(Anchor{AnchorId::SettingsMakeProfile});
    ASSERT_TRUE(ring.has_value());
    ASSERT_TRUE(button.has_value());
    EXPECT_FLOAT_EQ(ring->min.x, button->min.x);
    EXPECT_FLOAT_EQ(ring->min.y, button->min.y);
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    EXPECT_FALSE(card->Pos.x < button->max.x && button->min.x < card->Pos.x + card->Size.x &&
                 card->Pos.y < button->max.y && button->min.y < card->Pos.y + card->Size.y);
}

// The tutorial's profile deleted partway: the line says to go Back and
// make one, and doing so brings the steps back.
TEST_F(TutorialAppTest, TheTutorialsProfileDeletedIsMadeAgainByGoingBack) {
    WalkTo("change", "profiles");
    PickSection(SettingsPage::SettingsSection::Profiles);
    ClickAnchor(Anchor{AnchorId::SettingsDeleteProfile, ProfileIndex("Game")});
    PickSection(SettingsPage::SettingsSection::Behavior);
    StepFrames(2);
    EXPECT_EQ(NeedUp(), tutorial::Need::TutorialsProfile);
    Press(TutorialButton::Back);
    Press(TutorialButton::Back);
    ASSERT_EQ(StepUp(), "makeProfile");
    PickSection(SettingsPage::SettingsSection::Profiles);
    DoStep("makeProfile");
    DoStep("behavior");
    ASSERT_EQ(StepUp(), "change");
    EXPECT_FALSE(NeedUp().has_value()) << HintUp();
}

// Over a program that already has a profile of the user's, the card says
// the one made won't run - and the steps go on with it.
TEST_F(TutorialAppTest, AProgramWithAProfileAlreadyGetsASecondOneForPractice) {
    AppConfig config = DefaultConfig();
    Profile mine;
    mine.name = "Mine";
    mine.match.executables.push_back("game.exe");
    config.profiles = {mine};
    StartWith(config);
    WalkTo("makeProfile", "profiles");
    EXPECT_EQ(HintUp(), std::string(strings::kTutorialMakeProfileMissTaken));
    DoStep("makeProfile");
    ASSERT_EQ(StepUp(), "behavior") << HintUp();
    EXPECT_EQ(TutorialsProfile(), "Game");
    EXPECT_EQ(AppSettings().ActiveProfile(), 0u) << "the user's, first in the list";
}

// ===== Progress, kept (section 13.7) =====

TEST_F(TutorialAppTest, TheProgressIsKeptInTheSettingsAsItGoes) {
    EXPECT_EQ(Kept(), "") << "never shown";
    StartTheTutorial();
    EXPECT_EQ(Kept(), "started");
    EXPECT_EQ(Runner().Folder(), 0u) << "none while the welcome is read (section 13.5)";

    Press(TutorialButton::Next);
    StepFrame();
    EXPECT_NE(Runner().Folder(), 0u);
    EXPECT_EQ(Kept(), "started") << "no step is kept";

    Press(TutorialButton::Skip);
    EXPECT_EQ(Kept(), "skipped");
    Press(TutorialButton::Back);
    EXPECT_EQ(Kept(), "started");
    Press(TutorialButton::Skip);
    Press(TutorialButton::Done);
    EXPECT_EQ(Kept(), "skipped");
    EXPECT_EQ(Kept("drawing"), "") << "only the topic that ran";
}

TEST_F(TutorialAppTest, AFinishedTutorialIsKeptAsFinished) {
    WalkTo("end");
    Press(TutorialButton::Done);
    EXPECT_EQ(Kept(), "finished");
}

// Another topic started while one runs: the running one ends as Done,
// keep the folder would (section 13.3).
TEST_F(TutorialAppTest, AnotherTopicStartedLeavesTheRunningOnesStepAndFolder) {
    WalkTo("move");
    const FolderId basics = Runner().Folder();
    Overlay().StartTutorial("drawing");
    StepFrames(2);
    EXPECT_EQ(Overlay().TutorialTopic().id, "drawing");
    EXPECT_EQ(StepUp(), "drawingMode");
    EXPECT_NE(Runner().Folder(), basics);
    EXPECT_EQ(Canvases().FindFolder(Runner().Folder())->name, "Tutorial: Drawing and notes");
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(basics))) << "kept";
    EXPECT_EQ(Kept(), "started") << "left partway";
    EXPECT_EQ(Kept("drawing"), "started");
    EXPECT_EQ(App().InputStack().find("ConfirmDelete"), std::string::npos) << "nothing asked";
}

TEST_F(TutorialAppTest, AnotherTopicStartedFromAnEndCardOrASkipCardCountsItsEnd) {
    WalkTo("end");
    Overlay().StartTutorial("drawing");
    StepFrames(2);
    EXPECT_EQ(Kept(), "finished");

    Press(TutorialButton::Skip);
    Overlay().StartTutorial(tutorial::kBasicsTopic);
    StepFrames(2);
    EXPECT_EQ(Kept("drawing"), "skipped");
    EXPECT_EQ(Kept(), "started") << "started again";
}

// ===== The list (section 13.3) =====

// More topics ends the topic as Done does - the folder kept with Keep
// ticked - and opens the list, which has no way back to it (section 20).
TEST_F(TutorialAppTest, MoreTopicsEndsTheTopicAsDoneDoesAndOpensTheList) {
    WalkTo("end");
    const FolderId folder = Runner().Folder();
    Press(TutorialButton::Keep);
    Press(TutorialButton::MoreTopics);
    ASSERT_TRUE(App().TutorialListed());
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(Runner().GetOutcome(), tutorial::Tutorial::Outcome::Finished);
    EXPECT_EQ(App().TutorialStatus(*tutorial::FindTopic("basics")), TutorialCard::Status::Done);
    EXPECT_EQ(App().TutorialStatus(*tutorial::FindTopic("drawing")), TutorialCard::Status::New);
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(folder))) << "kept";
    EXPECT_EQ(App().InputStack().find("ConfirmDelete"), std::string::npos);
    EXPECT_EQ(Kept(), "finished");
    Press(TutorialButton::CloseList);
    EXPECT_FALSE(App().TutorialListed());
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    EXPECT_FALSE(card->Active) << "nothing to go back to";

    // Not ticked: asked, as at Done, with the list open behind - once
    // there is a folder, past the welcome.
    Overlay().StartTutorial();
    StepFrames(2);
    Press(TutorialButton::Next);
    StepFrame();
    const FolderId second = Runner().Folder();
    Press(TutorialButton::Skip);
    Press(TutorialButton::MoreTopics);
    EXPECT_TRUE(App().TutorialListed());
    EXPECT_NE(App().InputStack().find("ConfirmDelete"), std::string::npos);
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(second))) << "asked first";
    EXPECT_EQ(Kept(), "skipped");
}

// The list opened from Settings during a topic: its Close goes back to the
// topic, where it was.
TEST_F(TutorialAppTest, TheListOpenedDuringATopicClosesBackToIt) {
    WalkTo("move");
    Overlay().OpenTutorialList();
    StepFrames(2);
    ASSERT_TRUE(App().TutorialListed());
    EXPECT_EQ(App().TutorialStatus(*tutorial::FindTopic("basics")), TutorialCard::Status::Running);
    Press(TutorialButton::CloseList);
    EXPECT_FALSE(App().TutorialListed());
    EXPECT_EQ(StepUp(), "move");
}

// Keep starts unticked on every end and skip card: going Back from one
// leaves it unticked for the next.
TEST_F(TutorialAppTest, KeepStartsUntickedOnEachCard) {
    WalkTo("move");
    Press(TutorialButton::Skip);
    Press(TutorialButton::Keep);
    EXPECT_TRUE(App().TutorialKeep());
    Press(TutorialButton::Back);
    Press(TutorialButton::Skip);
    EXPECT_FALSE(App().TutorialKeep());
}

// The list always at the top center, where it first was: a card dragged
// during a topic is placed as at the start again once the list opens, and
// stays so for the topic started from it (docs/TUTORIAL.md, section 19.5).
TEST_F(TutorialAppTest, TheListForgetsWhereTheCardWasDragged) {
    Overlay().OpenTutorialList();
    StepFrames(2);
    const ImGuiWindow* card = ImGui::FindWindowByName("##tutorial_card");
    ASSERT_NE(card, nullptr);
    const ImVec2 listAt = card->Pos;
    EXPECT_FLOAT_EQ(listAt.x, (kDisplayWidth - card->Size.x) * 0.5f) << "centered";

    StartTheTutorial();
    const ImVec2 cardAt = card->Pos;
    DragCard(-200.0f, 150.0f);
    ASSERT_FLOAT_EQ(card->Pos.x, cardAt.x - 200.0f) << "dragged";
    Press(TutorialButton::Next);
    StepFrames(20);
    EXPECT_FLOAT_EQ(card->Pos.x, cardAt.x - 200.0f) << "left where it was put";

    // Over the Overview, the list still at the top center.
    Press(TutorialButton::Skip);
    Press(TutorialButton::Keep);
    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
    StepFrames(2);
    Press(TutorialButton::MoreTopics);
    StepFrames(2);
    ASSERT_TRUE(App().TutorialListed());
    EXPECT_FLOAT_EQ(card->Pos.x, listAt.x);
    EXPECT_FLOAT_EQ(card->Pos.y, listAt.y);
    Overlay().StartTutorial("pinning");
    StepFrames(20);
    EXPECT_NE(card->Pos.x, cardAt.x - 200.0f) << "a new topic placed as at the start";
}

// ===== The folder (sections 6.4, 7.6 and 9) =====

TEST_F(TutorialAppTest, ACaptureHotkeyDuringTheTutorialLandsInItsFolder) {
    WalkTo("screenshot");
    const CanvasId before = Canvases().CurrentCanvasId();
    TriggerHotkey(config_.hotkeyQuickCapture);
    StepFrames(2);
    EXPECT_NE(Canvases().CurrentCanvasId(), before) << "a canvas of its own";
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, Runner().Folder());
}

TEST_F(TutorialAppTest, WithItsFolderDeletedGoBackToTheTutorialMakesANewOne) {
    WalkTo("move");
    const FolderId old = Runner().Folder();
    ASSERT_TRUE(controller_->GetSession().Delete(old));
    StepFrames(2);
    ASSERT_EQ(NeedUp(), tutorial::Need::InTutorialFolder) << HintUp();

    Overlay().PressTutorialHint();
    StepFrames(2);
    const FolderId made = Runner().Folder();
    EXPECT_NE(made, old);
    ASSERT_NE(Canvases().FindFolder(made), nullptr);
    EXPECT_EQ(Canvases().FindFolder(made)->name, "Tutorial: Basics");
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, made);
    EXPECT_EQ(StepUp(), "move");
    EXPECT_EQ(NeedUp(), tutorial::Need::ASubject) << "nothing in it yet: " << HintUp();
}

TEST_F(TutorialAppTest, StartingAgainMakesANewFolderAndLeavesTheOldOne) {
    WalkTo("move");
    const FolderId old = Runner().Folder();
    Overlay().StartTutorial();
    StepFrames(2);
    EXPECT_EQ(StepUp(), "welcome");
    Press(TutorialButton::Next);
    StepFrame();
    EXPECT_NE(Runner().Folder(), 0u);
    EXPECT_NE(Runner().Folder(), old);
    ASSERT_NE(Canvases().FindFolder(old), nullptr);
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(old)));
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, Runner().Folder());
}

// Done ends the tutorial with its folder in the trash; with Keep ticked,
// it keeps it (question 9) - on the end card and on the skip card.
TEST_F(TutorialAppTest, DoneOnTheEndCardPutsTheFolderInTheTrash) {
    AppConfig config = DefaultConfig();
    config.confirmDelete = false;
    StartWith(config);
    WalkTo("end");
    const FolderId folder = Runner().Folder();
    Press(TutorialButton::Done);
    EXPECT_FALSE(Runner().On());
    ASSERT_NE(Canvases().FindFolder(folder), nullptr);
    EXPECT_TRUE(Canvases().IsDeleted(*Canvases().FindFolder(folder))) << "in the trash, to restore";
    ASSERT_NE(Canvases().CurrentOrNull(), nullptr);
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().CurrentOrNull())) << "left for a canvas not deleted";
    EXPECT_EQ(Kept(), "finished");
}

TEST_F(TutorialAppTest, DoneAsksFirstWhereSettingsSaysTo) {
    WalkTo("end");
    const FolderId folder = Runner().Folder();
    Press(TutorialButton::Done);
    EXPECT_FALSE(Runner().On());
    EXPECT_NE(App().InputStack().find("ConfirmDelete"), std::string::npos) << App().InputStack();
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(folder))) << "until the confirmation says so";
}

TEST_F(TutorialAppTest, DoneWithKeepTickedKeepsTheFolder) {
    WalkTo("end");
    const FolderId folder = Runner().Folder();
    DoneKeeping();
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(Runner().GetOutcome(), tutorial::Tutorial::Outcome::Finished);
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(folder)));
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, folder) << "and stays in it";
    EXPECT_EQ(App().InputStack().find("ConfirmDelete"), std::string::npos);
    EXPECT_EQ(Kept(), "finished");
}

TEST_F(TutorialAppTest, TheSkipCardEndsWithTheFolderTrashedOrKept) {
    AppConfig config = DefaultConfig();
    config.confirmDelete = false;
    StartWith(config);
    WalkTo("move");
    const FolderId first = Runner().Folder();
    Press(TutorialButton::Skip);
    DoneKeeping();
    EXPECT_FALSE(Runner().On());
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(first)));
    EXPECT_EQ(Kept(), "skipped");

    Overlay().StartTutorial();
    StepFrames(2);
    Press(TutorialButton::Next);
    StepFrame();
    const FolderId second = Runner().Folder();
    Press(TutorialButton::Skip);
    Press(TutorialButton::Done);
    EXPECT_FALSE(Runner().On());
    EXPECT_TRUE(Canvases().IsDeleted(*Canvases().FindFolder(second)));
    EXPECT_FALSE(Canvases().IsDeleted(*Canvases().FindFolder(first))) << "only this run's";
}

TEST_F(TutorialAppTest, ADoneWithNoTutorialOnTrashesNothing) {
    ShowEditMode();
    StepFrame();
    Press(TutorialButton::Done);
    EXPECT_EQ(App().InputStack().find("ConfirmDelete"), std::string::npos);
    EXPECT_FALSE(Runner().On());
}

// ===== The start (sections 13.4, 13.7 and 9) =====

TEST_F(TutorialAppTest, AFirstRunPlacesNoNotesAndStartsTheChain) {
    StartAsFirstRun();
    EXPECT_EQ(controller_->State(), app::OverlayState::Edit) << "a first run comes up in edit mode";
    StepFrames(2);
    ASSERT_TRUE(Runner().On());
    EXPECT_EQ(StepUp(), "welcome");
    // The folder a first run makes, left empty for the user's own work,
    // and the tutorial's beside it once the welcome is read (section 13.5).
    EXPECT_EQ(Canvases().Folders().size(), 1u) << "none yet for the tutorial";
    Press(TutorialButton::Next);
    StepFrame();
    EXPECT_EQ(StepUp(), "screenshot");
    ASSERT_EQ(Canvases().Folders().size(), 2u);
    EXPECT_EQ(Canvases().Folders().back().id, Runner().Folder());
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, Runner().Folder());
    for (const Canvas& canvas : Canvases().Canvases()) {
        EXPECT_TRUE(canvas.items.empty()) << "no notes";
    }
}

// Skipped on the welcome, a topic has made no folder, and its Done ends
// it with nothing to put away: no confirmation, and no checkbox (section
// 13.5). A returning user who skips at once had to put one away too.
TEST_F(TutorialAppTest, ASkipOnTheWelcomeLeavesNoFolderToPutAway) {
    StartAsFirstRun();
    StepFrames(2);
    ASSERT_EQ(StepUp(), "welcome");
    EXPECT_EQ(Runner().Folder(), 0u);
    Press(TutorialButton::Skip);
    StepFrame();
    EXPECT_EQ(Runner().Folder(), 0u) << "the skip card is no do step";
    EXPECT_EQ(App().TutorialSkipWarnings().size(), 3u) << "the warnings, all the same";
    EXPECT_TRUE(Runner().MadeFolders().empty()) << "nothing for the checkbox to keep";
    Press(TutorialButton::Done);
    StepFrame();
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(App().InputStack().find("ConfirmDelete"), std::string::npos) << "nothing asked";
    EXPECT_EQ(Canvases().Folders().size(), 1u);
}

// Back from that skip card and on: the folder comes with the first do
// step, there from its first frame.
TEST_F(TutorialAppTest, TheFolderComesWithTheFirstDoStep) {
    StartAsFirstRun();
    StepFrames(2);
    Press(TutorialButton::Skip);
    Press(TutorialButton::Back);
    StepFrame();
    ASSERT_EQ(StepUp(), "welcome");
    EXPECT_EQ(Canvases().Folders().size(), 1u);
    Press(TutorialButton::Next);
    StepFrame();
    ASSERT_EQ(StepUp(), "screenshot");
    EXPECT_NE(Runner().Folder(), 0u);
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, Runner().Folder());
    EXPECT_FALSE(Runner().CurrentHint().has_value()) << "in its folder, with nothing to say";
}

// Quit partway, Basics starts over from its welcome at the next start:
// nothing goes on after a restart. The run's folder stays, as Keep leaves
// it, and the new run makes its own with its first do step.
TEST_F(TutorialAppTest, BasicsQuitPartwayStartsOverAtTheNextStart) {
    AppConfig config = DefaultConfig();
    config.tutorialProgress = {{"basics", "started"}};
    StartWithLibrary(config);
    WalkTo("move");
    const FolderId folder = Runner().Folder();
    const size_t folders = Canvases().Folders().size();
    ASSERT_EQ(Kept(), "started");

    StartWith(AppSettings().Stored());  // the library file is kept
    EXPECT_FALSE(Runner().On()) << "not before edit mode comes up";
    ShowEditMode();
    StepFrames(2);
    ASSERT_TRUE(Runner().On());
    EXPECT_EQ(StepUp(), "welcome");
    EXPECT_EQ(Runner().Folder(), 0u);
    EXPECT_NE(Canvases().FindFolder(folder), nullptr) << "the last run's folder, left as it was";
    EXPECT_EQ(Canvases().Folders().size(), folders);
    Press(TutorialButton::Next);
    StepFrame();
    EXPECT_NE(Runner().Folder(), folder) << "a folder of its own";
}

// Any other topic quit partway is over: it shows as started in the list,
// and nothing starts.
TEST_F(TutorialAppTest, AnotherTopicQuitPartwayIsOverAtTheNextStart) {
    StartWithLibrary();  // Basics finished
    WalkTo("draw", "drawing");
    ASSERT_EQ(Kept("drawing"), "started");

    StartWith(AppSettings().Stored());
    ShowEditMode();
    StepFrames(2);
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(Kept("drawing"), "started");
}

TEST_F(TutorialAppTest, BasicsSkippedStartsNothing) {
    AppConfig config = DefaultConfig();
    config.tutorialProgress = {{"basics", "skipped"}};
    StartWithLibrary(config);
    ShowEditMode();
    StepFrames(2);
    EXPECT_FALSE(Runner().On());
}

// What builds before 0.2.3 kept: a step's id for each topic left partway,
// and the topic running. A step id reads as started, so Basics starts over;
// another topic's does not start.
TEST_F(TutorialAppTest, AStepIdKeptByAnOlderBuildReadsAsStarted) {
    AppConfig config = DefaultConfig();
    config.tutorialProgress = {{"basics", "move"}};
    StartWithLibrary(config);
    ShowEditMode();
    StepFrames(2);
    ASSERT_TRUE(Runner().On());
    EXPECT_EQ(StepUp(), "welcome");

    config.tutorialProgress = {{"basics", "finished"}, {"drawing", "draw"}};
    StartWithLibrary(config);
    ShowEditMode();
    StepFrames(2);
    EXPECT_FALSE(Runner().On());
}

// Treated as a new user (question 11): Basics starts, the first time edit
// mode comes up.
TEST_F(TutorialAppTest, AnInstallFromBeforeStartsBasicsLikeAFirstRun) {
    StartWithLibraryFromBefore();
    EXPECT_EQ(controller_->State(), app::OverlayState::Hidden) << "not a first run";
    EXPECT_FALSE(Runner().On());
    ShowEditMode();
    StepFrames(2);
    ASSERT_TRUE(Runner().On());
    EXPECT_EQ(Overlay().TutorialTopic().id, "basics");
    EXPECT_EQ(StepUp(), "welcome");
    Press(TutorialButton::Next);
    StepFrame();
    EXPECT_NE(Runner().Folder(), 0u);
    EXPECT_EQ(Canvases().CurrentOrNull()->folderId, Runner().Folder());
}

TEST_F(TutorialAppTest, ALibraryWhoseTutorialIsOverStartsNothing) {
    StartWithLibrary();  // Basics finished
    ShowEditMode();
    StepFrames(2);
    EXPECT_FALSE(Runner().On());
    EXPECT_EQ(ImGui::FindWindowByName("##tutorial_card") != nullptr &&
                  ImGui::FindWindowByName("##tutorial_card")->Active,
              false);
}

// A new library is not a new person: deleted, or set aside, with the
// settings kept, it is someone who has met the app (section 13.7). The
// overlay still comes up, for the hotkey; Basics did too, until 2026-10-02.
TEST_F(TutorialAppTest, ANewLibraryWhoseSettingsSayTheTutorialIsOverStartsNothing) {
    AppConfig config = DefaultConfig();
    config.tutorialProgress = {{"basics", "finished"}};
    StartAsFirstRun(config);
    EXPECT_EQ(controller_->State(), app::OverlayState::Edit) << "a new library comes up in edit mode";
    StepFrames(2);
    EXPECT_FALSE(Runner().On());
}

// ===== The derail matrix (sections 6.1 and 9) =====
//
// Each do step, crossed with each way off the path that applies to it: the
// card says a line that applies, and doing what the lines say - one after
// the other, as a user would - and then the step completes it.

// A way off the path, as a user takes it.
enum class Way {
    Overview,
    CheatSheet,
    OtherFolder,
    OtherCanvasHere,
    HiddenAndShown,
    ViewOnly,
    Deleted,
    UndoneSeveralTimes,
    Minimized,
    Fullscreen,
    CaptureHotkey,
    LeftDrawingMode,
    EnteredDrawingMode,
    Unpinned,
    EraserPicked,
    ColorChooserUp,
    PropertiesUp,
    OpacityByWheel,
    DrewFreehand,
    DrewALine,
    ErasedRound,
    ErasedWithTheEraser,
    TypingLeftOpen,
    FramedAScreenshot,
    DrawingToolPicked,
    CopyPasted,
    OverviewClosed,
    SettingsTab,
    OtherFolderBrowsed,
    NewCanvasMade,
    ShowDeletedOff,
    CanvasesTabPicked,
    OtherSection,
    NothingUnderneath,
    NewProfileMade,
    ShowingDefaults,
    TickedBack,
};

const char* WayName(Way way) {
    switch (way) {
        case Way::Overview: return "Overview";
        case Way::CheatSheet: return "CheatSheet";
        case Way::OtherFolder: return "OtherFolder";
        case Way::OtherCanvasHere: return "OtherCanvasHere";
        case Way::HiddenAndShown: return "HiddenAndShown";
        case Way::ViewOnly: return "ViewOnly";
        case Way::Deleted: return "Deleted";
        case Way::UndoneSeveralTimes: return "UndoneSeveralTimes";
        case Way::Minimized: return "Minimized";
        case Way::Fullscreen: return "Fullscreen";
        case Way::CaptureHotkey: return "CaptureHotkey";
        case Way::LeftDrawingMode: return "LeftDrawingMode";
        case Way::EnteredDrawingMode: return "EnteredDrawingMode";
        case Way::Unpinned: return "Unpinned";
        case Way::EraserPicked: return "EraserPicked";
        case Way::ColorChooserUp: return "ColorChooserUp";
        case Way::PropertiesUp: return "PropertiesUp";
        case Way::OpacityByWheel: return "OpacityByWheel";
        case Way::DrewFreehand: return "DrewFreehand";
        case Way::DrewALine: return "DrewALine";
        case Way::ErasedRound: return "ErasedRound";
        case Way::ErasedWithTheEraser: return "ErasedWithTheEraser";
        case Way::TypingLeftOpen: return "TypingLeftOpen";
        case Way::FramedAScreenshot: return "FramedAScreenshot";
        case Way::DrawingToolPicked: return "DrawingToolPicked";
        case Way::CopyPasted: return "CopyPasted";
        case Way::OverviewClosed: return "OverviewClosed";
        case Way::SettingsTab: return "SettingsTab";
        case Way::OtherFolderBrowsed: return "OtherFolderBrowsed";
        case Way::NewCanvasMade: return "NewCanvasMade";
        case Way::ShowDeletedOff: return "ShowDeletedOff";
        case Way::CanvasesTabPicked: return "CanvasesTabPicked";
        case Way::OtherSection: return "OtherSection";
        case Way::NothingUnderneath: return "NothingUnderneath";
        case Way::NewProfileMade: return "NewProfileMade";
        case Way::ShowingDefaults: return "ShowingDefaults";
        case Way::TickedBack: return "TickedBack";
    }
    return "?";
}

struct Derail {
    const char* topic;
    const char* step;
    Way way;
    // The need the card says a line for - none where the way breaks
    // nothing the step needs.
    std::optional<tutorial::Need> says;
};

std::vector<Derail> Matrix() {
    using enum Way;
    using tutorial::Need;
    const std::optional<Need> nothing;
    std::vector<Derail> cases = {
        {"basics", "screenshot", Overview, Need::CanvasUncovered},
        {"basics", "screenshot", CheatSheet, Need::CanvasUncovered},
        {"basics", "screenshot", OtherFolder, Need::InTutorialFolder},
        {"basics", "screenshot", HiddenAndShown, nothing},
        {"basics", "screenshot", ViewOnly, nothing},
        {"basics", "screenshot", CaptureHotkey, nothing},
    };
    for (const char* step : {"move", "resize"}) {
        cases.push_back({"basics", step, Overview, Need::CanvasUncovered});
        cases.push_back({"basics", step, CheatSheet, Need::CanvasUncovered});
        cases.push_back({"basics", step, OtherFolder, Need::InTutorialFolder});
        cases.push_back({"basics", step, OtherCanvasHere, Need::SubjectHere});
        cases.push_back({"basics", step, HiddenAndShown, nothing});
        cases.push_back({"basics", step, ViewOnly, nothing});
        cases.push_back({"basics", step, Deleted, Need::ASubject});
        cases.push_back({"basics", step, UndoneSeveralTimes, Need::ASubject});
        cases.push_back({"basics", step, Minimized, Need::SubjectOnScreen});
        cases.push_back({"basics", step, Fullscreen, Need::SubjectCanMove});
        cases.push_back({"basics", step, CaptureHotkey, Need::SubjectHere});
        cases.push_back({"basics", step, EnteredDrawingMode, Need::NoDrawingMode});
    }
    const std::vector<Derail> more = {
        {"drawing", "drawingMode", Overview, Need::CanvasUncovered},
        {"drawing", "drawingMode", CheatSheet, Need::CanvasUncovered},
        {"drawing", "drawingMode", OtherFolder, Need::InTutorialFolder},
        {"drawing", "drawingMode", OtherCanvasHere, Need::SubjectHere},
        {"drawing", "drawingMode", HiddenAndShown, nothing},
        {"drawing", "drawingMode", ViewOnly, nothing},
        {"drawing", "drawingMode", Deleted, Need::ASubject},
        {"drawing", "drawingMode", Minimized, Need::SubjectOnScreen},
        {"drawing", "drawingMode", Fullscreen, nothing},
        {"drawing", "draw", Overview, Need::CanvasUncovered},
        {"drawing", "draw", CheatSheet, Need::CanvasUncovered},
        {"drawing", "draw", HiddenAndShown, nothing},
        {"drawing", "draw", ViewOnly, Need::DrawingOnSubject},
        {"drawing", "draw", LeftDrawingMode, Need::DrawingOnSubject},
        {"basics", "delete", Overview, Need::CanvasUncovered},
        {"basics", "delete", CheatSheet, Need::CanvasUncovered},
        {"basics", "delete", OtherFolder, Need::InTutorialFolder},
        {"basics", "delete", OtherCanvasHere, Need::SubjectHere},
        {"basics", "delete", HiddenAndShown, nothing},
        {"basics", "delete", ViewOnly, nothing},
        {"basics", "delete", Minimized, Need::SubjectOnScreen},
        {"basics", "delete", EnteredDrawingMode, Need::NoDrawingMode},
        {"basics", "undo", Overview, Need::CanvasUncovered},
        {"basics", "undo", CheatSheet, Need::CanvasUncovered},
        {"basics", "undo", OtherFolder, Need::InTutorialFolder},
        {"basics", "undo", OtherCanvasHere, Need::SubjectHere},
        {"basics", "undo", HiddenAndShown, nothing},
        {"basics", "undo", ViewOnly, nothing},
        {"basics", "away", Overview, nothing},
        {"basics", "away", CheatSheet, nothing},
        {"pinning", "pin", Overview, Need::CanvasUncovered},
        {"pinning", "pin", CheatSheet, Need::CanvasUncovered},
        {"pinning", "pin", OtherFolder, Need::InTutorialFolder},
        {"pinning", "pin", OtherCanvasHere, Need::SubjectHere},
        {"pinning", "pin", Deleted, Need::ASubject},
        {"pinning", "pin", Minimized, Need::SubjectOnScreen},
        {"pinning", "pin", EnteredDrawingMode, nothing},
        {"pinning", "pinnedAway", Overview, nothing},
        {"pinning", "pinnedAway", OtherFolder, Need::InTutorialFolder},
        {"pinning", "pinnedAway", OtherCanvasHere, Need::SubjectHere},
        {"pinning", "pinnedAway", Deleted, Need::ASubject},
        {"pinning", "pinnedAway", Minimized, Need::SubjectOnScreen},
        {"pinning", "pinnedAway", Unpinned, Need::SubjectPinned},
        {"pinning", "pinnedAway", ViewOnly, nothing},
        {"pinning", "opacity", Overview, Need::CanvasUncovered},
        {"pinning", "opacity", CheatSheet, Need::CanvasUncovered},
        {"pinning", "opacity", OtherFolder, Need::InTutorialFolder},
        {"pinning", "opacity", OtherCanvasHere, Need::SubjectHere},
        {"pinning", "opacity", Deleted, Need::ASubject},
        {"pinning", "opacity", Minimized, Need::SubjectOnScreen},
        {"pinning", "opacity", EnteredDrawingMode, nothing},
        {"pinning", "viewMode", Overview, nothing},
        {"pinning", "viewMode", HiddenAndShown, nothing},
        {"pinning", "unpin", Overview, Need::CanvasUncovered},
        {"pinning", "unpin", OtherFolder, Need::InTutorialFolder},
        {"pinning", "unpin", OtherCanvasHere, Need::SubjectHere},
        {"pinning", "unpin", Deleted, Need::ASubject},
        {"pinning", "unpin", Minimized, Need::SubjectOnScreen},
        {"pinning", "unpin", EnteredDrawingMode, nothing},
        {"pinning", "opacity", PropertiesUp, nothing},
        {"drawing", "color", ColorChooserUp, nothing},
        {"drawing", "width", OpacityByWheel, nothing},
        {"drawing", "line", DrewFreehand, nothing},
        {"drawing", "rectangle", DrewALine, nothing},
        {"drawing", "eraseRect", ErasedRound, nothing},
        {"drawing", "eraseRight", ErasedWithTheEraser, nothing},
        {"drawing", "note", TypingLeftOpen, nothing},
        {"capturing", "newDrawing", Overview, Need::CanvasUncovered},
        {"capturing", "newDrawing", CheatSheet, Need::CanvasUncovered},
        {"capturing", "newDrawing", OtherFolder, Need::InTutorialFolder},
        {"capturing", "newDrawing", HiddenAndShown, nothing},
        {"capturing", "newDrawing", FramedAScreenshot, nothing},
        {"capturing", "fullscreen", Overview, Need::CanvasUncovered},
        {"capturing", "fullscreen", CheatSheet, Need::CanvasUncovered},
        {"capturing", "fullscreen", OtherFolder, Need::InTutorialFolder},
        {"capturing", "fullscreen", HiddenAndShown, nothing},
        {"capturing", "fullscreen", FramedAScreenshot, nothing},
        {"capturing", "fullscreen", DrawingToolPicked, Need::NoOtherTool},
        {"capturing", "quickCapture", OtherFolder, Need::InTutorialFolder},
        {"capturing", "quickCapture", Overview, nothing},
        {"capturing", "quickCapture", HiddenAndShown, nothing},
        {"folders", "newCanvas", OtherFolder, Need::InTutorialFolder},
        {"folders", "newCanvas", HiddenAndShown, nothing},
        {"folders", "moveSnippet", Overview, Need::CanvasUncovered},
        {"folders", "moveSnippet", CheatSheet, Need::CanvasUncovered},
        {"folders", "moveSnippet", OtherFolder, Need::InTutorialFolder},
        {"folders", "moveSnippet", HiddenAndShown, nothing},
        {"folders", "moveSnippet", CopyPasted, nothing},
        {"folders", "overview", OtherFolder, Need::InTutorialFolder},
        {"folders", "overview", HiddenAndShown, nothing},
        {"folders", "newFolder", NewCanvasMade, nothing},
        {"folders", "restore", ShowDeletedOff, Need::DeletedShown},
        // The Overview closed over a canvas of the tutorial's is the step
        // done (chains_test), so no OverviewClosed row.
        {"folders", "openCanvas", HiddenAndShown, nothing},
        {"folders", "openCanvas", SettingsTab, Need::CanvasesTab},
        {"folders", "openCanvas", OtherFolderBrowsed, nothing},
    };
    cases.insert(cases.end(), more.begin(), more.end());
    // Drawing's steps after the stroke: each needs drawing mode on its
    // snippet, and those that draw need the pen.
    for (const char* step : {"color", "width", "line", "rectangle", "erase", "eraseRect", "eraseRight", "note"}) {
        cases.push_back({"drawing", step, LeftDrawingMode, Need::DrawingOnSubject});
        cases.push_back({"drawing", step, CheatSheet, Need::CanvasUncovered});
        cases.push_back({"drawing", step, Minimized, Need::SubjectOnScreen});
    }
    for (const char* step : {"color", "width", "line", "rectangle"}) {
        cases.push_back({"drawing", step, EraserPicked, Need::PenInHand});
    }
    // Folders and canvases' steps in the Overview: each needs it up, and
    // on its canvases.
    for (const char* step :
         {"newFolder", "rename", "switchFolder", "moveCanvas", "deleteCanvas", "showDeleted", "restore"}) {
        cases.push_back({"folders", step, OverviewClosed, Need::OverviewUp});
        cases.push_back({"folders", step, HiddenAndShown, nothing});  // hidden keeps the Overview up
        cases.push_back({"folders", step, SettingsTab, Need::CanvasesTab});
        cases.push_back({"folders", step, OtherFolderBrowsed, nothing});
    }
    // Profiles' steps in Settings: each needs it up, on its Settings tab,
    // in its section. Put away and back over the same program keeps both
    // the panel and Showing.
    for (const char* step : {"makeProfile", "behavior", "change", "revert"}) {
        cases.push_back({"profiles", step, OverviewClosed, Need::SettingsUp});
        cases.push_back({"profiles", step, CanvasesTabPicked, Need::SettingsTab});
    }
    const std::vector<Derail> profiles = {
        {"profiles", "makeProfile", HiddenAndShown, nothing},
        {"profiles", "makeProfile", OtherSection, Need::ProfilesSection},
        {"profiles", "makeProfile", NothingUnderneath, Need::AProgramUnderneath},
        {"profiles", "makeProfile", NewProfileMade, nothing},
        {"profiles", "behavior", HiddenAndShown, nothing},
        {"profiles", "behavior", OtherSection, nothing},
        {"profiles", "change", HiddenAndShown, nothing},
        {"profiles", "change", OtherSection, Need::BehaviorSection},
        {"profiles", "change", ShowingDefaults, Need::ShowingIt},
        {"profiles", "revert", HiddenAndShown, nothing},
        {"profiles", "revert", OtherSection, Need::BehaviorSection},
        {"profiles", "revert", ShowingDefaults, Need::ShowingIt},
        {"profiles", "revert", TickedBack, nothing},
    };
    cases.insert(cases.end(), profiles.begin(), profiles.end());
    return cases;
}

class TutorialDerailTest : public TutorialAppTest, public ::testing::WithParamInterface<Derail> {
protected:
    void TakeTheWay(Way way) {
        const std::optional<ItemId> subject = Runner().Subject();
        switch (way) {
            case Way::Overview:
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
                break;
            case Way::CheatSheet:
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::CheatSheet}));
                break;
            case Way::OtherFolder:
                // The canvas the app started on, outside the tutorial's folder.
                for (const Canvas& canvas : Canvases().Canvases()) {
                    if (canvas.folderId != Runner().Folder()) {
                        controller_->GetSession().SwitchToCanvas(canvas.id);
                        break;
                    }
                }
                ASSERT_NE(Canvases().CurrentOrNull()->folderId, Runner().Folder());
                break;
            case Way::OtherCanvasHere:
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::NewCanvas}));
                break;
            case Way::HiddenAndShown:
                ShowEditMode();
                StepFrame();
                ShowEditMode();
                break;
            case Way::ViewOnly:
                ShowViewMode();
                StepFrame();
                ShowEditMode();
                break;
            case Way::Deleted:
                ASSERT_TRUE(subject.has_value());
                ASSERT_TRUE(controller_->GetSession().DeleteItem(*subject));
                break;
            case Way::UndoneSeveralTimes:
                for (int i = 0; i < 4; ++i) {
                    PressCtrlKey(ImGuiKey_Z);
                }
                break;
            case Way::Minimized:
                ASSERT_TRUE(subject.has_value());
                // Minimize acts on the selection: a practice snippet put
                // there is not selected yet, as a screenshot just made is.
                if (!App().IsSelected(*subject)) {
                    RawClick(SubjectMiddle().x, SubjectMiddle().y);
                }
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Minimize, *subject}));
                break;
            case Way::Fullscreen:
                ASSERT_TRUE(subject.has_value());
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::ToggleFullscreen, *subject}));
                break;
            case Way::CaptureHotkey:
                TriggerHotkey(config_.hotkeyQuickCapture);
                break;
            case Way::LeftDrawingMode:
                RawClick(1200.0f, 120.0f);
                break;
            case Way::EnteredDrawingMode:
                DoubleClick(SubjectMiddle().x, SubjectMiddle().y);
                break;
            case Way::Unpinned:
                ASSERT_TRUE(subject.has_value());
                controller_->GetSession().SetPinned({*subject}, false);
                break;
            case Way::EraserPicked:
                PressBar(ChromeButton::Eraser);
                break;
            case Way::ColorChooserUp:
                PressBar(ChromeButton::Color);
                StepFrames(2);
                ASSERT_TRUE(App().IsColorChooserOpen());
                break;
            case Way::PropertiesUp:
                SelectTheSubject();
                PressBar(ChromeButton::More);
                StepFrames(2);
                break;
            case Way::OpacityByWheel:
                MoveTo(SubjectMiddle().x, SubjectMiddle().y);
                KeyEvent(ImGuiMod_Ctrl, true);
                Wheel(-1.0f);
                Wheel(-1.0f);
                Wheel(-1.0f);
                KeyEvent(ImGuiMod_Ctrl, false);
                StepFrame();
                break;
            case Way::DrewFreehand:
                DrawAcross(-50.0f);
                break;
            case Way::DrewALine:
                ASSERT_EQ(App().PenShape(), DrawShape::Line) << "the pen as the step before left it";
                DrawAcross(-50.0f);
                break;
            case Way::ErasedRound: {
                ASSERT_EQ(App().ActiveTool(), Tool::Erase) << "the eraser as the step before left it";
                const ImVec2 middle = SubjectMiddle();
                Drag(middle.x - 20.0f, middle.y - 80.0f, middle.x - 20.0f, middle.y + 80.0f);
                break;
            }
            case Way::ErasedWithTheEraser: {
                ASSERT_EQ(App().ActiveTool(), Tool::Erase) << "the eraser as the step before left it";
                const ImVec2 middle = SubjectMiddle();
                Drag(middle.x - 50.0f, middle.y - 80.0f, middle.x - 30.0f, middle.y + 80.0f);
                break;
            }
            case Way::FramedAScreenshot:
                // Away from where the steps make theirs.
                Drag(850.0f, 520.0f, 1050.0f, 650.0f);
                break;
            case Way::DrawingToolPicked:
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::NewDrawingTool}));
                break;
            case Way::CopyPasted:
                SelectTheSubject();
                PressCtrlKey(ImGuiKey_C);
                ToTheOtherCanvas();
                PressCtrlKey(ImGuiKey_V);
                ASSERT_EQ(HintUp(), std::string(strings::kTutorialMoveSnippetMissCopy));
                break;
            case Way::OverviewClosed:
                ASSERT_TRUE(App().IsOverviewOpen());
                PressKey(ImGuiKey_Escape);
                break;
            case Way::SettingsTab:
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Settings}));
                break;
            case Way::OtherFolderBrowsed: {
                // The folder the app started with, not the tutorial's.
                const Folder& mine = Canvases().Folders().front();
                ASSERT_NE(mine.id, Runner().Folder());
                ClickAnchor(Anchor{AnchorId::OverviewFolderRow, mine.id});
                break;
            }
            case Way::NewCanvasMade:
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::NewCanvas}));
                StepFrames(2);
                ASSERT_EQ(HintUp(), std::string(strings::kTutorialNewFolderMissCanvas));
                break;
            case Way::ShowDeletedOff:
                ClickAnchor(Anchor{AnchorId::OverviewShowDeleted});
                break;
            case Way::CanvasesTabPicked:
                // Its tab pressed - which opening the Overview comes back
                // to, as a press has no anchor to find it by.
                ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
                break;
            case Way::OtherSection:
                PickSection(SettingsPage::SettingsSection::Appearance);
                break;
            case Way::NothingUnderneath:
                ComeBackOver(platform::ForegroundApp{});
                break;
            case Way::NewProfileMade:
                ClickAnchor(Anchor{AnchorId::SettingsNewProfile});
                StepFrames(2);
                ASSERT_EQ(HintUp(), std::string(strings::kTutorialMakeProfileMissBlank));
                break;
            case Way::ShowingDefaults:
                PickShowing(std::nullopt);
                break;
            case Way::TickedBack:
                ClickAnchor(Anchor{AnchorId::SettingsDontStealFocus});
                StepFrames(2);
                ASSERT_EQ(HintUp(), std::string(strings::kTutorialRevertMissTickedBack));
                break;
            case Way::TypingLeftOpen:
                PressBar(ChromeButton::Text);
                RawClick(SubjectMiddle().x, SubjectMiddle().y);
                ImGui::GetIO().AddInputCharacter('x');
                StepFrame();
                ASSERT_TRUE(App().EditingNote().has_value());
                break;
        }
        StepFrames(3);
    }

    // What the lines say, done one after the other until there is none:
    // the card's button where the line has one, and otherwise what its
    // words say to do.
    void FollowTheLines() {
        for (int guard = 0; guard < 6; ++guard) {
            const std::optional<tutorial::Hint>& hint = Runner().CurrentHint();
            if (!hint.has_value() || !hint->need.has_value()) {
                return;
            }
            if (hint->button != tutorial::HintButton::None) {
                Overlay().PressTutorialHint();
                StepFrames(3);
                continue;
            }
            switch (*hint->need) {
                case tutorial::Need::CanvasUncovered:
                case tutorial::Need::NoOtherTool:
                    PressKey(ImGuiKey_Escape);
                    break;
                case tutorial::Need::NoDrawingMode:
                    // "Click outside the snippet, or press Esc."
                    PressKey(ImGuiKey_Escape);
                    break;
                case tutorial::Need::SubjectOnScreen: {
                    const std::optional<AnchorRect> chip = App().TutorialSpot();
                    ASSERT_TRUE(chip.has_value()) << "the ring on the chip";
                    Click(Center(*chip).x, Center(*chip).y);
                    break;
                }
                case tutorial::Need::SubjectCanMove:
                    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::ToggleFullscreen, *Runner().Subject()}));
                    break;
                case tutorial::Need::SubjectSelected:
                    RawClick(SubjectMiddle().x, SubjectMiddle().y);
                    break;
                case tutorial::Need::DrawingOnSubject:
                    DoubleClick(SubjectMiddle().x, SubjectMiddle().y);
                    break;
                case tutorial::Need::SubjectPinned:
                    // "Select it, and press Pin on its bar."
                    SelectTheSubject();
                    PressBarPin();
                    break;
                case tutorial::Need::PenInHand:
                    PressBar(ChromeButton::Pen);
                    break;
                case tutorial::Need::OverviewUp:
                    // "Right-click an empty spot and choose Overview."
                    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
                    break;
                case tutorial::Need::CanvasesTab:
                    // Its tab pressed - which opening the Overview again
                    // comes back to, as a press has no anchor to find it by.
                    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
                    break;
                case tutorial::Need::DeletedShown:
                    ClickAnchor(Anchor{AnchorId::OverviewShowDeleted});
                    break;
                case tutorial::Need::SettingsUp:
                case tutorial::Need::SettingsTab:
                    // "Right-click an empty spot and choose Settings", or
                    // "Press Settings, at the top of the Overview".
                    ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Settings}));
                    break;
                case tutorial::Need::ProfilesSection:
                    PickSection(SettingsPage::SettingsSection::Profiles);
                    break;
                case tutorial::Need::BehaviorSection:
                    PickSection(SettingsPage::SettingsSection::Behavior);
                    break;
                case tutorial::Need::AProgramUnderneath:
                    ComeBackOver(kGame);
                    break;
                case tutorial::Need::ShowingIt:
                    PickShowing(TutorialsProfile());
                    break;
                default:
                    FAIL() << "a line with nothing to do: " << hint->text;
            }
            StepFrames(3);
        }
        FAIL() << "the lines never ran out: " << HintUp();
    }

};

TEST_P(TutorialDerailTest, TheCardSaysALineAndFollowingItGetsTheStepDone) {
    const Derail& derail = GetParam();
    WalkTo(derail.step, derail.topic);
    // A topic that starts with nothing to practice on: first the snippet
    // its line puts there, as a user would take it.
    if (NeedUp() == tutorial::Need::ASubject) {
        Overlay().PressTutorialHint();
        StepFrames(3);
        ASSERT_TRUE(Runner().Subject().has_value());
    }
    // A step that begins with the Overview closed: first open, as its
    // line says. Or with Showing on the defaults, which the overlay came
    // up over.
    if (NeedUp() == tutorial::Need::OverviewUp) {
        ASSERT_TRUE(Overlay().Dispatch(Command{CommandId::Overview}));
        StepFrames(3);
    }
    if (NeedUp() == tutorial::Need::ShowingIt) {
        PickShowing(TutorialsProfile());
    }
    const bool gated = Runner().CurrentStep().gated;

    TakeTheWay(derail.way);
    ASSERT_EQ(StepUp(), derail.step) << "the way off the path did not do the step";
    EXPECT_EQ(NeedUp(), derail.says) << "the line: " << HintUp();
    // Never trapped: Skip is on every step, and Next where it always is.
    EXPECT_EQ(Runner().GetState(), tutorial::Tutorial::State::OnStep);
    if (!gated) {
        EXPECT_TRUE(Runner().NextEnabled());
    }

    FollowTheLines();
    EXPECT_FALSE(NeedUp().has_value()) << HintUp();
    DoStep(derail.step);
    EXPECT_NE(StepUp(), derail.step) << "not done after following the lines: " << HintUp();
}

INSTANTIATE_TEST_SUITE_P(Matrix, TutorialDerailTest, ::testing::ValuesIn(Matrix()),
                         [](const ::testing::TestParamInfo<Derail>& info) {
                             return std::string(info.param.topic) + "_" + info.param.step + "_" +
                                    WayName(info.param.way);
                         });

}  // namespace
}  // namespace sz::test
