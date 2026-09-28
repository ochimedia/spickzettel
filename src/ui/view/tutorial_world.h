#pragma once

// The tutorial's world as the running app answers it - docs/TUTORIAL.md,
// section 7.1: from the editor, the session and the settings, read as they
// are whenever it is asked, and every answer a value. The one thing it
// keeps is how many times the overlay has come up, and has entered the
// pinned view and view mode, which nothing else counts.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/session/session.h"
#include "core/session/settings.h"
#include "ui/editor.h"
#include "ui/tutorial/world.h"

namespace sz::ui {

class AppWorld final : public tutorial::World {
public:
    AppWorld(const core::Session& session, const core::Settings& settings, const Editor& editor)
        : session_(session), settings_(settings), editor_(editor) {}

    // The overlay has come up - see OverlayApp::OnOverlayShown - and a
    // transition has entered the pinned view, or view mode (see
    // OverlayApp::OnModeEntered).
    void CountShowing() { ++showings_; }
    void CountPinnedView() { ++pinnedViews_; }
    void CountViewMode() { ++viewModes_; }

    uint64_t Showings() const override { return showings_; }
    uint64_t PinnedViews() const override { return pinnedViews_; }
    uint64_t ViewModes() const override { return viewModes_; }
    tutorial::Cover CanvasCover() const override;
    std::optional<core::ItemId> DrawingItem() const override { return editor_.DrawingItem(); }
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
    uint64_t showings_ = 0;
    uint64_t pinnedViews_ = 0;
    uint64_t viewModes_ = 0;
};

}  // namespace sz::ui
