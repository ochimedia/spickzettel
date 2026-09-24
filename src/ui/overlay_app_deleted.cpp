#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <cstdio>
#include <ctime>
#include <string>

#include "ui/icons_generated.h"

#include <imgui.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Show deleted =================
//
// What the Canvases tab adds with Show deleted on: deleted folders in the
// sidebar and deleted canvases in the grid, where they were, marked out in
// red with Restore and Delete permanently on each - so where a restore puts
// a thing back is where it is seen - and everything else dimmed. A folder
// is marked out when it is deleted or holds a deleted canvas, and its two
// buttons act on what is deleted in it (see CanvasManager::Restore and
// Session::DeleteMarkedCanvasesPermanently). Snippets are not shown: a
// deleted snippet comes back by undo or not at all.

namespace {

// A local calendar time, copied out of the buffer std::localtime shares -
// see TimestampName for why that spelling, and the pragma.
std::tm LocalTime(std::time_t when) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const std::tm* local = std::localtime(&when);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return local != nullptr ? *local : std::tm{};
}

}  // namespace

namespace overlay_detail {

std::string DeletedWhen(int64_t deletedAt, std::time_t now) {
    const std::tm at = LocalTime(static_cast<std::time_t>(deletedAt));
    const std::tm today = LocalTime(now);
    const std::tm yesterday = LocalTime(now - 24 * 60 * 60);
    const auto sameDay = [](const std::tm& a, const std::tm& b) {
        return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
    };
    char clock[16] = "";
    std::strftime(clock, sizeof(clock), "%H:%M", &at);
    char day[64] = "";
    if (sameDay(at, today)) {
        std::snprintf(day, sizeof(day), strings::kDeletedToday, clock);
    } else if (sameDay(at, yesterday)) {
        std::snprintf(day, sizeof(day), strings::kDeletedYesterday, clock);
    } else {
        std::strftime(day, sizeof(day), "%Y-%m-%d %H:%M", &at);
    }
    constexpr int64_t kMinute = 60;
    constexpr int64_t kHour = 60 * kMinute;
    constexpr int64_t kDay = 24 * kHour;
    const int64_t seconds = static_cast<int64_t>(now) - deletedAt;
    char ago[48] = "";
    if (seconds >= 0 && seconds < kMinute) {
        std::snprintf(ago, sizeof(ago), "%s", strings::kDeletedJustNow);
    } else if (seconds >= kMinute && seconds < kHour) {
        std::snprintf(ago, sizeof(ago), strings::kDeletedMinutesAgo, static_cast<int>(seconds / kMinute));
    } else if (seconds >= kHour && seconds < kDay) {
        std::snprintf(ago, sizeof(ago), strings::kDeletedHoursAgo, static_cast<int>(seconds / kHour));
    }
    char line[160];
    if (ago[0] != '\0') {
        std::snprintf(line, sizeof(line), strings::kDeletedAtAgo, day, ago);
    } else {
        std::snprintf(line, sizeof(line), strings::kDeletedAt, day);
    }
    return line;
}

std::string GoesOn(int64_t deletedAt, int days) {
    const std::tm on = LocalTime(static_cast<std::time_t>(deletedAt + int64_t{days} * 24 * 60 * 60));
    char date[32] = "";
    std::strftime(date, sizeof(date), "%Y-%m-%d", &on);
    char line[96];
    std::snprintf(line, sizeof(line), strings::kDeletedGoesOn, date);
    return line;
}

DeletedButton DeletedButtons(const char* restoreTip, const char* deleteTip) {
    DeletedButton pressed = DeletedButton::None;
    // Accent-filled rather than neutral: of the two, it is the one meant.
    if (PillIconButton("##restore", icons::kUndo, /*active=*/true)) {
        pressed = DeletedButton::Restore;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", restoreTip);
    }
    ImGui::SameLine(0.0f, kDeletedButtonGap);
    if (DangerIconButton("##deleteforgood", icons::kTrash)) {
        pressed = DeletedButton::DeleteForGood;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", deleteTip);
    }
    return pressed;
}

}  // namespace overlay_detail

FolderId OverlayApp::OverviewFolderId() const {
    return ShowingDeleted() && deletedFolderShown_.has_value() ? *deletedFolderShown_ : Manager().CurrentFolderId();
}

void OverlayApp::SettleDeletedFolderShown() {
    if (!deletedFolderShown_.has_value()) {
        return;
    }
    const Folder* folder = Manager().FindFolder(*deletedFolderShown_);
    if (ShowingDeleted() && folder != nullptr && Manager().IsDeleted(*folder)) {
        return;
    }
    // Restored while it was showing: still the folder being looked at, now
    // as any live one is.
    if (ShowingDeleted() && folder != nullptr) {
        Manager().SwitchToFolder(folder->id);
    }
    deletedFolderShown_.reset();
}

float OverlayApp::OverviewSidebarWidth() const {
    // Room for the second button on top of what a row of one needs - see
    // kOverviewSidebarWidth.
    return kOverviewSidebarWidth + (ShowingDeleted() ? kPillButtonSize + kDeletedButtonGap : 0.0f);
}

}  // namespace sz::ui
