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
    // Selected, it wears handles, and its corner is one.
    const PointerTarget corner = editor_.ResolvePointerTarget(100.0f, 100.0f);
    EXPECT_EQ(corner.kind, PointerTarget::Kind::Handle);
    EXPECT_EQ(corner.handle, ResizeHandle::NW);

    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::DeleteSelection}));
    EXPECT_TRUE(editor_.Selection().empty());
    EXPECT_EQ(editor_.ResolvePointerTarget(150.0f, 150.0f).kind, PointerTarget::Kind::None);
    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::Undo}));
    EXPECT_EQ(editor_.ResolvePointerTarget(150.0f, 150.0f).item, a);
}

TEST_F(EditorTest, AMarkingToolKeyEntersDrawingModeOnTheSelection) {
    const ItemId a = test::Model(session_).CreateItem(false, Rect{100, 100, 200, 150}, "A");
    EXPECT_TRUE(editor_.Dispatch(Command{CommandId::DrawTool}));
    EXPECT_FALSE(editor_.DrawingItem().has_value()) << "nothing selected to draw on";
    editor_.SelectOnly(a);
    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::EraseTool}));
    EXPECT_EQ(editor_.DrawingItem(), a);
    EXPECT_EQ(editor_.ActiveTool(), Tool::Erase);
    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::PutDown}));
    EXPECT_FALSE(editor_.DrawingItem().has_value());
    EXPECT_EQ(editor_.ActiveTool(), Tool::Select);
}

}  // namespace
}  // namespace sz::ui
