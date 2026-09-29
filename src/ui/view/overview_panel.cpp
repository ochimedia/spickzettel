#include "ui/view/overview_panel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "core/build_info/build_info.h"
#include "core/config/settings_catalog.h"
#include "core/util/timestamp_name.h"
#include "generated/ui_strings.h"
#include "ui/icons_generated.h"
#include "ui/interaction/levels.h"
#include "ui/item_painting.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace sz::ui {

using namespace ::sz::core;

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

// The folder list down the Overview's left side - the sidebar's width, and
// where the footer's "New canvas" lines up (see OverviewSidebarWidth,
// which adds to it with Show deleted on). Wide enough for a folder's
// default name, which is a full timestamp ("2026-09-07 22:53:26" - see
// TimestampName). Narrower clips the last digit of the seconds, which reads
// as a rendering bug rather than as a name that is simply long. The row
// reserves 34 for the delete button and insets the text by 10, so this is
// the name's width plus room to breathe.
constexpr float kOverviewSidebarWidth = 200.0f;

// Between a deleted thing's Restore and its Delete permanently.
constexpr float kDeletedButtonGap = 4.0f;
// How much of itself a folder or canvas with nothing deleted about it keeps
// while Show deleted is on: there, still usable, and plainly not what the
// view is about.
constexpr float kDimmedAlpha = 0.4f;

// A deleted folder's or canvas's two buttons, side by side at the cursor:
// Restore, and Delete permanently. Ids "##restore" and "##deleteforgood",
// under whatever the caller has pushed. Where Restore was drawn goes to
// `restore`, for the caller to mark.
enum class DeletedButton { None, Restore, DeleteForGood };
DeletedButton DeletedButtons(const char* restoreTip, const char* deleteTip, ImRect& restore);

// A local calendar time, copied out of the buffer std::localtime shares -
// see TimestampName for why that spelling, and the pragma. None for a time
// the C runtime has no date for: before 1970, or past the year 3000 with
// Microsoft's. That was a zeroed std::tm, whose day 0 strftime refuses -
// and the invalid-parameter handler takes a refusal for a crash (see
// platform/win32/win32_crash_dump.cpp), so a tooltip ended the app.
std::optional<std::tm> LocalTime(std::time_t when) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const std::tm* local = std::localtime(&when);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (local == nullptr) {
        return std::nullopt;
    }
    return *local;
}

}  // namespace

std::string DeletedWhen(int64_t deletedAt, std::time_t now) {
    const std::optional<std::tm> at = LocalTime(static_cast<std::time_t>(deletedAt));
    if (!at.has_value()) {
        char line[96];
        std::snprintf(line, sizeof(line), strings::kDeletedAt, strings::kDeletedAtUnknownTime);
        return line;
    }
    const std::optional<std::tm> today = LocalTime(now);
    const std::optional<std::tm> yesterday = LocalTime(now - 24 * 60 * 60);
    const auto sameDay = [](const std::tm& a, const std::optional<std::tm>& b) {
        return b.has_value() && a.tm_year == b->tm_year && a.tm_yday == b->tm_yday;
    };
    char clock[16] = "";
    std::strftime(clock, sizeof(clock), "%H:%M", &*at);
    char day[64] = "";
    if (sameDay(*at, today)) {
        std::snprintf(day, sizeof(day), strings::kDeletedToday, clock);
    } else if (sameDay(*at, yesterday)) {
        std::snprintf(day, sizeof(day), strings::kDeletedYesterday, clock);
    } else {
        std::strftime(day, sizeof(day), "%Y-%m-%d %H:%M", &*at);
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
    const std::optional<std::tm> on = LocalTime(static_cast<std::time_t>(deletedAt + int64_t{days} * 24 * 60 * 60));
    if (!on.has_value()) {
        return strings::kDeletedGoesOnUnknownDate;
    }
    char date[32] = "";
    std::strftime(date, sizeof(date), "%Y-%m-%d", &*on);
    char line[96];
    std::snprintf(line, sizeof(line), strings::kDeletedGoesOn, date);
    return line;
}

namespace {

DeletedButton DeletedButtons(const char* restoreTip, const char* deleteTip, ImRect& restore) {
    DeletedButton pressed = DeletedButton::None;
    // Accent-filled rather than neutral: of the two, it is the one meant.
    if (PillIconButton("##restore", icons::kUndo, /*active=*/true)) {
        pressed = DeletedButton::Restore;
    }
    restore = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", restoreTip);
    }
    ImGui::SameLine(0.0f, Px(kDeletedButtonGap));
    if (DangerIconButton("##deleteforgood", icons::kTrash)) {
        pressed = DeletedButton::DeleteForGood;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", deleteTip);
    }
    return pressed;
}

}  // namespace

FolderId OverviewPanel::OverviewFolderId() const {
    return ShowingDeleted() && deletedFolderShown_.has_value() ? *deletedFolderShown_ : Manager().CurrentFolderId();
}

void OverviewPanel::SettleDeletedFolderShown() {
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
        session_.SwitchToFolder(folder->id);
    }
    deletedFolderShown_.reset();
}

float OverviewPanel::OverviewSidebarWidth() const {
    // Room for the second button on top of what a row of one needs - see
    // kOverviewSidebarWidth.
    return Px(kOverviewSidebarWidth) + (ShowingDeleted() ? Px(kPillButtonSize) + Px(kDeletedButtonGap) : 0.0f);
}

OverviewPanel::OverviewPanel(Session& session, Settings& settings, Editor& editor, ViewHost& host)
    : session_(session), settings_(settings), editor_(editor), host_(host) {}

bool OverviewPanel::IsOpen() const {
    const Panel* panel = editor_.Input().As<Panel>(Level::Panel);
    return panel != nullptr && panel->Kind() == PanelKind::Overview;
}

void OverviewPanel::Open() {
    pickerItemId_.reset();
    if (!IsOpen()) {
        editor_.Input().Push(std::make_unique<Panel>(PanelKind::Overview), Event{});
    }
    overviewTab_ = OverviewTab::Canvases;
    showDeleted_ = false;
    deletedFolderShown_.reset();
}

void OverviewPanel::OpenSettings() {
    Open();
    SwitchOverviewTab(OverviewTab::Settings);
}

// Opened as Open opens it, on the Canvases tab with nothing deleted shown -
// see "The Overview" in docs/ARCHITECTURE.md.
void OverviewPanel::OpenPicker(ItemId itemId, bool isCopy) {
    Open();
    pickerItemId_ = itemId;
    pickerIsCopy_ = isCopy;
}

void OverviewPanel::Close() {
    pickerItemId_.reset();
    if (editor_.Input().At(Level::Text) != nullptr && editor_.Input().As<TypingNote>(Level::Text) == nullptr) {
        editor_.Input().End(Level::Text);
    }
    // Nothing, when the machine has ended it already - see Panel.
    if (IsOpen()) {
        editor_.Input().End(Level::Panel);
    }
}

void OverviewPanel::Draw(float displayW, float displayH, const std::function<void()>& settingsBody) {
    if (!IsOpen()) {
        return;
    }
    // A click on the backdrop closes it - after this frame, which draws it
    // whole, as every action waits for the draw to finish (see Act).
    if (PanelBackdrop("##overview_backdrop", displayW, displayH)) {
        host_.Act(action::ClosePanel{PanelKind::Overview});
    }

    constexpr float kPanelMarginFrac = 0.08f;
    const ImVec2 panelMin(displayW * kPanelMarginFrac, displayH * kPanelMarginFrac);
    const ImVec2 panelSize(displayW * (1.0f - 2.0f * kPanelMarginFrac), displayH * (1.0f - 2.0f * kPanelMarginFrac));
    ImGui::SetNextWindowPos(panelMin);
    ImGui::SetNextWindowSize(panelSize);
    ImGui::Begin("##overview_panel", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                      ImGuiWindowFlags_NoMove);

    RenderOverviewHeader();
    ImGui::Separator();
    const ViewHost::PreviewDrawing previews = host_.Previews();

    // Both the Canvases body below and the Settings page render
    // fine even while picking (pickerItemId_ set) - the tab strip is only
    // hidden there, not the tab logic - but picking a move/copy
    // destination is exactly the "browse canvases" task, so force the
    // Canvases body regardless of whatever overviewTab_ happens to still
    // hold from a previous, non-picker visit.
    const bool showCanvasesBody = pickerItemId_.has_value() || overviewTab_ == OverviewTab::Canvases;

    ImGui::BeginChild("##overview_body", ImVec2(0.0f, -Px(40.0f)), ImGuiChildFlags_None);
    // A new page starts at its beginning. All three tabs and both About
    // pages share this one scrolling child, so without this, opening the
    // licenses from halfway down the About text drops you halfway down the
    // licenses - and the buttons that ask for the switch are in the footer,
    // outside this child, where SetScrollY would move the wrong window.
    if (overviewBodyScrollToTop_) {
        ImGui::SetScrollY(0.0f);
        overviewBodyScrollToTop_ = false;
    }
    if (!showCanvasesBody && overviewTab_ == OverviewTab::About) {
        RenderOverviewAboutPanel();
    } else if (!showCanvasesBody) {
        settingsBody();
    } else {
        SettleDeletedFolderShown();
        RenderFolderSidebar();
        ImGui::SameLine();
        RenderCanvasGrid(displayW, displayH, previews);
    }
    ImGui::EndChild();  // ##overview_body

    RenderOverviewFooter(showCanvasesBody);

    ImGui::End();
}

void OverviewPanel::RenderOverviewHeader() {
    // Null with an empty library (see CanvasManager's class comment) -
    // the Overview is exactly the screen that has to keep working then,
    // since it's where a new canvas or folder comes from.
    const Canvas* currentCanvas = Manager().CurrentOrNull();
    if (pickerItemId_.has_value() && currentCanvas) {
        const auto it = std::find_if(currentCanvas->items.begin(), currentCanvas->items.end(),
                                      [&](const Item& i) { return i.id == *pickerItemId_; });
        const std::string itemName = it != currentCanvas->items.end() ? it->name : strings::kMoveCopyItemWord;
        ImGui::TextColored(theme::Accent(), strings::kMoveCopyPrompt, pickerIsCopy_ ? strings::kMoveCopyCopy : strings::kMoveCopyMove, itemName.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(Labeled(strings::kMoveCopyCancel, "pickercancel"))) {
            host_.Act(action::ClosePanel{PanelKind::Overview});
        }
        return;
    }

    // Picker mode (above) never shows these - it has its own
    // single-purpose header, and Settings has no business being
    // reachable mid-pick (see overviewTab_'s own doc comment).
    if (TabButton("overviewtabcanvases", strings::kOverviewTabCanvases, overviewTab_ == OverviewTab::Canvases)) {
        SwitchOverviewTab(OverviewTab::Canvases);
    }
    ImGui::SameLine();
    if (TabButton("overviewtabsettings", strings::kOverviewTabSettings, overviewTab_ == OverviewTab::Settings)) {
        SwitchOverviewTab(OverviewTab::Settings);
    }
    ImGui::SameLine();
    if (TabButton("overviewtababout", strings::kOverviewTabAbout, overviewTab_ == OverviewTab::About)) {
        SwitchOverviewTab(OverviewTab::About);
    }
    if (overviewTab_ != OverviewTab::Canvases) {
        return;
    }

    // What the canvas thumbnails may draw, right-aligned on the tab row -
    // the reason to reach for either is being unable to tell two canvases
    // apart, so they belong next to the canvases rather than in Settings.
    // Only while a canvas grid is showing: a preview toggle over the About
    // text is a control with nothing to act on.
    //
    // What is deleted is shown in the same folders and grid, where it was,
    // rather than on a page of its own - so this is a way of looking at
    // them, beside the other two, and says how much there is to see.
    char deletedLabel[96];
    const size_t deletedCount = Manager().DeletedFolderAndCanvasCount();
    if (deletedCount > 0) {
        std::snprintf(deletedLabel, sizeof(deletedLabel), strings::kOverviewShowDeletedCount, deletedCount);
    } else {
        std::snprintf(deletedLabel, sizeof(deletedLabel), "%s", strings::kOverviewShowDeleted);
    }
    const float checkboxWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float gapBeforePreviews = ImGui::GetStyle().ItemSpacing.x * 4.0f;
    const float controlsWidth = checkboxWidth + ImGui::CalcTextSize(deletedLabel).x +
                                gapBeforePreviews + ImGui::CalcTextSize(strings::kOverviewPreviewsLabel).x +
                                ImGui::GetStyle().ItemSpacing.x + checkboxWidth +
                                ImGui::CalcTextSize(strings::kOverviewPreviewsVector).x +
                                ImGui::GetStyle().ItemSpacing.x + checkboxWidth +
                                ImGui::CalcTextSize(strings::kOverviewPreviewsBitmap).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                          std::max(0.0f, ImGui::GetContentRegionAvail().x - controlsWidth));
    // Vertically centered against the tab buttons, which are taller
    // than a checkbox's own frame.
    const float rowCenterOffset = (ImGui::GetItemRectSize().y - ImGui::GetFrameHeight()) * 0.5f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, rowCenterOffset));
    if (ImGui::Checkbox(Labeled(deletedLabel, "showdeleted"), &showDeleted_)) {
        SettleDeletedFolderShown();
    }
    host_.Mark(Anchor{AnchorId::OverviewShowDeleted}, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kOverviewShowDeletedHelp);
    }
    ImGui::SameLine(0.0f, gapBeforePreviews);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGraphite300, "%s", strings::kOverviewPreviewsLabel);
    ImGui::SameLine();
    // "Vector", not "Strokes": in bitmap mode a stroke *is* pixels, so a
    // box labeled Strokes that leaves them showing when it is unchecked
    // reads as a bug rather than as the two halves of the drawing model.
    bool strokes = settings_.Get(setting::kOverviewShowsStrokes);
    if (ImGui::Checkbox(Labeled(strings::kOverviewPreviewsVector, "prevvector"), &strokes)) {
        settings_.Set(setting::kOverviewShowsStrokes, strokes);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kOverviewPreviewsVectorHelp);
    }
    ImGui::SameLine();
    bool bitmaps = settings_.Get(setting::kOverviewShowsBitmaps);
    if (ImGui::Checkbox(Labeled(strings::kOverviewPreviewsBitmap, "prevbitmap"), &bitmaps)) {
        settings_.Set(setting::kOverviewShowsBitmaps, bitmaps);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kOverviewPreviewsBitmapHelp);
    }
}

void OverviewPanel::RenderFolderSidebar() {
    constexpr float kFolderRowHeight = 34.0f;
    constexpr float kFolderRowGap = 4.0f;
    const std::vector<Folder>& folders = Manager().Folders();
    const FolderId currentFolderId = OverviewFolderId();
    const bool showingDeleted = ShowingDeleted();
    const std::time_t now = std::time(nullptr);

    ImGui::BeginChild("##folder_sidebar", ImVec2(OverviewSidebarWidth(), 0.0f), ImGuiChildFlags_None);
    if (!showingDeleted &&
        std::all_of(folders.begin(), folders.end(), [&](const Folder& f) { return Manager().IsDeleted(f); })) {
        // Every folder has been deleted - legal now (see CanvasManager's
        // class comment), and reachable in one step from a library with a
        // single folder in it. "New folder" in the footer is still right
        // there, and so is "New canvas", which mints a folder to put it in.
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kOverviewNoFolders);
        ImGui::PopTextWrapPos();
    }
    for (size_t fi = 0; fi < folders.size(); ++fi) {
        const Folder& f = folders[fi];
        const bool deleted = Manager().IsDeleted(f);
        // Skipped here rather than filtered out beforehand, so that `fi`
        // stays the folder's place in Folders(), which ReorderFolder takes.
        if (deleted && !showingDeleted) {
            continue;
        }
        // With Show deleted on, a folder that is deleted or holds a deleted
        // canvas is marked out, with the two buttons that act on what is
        // deleted in it, and every other folder is dimmed.
        const bool marked = showingDeleted && Manager().HoldsDeleted(f);
        const bool dimmed = showingDeleted && !marked;
        const bool isCurrentFolder = f.id == currentFolderId;
        const bool isRenamingThis = renamingFolderId_ == f.id;
        ImGui::PushID(static_cast<int>(f.id));
        if (dimmed) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * kDimmedAlpha);
        }

        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        // Room for the buttons is always reserved: the last folder is
        // deletable like any other.
        const float buttonsWidth = marked ? Px(kPillButtonSize) * 2.0f + Px(kDeletedButtonGap) : Px(kPillButtonSize);
        const float selectWidth = rowWidth - buttonsWidth - Px(6.0f);
        const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + Px(kFolderRowHeight));
        host_.Mark(Anchor{AnchorId::OverviewFolderRow, f.id}, rowMin, rowMax);
        ImDrawList* sidebarDrawList = ImGui::GetWindowDrawList();
        // Through GetColorU32, which a dimmed row's alpha applies to.
        if (isCurrentFolder) {
            const ImVec4 fill = marked ? ImVec4(theme::kDanger.x, theme::kDanger.y, theme::kDanger.z, 0.32f)
                                       : ImVec4(theme::Accent().x, theme::Accent().y, theme::Accent().z, 0.2f);
            sidebarDrawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(fill), Px(theme::kRadiusSm));
        } else if (marked) {
            sidebarDrawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(theme::kDangerSoft),
                                           Px(theme::kRadiusSm));
        }

        if (isRenamingThis) {
            // No InvisibleButton/drag-drop this frame: it would sit right
            // under the InputText drawn below at the same screen rect, and
            // (per the "overlapping widgets resolve first-submitted-wins"
            // gotcha - see docs/ARCHITECTURE.md) would win every click,
            // leaving the input field unclickable.
            ImGui::Dummy(ImVec2(selectWidth, Px(kFolderRowHeight)));
        } else {
            if (ImGui::InvisibleButton("##folderrow", ImVec2(selectWidth, Px(kFolderRowHeight)))) {
                // A deleted folder is looked into, not browsed - see
                // deletedFolderShown_.
                if (deleted) {
                    host_.Act(action::ShowDeletedFolder{f.id});
                } else {
                    host_.Act(action::SwitchFolder{f.id});
                }
            }
            const bool rowHovered = ImGui::IsItemHovered();
            if (rowHovered && !deleted && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                BeginRenaming(f.id, std::nullopt, f.name);
            }
            if (rowHovered && !isCurrentFolder) {
                sidebarDrawList->AddRectFilled(rowMin, ImVec2(rowMin.x + selectWidth, rowMax.y),
                                                ImGui::GetColorU32(theme::kHoverWash), Px(theme::kRadiusSm));
            }
            if (rowHovered && marked) {
                if (deleted && Cfg().purgeDeleted) {
                    ImGui::SetTooltip("%s\n%s", DeletedWhen(f.deletedAt, now).c_str(),
                                      GoesOn(f.deletedAt, Cfg().purgeDeletedAfterDays).c_str());
                } else if (deleted) {
                    ImGui::SetTooltip("%s", DeletedWhen(f.deletedAt, now).c_str());
                } else {
                    const size_t count = Manager().MarkedCanvasesIn(f.id).size();
                    ImGui::SetTooltip(count == 1 ? strings::kDeletedHoldsOne : strings::kDeletedHoldsMany, count);
                }
            }
            // Nothing is dragged out of or into what is deleted: it stays
            // where it was until it is restored.
            if (!deleted) {
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("SZ_FOLDER", &f.id, sizeof(FolderId));
                    ImGui::TextUnformatted(f.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SZ_FOLDER")) {
                        const FolderId draggedId = *static_cast<const FolderId*>(payload->Data);
                        if (draggedId != f.id) {
                            host_.Act(action::ReorderFolder{draggedId, fi});
                        }
                    }
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SZ_CANVAS")) {
                        const CanvasId draggedCanvasId = *static_cast<const CanvasId*>(payload->Data);
                        host_.Act(action::MoveCanvasToFolder{draggedCanvasId, f.id});
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        }

        if (isRenamingThis) {
            ImGui::SetCursorScreenPos(
                ImVec2(rowMin.x + Px(6.0f), rowMin.y + (Px(kFolderRowHeight) - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::SetNextItemWidth(selectWidth - Px(12.0f));
            if (renameJustFocused_) {
                ImGui::SetKeyboardFocusHere();
                renameJustFocused_ = false;
            }
            ImGui::InputText("##renamefolder", renameBuffer_, sizeof(renameBuffer_));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                host_.Act(action::RenameFolder{f.id, std::string(renameBuffer_)});
                renamingFolderId_.reset();
                host_.Window()->ReleaseTextInput();
            } else if (ImGui::IsItemDeactivated()) {
                renamingFolderId_.reset();
                host_.Window()->ReleaseTextInput();
            }
        } else {
            const ImVec4& ink = marked && !isCurrentFolder ? theme::kDeletedInk
                                : isCurrentFolder         ? theme::kWhite
                                                          : theme::kGraphite200;
            const ImVec2 textPos(rowMin.x + Px(10.0f),
                                 rowMin.y + (Px(kFolderRowHeight) - ImGui::GetTextLineHeight()) * 0.5f);
            sidebarDrawList->PushClipRect(rowMin, ImVec2(rowMin.x + selectWidth - Px(4.0f), rowMax.y), true);
            sidebarDrawList->AddText(textPos, ImGui::GetColorU32(ink), f.name.c_str());
            sidebarDrawList->PopClipRect();
        }

        if (!isRenamingThis) {
            const float buttonY = rowMin.y + (Px(kFolderRowHeight) - Px(kPillButtonSize)) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + selectWidth + Px(4.0f), buttonY));
            if (marked) {
                // Both act on what is deleted in the folder: all of it back,
                // or all of it gone for good - the folder with it only if
                // the folder is what was deleted.
                ImRect restore;
                const DeletedButton pressed = DeletedButtons(
                    strings::kDeletedRestoreFolderTip,
                    deleted ? strings::kDeletedDeleteForGoodTip : strings::kDeletedDeleteDeletedInFolderTip, restore);
                host_.Mark(Anchor{AnchorId::OverviewRestore, f.id}, restore.Min, restore.Max);
                switch (pressed) {
                    case DeletedButton::Restore:
                        host_.Act(action::Restore{f.id});
                        break;
                    case DeletedButton::DeleteForGood:
                        host_.AskToDelete(DeleteTarget{deleted ? DeleteTarget::Kind::Folder
                                                         : DeleteTarget::Kind::DeletedCanvasesIn,
                                                 f.id, f.name, /*forGood=*/true});
                        break;
                    case DeletedButton::None:
                        break;
                }
            } else {
                // Not while picking: see SendPickedItemTo.
                ImGui::BeginDisabled(pickerItemId_.has_value());
                const bool deletePressed = DangerIconButton("##delfolder", icons::kTrash);
                ImGui::EndDisabled();
                if (deletePressed) {
                    host_.AskToDelete(DeleteTarget{DeleteTarget::Kind::Folder, f.id, f.name});
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", strings::kOverviewDeleteFolder);
                }
            }
        }

        // A just-made folder is the last row, which may be below the fold -
        // bring it into view once (see overviewScrollToFolderId_). By
        // position rather than ImGui::SetScrollHereY, which measures the
        // last item submitted: that is this row's delete button, not the
        // row, and the two are different heights.
        if (overviewScrollToFolderId_ == f.id) {
            ImGui::SetScrollFromPosY(rowMin.y - ImGui::GetWindowPos().y, 0.5f);
        }
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y + Px(kFolderRowGap)));
        if (dimmed) {
            ImGui::PopStyleVar();
        }
        ImGui::PopID();
    }
    // Whether or not it was found, so a stale id can't keep pulling the
    // sidebar around on later frames - same as the canvas grid's own.
    overviewScrollToFolderId_.reset();
    // Closes out the last row's SetCursorScreenPos boundary extension above -
    // ImGui asserts if a window/child ends right after a manual cursor move
    // with no item submitted afterward to confirm growing to that position.
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndChild();
}

void OverviewPanel::RenderCanvasGrid(float displayW, float displayH, const ViewHost::PreviewDrawing& previews) {
    const ImVec2 tileSize = Px(200.0f, 130.0f);
    constexpr float kSpacing = 14.0f;
    const std::vector<Canvas>& canvases = Manager().Canvases();
    const CanvasId currentCanvasId = Manager().CurrentCanvasId();  // 0 when there is none
    const FolderId currentFolderId = OverviewFolderId();
    const bool showingDeleted = ShowingDeleted();
    const std::time_t now = std::time(nullptr);

    // Canvases belonging to the shown folder that aren't deleted (or all of
    // them, with Show deleted on), in Canvases() order, and beside each its
    // place among *all* of that folder's canvases - which is what
    // ReorderCanvas expects for `newIndex` (it's scoped within the moved
    // canvas's own folder, hidden ones and all).
    std::vector<size_t> folderCanvasIndices;
    std::vector<size_t> folderCanvasPlaces;
    size_t placeInFolder = 0;
    for (size_t i = 0; i < canvases.size(); ++i) {
        if (canvases[i].folderId != currentFolderId) {
            continue;
        }
        if (showingDeleted || !Manager().IsDeleted(canvases[i])) {
            folderCanvasIndices.push_back(i);
            folderCanvasPlaces.push_back(placeInFolder);
        }
        ++placeInFolder;
    }

    ImGui::BeginChild("##overview_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    const float availW = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((availW + Px(kSpacing)) / (tileSize.x + Px(kSpacing))));

    // Built once for the whole grid rather than per tile.
    const PreviewTextureFn& previewTexture = previews.textures;

    if (folderCanvasIndices.empty()) {
        // A folder holding nothing has always been possible (a freshly
        // created one starts out that way); what's new is that the
        // *library* can be in the same state, with no canvas current at
        // all - so the two get separate wording, since only the second
        // one means there's nothing being edited behind this panel.
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(theme::kGraphite200, "%s",
                           Manager().HasCurrentCanvas() ? strings::kOverviewFolderEmpty : strings::kOverviewLibraryEmpty);
        ImGui::PopTextWrapPos();
    }
    for (size_t idxInFolder = 0; idxInFolder < folderCanvasIndices.size(); ++idxInFolder) {
        const Canvas& c = canvases[folderCanvasIndices[idxInFolder]];
        if (idxInFolder % static_cast<size_t>(columns) != 0) {
            ImGui::SameLine(0.0f, Px(kSpacing));
        }
        ImGui::PushID(static_cast<int>(c.id));
        // Only ever with Show deleted on: deleted on its own or with its
        // folder, and marked out either way. Everything else is dimmed.
        const bool deleted = Manager().IsDeleted(c);
        const bool dimmed = showingDeleted && !deleted;
        if (dimmed) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * kDimmedAlpha);
        }
        ImGui::BeginGroup();

        const ImVec2 thumbMin = ImGui::GetCursorScreenPos();
        const ImVec2 thumbMax(thumbMin.x + tileSize.x, thumbMin.y + tileSize.y);
        host_.Mark(Anchor{AnchorId::OverviewCanvasTile, c.id}, thumbMin, thumbMax);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        DrawCanvasPreview(drawList, c, thumbMin, thumbMax, displayW, displayH, Cfg().overviewShowsStrokes,
                          previewTexture, previews.meshes, previews.hooks);
        const bool isActive = c.id == currentCanvasId;
        if (deleted) {
            drawList->AddRectFilled(thumbMin, thumbMax, ImGui::GetColorU32(theme::kDangerSoft), Px(4.0f));
            drawList->AddRect(thumbMin, thumbMax, ImGui::GetColorU32(theme::kDanger), Px(4.0f), ImDrawFlags_None,
                              PxWhole(2.0f));
        } else {
            // The preview's own pictures are drawn at full strength whatever
            // the style's alpha, so a dimmed tile is dimmed by a veil.
            if (dimmed) {
                drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(14, 16, 20, 150), Px(4.0f));
            }
            drawList->AddRect(thumbMin, thumbMax,
                               ImGui::GetColorU32(isActive ? theme::Accent() : ImVec4(0.275f, 0.298f, 0.345f, 1.0f)),
                               Px(4.0f), ImDrawFlags_None, isActive ? PxWhole(2.0f) : 1.0f);
        }

        // A tile is where a snippet being sent somewhere goes, or else the
        // canvas to go to, with the Overview's job done.
        if (ImGui::InvisibleButton("##tile", tileSize) && !deleted) {
            if (pickerItemId_.has_value()) {
                host_.Act(action::SendPicked{c.id});
            } else {
                host_.Act(action::SwitchCanvas{c.id});
                host_.Act(action::ClosePanel{PanelKind::Overview});
            }
        }
        if (deleted && ImGui::IsItemHovered()) {
            // Its own stamp, or its folder's when it went with the folder.
            const Folder* folder = Manager().FindFolder(c.folderId);
            const int64_t stamp = c.deletedAt != 0 ? c.deletedAt : folder != nullptr ? folder->deletedAt : 0;
            if (Cfg().purgeDeleted) {
                ImGui::SetTooltip("%s\n%s\n%s", DeletedWhen(stamp, now).c_str(),
                                  GoesOn(stamp, Cfg().purgeDeletedAfterDays).c_str(), strings::kDeletedRestoreToOpen);
            } else {
                ImGui::SetTooltip("%s\n%s", DeletedWhen(stamp, now).c_str(), strings::kDeletedRestoreToOpen);
            }
        }
        if (!deleted && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("SZ_CANVAS", &c.id, sizeof(CanvasId));
            ImGui::TextUnformatted(c.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (!deleted && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SZ_CANVAS")) {
                const CanvasId draggedId = *static_cast<const CanvasId*>(payload->Data);
                if (draggedId != c.id) {
                    host_.Act(action::ReorderCanvas{draggedId, folderCanvasPlaces[idxInFolder]});
                }
            }
            ImGui::EndDragDropTarget();
        }

        const bool isRenamingThisCanvas = renamingCanvasId_ == c.id;
        if (isRenamingThisCanvas) {
            ImGui::SetNextItemWidth(tileSize.x);
            if (renameJustFocused_) {
                ImGui::SetKeyboardFocusHere();
                renameJustFocused_ = false;
            }
            ImGui::InputText("##renamecanvas", renameBuffer_, sizeof(renameBuffer_));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                host_.Act(action::RenameCanvas{c.id, std::string(renameBuffer_)});
                renamingCanvasId_.reset();
                host_.Window()->ReleaseTextInput();
            } else if (ImGui::IsItemDeactivated()) {
                renamingCanvasId_.reset();
                host_.Window()->ReleaseTextInput();
            }
        } else {
            ImGui::TextColored(deleted ? theme::kDeletedInk : theme::kWhite, "%s", c.name.c_str());
            if (!deleted && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                BeginRenaming(std::nullopt, c.id, c.name);
            }
        }
        // No "...and this folder holds more than one" condition: the last
        // canvas in a folder is deletable, and a button that is simply not
        // drawn reads as a bug. See CanvasManager's class comment for the
        // invariant behind it, now gone.
        if (deleted) {
            ImGui::SameLine(tileSize.x - Px(kPillButtonSize) * 2.0f - Px(kDeletedButtonGap));
            // Out of a deleted folder the folder comes back to hold it, and
            // the rest of what went with the folder stays deleted - see
            // CanvasManager::Restore.
            const Folder* folder = Manager().FindFolder(c.folderId);
            const bool folderDeleted = folder != nullptr && Manager().IsDeleted(*folder);
            ImRect restore;
            const DeletedButton pressed =
                DeletedButtons(folderDeleted ? strings::kDeletedRestoreCanvasAndFolderTip
                                             : strings::kDeletedRestoreCanvasTip,
                               strings::kDeletedDeleteForGoodTip, restore);
            host_.Mark(Anchor{AnchorId::OverviewRestore, c.id}, restore.Min, restore.Max);
            switch (pressed) {
                case DeletedButton::Restore:
                    host_.Act(action::Restore{c.id});
                    break;
                case DeletedButton::DeleteForGood:
                    host_.AskToDelete(DeleteTarget{DeleteTarget::Kind::Canvas, c.id, c.name, /*forGood=*/true});
                    break;
                case DeletedButton::None:
                    break;
            }
        } else if (!isRenamingThisCanvas) {
            ImGui::SameLine(tileSize.x - Px(kPillButtonSize));
            // Not while picking: see SendPickedItemTo.
            ImGui::BeginDisabled(pickerItemId_.has_value());
            const bool deletePressed = DangerIconButton("##delcanvas", icons::kTrash);
            ImGui::EndDisabled();
            host_.Mark(Anchor{AnchorId::OverviewCanvasDelete, c.id}, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (deletePressed) {
                host_.AskToDelete(DeleteTarget{DeleteTarget::Kind::Canvas, c.id, c.name});
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", strings::kOverviewDeleteCanvas);
            }
        }

        ImGui::EndGroup();
        if (dimmed) {
            ImGui::PopStyleVar();
        }
        // A just-created canvas is at the end of its folder, which may be
        // past the bottom of this list - scroll it into view once (see
        // overviewScrollToCanvasId_). Centered rather than merely made
        // visible: the tile that just appeared should be the one you are
        // looking at.
        if (overviewScrollToCanvasId_ == c.id) {
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::PopID();
    }
    // Whether or not it was found - the target may be in another folder,
    // or already gone - so a stale id can't keep yanking the list around
    // on later frames.
    overviewScrollToCanvasId_.reset();
    ImGui::EndChild();
}

void OverviewPanel::RenderOverviewFooter(bool showCanvasesBody) {
    if (!showCanvasesBody && overviewTab_ == OverviewTab::About) {
        // In the footer rather than at the end of the text it belongs to:
        // the licenses run to a couple of hundred lines, and a way out
        // that has to be scrolled back to is a way out you stop using.
        // The footer is the panel's own row of verbs - the Canvases tab
        // keeps New folder / New canvas here - so this is where a reader
        // already looks for one.
        if (aboutShowsNotices_) {
            if (ImGui::Button(Labeled(strings::kAboutBackToAbout, "noticesback"))) {
                aboutShowsNotices_ = false;
                overviewBodyScrollToTop_ = true;
            }
        } else if (ImGui::Button(Labeled(strings::kAboutThirdPartyLicenses, "noticesopen"))) {
            aboutShowsNotices_ = true;
            overviewBodyScrollToTop_ = true;
        }
        return;
    }
    if (!showCanvasesBody) {
        return;
    }

    // Not while picking, since it switches to the canvas it makes: see
    // SendPickedItemTo. New canvas stays, as a destination to send to.
    ImGui::BeginDisabled(pickerItemId_.has_value());
    const bool newFolderPressed = PrimaryButton("##newfolder", icons::kPlus, strings::kOverviewNewFolder);
    ImGui::EndDisabled();
    host_.Mark(Anchor{AnchorId::OverviewNewFolder}, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (newFolderPressed) {
        host_.Act(action::NewFolder{});
    }
    // Lined up with the canvas grid above it, which starts past the
    // window's padding, the sidebar and the gap after it. SameLine counts
    // from the window's edge rather than from inside its padding, so the
    // padding is added here - without it the button sat that far left of
    // the tiles.
    ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + OverviewSidebarWidth() + ImGui::GetStyle().ItemSpacing.x);
    // Not into a deleted folder, which is the one on show: a new canvas goes
    // to the folder being browsed, and that would be somewhere else.
    const bool showsDeletedFolder = ShowingDeleted() && deletedFolderShown_.has_value();
    ImGui::BeginDisabled(showsDeletedFolder);
    const bool newCanvasPressed = PrimaryButton("##newcanvas", icons::kPlus, strings::kOverviewNewCanvas);
    ImGui::EndDisabled();
    if (newCanvasPressed) {
        host_.Act(action::NewCanvas{});
    }
}

void OverviewPanel::SendPickedItemTo(CanvasId target) {
    if (!pickerItemId_.has_value()) {
        return;
    }
    const ItemId item = *pickerItemId_;
    const bool isCopy = pickerIsCopy_;
    const CanvasId source = Manager().CurrentCanvasId();
    // The item is sent from the current canvas, so nothing in the picker
    // may change which canvas that is: New folder and the delete buttons
    // are off while it is open. And what happened is checked rather than
    // assumed: a move that did nothing said "Moved to" all the same, and
    // threw away the history of a snippet that had not gone anywhere.
    if (target != source) {
        const Canvas* targetCanvas = Manager().FindCanvas(target);
        const std::string targetName = targetCanvas ? targetCanvas->name : strings::kDeleteConfirmCanvasWord;
        const Session::Placed sent = session_.SendItemsTo({item}, target, isCopy);
        if (sent.items.empty()) {
            pickerItemId_.reset();
            return;
        }
        host_.Say(sent.pictureLost ? std::string(strings::kToastCopiedWithoutPicture)
                                         : std::string(isCopy ? strings::kToastCopiedToPrefix
                                                              : strings::kToastMovedToPrefix) +
                                               targetName);
    }
    // Out of picker mode (the move/copy is already done), with the Overview
    // itself left open - the user can click the target canvas to switch to
    // it, or close the Overview themselves; closing it here would make the
    // whole panel vanish the instant a canvas was picked, with nothing but
    // the toast to explain what had just happened.
    pickerItemId_.reset();
}

namespace {
// What this tab shows: the component and the license it is under, in the
// order they matter to someone glancing at it. The full texts are in
// THIRD-PARTY-NOTICES.md, which is where they have to be complete - this
// list is the summary, and is the reason most people never open the other.
struct BuiltWithRow {
    const char* component;
    const char* license;
};
const BuiltWithRow kBuiltWith[] = {
    {strings::kAboutComponentImgui, strings::kAboutLicenseMit},
    {strings::kAboutComponentJson, strings::kAboutLicenseMit},
    {strings::kAboutComponentStb, strings::kAboutLicenseMitOrPublicDomain},
    {strings::kAboutComponentQoi, strings::kAboutLicenseMit},
    {strings::kAboutComponentManrope, strings::kAboutLicenseOfl},
    {strings::kAboutComponentIcons, strings::kAboutLicenseIscMit},
};

// ABOUT.md and THIRD-PARTY-NOTICES.md, rendered with just enough Markdown
// awareness to look like prose rather than like a file someone forgot to
// format: headings and bullets, nothing else. A real Markdown renderer
// would be a project of its own (ImGui has none built in), and the
// alternative - dumping the raw text with its '#' and '-' prefixes intact -
// looks like a bug. Whoever edits either file gets a live preview by
// opening this tab, so the limits are self-evident rather than needing to
// be documented somewhere they'd be missed.
//
// Rendered by *paragraph*, not by line. Both files are hard-wrapped at
// around 76 columns like any readable source file, so drawing a line at a
// time would reproduce those breaks verbatim - a narrow column stranded in
// a wide panel, re-wrapped at the wrong width whenever the panel is a
// different size than the author's editor was. Consecutive lines are
// joined back into one string and handed to ImGui to wrap, which is also
// what makes a bullet's second line line up under its first: it's one
// wrapped item rather than two unrelated ones.
void RenderMarkdownSubset(std::string_view document) {
    constexpr float kBulletIndent = 16.0f;
    std::string paragraph;
    bool paragraphIsBullet = false;
    const auto flush = [&]() {
        if (paragraph.empty()) {
            return;
        }
        if (paragraphIsBullet) {
            ImGui::Indent(Px(kBulletIndent));
            ImGui::Bullet();
            // Bullet() already ends flush against whatever follows it, so
            // this gap is the whole separation between the dot and the
            // word - 4px read as the two touching.
            ImGui::SameLine(0.0f, Px(10.0f));
            ImGui::TextUnformatted(paragraph.data(), paragraph.data() + paragraph.size());
            ImGui::Unindent(Px(kBulletIndent));
        } else {
            ImGui::TextUnformatted(paragraph.data(), paragraph.data() + paragraph.size());
        }
        paragraph.clear();
        paragraphIsBullet = false;
    };

    size_t pos = 0;
    while (pos <= document.size()) {
        const size_t eol = document.find('\n', pos);
        std::string_view line = document.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? document.size() + 1 : eol + 1;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        std::string_view trimmed = line;
        while (!trimmed.empty() && trimmed.front() == ' ') {
            trimmed.remove_prefix(1);
        }

        if (trimmed.empty()) {
            // A blank line is Markdown's paragraph break, and the only
            // thing that ends one here.
            flush();
            ImGui::Spacing();
            continue;
        }
        size_t hashes = 0;
        while (hashes < trimmed.size() && trimmed[hashes] == '#') {
            ++hashes;
        }
        if (hashes > 0 && hashes < trimmed.size() && trimmed[hashes] == ' ') {
            flush();
            const std::string heading(trimmed.substr(hashes + 1));
            ImGui::Spacing();
            // Only the top level gets larger type; deeper ones stay at body
            // size and lean on color alone, so a document with four levels
            // doesn't turn into four competing sizes.
            if (hashes == 1) {
                ImGui::PushFont(nullptr, 22.0f);
            }
            ImGui::TextColored(theme::Accent(), "%s", heading.c_str());
            if (hashes == 1) {
                ImGui::PopFont();
            }
            ImGui::Spacing();
            continue;
        }
        if (trimmed.size() > 2 && trimmed[0] == '-' && trimmed[1] == ' ') {
            flush();
            paragraphIsBullet = true;
            paragraph.assign(trimmed.substr(2));
            continue;
        }
        // Anything else continues whatever is being built - a bullet's
        // indented second line, or the next line of a paragraph.
        if (!paragraph.empty()) {
            paragraph += ' ';
        }
        paragraph.append(trimmed);
    }
    flush();
}
}  // namespace

void OverviewPanel::SwitchOverviewTab(OverviewTab tab) {
    if (overviewTab_ == tab) {
        return;
    }
    overviewTab_ = tab;
    overviewBodyScrollToTop_ = true;
    // A Settings row waiting for its key goes out of sight with the tab,
    // and stops waiting: left to wait, Escape pressed to close the Overview
    // went to it and unbound a shortcut, and a letter bound that letter.
    if (editor_.Input().As<KeyCapture>(Level::Text) != nullptr) {
        editor_.Input().End(Level::Text);
    }
    // Leaving About also leaves its license page: coming back to a tab
    // that is still showing somebody else's MIT text, several tabs later,
    // is not a place anyone meant to return to.
    aboutShowsNotices_ = false;
}

void OverviewPanel::RenderOverviewAboutPanel() {
    // 0.0f means "wrap at the window's own right edge", which is what this
    // wants. PushTextWrapPos takes a window-local X *position*, not a
    // width, so passing GetContentRegionAvail().x - the obvious-looking
    // thing, and what the Settings panel happens to get away with - wraps
    // at roughly half the width once the panel is wide.
    ImGui::PushTextWrapPos(0.0f);

    if (aboutShowsNotices_) {
        // The licenses, in the same panel and the same scroll region as
        // the About text rather than in a popup: this is a page you read,
        // not a thing you act on, and the Overview has enough windows
        // stacked over it already (see OverlayApp::StackSurfaces). The way back
        // is in the panel's footer, which does not scroll.
        RenderMarkdownSubset(build::NoticesText());
        ImGui::PopTextWrapPos();
        return;
    }

    // Which build this is, first and plainly - it's the one thing someone
    // opens this tab to find, and the reason the tab is worth having at all
    // when a tester needs to say which binary they were using.
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kAboutVersion);
    ImGui::SameLine(Px(90.0f));
    const std::string version = build::VersionLine();
    ImGui::TextUnformatted(version.c_str());
    // Whose it is, right under what it is. This one stays in plain sight
    // rather than behind the button below: it is the app saying who owns
    // it, which is a different job from reproducing other people''s terms.
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kAboutCopyrightLabel);
    ImGui::SameLine(Px(90.0f));
    ImGui::TextUnformatted(strings::kAboutCopyright);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    RenderMarkdownSubset(build::AboutText());

    // What is inside this binary that somebody else wrote. The list is
    // short enough to read at a glance and is the part most people want;
    // the licenses themselves are long enough that they would bury the
    // rest of this tab, so they are one click away.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushFont(nullptr, 22.0f);
    ImGui::TextColored(theme::Accent(), "%s", strings::kAboutBuiltWith);
    ImGui::PopFont();
    ImGui::Spacing();
    for (const BuiltWithRow& row : kBuiltWith) {
        ImGui::TextUnformatted(row.component);
        ImGui::SameLine(Px(260.0f));
        ImGui::TextColored(theme::kGraphite300, "%s", row.license);
    }

    ImGui::PopTextWrapPos();
}

void OverviewPanel::BeginRenaming(std::optional<FolderId> folder, std::optional<CanvasId> canvas,
                               const std::string& name) {
    renamingFolderId_ = folder;
    renamingCanvasId_ = canvas;
    std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s", name.c_str());
    renameJustFocused_ = true;
    if (host_.Window()) {
        host_.Window()->RequestTextInput();
    }
    // Every key is the field's until it lets go - see NameEdit.
    editor_.Input().Push(std::make_unique<NameEdit>(
                             [this] { return renamingFolderId_.has_value() || renamingCanvasId_.has_value(); },
                             [this] {
                                 // A rename field gives the keyboard back when ImGui
                                 // deactivates it (see the InputText sites in
                                 // RenderFolderSidebar and RenderCanvasGrid). Stopped from outside - the Overview
                                 // closed between frames - that frame never comes, and the
                                 // keyboard stayed borrowed from the game for the rest of
                                 // the session. ReleaseTextInput is idempotent.
                                 const bool wasRenaming = renamingFolderId_.has_value() || renamingCanvasId_.has_value();
                                 renamingFolderId_.reset();
                                 renamingCanvasId_.reset();
                                 if (wasRenaming && host_.Window()) {
                                     host_.Window()->ReleaseTextInput();
                                 }
                             }),
                         Event{});
}

}  // namespace sz::ui
