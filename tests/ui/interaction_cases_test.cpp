// The cases of docs/INTERACTIONS.md, section 9, each as the machine sees
// it: events offered to the editor's input machine with no view, no ImGui
// and no frame - what it does to the stack and to the library, and what
// is kept, called off or ended. The randomized test at the end drives it
// the same way, with Escape anywhere, for thousands of seeds.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "core/config/app_config.h"
#include "core/session/session.h"
#include "core/session/settings.h"
#include "support/session_test_access.h"
#include "ui/editor.h"
#include "ui/interaction/bursts.h"
#include "ui/interaction/gestures.h"

namespace sz::ui {
namespace {

using platform::KeyCombo;
using platform::MouseButton;

constexpr float kDisplayW = 1280.0f;
constexpr float kDisplayH = 768.0f;

class InteractionCasesTest : public ::testing::Test {
protected:
    InteractionCasesTest() : settings_(AppConfig{}), editor_(settings_, session_) {
        session_.SyncItemsToDisplaySize(kDisplayW, kDisplayH);
        editor_.SetDisplaySize(kDisplayW, kDisplayH);
    }

    // ===== The hand =====

    void Offer(Event event) {
        now_ += 0.01;  // events come apart in time, as a hand's do
        event.seconds = now_;
        event.modifiers = held_;
        editor_.SetHeld(held_);
        editor_.SetNow(now_);
        editor_.Input().Offer(event);
    }
    void Down(float x, float y, MouseButton button = MouseButton::Left) {
        Event event;
        event.kind = EventKind::PointerDown;
        event.position = platform::Vec2{x, y};
        event.button = button;
        buttons_ |= platform::ButtonBit(button);
        Offer(event);
    }
    void Move(float x, float y) {
        Event event;
        event.kind = EventKind::PointerMove;
        event.position = platform::Vec2{x, y};
        event.buttons = buttons_;
        Offer(event);
    }
    void Up(float x, float y, MouseButton button = MouseButton::Left) {
        Event event;
        event.kind = EventKind::PointerUp;
        event.position = platform::Vec2{x, y};
        event.button = button;
        buttons_ &= static_cast<uint8_t>(~platform::ButtonBit(button));
        Offer(event);
    }
    // A drag from one point to another in a few moves, released.
    void Drag(float fromX, float fromY, float toX, float toY, MouseButton button = MouseButton::Left) {
        Down(fromX, fromY, button);
        for (int i = 1; i <= 4; ++i) {
            const float t = static_cast<float>(i) / 4.0f;
            Move(fromX + (toX - fromX) * t, fromY + (toY - fromY) * t);
        }
        Up(toX, toY, button);
        Pause();
    }
    void Click(float x, float y, MouseButton button = MouseButton::Left) {
        Down(x, y, button);
        Up(x, y, button);
    }
    void Key(int key, bool repeat = false) {
        Event event;
        event.kind = EventKind::KeyDown;
        event.key = key;
        event.repeat = repeat;
        Offer(event);
        event.kind = EventKind::KeyUp;
        Offer(event);
    }
    void Escape() { Key(KeyCombo::kEscape); }
    // Time passing with nothing else happening - a frame's tick, however
    // long it has been.
    void Tick(double seconds) {
        now_ += seconds;
        Event event;
        event.kind = EventKind::Tick;
        Offer(event);
    }
    // Long enough apart that the next press is no double-click's second.
    void Pause() { Tick(1.0); }
    void Hotkey(CommandId command) {
        Event event;
        event.kind = EventKind::Hotkey;
        event.command = command;
        Offer(event);
    }
    // A key held down - repeating, as the OS repeats it - and let go of.
    void KeyDown(int key, bool repeat = false) {
        Event event;
        event.kind = EventKind::KeyDown;
        event.key = key;
        event.repeat = repeat;
        Offer(event);
    }
    void KeyUp(int key) {
        Event event;
        event.kind = EventKind::KeyUp;
        event.key = key;
        Offer(event);
    }
    void Wheel(float notches) {
        Event event;
        event.kind = EventKind::Wheel;
        event.wheel = notches;
        Offer(event);
    }
    void Undo() { editor_.Dispatch(Command{CommandId::Undo}); }

    // ===== The library =====

    ItemId MakeSnippet(Rect rect, bool picture = false) {
        return test::Model(session_).CreateItem(picture, rect, "Snippet");
    }
    const Item& ItemOf(ItemId id) const { return *editor_.Manager().FindItemAnywhere(id); }
    const std::vector<Item>& Items() const { return editor_.Manager().CurrentOrNull()->items; }
    size_t Strokes(ItemId id) const { return ItemOf(id).strokes.size(); }
    // What the machine holds, bottom to top.
    std::string Stack() const { return editor_.Input().Describe(); }
    std::string GestureLevel() const {
        const Interaction* gesture = editor_.Input().At(Level::Gesture);
        return gesture != nullptr ? gesture->Name() : "-";
    }
    bool Filed() { return session_.CanUndo(); }

    Settings settings_;
    Session session_;
    Editor editor_;
    platform::Modifiers held_;
    uint8_t buttons_ = 0;
    double now_ = 100.0;
};

// ===== Marking the snippet in drawing mode =====

TEST_F(InteractionCasesTest, AFreehandStrokeIsOneStepAndEscapeLeavesNothingOfIt) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    ASSERT_EQ(Stack(), "Canvas / DrawingMode / - / - / - / -");

    Down(200.0f, 200.0f);
    EXPECT_EQ(GestureLevel(), "Marking") << "rule 4: at once";
    Move(260.0f, 230.0f);
    Move(320.0f, 260.0f);
    Up(320.0f, 260.0f);
    EXPECT_EQ(Strokes(drawing), 1u);
    EXPECT_EQ(GestureLevel(), "-");

    Pause();
    Down(200.0f, 300.0f);
    Move(300.0f, 320.0f);
    Escape();
    EXPECT_EQ(GestureLevel(), "Spent") << "the rest of the press";
    Move(400.0f, 340.0f);
    Up(400.0f, 340.0f);
    EXPECT_EQ(Strokes(drawing), 1u) << "nothing of it";
    EXPECT_TRUE(session_.LiveLayer().Strokes().empty());
    EXPECT_EQ(editor_.DrawingItem(), drawing) << "Escape went to the stroke, not to drawing mode";
}

TEST_F(InteractionCasesTest, AStrokeInterruptedIsKeptAndTheRestOfThePressIsSpent) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    Down(200.0f, 200.0f);
    Move(300.0f, 250.0f);
    Hotkey(CommandId::ToggleEditMode);  // no tray here: it only ends what its scope covers
    EXPECT_EQ(Strokes(drawing), 1u) << "kept as far as it got";
    EXPECT_EQ(GestureLevel(), "Spent");
    Move(400.0f, 300.0f);
    Up(400.0f, 300.0f);
    EXPECT_EQ(Strokes(drawing), 1u) << "the rest drew nothing";
    EXPECT_EQ(GestureLevel(), "-");
}

TEST_F(InteractionCasesTest, AShapeSwitchesWithItsModifierAndEscapeDropsIt) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    held_.shift = true;
    Drag(150.0f, 150.0f, 400.0f, 300.0f);
    held_.shift = false;
    ASSERT_EQ(Strokes(drawing), 1u);
    EXPECT_EQ(ItemOf(drawing).strokes[0].points.size(), 2u) << "a line";

    held_.shift = true;
    Down(150.0f, 350.0f);
    held_ = platform::Modifiers{};
    held_.ctrl = true;  // switched mid-drag
    Move(300.0f, 450.0f);
    Up(300.0f, 450.0f);
    held_ = platform::Modifiers{};
    ASSERT_EQ(Strokes(drawing), 2u);
    EXPECT_EQ(ItemOf(drawing).strokes[1].points.size(), 5u) << "a rectangle";

    Pause();
    held_.shift = true;
    Down(150.0f, 150.0f);
    Move(400.0f, 150.0f);
    Escape();
    held_ = platform::Modifiers{};
    Up(400.0f, 150.0f);
    EXPECT_EQ(Strokes(drawing), 2u);
}

TEST_F(InteractionCasesTest, AnEraseIsRolledBackByEscape) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    Drag(150.0f, 300.0f, 650.0f, 300.0f);
    const std::vector<Stroke> drawn = ItemOf(drawing).strokes;
    ASSERT_EQ(drawn.size(), 1u);
    Key('E');  // the eraser
    ASSERT_EQ(editor_.ActiveTool(), Tool::Erase);

    Down(300.0f, 300.0f);
    Move(400.0f, 300.0f);
    ASSERT_NE(ItemOf(drawing).strokes, drawn) << "erasing";
    Escape();
    EXPECT_EQ(ItemOf(drawing).strokes, drawn) << "rolled back";
    Up(400.0f, 300.0f);
    Pause();

    Drag(300.0f, 300.0f, 400.0f, 300.0f);
    EXPECT_NE(ItemOf(drawing).strokes, drawn) << "and erased, let go of";
}

TEST_F(InteractionCasesTest, ARectangleEraseErasesOnlyByItsReleaseAndOnlyIfBigEnough) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    Drag(150.0f, 150.0f, 200.0f, 200.0f);
    ASSERT_EQ(Strokes(drawing), 1u);
    Key('E');
    held_.ctrl = true;

    Drag(140.0f, 140.0f, 150.0f, 150.0f);  // too small to be meant
    EXPECT_EQ(Strokes(drawing), 1u);
    Down(120.0f, 120.0f);
    Move(300.0f, 300.0f);
    Hotkey(CommandId::ToggleEditMode);  // ended from outside
    Up(300.0f, 300.0f);
    EXPECT_EQ(Strokes(drawing), 1u) << "interrupted, it erases nothing";
    Pause();
    Drag(120.0f, 120.0f, 300.0f, 300.0f);
    EXPECT_EQ(Strokes(drawing), 0u);
    held_.ctrl = false;
}

TEST_F(InteractionCasesTest, TheRightButtonErasesOnTheDrawingAndItsClickLeaves) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    Drag(150.0f, 300.0f, 650.0f, 300.0f);
    const std::vector<Stroke> drawn = ItemOf(drawing).strokes;

    Down(300.0f, 300.0f, MouseButton::Right);
    EXPECT_EQ(GestureLevel(), "Pending") << "rule 12: a click or a drag";
    Move(400.0f, 300.0f);
    EXPECT_EQ(GestureLevel(), "Marking");
    Up(400.0f, 300.0f, MouseButton::Right);
    EXPECT_NE(ItemOf(drawing).strokes, drawn);
    Pause();

    Click(300.0f, 450.0f, MouseButton::Right);
    EXPECT_FALSE(editor_.DrawingItem().has_value()) << "a right click leaves drawing mode";
}

// ===== The selection's gestures =====

TEST_F(InteractionCasesTest, AMoveBeginsOnlyAsADragAndEscapePutsItBack) {
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    Down(150.0f, 150.0f);
    EXPECT_EQ(editor_.Selection(), std::vector<ItemId>{a}) << "selected at once";
    EXPECT_EQ(GestureLevel(), "Pending");
    EXPECT_FALSE(test::HandGestureOpen(session_)) << "nothing to move yet";
    Move(152.0f, 151.0f);
    EXPECT_EQ(GestureLevel(), "Pending") << "still a click";
    Move(200.0f, 200.0f);
    EXPECT_EQ(GestureLevel(), "Move");
    EXPECT_EQ(ItemOf(a).rect, (Rect{150, 150, 200, 150})) << "the whole way from the press";
    Escape();
    EXPECT_EQ(ItemOf(a).rect, (Rect{100, 100, 200, 150}));
    Move(300.0f, 300.0f);
    Up(300.0f, 300.0f);
    EXPECT_EQ(ItemOf(a).rect, (Rect{100, 100, 200, 150})) << "the rest moved nothing";
    EXPECT_FALSE(Filed());
    Pause();

    Drag(150.0f, 150.0f, 250.0f, 150.0f);
    EXPECT_EQ(ItemOf(a).rect, (Rect{200, 100, 200, 150}));
    EXPECT_TRUE(Filed()) << "one step";
}

TEST_F(InteractionCasesTest, AMoveInterruptedIsFiledWhereItGot) {
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    Down(150.0f, 150.0f);
    Move(250.0f, 150.0f);
    Key(KeyCombo::kRightArrow);  // a nudge, which ends the drag first
    EXPECT_EQ(ItemOf(a).rect.x, 201.0f) << "filed where it got to, then nudged";
    Up(300.0f, 150.0f);
    EXPECT_EQ(ItemOf(a).rect.x, 201.0f);
}

// A release says where the hand let go. Movement comes once a frame, and a
// button's release carries its own position: a quick drag lets go
// somewhere the last move never reached, and a handle can be let go of
// with no move at all.
TEST_F(InteractionCasesTest, AMoveAndAResizeEndWhereTheyAreLetGoOf) {
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    Down(150.0f, 150.0f);
    Move(200.0f, 200.0f);
    Up(250.0f, 150.0f);
    EXPECT_EQ(ItemOf(a).rect, (Rect{200, 100, 200, 150})) << "where it was let go of";
    Pause();

    Down(400.0f, 250.0f);  // the south-east corner, selected by the move
    ASSERT_EQ(GestureLevel(), "Resize");
    Up(460.0f, 310.0f);
    // Resized by the release alone, 60 px out - its aspect kept, as a
    // snippet's own setting has it.
    EXPECT_EQ(ItemOf(a).rect.x, 200.0f);
    EXPECT_EQ(ItemOf(a).rect.y, 100.0f);
    EXPECT_GE(ItemOf(a).rect.w, 260.0f);
    EXPECT_GT(ItemOf(a).rect.h, 150.0f);
}

TEST_F(InteractionCasesTest, AnEraseEndsWhereItIsLetGoOf) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    Drag(150.0f, 300.0f, 650.0f, 300.0f);
    ASSERT_EQ(Strokes(drawing), 1u);
    Key('E');
    Pause();

    // Pressed above the line and let go of below it, with no move between:
    // the way from one to the other crosses it.
    Down(300.0f, 200.0f);
    Up(300.0f, 400.0f);
    EXPECT_EQ(Strokes(drawing), 2u) << "cut in two";
}

TEST_F(InteractionCasesTest, AHandleResizesAtOnceAndEscapePutsItBack) {
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    Click(150.0f, 150.0f);
    Pause();
    Down(300.0f, 250.0f);  // the south-east corner
    EXPECT_EQ(GestureLevel(), "Resize") << "rule 3: at once";
    Move(360.0f, 310.0f);
    EXPECT_GT(ItemOf(a).rect.w, 200.0f);
    Escape();
    EXPECT_EQ(ItemOf(a).rect, (Rect{100, 100, 200, 150}));
    Up(360.0f, 310.0f);
}

TEST_F(InteractionCasesTest, ARightDragResizesFromTheNearestEdgeAndARightClickAsksForTheMenu) {
    const ItemId a = MakeSnippet(Rect{100, 100, 300, 300});
    Down(390.0f, 250.0f, MouseButton::Right);  // near the east edge
    Move(450.0f, 250.0f);
    EXPECT_EQ(GestureLevel(), "Resize");
    Up(450.0f, 250.0f, MouseButton::Right);
    EXPECT_NEAR(ItemOf(a).rect.w, 360.0f, 1.0f);
    Pause();

    Click(250.0f, 250.0f, MouseButton::Right);
    EXPECT_EQ(editor_.LastCommand(), CommandId::ItemMenu);
}

TEST_F(InteractionCasesTest, AShiftBoxAddsWhatItTouches) {
    const ItemId a = MakeSnippet(Rect{100, 100, 100, 100});
    const ItemId b = MakeSnippet(Rect{400, 100, 100, 100});
    held_.shift = true;
    Drag(50.0f, 50.0f, 450.0f, 150.0f);
    held_.shift = false;
    EXPECT_EQ(editor_.Selection(), (std::vector<ItemId>{a, b}));
}

TEST_F(InteractionCasesTest, ABarButtonFiresOnlyOnAReleaseOverItself) {
    const ItemId a = MakeSnippet(Rect{300, 300, 200, 150});
    Click(350.0f, 350.0f);
    Pause();
    const std::optional<platform::Vec2> close = editor_.SelectionBarButtonCenter(ChromeButton::Close);
    ASSERT_TRUE(close.has_value());
    Down(close->x, close->y);
    EXPECT_EQ(GestureLevel(), "BarPress");
    Move(close->x + 200.0f, close->y);
    Up(close->x + 200.0f, close->y);
    EXPECT_NE(editor_.Manager().FindItemAnywhere(a), nullptr);
    EXPECT_EQ(editor_.ResolvePointerTarget(400.0f, 350.0f).item, a) << "let go elsewhere: nothing";
    Pause();

    Click(close->x, close->y);
    EXPECT_EQ(editor_.ResolvePointerTarget(400.0f, 350.0f).kind, PointerTarget::Kind::None) << "deleted";
}

// The pen's and the eraser's buttons have a menu of their shapes (rule 2):
// a right click opens it, and so does a press held still - whose release
// then fires nothing. A press that strays is no hold, and let go of over
// the button is its click after all. The other buttons have no menu.
TEST_F(InteractionCasesTest, ThePenAndEraserButtonsOpenTheirShapesOnARightClickOrAHold) {
    const ItemId drawing = MakeSnippet(Rect{300, 300, 300, 200});
    editor_.EnterDrawingMode(drawing);
    const std::optional<platform::Vec2> pen = editor_.SelectionBarButtonCenter(ChromeButton::Pen);
    const std::optional<platform::Vec2> eraser = editor_.SelectionBarButtonCenter(ChromeButton::Eraser);
    const std::optional<platform::Vec2> text = editor_.SelectionBarButtonCenter(ChromeButton::Text);
    ASSERT_TRUE(pen.has_value() && eraser.has_value() && text.has_value());

    Down(pen->x, pen->y, MouseButton::Right);
    EXPECT_EQ(GestureLevel(), "BarPress");
    Up(pen->x, pen->y, MouseButton::Right);
    EXPECT_EQ(editor_.LastCommand(), CommandId::PenMenu);
    EXPECT_EQ(editor_.PenShape(), DrawShape::Freehand) << "the menu, and nothing picked";
    Pause();

    Down(eraser->x, eraser->y);
    Tick(kHoldSeconds);
    EXPECT_EQ(editor_.LastCommand(), CommandId::EraserMenu);
    EXPECT_EQ(GestureLevel(), "Spent") << "the rest of the press";
    Up(eraser->x, eraser->y);
    EXPECT_EQ(editor_.ActiveTool(), Tool::Draw) << "the release fired nothing";
    Pause();

    Down(pen->x, pen->y);
    Move(pen->x, pen->y + kDoubleClickPx + 2.0f);
    Move(pen->x, pen->y);
    Tick(kHoldSeconds);
    EXPECT_EQ(GestureLevel(), "BarPress") << "a hold is a finger on one spot";
    Up(pen->x, pen->y);
    EXPECT_EQ(editor_.LastCommand(), CommandId::PenButton);
    EXPECT_FALSE(editor_.DrawingItem().has_value()) << "a click after all: the pen in hand, put down";
    Pause();

    Click(text->x, text->y, MouseButton::Right);
    EXPECT_FALSE(editor_.DrawingItem().has_value()) << "the right click did nothing";
    Down(text->x, text->y);
    Tick(kHoldSeconds);
    Up(text->x, text->y);
    Pause();
    EXPECT_EQ(editor_.LastCommand(), CommandId::TextButton) << "no menu: its click, on the release";
    EXPECT_EQ(editor_.DrawingItem(), drawing);
    EXPECT_EQ(editor_.ActiveTool(), Tool::Text);
}

// A row of the menu puts its tool in hand drawing its shape, whichever
// tool was in hand - and, from the snippet bar, enters drawing mode as the
// pen's button does.
TEST_F(InteractionCasesTest, AShapePickedFromTheMenuIsTheToolsAtOnce) {
    const ItemId drawing = MakeSnippet(Rect{300, 300, 300, 200});
    Click(350.0f, 350.0f);
    Pause();
    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::PickRectangle}));
    EXPECT_EQ(editor_.DrawingItem(), drawing);
    EXPECT_EQ(editor_.ActiveTool(), Tool::Draw);
    EXPECT_EQ(editor_.PenShape(), DrawShape::Rectangle);

    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::PickRectangleEraser}));
    EXPECT_EQ(editor_.ActiveTool(), Tool::Erase);
    EXPECT_EQ(editor_.EraserShape(), DrawShape::Rectangle);

    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::PickLine}));
    EXPECT_EQ(editor_.ActiveTool(), Tool::Draw);
    EXPECT_EQ(editor_.PenShape(), DrawShape::Line) << "not the plain pen a tool change gives";
    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::PickPen}));
    EXPECT_EQ(editor_.PenShape(), DrawShape::Freehand);
}

// ===== Making a snippet =====

TEST_F(InteractionCasesTest, ADragOnEmptyCanvasFramesAndAClickMakesNothing) {
    const size_t before = Items().size();
    Click(600.0f, 400.0f);
    Pause();
    EXPECT_EQ(Items().size(), before) << "a click makes nothing";
    Drag(100.0f, 100.0f, 110.0f, 110.0f);
    EXPECT_EQ(Items().size(), before) << "too small to be meant";
    held_.ctrl = true;  // a drawing
    Drag(100.0f, 100.0f, 400.0f, 300.0f);
    held_.ctrl = false;
    ASSERT_EQ(Items().size(), before + 1);
    EXPECT_EQ(editor_.DrawingItem(), Items().back().id) << "a drawing, made to be drawn in";
}

TEST_F(InteractionCasesTest, AHoldMakesAFullscreenSnippetAndTheRestOfThePressIsSpent) {
    Down(600.0f, 400.0f);
    Tick(0.3);
    EXPECT_TRUE(Items().empty());
    Tick(0.3);
    ASSERT_EQ(Items().size(), 1u) << "held still for half a second";
    EXPECT_TRUE(Items()[0].isFullscreen);
    EXPECT_EQ(GestureLevel(), "Spent");
    Move(900.0f, 600.0f);
    Up(900.0f, 600.0f);
    EXPECT_EQ(Items().size(), 1u) << "the rest framed nothing";
}

TEST_F(InteractionCasesTest, ADoubleClickActsOnItsSecondPress) {
    Click(600.0f, 400.0f);
    Down(600.0f, 400.0f);
    ASSERT_EQ(Items().size(), 1u) << "made on the second press";
    EXPECT_EQ(GestureLevel(), "Spent");
    Up(600.0f, 400.0f);
    Pause();

    // On a snippet, the first click selects and the second enters drawing
    // mode on it.
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    Click(150.0f, 150.0f);
    EXPECT_EQ(editor_.Selection(), std::vector<ItemId>{a});
    Down(150.0f, 150.0f);
    EXPECT_EQ(editor_.DrawingItem(), a);
    Up(150.0f, 150.0f);
}

TEST_F(InteractionCasesTest, ACreationToolPlacesOnceWhereverItLands) {
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    Key('S');  // the screenshot tool
    ASSERT_EQ(Stack(), "Canvas / CreationTool / - / - / - / -");
    Drag(120.0f, 120.0f, 400.0f, 350.0f);  // over the snippet, too
    EXPECT_EQ(Items().size(), 2u);
    EXPECT_EQ(editor_.ActiveTool(), Tool::Select) << "a screenshot tool places once";
    EXPECT_NE(Items().back().id, a);
}

// ===== Escape's stages =====

TEST_F(InteractionCasesTest, EscapePutsTheHandDownOneStageAtATime) {
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    Click(150.0f, 150.0f);
    Key('X', false);  // nothing: Cut wants Ctrl
    held_.ctrl = true;
    Key('X');  // cut
    held_.ctrl = false;
    Key('P');  // the pen: drawing mode on the selection
    ASSERT_EQ(editor_.DrawingItem(), a);

    Escape();
    EXPECT_FALSE(editor_.DrawingItem().has_value()) << "drawing mode first";
    EXPECT_TRUE(editor_.IsWaitingToBeCut(a));
    Escape();
    EXPECT_FALSE(editor_.IsWaitingToBeCut(a)) << "then the cut";
    EXPECT_FALSE(editor_.Selection().empty());
    Escape();
    EXPECT_TRUE(editor_.Selection().empty()) << "then the selection";
}

// ===== What arrives mid-gesture =====

TEST_F(InteractionCasesTest, ALostReleaseEndsTheGestureAndThePressIsTakenAfresh) {
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    Down(150.0f, 150.0f);
    Move(250.0f, 150.0f);
    // The release went missing: the next press ends the drag where it got
    // to, and is a press of its own.
    buttons_ = 0;
    Pause();
    Down(600.0f, 500.0f);
    EXPECT_EQ(ItemOf(a).rect.x, 200.0f);
    EXPECT_TRUE(editor_.Selection().empty()) << "a press on empty canvas";
    Up(600.0f, 500.0f);
}

TEST_F(InteractionCasesTest, AnotherButtonMidGestureIsIgnored) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    Down(200.0f, 200.0f);
    Move(260.0f, 230.0f);
    Down(260.0f, 230.0f, MouseButton::Right);  // a touch hold's injected right press
    Up(260.0f, 230.0f, MouseButton::Right);
    Move(320.0f, 260.0f);
    Up(320.0f, 260.0f);
    EXPECT_EQ(Strokes(drawing), 1u) << "one stroke, undisturbed";
    EXPECT_EQ(editor_.DrawingItem(), drawing) << "and the right click did not leave";
}

TEST_F(InteractionCasesTest, ATouchHoldsInjectedRightPressLandsOnSpent) {
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    Down(150.0f, 150.0f);
    Tick(0.6);
    ASSERT_EQ(editor_.DrawingItem(), a) << "the hold";
    Click(150.0f, 150.0f, MouseButton::Right);
    EXPECT_EQ(editor_.DrawingItem(), a) << "swallowed, not a right click that leaves";
    Up(150.0f, 150.0f);
}

TEST_F(InteractionCasesTest, AMouseButtonWaitsForTheGestureInFlight) {
    const ItemId a = MakeSnippet(Rect{100, 100, 200, 150});
    settings_.SetShortcut(ShortcutAction::Duplicate, KeyCombo{false, false, false, KeyCombo::kMiddleButton},
                          std::nullopt);
    Click(150.0f, 150.0f);
    Pause();
    Down(150.0f, 150.0f);
    Click(150.0f, 150.0f, MouseButton::Middle);
    EXPECT_EQ(Items().size(), 1u) << "no duplicate mid-press";
    Up(150.0f, 150.0f);
    Click(150.0f, 150.0f, MouseButton::Middle);
    EXPECT_EQ(Items().size(), 2u);
    (void)a;
}

// ===== The overlay coming and going =====

TEST_F(InteractionCasesTest, ShownForgetsWhatWasHeldAndViewOnlyEndsEverything) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    Down(200.0f, 200.0f);
    Move(300.0f, 250.0f);
    editor_.Settle(Scope::Hand);  // put away
    EXPECT_EQ(Strokes(drawing), 1u) << "kept";
    EXPECT_EQ(GestureLevel(), "Spent");
    editor_.ForgetTheHand();  // shown again: the release went elsewhere
    buttons_ = 0;
    EXPECT_EQ(Stack(), "Canvas / DrawingMode / - / - / - / -") << "drawing mode survives";

    editor_.Settle(Scope::All);  // view-only
    EXPECT_EQ(Stack(), "Canvas / - / - / - / - / -");
}

// ===== Bursts =====

TEST_F(InteractionCasesTest, AHeldArrowIsOneStepAndEscapeWhileItIsHeldTakesItBack) {
    const ItemId a = MakeSnippet(Rect{200, 200, 300, 200});
    editor_.SelectOnly(a);
    KeyDown(KeyCombo::kRightArrow);
    EXPECT_EQ(GestureLevel(), "NudgeBurst");
    for (int i = 0; i < 4; ++i) {
        KeyDown(KeyCombo::kRightArrow, /*repeat=*/true);
    }
    KeyUp(KeyCombo::kRightArrow);
    EXPECT_EQ(ItemOf(a).rect.x, 205.0f);
    EXPECT_FALSE(Filed()) << "held open until it is over";
    Tick(0.5);
    EXPECT_EQ(GestureLevel(), "NudgeBurst") << "a second without a nudge ends it, not the key's release";
    Tick(0.6);
    EXPECT_EQ(GestureLevel(), "-");
    ASSERT_TRUE(Filed());
    Undo();
    EXPECT_EQ(ItemOf(a).rect.x, 200.0f) << "the whole burst, in one step";
    EXPECT_FALSE(Filed());

    KeyDown(KeyCombo::kDownArrow);
    KeyDown(KeyCombo::kDownArrow, /*repeat=*/true);
    held_.shift = true;
    KeyDown(KeyCombo::kDownArrow, /*repeat=*/true);  // Shift, pressed mid-burst: ten pixels
    held_ = platform::Modifiers{};
    EXPECT_EQ(ItemOf(a).rect.y, 212.0f);
    Escape();
    EXPECT_EQ(ItemOf(a).rect.y, 200.0f) << "back to where it began";
    EXPECT_EQ(GestureLevel(), "-");
    EXPECT_FALSE(Filed()) << "and nothing filed";
    EXPECT_EQ(editor_.Selection(), std::vector<ItemId>{a}) << "Escape went to the burst, not the selection";
    KeyUp(KeyCombo::kDownArrow);
}

TEST_F(InteractionCasesTest, ARunOfArrowPressesIsOneStepAndEscapeAfterThemGoesOn) {
    const ItemId a = MakeSnippet(Rect{200, 200, 300, 200});
    editor_.SelectOnly(a);
    Key(KeyCombo::kRightArrow);
    Tick(0.5);
    Key(KeyCombo::kRightArrow);
    Key(KeyCombo::kUpArrow);
    EXPECT_EQ(ItemOf(a).rect.x, 202.0f);
    EXPECT_EQ(ItemOf(a).rect.y, 199.0f);
    Escape();
    EXPECT_EQ(ItemOf(a).rect.x, 202.0f) << "presses let go of are kept";
    EXPECT_TRUE(editor_.Selection().empty()) << "Escape went on, and put the hand down";
    EXPECT_EQ(GestureLevel(), "-");
    Undo();
    EXPECT_EQ(ItemOf(a).rect.x, 200.0f) << "the run, in one step";
    EXPECT_EQ(ItemOf(a).rect.y, 200.0f);
    EXPECT_FALSE(Filed());

    // A press ends a burst, filed, and is its own.
    editor_.SelectOnly(a);
    Key(KeyCombo::kLeftArrow);
    Click(900.0f, 600.0f);
    EXPECT_NE(GestureLevel(), "NudgeBurst");
    EXPECT_EQ(ItemOf(a).rect.x, 199.0f);
    EXPECT_TRUE(Filed());
    EXPECT_TRUE(editor_.Selection().empty()) << "the press on empty canvas";

    // And a command ends it before it runs: an undo takes back the burst.
    editor_.SelectOnly(a);
    Key(KeyCombo::kLeftArrow);
    Key(KeyCombo::kLeftArrow);
    Undo();
    EXPECT_EQ(ItemOf(a).rect.x, 199.0f);
    EXPECT_EQ(GestureLevel(), "-");
}

TEST_F(InteractionCasesTest, ASpinOfTheWheelIsOneStepAndEscapeTakesItBack) {
    const ItemId a = MakeSnippet(Rect{200, 200, 300, 200});
    editor_.SelectOnly(a);
    const Rect start = ItemOf(a).rect;
    Wheel(1.0f);
    EXPECT_EQ(GestureLevel(), "WheelBurst");
    Wheel(1.0f);
    Wheel(1.0f);
    EXPECT_GT(ItemOf(a).rect.w, start.w * 1.3f);
    Tick(0.9);
    Escape();
    EXPECT_EQ(ItemOf(a).rect, start) << "within the second: back to where it began";
    EXPECT_FALSE(Filed());
    EXPECT_EQ(editor_.Selection(), std::vector<ItemId>{a});

    Wheel(1.0f);
    Wheel(1.0f);
    Tick(1.1);
    EXPECT_EQ(GestureLevel(), "-") << "over";
    ASSERT_TRUE(Filed());
    Undo();
    EXPECT_EQ(ItemOf(a).rect, start) << "the spin, in one step";
    EXPECT_FALSE(Filed());

    // A notch of another kind ends the burst, filed, and begins its own.
    Wheel(1.0f);
    const Rect scaled = ItemOf(a).rect;
    held_.shift = true;
    Wheel(-1.0f);
    Wheel(-1.0f);
    held_ = platform::Modifiers{};
    EXPECT_FLOAT_EQ(ItemOf(a).foregroundOpacity, 0.9f);
    Escape();
    EXPECT_FLOAT_EQ(ItemOf(a).foregroundOpacity, 1.0f) << "the opacity burst taken back";
    EXPECT_EQ(ItemOf(a).rect, scaled) << "the size burst before it filed";
    Undo();
    EXPECT_EQ(ItemOf(a).rect, start);
    EXPECT_FALSE(Filed());
}

TEST_F(InteractionCasesTest, AToolsSizeAndACanvasStepAreNoBursts) {
    const ItemId drawing = MakeSnippet(Rect{100, 100, 600, 400});
    editor_.EnterDrawingMode(drawing);
    Wheel(1.0f);
    EXPECT_EQ(GestureLevel(), "-") << "the pen's size files nothing, and needs no burst";
    editor_.ExitDrawingMode();
    held_.alt = true;
    Wheel(1.0f);
    held_ = platform::Modifiers{};
    EXPECT_EQ(GestureLevel(), "-");
    editor_.ClearSelection();
    Wheel(1.0f);
    EXPECT_EQ(GestureLevel(), "-") << "nothing selected to change";

    // A burst that changed nothing - a fullscreen snippet has no size of
    // its own - has nothing for Escape to take back, and lets it go on.
    editor_.SelectOnly(drawing);
    editor_.ToggleFullscreenUndoably(drawing, /*stretch=*/false);
    Wheel(1.0f);
    ASSERT_EQ(GestureLevel(), "WheelBurst");
    Escape();
    EXPECT_EQ(GestureLevel(), "-");
    EXPECT_TRUE(editor_.Selection().empty()) << "Escape went on to the selection";
}

// ===== Anything, anywhere =====

// Presses of either button on snippets and off them, moves, releases - a
// quarter of them lost - modifiers, time passing long enough for a hold,
// the wheel, every command by its key - let go of or held - its hotkey or
// dispatched as a menu row or a bar button would, and Escape anywhere;
// against the machine and the editor alone, for thousands of seeds. After every event the stack is well formed
// (Machine::CheckLevels, in a debug build). After a command that ran,
// nothing is in flight but the rest of a press, and nothing is open on the
// session. Whenever the hand is at rest, nothing is open on the session
// but a note being typed. And whenever Escape cancels a move, a resize, a
// mark or a burst, every snippet is exactly as it found them.
TEST(InteractionRandomTest, AnythingAnywhereEscapeIncluded) {
    constexpr uint32_t kSeeds = 2000;
    constexpr int kSteps = 150;
    std::vector<bool> ran(kCommandCount, false);
    size_t cancelsChecked = 0;
    size_t burstCancelsChecked = 0;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        Settings settings{AppConfig{}};
        Session session;
        Editor editor(settings, session);
        session.SyncItemsToDisplaySize(kDisplayW, kDisplayH);
        editor.SetDisplaySize(kDisplayW, kDisplayH);
        CanvasManager& model = test::Model(session);
        model.CreateItem(false, Rect{100, 100, 400, 300}, "A");
        model.CreateItem(true, Rect{350, 250, 400, 300}, "B");
        model.CreateItem(false, Rect{800, 400, 300, 250}, "C");

        std::mt19937 rng(seed);
        const auto pick = [&rng](size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(rng); };
        double now = 100.0;
        platform::Modifiers held;
        uint8_t buttons = 0;
        platform::Vec2 pointer{640.0f, 400.0f};
        const auto offer = [&](Event event) {
            now += 0.02;
            event.seconds = now;
            event.modifiers = held;
            editor.SetHeld(held);
            editor.SetNow(now);
            editor.Input().Offer(event);
        };
        const auto somewhere = [&]() -> platform::Vec2 {
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            const Canvas* canvas = editor.Manager().CurrentOrNull();
            if (canvas != nullptr && !canvas->items.empty() && pick(4) != 0) {
                const Rect& rect = canvas->items[pick(canvas->items.size())].rect;
                return platform::Vec2{rect.x + rect.w * unit(rng), rect.y + rect.h * unit(rng)};
            }
            return platform::Vec2{kDisplayW * unit(rng), kDisplayH * unit(rng)};
        };
        const auto snippets = [&]() {
            const Canvas* canvas = editor.Manager().CurrentOrNull();
            return canvas != nullptr ? canvas->items : std::vector<Item>{};
        };
        const auto changesTheLibrary = [](const Interaction* gesture) {
            return dynamic_cast<const Placement*>(gesture) != nullptr ||
                   dynamic_cast<const Marking*>(gesture) != nullptr ||
                   dynamic_cast<const NudgeBurst*>(gesture) != nullptr ||
                   dynamic_cast<const WheelBurst*>(gesture) != nullptr;
        };
        // Whether Escape would cancel it now, rather than let it end and go
        // on: a burst only while it holds something open, and the arrows'
        // only while one is held.
        const auto escapeCancels = [&session](const Interaction* gesture) {
            if (const auto* nudge = dynamic_cast<const NudgeBurst*>(gesture)) {
                return nudge->ArrowHeld() && session.PlacementOpen();
            }
            if (const auto* wheel = dynamic_cast<const WheelBurst*>(gesture)) {
                return wheel->Kind() == Editor::WheelKind::SelectionOpacity ? session.StyleEditOpen()
                                                                           : session.PlacementOpen();
            }
            return gesture != nullptr;
        };
        // The move, the resize, the mark or the burst on top, and what the
        // library held before the event that began it - which Escape must
        // give back.
        uint64_t tracked = 0;
        std::vector<Item> foundIt;
        const auto serialOf = [](const Interaction* gesture) -> uint64_t {
            return gesture != nullptr ? gesture->Serial() : 0;
        };

        for (int step = 0; step < kSteps && !::testing::Test::HasFailure(); ++step) {
            const std::vector<Item> before = snippets();
            const uint64_t topBefore = serialOf(editor.Input().At(Level::Gesture));
            const bool changerBefore = changesTheLibrary(editor.Input().At(Level::Gesture));
            const bool cancelsBefore = escapeCancels(editor.Input().At(Level::Gesture));
            const bool burstBefore = editor.Input().As<NudgeBurst>(Level::Gesture) != nullptr ||
                                     editor.Input().As<WheelBurst>(Level::Gesture) != nullptr;
            bool escaped = false;
            const size_t what = pick(22);
            if (what < 4) {
                const MouseButton button = pick(3) == 0 ? MouseButton::Right : MouseButton::Left;
                pointer = somewhere();
                Event event;
                event.kind = EventKind::PointerDown;
                event.position = pointer;
                event.button = button;
                buttons |= platform::ButtonBit(button);
                offer(event);
            } else if (what < 8) {
                std::uniform_real_distribution<float> by(-150.0f, 150.0f);
                pointer = platform::Vec2{std::clamp(pointer.x + by(rng), 0.0f, kDisplayW - 1.0f),
                                         std::clamp(pointer.y + by(rng), 0.0f, kDisplayH - 1.0f)};
                Event event;
                event.kind = EventKind::PointerMove;
                event.position = pointer;
                event.buttons = buttons;
                offer(event);
            } else if (what < 10) {
                const MouseButton button = pick(2) == 0 ? MouseButton::Left : MouseButton::Right;
                if ((buttons & platform::ButtonBit(button)) != 0) {
                    buttons &= static_cast<uint8_t>(~platform::ButtonBit(button));
                    if (pick(4) != 0) {  // and otherwise the release is lost on the way
                        Event event;
                        event.kind = EventKind::PointerUp;
                        event.position = pointer;
                        event.button = button;
                        offer(event);
                    }
                }
            } else if (what == 10) {
                bool* modifier = pick(3) == 0 ? &held.ctrl : pick(2) == 0 ? &held.shift : &held.alt;
                *modifier = !*modifier;
                Event event;
                event.kind = EventKind::Modifiers;
                offer(event);
            } else if (what < 13) {
                escaped = true;
                Event event;
                event.kind = EventKind::KeyDown;
                event.key = KeyCombo::kEscape;
                offer(event);
            } else if (what == 13) {
                now += pick(2) == 0 ? 0.1 : 0.6;  // long enough for a hold, now and then
                Event event;
                event.kind = EventKind::Tick;
                offer(event);
            } else if (what >= 20) {
                // A notch or a few, or part of one on a fine wheel.
                Event event;
                event.kind = EventKind::Wheel;
                event.position = pointer;
                event.wheel = (pick(2) == 0 ? 1.0f : -1.0f) * (pick(3) == 0 ? 0.5f : static_cast<float>(1 + pick(2)));
                offer(event);
            } else {
                // A command, by whatever reaches it.
                const auto id = static_cast<CommandId>(pick(kCommandCount));
                const CommandInfo& info = InfoFor(id);
                const std::vector<KeyCombo> keys = KeysFor(id, settings.Stored(), settings.Live().shortcuts);
                const uint64_t runBefore = editor.CommandsRun();
                if (info.hotkey.has_value()) {
                    Event event;
                    event.kind = EventKind::Hotkey;
                    event.command = id;
                    offer(event);
                } else if (!keys.empty()) {
                    const KeyCombo key = keys[pick(keys.size())];
                    const platform::Modifiers kept = held;
                    held = platform::Modifiers{};
                    held.ctrl = key.ctrl;
                    held.alt = key.alt;
                    held.shift = key.shift;
                    Event event;
                    if (key.IsMouseButton()) {
                        event.kind = EventKind::PointerDown;
                        event.button = key.key == KeyCombo::kMiddleButton ? MouseButton::Middle
                                       : key.key == KeyCombo::kX1Button   ? MouseButton::X1
                                                                          : MouseButton::X2;
                        event.position = pointer;
                    } else {
                        event.kind = EventKind::KeyDown;
                        event.key = key.key;
                        event.repeat = pick(3) == 0;
                    }
                    offer(event);
                    if (event.kind == EventKind::KeyDown && pick(2) == 0) {  // and otherwise it is held
                        event.kind = EventKind::KeyUp;
                        offer(event);
                    }
                    held = kept;
                } else {
                    Command command{id};
                    const std::vector<Item> items = snippets();
                    if (!items.empty()) {
                        command.item = items[pick(items.size())].id;
                    }
                    command.canvas = editor.Manager().Canvases()[pick(editor.Manager().Canvases().size())].id;
                    command.at = pointer;
                    command.rect = Rect{pointer.x, pointer.y, 200.0f, 150.0f};
                    editor.Dispatch(command);
                }
                if (editor.CommandsRun() > runBefore) {
                    const CommandId last = *editor.LastCommand();
                    ran[static_cast<size_t>(last)] = true;
                    if (IsNudge(last)) {
                        // A step of the burst it began or went on with,
                        // which is in the hand now, holding it open.
                        ASSERT_NE(editor.Input().As<NudgeBurst>(Level::Gesture), nullptr)
                            << "step " << step << ", " << InfoFor(last).name;
                    } else {
                        ASSERT_TRUE(editor.HandAtRest()) << "step " << step << ", " << InfoFor(last).name;
                        ASSERT_FALSE(test::HandGestureOpen(session)) << "step " << step << ", " << InfoFor(last).name;
                    }
                }
            }

            // Escape canceled the move, the resize or the mark on top: every
            // snippet as that gesture found it - but for a drawing nobody put
            // anything into, which a press elsewhere is allowed to discard.
            const Interaction* after = editor.Input().At(Level::Gesture);
            const uint64_t topAfter = serialOf(after);
            if (escaped && changerBefore && cancelsBefore && topBefore == tracked && topAfter != topBefore) {
                ++cancelsChecked;
                burstCancelsChecked += burstBefore ? 1 : 0;
                for (const Item& item : snippets()) {
                    const auto was = std::find_if(foundIt.begin(), foundIt.end(),
                                                  [&](const Item& earlier) { return earlier.id == item.id; });
                    ASSERT_NE(was, foundIt.end()) << "step " << step << ": a snippet the gesture made";
                    EXPECT_EQ(item, *was) << "step " << step << ": snippet " << item.name;
                }
                EXPECT_TRUE(session.LiveLayer().Strokes().empty() && !session.LiveLayer().ActiveStroke().has_value())
                    << "step " << step;
            }
            // One that took over from another in the same event - a press
            // that ended a stroke and began the next - found the library
            // with the other's work in it, which no snapshot here has.
            if (changesTheLibrary(after) && topAfter != topBefore) {
                tracked = changerBefore ? 0 : topAfter;
                foundIt = before;
            } else if (!changesTheLibrary(after)) {
                tracked = 0;
            }
            if (editor.HandAtRest() && !editor.EditingNote().has_value()) {
                ASSERT_FALSE(test::HandGestureOpen(session)) << "step " << step;
            }
        }

        // Both buttons let go of - a release lost on the way included - and
        // Escape pressed until nothing is left: whatever came before, the
        // hand is at rest.
        for (const MouseButton button : {MouseButton::Left, MouseButton::Right}) {
            Event event;
            event.kind = EventKind::PointerUp;
            event.position = pointer;
            event.button = button;
            offer(event);
        }
        buttons = 0;
        held = platform::Modifiers{};
        for (int i = 0; i < 4; ++i) {
            Event event;
            event.kind = EventKind::KeyDown;
            event.key = KeyCombo::kEscape;
            offer(event);
        }
        EXPECT_EQ(editor.Input().Describe(), "Canvas / - / - / - / - / -");
    }
    EXPECT_GE(cancelsChecked, 100u) << "the cancel check has to run to mean anything";
    EXPECT_GE(burstCancelsChecked, 50u) << "for bursts too";
    for (const CommandInfo& info : kCommands) {
        EXPECT_TRUE(ran[static_cast<size_t>(info.id)]) << info.name << " never ran";
    }
}

}  // namespace
}  // namespace sz::ui
