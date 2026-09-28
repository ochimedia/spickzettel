#pragma once

// A world for the tutorial's tests (see ui/tutorial/world.h): every answer
// a field the test sets, and snippets a test makes, moves and deletes by
// hand - the app's side of docs/TUTORIAL.md, section 7.1, with no app.

#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "ui/tutorial/world.h"

namespace sz::ui::tutorial {

struct FakeWorld : World {
    // Folder 1 is the tutorial's, with canvases 11 and 12; folder 2 is
    // the user's own, with canvas 21.
    static constexpr core::FolderId kTutorialFolder = 1;
    static constexpr core::FolderId kOtherFolder = 2;
    static constexpr core::CanvasId kCanvas = 11;
    static constexpr core::CanvasId kSecondCanvas = 12;
    static constexpr core::CanvasId kOtherCanvas = 21;

    uint64_t showings = 1;
    Cover cover = Cover::None;
    std::optional<core::ItemId> drawing;
    std::vector<core::ItemId> selection;
    std::optional<core::ItemCreationKind> tool;
    core::CanvasId current = kCanvas;
    std::unordered_map<core::CanvasId, core::FolderId> folderOf{
        {kCanvas, kTutorialFolder}, {kSecondCanvas, kTutorialFolder}, {kOtherCanvas, kOtherFolder}};
    std::vector<SnippetFacts> snippets;
    std::unordered_map<CommandId, std::string> keys{{CommandId::Undo, "Ctrl+Z"},
                                                    {CommandId::CheatSheet, "Ctrl+H"},
                                                    {CommandId::ToggleEditMode, "Ctrl+Alt+S"},
                                                    {CommandId::NewScreenshotTool, "S"}};
    core::CreationTrigger screenshotTrigger = core::CreationTrigger::Plain;
    core::CreationTrigger drawingTrigger = core::CreationTrigger::Ctrl;

    // A snippet made on the current canvas: a screenshot, unless said.
    SnippetFacts& Make(core::ItemId id, bool picture = true) {
        SnippetFacts snippet;
        snippet.id = id;
        snippet.canvas = current;
        snippet.rect = core::Rect{100.0f, 100.0f, 200.0f, 150.0f};
        snippet.picture = picture;
        snippets.push_back(snippet);
        return snippets.back();
    }
    SnippetFacts& At(core::ItemId id) {
        for (SnippetFacts& snippet : snippets) {
            if (snippet.id == id) {
                return snippet;
            }
        }
        throw std::out_of_range("no such snippet");
    }

    uint64_t Showings() const override { return showings; }
    Cover CanvasCover() const override { return cover; }
    std::optional<core::ItemId> DrawingItem() const override { return drawing; }
    std::vector<core::ItemId> Selection() const override { return selection; }
    std::optional<core::ItemCreationKind> CreationToolInHand() const override { return tool; }
    core::CanvasId CurrentCanvas() const override { return current; }
    core::FolderId FolderOf(core::CanvasId canvas) const override {
        const auto it = folderOf.find(canvas);
        return it == folderOf.end() ? 0 : it->second;
    }
    std::string CanvasName(core::CanvasId canvas) const override { return "Canvas " + std::to_string(canvas); }
    std::vector<SnippetFacts> SnippetsIn(core::FolderId folder) const override {
        std::vector<SnippetFacts> in;
        for (const SnippetFacts& snippet : snippets) {
            if (FolderOf(snippet.canvas) == folder) {
                in.push_back(snippet);
            }
        }
        return in;
    }
    std::optional<std::string> KeyLabel(CommandId command) const override {
        const auto it = keys.find(command);
        return it == keys.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    core::CreationTrigger ScreenshotTrigger() const override { return screenshotTrigger; }
    core::CreationTrigger DrawingTrigger() const override { return drawingTrigger; }
};

}  // namespace sz::ui::tutorial
