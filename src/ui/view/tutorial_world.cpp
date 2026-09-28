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

std::vector<tutorial::FolderFacts> AppWorld::Folders() const {
    const core::CanvasManager& manager = session_.Manager();
    std::vector<tutorial::FolderFacts> folders;
    for (const core::Folder& folder : manager.Folders()) {
        folders.push_back(tutorial::FolderFacts{folder.id, folder.name, manager.IsDeleted(folder)});
    }
    return folders;
}

std::vector<tutorial::CanvasFacts> AppWorld::CanvasesIn(core::FolderId folder) const {
    const core::CanvasManager& manager = session_.Manager();
    std::vector<tutorial::CanvasFacts> canvases;
    for (const core::Canvas& canvas : manager.Canvases()) {
        if (canvas.folderId == folder) {
            canvases.push_back(tutorial::CanvasFacts{canvas.id, folder, canvas.name, manager.IsDeleted(canvas)});
        }
    }
    return canvases;
}

std::string AppWorld::Underneath() const {
    const platform::ForegroundApp& app = settings_.UnderlyingApplication();
    return !app.executable.empty() ? app.executable : app.title;
}

std::vector<tutorial::ProfileFacts> AppWorld::Profiles() const {
    const std::vector<core::Profile>& profiles = settings_.Profiles();
    const core::ProfileableSettings& base = settings_.Base();
    std::vector<tutorial::ProfileFacts> facts;
    for (size_t i = 0; i < profiles.size(); ++i) {
        const core::Profile& profile = profiles[i];
        const core::ProfileMatch& match = profile.match;
        tutorial::ProfileFacts each;
        each.name = profile.name;
        each.program = !match.executables.empty()    ? match.executables.front()
                       : !match.titleContains.empty() ? match.titleContains.front()
                                                      : std::string();
        each.matchesUnderneath = match.Matches(settings_.UnderlyingApplication());
        each.running = settings_.ActiveProfile() == i;
        each.stated = profile.overrides.OverriddenCount(core::ProfileGroup::Behavior);
        // A row stated all the same as the defaults: set back by hand
        // rather than handed back (docs/TUTORIAL.md, section 18.2).
        const auto asDefaults = [&](const auto& row) {
            const auto& stated = profile.overrides.*row.override;
            return stated.has_value() && *stated == base.*row.value ? size_t{1} : size_t{0};
        };
        namespace setting = core::setting;
        each.statedAsDefaults = asDefaults(setting::kDontStealFocus) + asDefaults(setting::kTakeFocusOverElevated) +
                                asDefaults(setting::kSoftwarePointer) + asDefaults(setting::kRawMouseInput) +
                                asDefaults(setting::kDontForwardKeystrokes) +
                                asDefaults(setting::kCounterRawMouseInput) + asDefaults(setting::kCounterThreshold) +
                                asDefaults(setting::kFreezeScreen);
        facts.push_back(std::move(each));
    }
    return facts;
}

tutorial::SettingsSection AppWorld::SettingsSectionShown() const {
    switch (settingsPage_.Section()) {
        case SettingsPage::SettingsSection::Behavior:
            return tutorial::SettingsSection::Behavior;
        case SettingsPage::SettingsSection::Profiles:
            return tutorial::SettingsSection::Profiles;
        case SettingsPage::SettingsSection::Appearance:
        case SettingsPage::SettingsSection::Interaction:
        case SettingsPage::SettingsSection::Defaults:
        case SettingsPage::SettingsSection::Hotkeys:
        case SettingsPage::SettingsSection::Debug:
            break;
    }
    return tutorial::SettingsSection::Other;
}

std::optional<std::string> AppWorld::SettingsShowing() const {
    const std::optional<size_t> showing = settingsPage_.Showing();
    if (!showing.has_value() || *showing >= settings_.Profiles().size()) {
        return std::nullopt;
    }
    return settings_.Profiles()[*showing].name;
}

std::optional<std::string> AppWorld::KeyLabel(CommandId command) const {
    // A global hotkey another program holds does nothing when pressed, so
    // a card has no key to name (docs/TUTORIAL.md, section 16.3).
    if (const std::optional<core::HotkeySlot> slot = InfoFor(command).hotkey;
        slot.has_value() && registered_ && !registered_(*slot)) {
        return std::nullopt;
    }
    // The first of its keys, as the cheat sheet names it.
    const std::vector<platform::KeyCombo> keys = KeysFor(command, settings_.Stored(), settings_.Live().shortcuts);
    return keys.empty() ? std::nullopt : std::optional<std::string>(FormatKeyComboLabel(keys.front()));
}

}  // namespace sz::ui
