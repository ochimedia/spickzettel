#include "ui/overlay_app_internal.h"

#include <gtest/gtest.h>

#include "core/config/shortcut_action.h"

namespace sz::ui {
namespace {

// The mapping between the config layer's flat list and the app's own three
// enums (see overlay_detail::kShortcutTargets). A table rather than a
// switch, so this is what stands in for the compiler's exhaustiveness
// check.
TEST(ShortcutTargetsTest, EveryActionRunsExactlyOneThing) {
    for (const ShortcutAction action : kAllShortcutActions) {
        const overlay_detail::ShortcutTarget& target = overlay_detail::TargetForShortcut(action);
        EXPECT_EQ(target.action, action) << "no target for " << ShortcutActionKey(action);
        const int set = static_cast<int>(target.tool.has_value()) + static_cast<int>(target.create.has_value()) +
                        static_cast<int>(target.clipboard.has_value());
        EXPECT_EQ(set, 1) << ShortcutActionKey(action)
                          << " must be a tool, a create action or a clipboard action, and only one of them";
    }
}

TEST(ShortcutTargetsTest, EveryToolAndCreateActionCanBeBound) {
    for (const overlay_detail::GalleryTool& tool : overlay_detail::kGalleryTools) {
        EXPECT_EQ(overlay_detail::TargetForShortcut(overlay_detail::ShortcutForTool(tool.tool)).tool,
                   tool.tool);
    }
    for (const overlay_detail::CreateActionInfo& info : overlay_detail::kCreateActions) {
        EXPECT_EQ(
            overlay_detail::TargetForShortcut(overlay_detail::ShortcutForCreateAction(info.action)).create,
            info.action);
    }
    for (const overlay_detail::ClipboardActionInfo& info : overlay_detail::kClipboardActions) {
        EXPECT_EQ(
            overlay_detail::TargetForShortcut(overlay_detail::ShortcutForClipboardAction(info.action)).clipboard,
            info.action);
    }
}

}  // namespace
}  // namespace sz::ui
