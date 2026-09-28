#include "ui/view/tutorial_world.h"

#include "ui/interaction/levels.h"
#include "ui/widgets.h"

namespace sz::ui {

tutorial::Cover AppWorld::CanvasCover() const {
    const Machine& input = editor_.Input();
    if (const Panel* panel = input.As<Panel>(Level::Panel)) {
        return panel->Kind() == PanelKind::Overview ? tutorial::Cover::Overview : tutorial::Cover::CheatSheet;
    }
    return input.At(Level::Popup) != nullptr ? tutorial::Cover::Popup : tutorial::Cover::None;
}

std::optional<core::ItemCreationKind> AppWorld::CreationToolInHand() const {
    return core::CreationKindFor(editor_.ActiveTool());
}

core::FolderId AppWorld::FolderOf(core::CanvasId canvas) const {
    const core::CanvasManager& manager = session_.Manager();
    const core::Canvas* found = manager.FindCanvas(canvas);
    // A deleted canvas, or one in a deleted folder, is in none.
    return found != nullptr && !manager.IsDeleted(*found) ? found->folderId : 0;
}

std::string AppWorld::CanvasName(core::CanvasId canvas) const {
    const core::Canvas* found = session_.Manager().FindCanvas(canvas);
    return found != nullptr ? found->name : std::string();
}

std::vector<tutorial::SnippetFacts> AppWorld::SnippetsIn(core::FolderId folder) const {
    const core::CanvasManager& manager = session_.Manager();
    std::vector<tutorial::SnippetFacts> snippets;
    const core::Folder* found = manager.FindFolder(folder);
    if (found == nullptr || manager.IsDeleted(*found)) {
        return snippets;
    }
    for (const core::Canvas& canvas : manager.Canvases()) {
        if (canvas.folderId != folder || manager.IsDeleted(canvas)) {
            continue;
        }
        for (const core::Item& item : canvas.items) {
            tutorial::SnippetFacts facts;
            facts.id = item.id;
            facts.canvas = canvas.id;
            facts.rect = item.rect;
            facts.picture = item.hasBackground;
            facts.fullscreen = item.isFullscreen;
            facts.minimized = item.minimized;
            facts.deleted = manager.IsDeleted(canvas, item);
            facts.strokes = item.strokes.size();
            facts.pinned = item.pinned;
            facts.pictureOpacity = item.picture.opacity;
            facts.drawingOpacity = item.foregroundOpacity;
            snippets.push_back(facts);
        }
    }
    return snippets;
}

std::optional<std::string> AppWorld::KeyLabel(CommandId command) const {
    // The first of its keys, as the cheat sheet names it.
    const std::vector<platform::KeyCombo> keys = KeysFor(command, settings_.Stored(), settings_.Live().shortcuts);
    return keys.empty() ? std::nullopt : std::optional<std::string>(FormatKeyComboLabel(keys.front()));
}

}  // namespace sz::ui
