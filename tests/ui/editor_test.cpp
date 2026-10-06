#include "ui/editor.h"

#include <gtest/gtest.h>

#include "core/config/app_config.h"
#include "core/session/session.h"
#include "core/session/settings.h"
#include "support/session_test_access.h"

namespace sz::ui {
namespace {

// The editor needs no frame and no ImGui: a session, the settings and the
// display size are all there is to it - which is what lets the input
// machine of docs/INTERACTIONS.md be driven and tested on its own.
class EditorTest : public ::testing::Test {
protected:
    EditorTest() : settings_(AppConfig{}), editor_(settings_, session_) {
        session_.SyncItemsToDisplaySize(1280.0f, 768.0f);
        editor_.SetDisplaySize(1280.0f, 768.0f);
    }

    Settings settings_;
    Session session_;
    Editor editor_;
};

TEST_F(EditorTest, SelectsWhatIsHitAndDeletesItUndoably) {
    const ItemId a = test::Model(session_).CreateItem(false, Rect{100, 100, 200, 150}, "A");
    const PointerTarget body = editor_.ResolvePointerTarget(150.0f, 150.0f);
    ASSERT_EQ(body.kind, PointerTarget::Kind::Body);
    EXPECT_EQ(body.item, a);

    editor_.SelectOnly(a);
    // Selected, it has a resize band, just outside its corner too.
    const PointerTarget corner = editor_.ResolvePointerTarget(96.0f, 96.0f);
    EXPECT_EQ(corner.kind, PointerTarget::Kind::Band);
    EXPECT_EQ(corner.handle, ResizeHandle::NW);

    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::DeleteSelection}));
    EXPECT_TRUE(editor_.Selection().empty());
    EXPECT_EQ(editor_.ResolvePointerTarget(150.0f, 150.0f).kind, PointerTarget::Kind::None);
    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::Undo}));
    EXPECT_EQ(editor_.ResolvePointerTarget(150.0f, 150.0f).item, a);
}

// Delete and Backspace delete bare or with Shift - held on from growing
// the selection with Shift+click - and not with Ctrl or Alt, whose chords
// are other programs'. With any modifiers, Ctrl+Alt+Del deleted the
// selection on its way to the Windows screen. Escape and the arrows still
// take any.
TEST_F(EditorTest, DeleteTakesShiftButNotCtrlOrAlt) {
    const auto command = [this](int key, bool ctrl, bool shift, bool alt) {
        platform::Modifiers held;
        held.ctrl = ctrl;
        held.shift = shift;
        held.alt = alt;
        return editor_.CommandForKey(key, held, /*repeat=*/false);
    };
    for (const int key : {platform::KeyCombo::kDelete, platform::KeyCombo::kBackspace}) {
        EXPECT_EQ(command(key, false, false, false), CommandId::DeleteSelection);
        EXPECT_EQ(command(key, false, true, false), CommandId::DeleteSelection) << "Shift";
        EXPECT_EQ(command(key, true, false, true), std::nullopt) << "Ctrl+Alt+Del is Windows'";
        EXPECT_EQ(command(key, true, false, false), std::nullopt) << "Ctrl";
        EXPECT_EQ(command(key, false, false, true), std::nullopt) << "Alt";
        EXPECT_EQ(command(key, true, true, false), std::nullopt) << "Ctrl+Shift+Del is a browser's";
    }
    EXPECT_EQ(command(platform::KeyCombo::kEscape, true, true, true), CommandId::PutDown);
    EXPECT_EQ(command(platform::KeyCombo::kLeftArrow, true, false, true), CommandId::NudgeLeft);
    EXPECT_EQ(command('Z', true, false, false), CommandId::Undo);
    EXPECT_EQ(command('Z', true, true, false), std::nullopt) << "and a chosen key exactly";
}

TEST_F(EditorTest, AMarkingToolKeyEntersDrawingModeOnTheSelection) {
    const ItemId a = test::Model(session_).CreateItem(false, Rect{100, 100, 200, 150}, "A");
    EXPECT_TRUE(editor_.Dispatch(Command{CommandId::DrawTool}));
    EXPECT_FALSE(editor_.InDrawingMode()) << "nothing selected to draw on";
    editor_.SelectOnly(a);
    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::EraseTool}));
    EXPECT_EQ(editor_.DrawingItems(), std::vector<ItemId>{a});
    EXPECT_EQ(editor_.ActiveTool(), Tool::Erase);
    // Escape is drawing mode's to answer: it leaves it (see DrawingMode).
    Event escape;
    escape.kind = EventKind::KeyDown;
    escape.key = platform::KeyCombo::kEscape;
    editor_.Input().Offer(escape);
    EXPECT_FALSE(editor_.InDrawingMode());
    EXPECT_EQ(editor_.ActiveTool(), Tool::Select);
    EXPECT_EQ(editor_.Selection(), std::vector<ItemId>{a}) << "the selection is the next Escape's";
}

}  // namespace
}  // namespace sz::ui
