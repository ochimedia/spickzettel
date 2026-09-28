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
    uint64_t pinnedViews = 0;
    uint64_t viewModes = 0;
    uint64_t quickCaptures = 0;
    uint64_t silentCaptures = 0;
    Cover cover = Cover::None;
    std::optional<core::ItemId> drawing;
    std::vector<core::ItemId> selection;
    std::optional<core::ItemCreationKind> tool;
    core::Tool hand = core::Tool::Select;
    core::DrawShape eraserShape = core::DrawShape::Freehand;
    uint32_t penColor = 0xFF0000FFu;
    float penWidth = 3.0f;
    std::optional<core::ItemId> typing;
    core::CanvasId current = kCanvas;
    std::unordered_map<core::CanvasId, core::FolderId> folderOf{
        {kCanvas, kTutorialFolder}, {kSecondCanvas, kTutorialFolder}, {kOtherCanvas, kOtherFolder}};
    std::vector<SnippetFacts> snippets;
    std::unordered_map<CommandId, std::string> keys{{CommandId::Undo, "Ctrl+Z"},
                                                    {CommandId::CheatSheet, "Ctrl+H"},
                                                    {CommandId::ToggleEditMode, "Ctrl+Alt+S"},
                                                    {CommandId::ToggleViewMode, "Ctrl+Alt+V"},
                                                    {CommandId::NewScreenshotTool, "S"},
                                                    {CommandId::NewDrawingTool, "D"},
                                                    {CommandId::DeleteSelection, "Delete"},
                                                    {CommandId::QuickCapture, "Ctrl+Alt+C"},
                                                    {CommandId::SilentCapture, "Ctrl+Alt+X"}};
    core::CreationTrigger screenshotTrigger = core::CreationTrigger::Plain;
    core::CreationTrigger drawingTrigger = core::CreationTrigger::Ctrl;
    std::unordered_map<std::string, std::string> progress;

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
    // A stroke on snippet `id`, drawn with the pen as it is now: freehand
    // unless said, 100 px long.
    StrokeFacts& Draw(core::ItemId id, core::DrawShape shape = core::DrawShape::Freehand, float lengthPx = 100.0f) {
        StrokeFacts stroke;
        stroke.colorRGBA = penColor;
        stroke.widthPx = penWidth;
        stroke.lengthPx = lengthPx;
        stroke.shape = shape;
        At(id).strokes.push_back(stroke);
        return At(id).strokes.back();
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
    uint64_t PinnedViews() const override { return pinnedViews; }
    uint64_t ViewModes() const override { return viewModes; }
    uint64_t Captures(core::HotkeySlot by) const override {
        return by == core::HotkeySlot::QuickCapture ? quickCaptures
               : by == core::HotkeySlot::SilentCapture ? silentCaptures
                                                        : 0;
    }
    Cover CanvasCover() const override { return cover; }
    std::optional<core::ItemId> DrawingItem() const override { return drawing; }
    std::vector<core::ItemId> Selection() const override { return selection; }
    std::optional<core::ItemCreationKind> CreationToolInHand() const override { return tool; }
    core::Tool ToolInHand() const override { return hand; }
    core::DrawShape EraserShape() const override { return eraserShape; }
    uint32_t PenColor() const override { return penColor; }
    float PenWidth() const override { return penWidth; }
    std::optional<core::ItemId> NoteBeingTyped() const override { return typing; }
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
    std::string TopicProgress(std::string_view topic) const override {
        const auto it = progress.find(std::string(topic));
        return it == progress.end() ? std::string() : it->second;
    }
};

}  // namespace sz::ui::tutorial
