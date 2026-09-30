#pragma once

// What the tutorial reads of the app - docs/TUTORIAL.md, section 7.1. A
// narrow interface with only const queries, and nothing else of the app
// is the tutorial's to see: OverlayApp answers it from the editor, the
// session and the settings, and a test fakes it. Every answer is a value
// - an id, a rectangle, a count - never a reference into the model.
// No ImGui.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "core/config/app_config.h"
#include "core/session/actions.h"
#include "ui/interaction/command.h"

namespace sz::ui::tutorial {

// One stroke drawn on a snippet, as it looks on screen.
struct StrokeFacts {
    uint32_t colorRGBA = 0;
    // Its width and its length on screen: the stored ones, scaled by the
    // snippet's size, since a stroke is kept at the snippet's own scale.
    float widthPx = 0.0f;
    float lengthPx = 0.0f;
    // A straight line or a rectangle, as the pen's shapes store them.
    core::DrawShape shape = core::DrawShape::Freehand;
};

// One snippet, as the tutorial sees it.
struct SnippetFacts {
    core::ItemId id = 0;
    core::CanvasId canvas = 0;
    core::Rect rect;
    // A screenshot has a picture; a drawing has none.
    bool picture = false;
    bool fullscreen = false;
    // In the dock rather than on the canvas.
    bool minimized = false;
    // Marked deleted: still in the library, for undo to bring back.
    bool deleted = false;
    std::vector<StrokeFacts> strokes;
    // Its note: a caption typed with Text (Item::noteText), on the snippet
    // as it is typed.
    std::string note;
    // Stays on screen when the overlay is put away (Item::pinned).
    bool pinned = false;
    // The picture's opacity - a screenshot's image, a drawing's backing -
    // and the strokes' (Item::foregroundOpacity).
    float pictureOpacity = 1.0f;
    float drawingOpacity = 1.0f;

    // How much is drawn on it: the strokes' length on screen, all told.
    float InkPx() const {
        float ink = 0.0f;
        for (const StrokeFacts& stroke : strokes) {
            ink += stroke.lengthPx;
        }
        return ink;
    }
};

// A folder, as the tutorial sees it: deleted when it is in the trash.
struct FolderFacts {
    core::FolderId id = 0;
    std::string name;
    bool deleted = false;
};

// A canvas, as the tutorial sees it: deleted when it is in the trash, on
// its own or with its folder.
struct CanvasFacts {
    core::CanvasId id = 0;
    core::FolderId folder = 0;
    std::string name;
    bool deleted = false;
};

// A profile, as the tutorial sees it - docs/TUTORIAL.md, section 18.3.
// Known by its id, which a rename keeps; its name is unique too, but only
// at any one moment.
struct ProfileFacts {
    core::ProfileId id = 0;
    std::string name;
    // What it is matched on first: a program's file, else part of a
    // window's title - empty when it matches nothing.
    std::string program;
    // Its rules match the program the overlay is up over; and it is the
    // one that runs, the first in the list whose rules do.
    bool matchesUnderneath = false;
    bool running = false;
    // The Behavior settings it states for itself, and how many of those
    // hold the defaults' value all the same.
    size_t stated = 0;
    size_t statedAsDefaults = 0;
};

// The Settings panel's section, as far as a card needs to tell them
// apart.
enum class SettingsSection { Other, Behavior, Profiles };

// What covers the canvas, if anything - the machine's Panel and Popup
// levels, as far as a card needs to tell them apart. A snippet's own
// popups, the color chooser and Properties, cover nothing: a step may
// well be using them, and a press outside either closes it and does
// nothing else (docs/TUTORIAL.md, section 15.3).
enum class Cover { None, Overview, CheatSheet, Popup };

class World {
public:
    virtual ~World() = default;

    // How many times the overlay has come up since the app started (see
    // OverlayApp::OnOverlayShown).
    virtual uint64_t Showings() const = 0;
    // How many times the pinned view, and view mode, have come up since
    // the app started - each counted as a transition enters it (see
    // OverlayApp::OnModeEntered).
    virtual uint64_t PinnedViews() const = 0;
    virtual uint64_t ViewModes() const = 0;
    // How many screenshots a capture hotkey, the quick or the silent one,
    // has taken since the app started (see OverlayApp::QuickCapture).
    virtual uint64_t Captures(core::HotkeySlot by) const = 0;
    virtual Cover CanvasCover() const = 0;

    // The hand: the snippets in drawing mode, the selection, and the
    // creation tool in hand (nothing for a marking tool, or none).
    virtual std::vector<core::ItemId> DrawingItems() const = 0;
    bool IsDrawingOn(core::ItemId id) const {
        const std::vector<core::ItemId> drawing = DrawingItems();
        return std::find(drawing.begin(), drawing.end(), id) != drawing.end();
    }
    virtual std::vector<core::ItemId> Selection() const = 0;
    virtual std::optional<core::ItemCreationKind> CreationToolInHand() const = 0;
    // The tool in hand, Select for the hand at rest, and the shape the
    // eraser is cycled to while it is in hand.
    virtual core::Tool ToolInHand() const = 0;
    virtual core::DrawShape EraserShape() const = 0;
    // What the pen draws with: its color, and its width on screen.
    virtual uint32_t PenColor() const = 0;
    virtual float PenWidth() const = 0;
    // The snippet whose note is being typed, if one is.
    virtual std::optional<core::ItemId> NoteBeingTyped() const = 0;

    // The canvas being looked at, the folder a canvas is in (0 for none,
    // or a deleted one), and a canvas's name.
    virtual core::CanvasId CurrentCanvas() const = 0;
    virtual core::FolderId FolderOf(core::CanvasId canvas) const = 0;
    virtual std::string CanvasName(core::CanvasId canvas) const = 0;
    // Every snippet on the live canvases of `folder`, deleted ones
    // included, bottom to top per canvas. Nothing for a folder that is
    // gone.
    virtual std::vector<SnippetFacts> SnippetsIn(core::FolderId folder) const = 0;
    // Every folder, in the Overview's order, and every canvas of `folder`
    // - those in the trash among them.
    virtual std::vector<FolderFacts> Folders() const = 0;
    virtual std::vector<CanvasFacts> CanvasesIn(core::FolderId folder) const = 0;

    // The Overview, while it is up (CanvasCover): whether it shows the
    // canvases rather than Settings or About, and whether it shows what
    // is deleted. And whether the canvas bar is on at all (Settings >
    // Appearance).
    virtual bool OverviewShowsCanvases() const = 0;
    virtual bool OverviewShowsDeleted() const = 0;
    virtual bool CanvasBarOn() const = 0;

    // The program the overlay is up over: its file, else its window's
    // title - empty when it is known by neither. The profiles, in their
    // order. And the Settings panel: whether the Overview is on its
    // Settings tab, the section picked there, and whose values Showing
    // shows - a profile's name, or nothing for the defaults.
    virtual std::string Underneath() const = 0;
    virtual std::vector<ProfileFacts> Profiles() const = 0;
    virtual bool OverviewShowsSettings() const = 0;
    virtual SettingsSection SettingsSectionShown() const = 0;
    virtual std::optional<std::string> SettingsShowing() const = 0;

    // Words for the card: the key that runs `command`, as bound now -
    // nothing when it is unbound, or is a global hotkey another program
    // holds - and the triggers that make snippets
    // with a drag on empty canvas.
    virtual std::optional<std::string> KeyLabel(CommandId command) const = 0;
    virtual core::CreationTrigger ScreenshotTrigger() const = 0;
    virtual core::CreationTrigger DrawingTrigger() const = 0;

    // What is kept of a topic's progress (docs/TUTORIAL.md, section
    // 13.7): a step's id, "finished" or "skipped" - empty for a topic
    // never started. For the list, and the skip card's warnings.
    virtual std::string TopicProgress(std::string_view topic) const = 0;
};

}  // namespace sz::ui::tutorial
