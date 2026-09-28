#include "ui/view/tutorial_world.h"

#include <cmath>

#include "core/canvas/item_geometry.h"
#include "ui/interaction/levels.h"
#include "ui/widgets.h"

namespace sz::ui {

tutorial::Cover AppWorld::CanvasCover() const {
    const Machine& input = editor_.Input();
    if (const Panel* panel = input.As<Panel>(Level::Panel)) {
        return panel->Kind() == PanelKind::Overview ? tutorial::Cover::Overview : tutorial::Cover::CheatSheet;
    }
    // A snippet's own popups cover nothing (see tutorial::Cover).
    const Popup* popup = input.As<Popup>(Level::Popup);
    if (popup == nullptr || popup->Kind() == PopupKind::ColorChooser || popup->Kind() == PopupKind::ItemProperties) {
        return tutorial::Cover::None;
    }
    return tutorial::Cover::Popup;
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

namespace {

// A snippet's strokes as they look on screen: each carried out of the
// snippet's own space by its size, and a shape known by how the pen's
// shapes are stored - a line two points, a rectangle five that close an
// upright outline (see session_shapes.cpp).
std::vector<tutorial::StrokeFacts> StrokesOf(const core::Item& item) {
    const float scaleX = item.nativeW > 0.0f ? item.rect.w / item.nativeW : 1.0f;
    const float scaleY = item.nativeH > 0.0f ? item.rect.h / item.nativeH : 1.0f;
    std::vector<tutorial::StrokeFacts> strokes;
    strokes.reserve(item.strokes.size());
    for (const core::Stroke& stroke : item.strokes) {
        tutorial::StrokeFacts facts;
        facts.colorRGBA = stroke.colorRGBA;
        facts.widthPx = stroke.width * core::LengthScale(scaleX, scaleY);
        const std::vector<core::StrokePoint>& p = stroke.points;
        for (size_t i = 1; i < p.size(); ++i) {
            facts.lengthPx += std::hypot((p[i].x - p[i - 1].x) * scaleX, (p[i].y - p[i - 1].y) * scaleY);
        }
        if (p.size() == 2) {
            facts.shape = core::DrawShape::Line;
        } else if (p.size() == 5 && p[0] == p[4] && p[0].y == p[1].y && p[1].x == p[2].x && p[2].y == p[3].y &&
                   p[3].x == p[0].x) {
            facts.shape = core::DrawShape::Rectangle;
        }
        strokes.push_back(facts);
    }
    return strokes;
}

}  // namespace

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
            facts.strokes = StrokesOf(item);
            facts.note = item.noteText;
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
