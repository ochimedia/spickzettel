#pragma once

// The tutorial's world as the running app answers it - docs/TUTORIAL.md,
// section 7.1: from the editor, the session, the settings and the Overview, read as they
// are whenever it is asked, and every answer a value. The one thing it
// keeps is how many times the overlay has come up, has entered the pinned
// view and view mode, and each capture hotkey has taken a screenshot,
// which nothing else counts.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/session/session.h"
#include "core/session/settings.h"
#include "ui/editor.h"
#include "ui/view/overview_panel.h"
#include "ui/view/settings_page.h"
#include "ui/tutorial/world.h"

namespace sz::ui {

class AppWorld final : public tutorial::World {
public:
    // `overview` and `settingsPage` are only read, once the app is
    // running - they may be constructed after this.
    AppWorld(const core::Session& session, const core::Settings& settings, const Editor& editor,
             const OverviewPanel& overview, const SettingsPage& settingsPage)
        : session_(session), settings_(settings), editor_(editor), overview_(overview), settingsPage_(settingsPage) {}

    // The overlay has come up - see OverlayApp::OnOverlayShown - and a
    // transition has entered the pinned view, or view mode (see
    // OverlayApp::OnModeEntered).
    void CountShowing() { ++showings_; }
    void CountPinnedView() { ++pinnedViews_; }
    void CountViewMode() { ++viewModes_; }
    // A capture hotkey has taken a screenshot (see OverlayApp::QuickCapture).
    void CountCapture(core::HotkeySlot by) { ++captures_[static_cast<size_t>(by)]; }
    // See OverlayApp::SetHotkeyRegisteredQuery.
    void SetHotkeyRegisteredQuery(std::function<bool(core::HotkeySlot)> query) { registered_ = std::move(query); }

    uint64_t Showings() const override { return showings_; }
    uint64_t PinnedViews() const override { return pinnedViews_; }
    uint64_t ViewModes() const override { return viewModes_; }
    uint64_t Captures(core::HotkeySlot by) const override { return captures_[static_cast<size_t>(by)]; }
    tutorial::Cover CanvasCover() const override;
    std::vector<core::ItemId> DrawingItems() const override { return editor_.DrawingItems(); }
    std::vector<core::ItemId> Selection() const override { return editor_.Selection(); }
    std::optional<core::ItemCreationKind> CreationToolInHand() const override;
    core::Tool ToolInHand() const override { return editor_.ActiveTool(); }
    core::DrawShape EraserShape() const override { return editor_.EraserShape(); }
    uint32_t PenColor() const override { return editor_.DrawColorRGBA(); }
    float PenWidth() const override { return editor_.DrawWidth(); }
    std::optional<core::ItemId> NoteBeingTyped() const override { return editor_.EditingNote(); }
    core::CanvasId CurrentCanvas() const override { return session_.Manager().CurrentCanvasId(); }
    core::FolderId FolderOf(core::CanvasId canvas) const override;
    std::string CanvasName(core::CanvasId canvas) const override;
    std::vector<tutorial::SnippetFacts> SnippetsIn(core::FolderId folder) const override;
    std::vector<tutorial::FolderFacts> Folders() const override;
    std::vector<tutorial::CanvasFacts> CanvasesIn(core::FolderId folder) const override;
    bool OverviewShowsCanvases() const override { return overview_.ShowsCanvases(); }
    bool OverviewShowsDeleted() const override { return overview_.IsOpen() && overview_.ShowingDeleted(); }
    bool CanvasBarOn() const override { return settings_.Stored().showCanvasBar; }
    std::string Underneath() const override;
    std::vector<tutorial::ProfileFacts> Profiles() const override;
    bool OverviewShowsSettings() const override { return overview_.OnSettingsTab(); }
    tutorial::SettingsSection SettingsSectionShown() const override;
    std::optional<std::string> SettingsShowing() const override;
    std::optional<std::string> KeyLabel(CommandId command) const override;
    core::CreationTrigger ScreenshotTrigger() const override { return settings_.Stored().screenshotTrigger; }
    core::CreationTrigger DrawingTrigger() const override { return settings_.Stored().drawingTrigger; }
    std::string TopicProgress(std::string_view topic) const override {
        const auto& progress = settings_.Stored().tutorialProgress;
        const auto it = progress.find(std::string(topic));
        return it == progress.end() ? std::string() : it->second;
    }

private:
    const core::Session& session_;
    const core::Settings& settings_;
    const Editor& editor_;
    const OverviewPanel& overview_;
    const SettingsPage& settingsPage_;
    uint64_t showings_ = 0;
    uint64_t pinnedViews_ = 0;
    uint64_t viewModes_ = 0;
    std::array<uint64_t, std::size(core::kAllHotkeySlots)> captures_{};
    std::function<bool(core::HotkeySlot)> registered_;
};

}  // namespace sz::ui
