#pragma once

// The Overview - docs/VIEW_LAYER.md, section 7: every canvas and folder,
// switched to, made, renamed, reordered, deleted and restored; the picker
// that sends a snippet to another canvas; the About tab; and the Settings
// tab's frame, whose body is the Settings page's. A panel over a dimmed
// canvas, on the machine's Panel level. What it keeps of its own is which
// tab shows, the picker, Show deleted and the deleted folder shown, the
// name being edited, and the scroll requests.

#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "core/session/session.h"
#include "core/session/settings.h"
#include "ui/editor.h"
#include "ui/view/view_host.h"

namespace sz::ui {

class OverviewPanel {
public:
    OverviewPanel(core::Session& session, core::Settings& settings, Editor& editor, ViewHost& host);

    // Whether the Overview is up - the machine's Panel level holds it.
    bool IsOpen() const;
    // Up, on the Canvases tab, with Show deleted off and nothing being
    // picked - the switcher's own primary purpose is always what greets you.
    void Open();
    // Up, on the Settings tab.
    void OpenSettings();
    // Up as the picker: a tile picked is where `item` goes, moved or copied.
    void OpenPicker(core::ItemId item, bool isCopy);
    // Put away: the picker, a name being edited or a key being captured -
    // a row left armed would sit silently waiting, capturing whatever key
    // is pressed next time Settings reopens - and the panel, unless the
    // machine has ended it already.
    void Close();

    // The Overview, in the order it is drawn (Escape is its interaction's -
    // see Panel): the dimming backdrop, which closes the Overview on a click
    // outside the panel; the header, which is the picker's prompt while a
    // snippet is being sent somewhere and the tabs otherwise; the body - the
    // folder sidebar and the canvas grid (with what is deleted in them, see
    // ShowingDeleted), or the Settings tab's (`settingsBody`) or About; and
    // the footer, whose buttons belong to whichever body is showing.
    void Draw(float displayW, float displayH, const std::function<void()>& settingsBody);

    // ===== What the actions do to the Overview's own state (see Apply) =====

    // Whether a snippet is being sent somewhere.
    bool Picking() const { return pickerItemId_.has_value(); }
    // The picker's one outcome: the snippet it was opened for goes to
    // `target` - moved or copied, as the picker was opened - and the
    // picker closes, leaving the Overview open. Nothing moves when the
    // target is the canvas the snippet is already on.
    void SendPickedItemTo(core::CanvasId target);
    // A deleted folder picked in the sidebar, whose canvases the grid shows
    // - and let go of, for a live folder browsed instead.
    void ShowDeletedFolder(core::FolderId folder) { deletedFolderShown_ = folder; }
    void ForgetDeletedFolderShown() { deletedFolderShown_.reset(); }
    // Lets go of the deleted folder shown once it is no longer something to
    // show: Show deleted is off, the folder is gone, or it has been
    // restored - in which case it is browsed as any live folder is.
    void SettleDeletedFolderShown();
    // A folder or a canvas just made, brought into view the next time the
    // sidebar or the grid is drawn: a new one goes to the end of its list
    // (see CanvasManager::AddFolder and AddCanvas), which may be below the
    // fold, with nothing to say it worked.
    void ScrollToFolder(core::FolderId folder) { overviewScrollToFolderId_ = folder; }
    void ScrollToCanvas(core::CanvasId canvas) { overviewScrollToCanvasId_ = canvas; }

private:
    const core::CanvasManager& Manager() const { return session_.Manager(); }
    const core::AppConfig& Cfg() const { return settings_.Stored(); }

    void RenderOverviewHeader();
    // On the tab row, before Show deleted while it is on: which color is
    // which - red for a folder deleted whole, yellow for one holding a
    // deleted canvas - unless `withLegend` is false for want of room, then
    // Empty trash. `gap` is what goes between the two.
    void RenderDeletedControls(bool withLegend, float gap);
    // How wide the legend is.
    float DeletedLegendWidth() const;
    // The sidebar and the grid, which ask for what they are clicked for as
    // actions (see ViewHost::Act).
    void RenderFolderSidebar();
    void RenderCanvasGrid(float displayW, float displayH, const ViewHost::PreviewDrawing& previews);
    void RenderOverviewFooter(bool showCanvasesBody);
    // The third tab: which build this is (build::VersionLine) plus ABOUT.md,
    // compiled in so it travels with the binary rather than living next to
    // it as a file that can go missing - see build::AboutText.
    void RenderOverviewAboutPanel();

public:
    // Whether it is up on its Settings tab.
    bool OnSettingsTab() const { return IsOpen() && overviewTab_ == OverviewTab::Settings; }
    // Whether it is up showing the canvases: on its Canvases tab, or as the
    // picker, which shows them whatever the tab.
    bool ShowsCanvases() const {
        return IsOpen() && (pickerItemId_.has_value() || overviewTab_ == OverviewTab::Canvases);
    }
    // Whether the Canvases tab shows what is deleted - showDeleted_, never
    // while picking where a snippet goes, which is a place among the live
    // ones.
    bool ShowingDeleted() const { return showDeleted_ && !pickerItemId_.has_value(); }
    // The folder the sidebar marks as open and the grid shows: the one
    // being browsed, or a deleted one picked with Show deleted on.
    core::FolderId OverviewFolderId() const;

private:
    // The sidebar's width: wider with Show deleted on, where a row can
    // carry two buttons rather than one.
    float OverviewSidebarWidth() const;
    // Starts renaming a folder or a canvas, from `name` - and puts a
    // NameEdit on the machine's Text level for as long as it lasts.
    void BeginRenaming(std::optional<core::FolderId> folder, std::optional<core::CanvasId> canvas,
                       const std::string& name);

    core::Session& session_;
    core::Settings& settings_;
    Editor& editor_;
    ViewHost& host_;

    // Which of the three tabs is showing - Canvases (the folder sidebar and
    // the canvas tile grid), Settings or About. Not persisted, and reset to
    // Canvases every time the Overview opens (see Open). Never shown at all
    // while a snippet is being sent somewhere (see Draw) - picking a
    // destination has its own single-purpose header in its place, and
    // Settings has no business being reachable mid-pick.
    enum class OverviewTab { Canvases, Settings, About };
    OverviewTab overviewTab_ = OverviewTab::Canvases;
    // Switching tabs, plus the housekeeping that goes with it: the new tab
    // starts at the top of its own content, and About starts on About
    // rather than wherever its second page was left. One function so a
    // fourth tab, if there is ever one, cannot forget either.
    void SwitchOverviewTab(OverviewTab tab);
    // Whether the About tab is showing the third-party licenses instead of
    // its usual contents. A second page of the same tab rather than a tab
    // of its own: the licenses have to be *reachable*, not prominent, and a
    // permanent fourth entry in the tab row would charge every visit to
    // Canvases and Settings for something read once, if ever. Not persisted
    // - a fresh About always opens on About.
    bool aboutShowsNotices_ = false;
    // Whether the Canvases tab shows what is deleted alongside what is not:
    // deleted folders in the sidebar and deleted canvases in the grid,
    // marked out in red with Restore and Delete permanently on each - a
    // folder holding a deleted canvas in yellow - and everything else
    // dimmed. Not persisted, and off whenever the Overview
    // opens. See ShowingDeleted.
    bool showDeleted_ = false;
    // A deleted folder picked in the sidebar while Show deleted is on, whose
    // canvases the grid shows. Kept here rather than as the browsed folder:
    // that is where a new canvas lands, and the manager never lets it be a
    // deleted one (see CanvasManager::SettleOffDeleted). See
    // SettleDeletedFolderShown for when it lets go.
    std::optional<core::FolderId> deletedFolderShown_;
    // Set by anything that changes what the body is showing; consumed by
    // the body itself on its next frame. All three tabs and both About
    // pages share one scrolling child, so a page arrived at from halfway
    // down another one would otherwise open halfway down - and the switch
    // is asked for from outside that child (a tab button above it, a
    // footer button below it), where SetScrollY would scroll the panel
    // instead. See SwitchOverviewTab.
    bool overviewBodyScrollToTop_ = false;
    // Set only when the Overview was opened as the picker, from a snippet's
    // Move to canvas - picking a tile then moves or copies this snippet
    // there instead of just switching to it.
    std::optional<core::ItemId> pickerItemId_;
    bool pickerIsCopy_ = false;

    // In-place rename of a folder row or canvas tile name, triggered by a
    // double-click on it. At most one of the two ids is ever set;
    // `renameBuffer_` holds the in-progress edit for whichever one it is.
    // `renameJustFocused_` is consumed the first frame after a rename
    // starts, to call ImGui::SetKeyboardFocusHere() exactly once.
    std::optional<core::FolderId> renamingFolderId_;
    std::optional<core::CanvasId> renamingCanvasId_;
    char renameBuffer_[128] = {};
    bool renameJustFocused_ = false;
    // A canvas or a folder to bring into view the next time the grid or
    // the sidebar is drawn, then forget - see ScrollToCanvas. Consumed by
    // the loop whether or not it is found there.
    std::optional<core::CanvasId> overviewScrollToCanvasId_;
    std::optional<core::FolderId> overviewScrollToFolderId_;
};

// The Show deleted tooltip's lines, public for the tests.
//
// "Deleted today, 14:05 - 32 min ago": the day in words while that is
// shorter than a date, and how long ago while that is the quicker thing to
// read - which is what "I deleted something half an hour ago" is looking
// for. "Deleted at an unknown time" for a stamp with no date.
std::string DeletedWhen(int64_t deletedAt, std::time_t now);
// "Deleted permanently from <date> on": when the retention period, `days`
// long, takes something deleted at `deletedAt` - the first start from that
// day on (see TrayController::Initialize) - or that this date is unknown.
std::string GoesOn(int64_t deletedAt, int days);

}  // namespace sz::ui
