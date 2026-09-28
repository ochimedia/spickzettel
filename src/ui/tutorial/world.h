#pragma once

// What the tutorial reads of the app - docs/TUTORIAL.md, section 7.1. A
// narrow interface with only const queries, and nothing else of the app
// is the tutorial's to see: OverlayApp answers it from the editor, the
// session and the settings, and a test fakes it. Every answer is a value
// - an id, a rectangle, a count - never a reference into the model.
// No ImGui.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "core/config/app_config.h"
#include "core/session/actions.h"
#include "ui/interaction/command.h"

namespace sz::ui::tutorial {

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
    size_t strokes = 0;
};

// What covers the canvas, if anything - the machine's Panel and Popup
// levels, as far as a card needs to tell them apart.
enum class Cover { None, Overview, CheatSheet, Popup };

class World {
public:
    virtual ~World() = default;

    // How many times the overlay has come up since the app started (see
    // OverlayApp::OnOverlayShown).
    virtual uint64_t Showings() const = 0;
    virtual Cover CanvasCover() const = 0;

    // The hand: the snippet in drawing mode, the selection, and the
    // creation tool in hand (nothing for a marking tool, or none).
    virtual std::optional<core::ItemId> DrawingItem() const = 0;
    virtual std::vector<core::ItemId> Selection() const = 0;
    virtual std::optional<core::ItemCreationKind> CreationToolInHand() const = 0;

    // The canvas being looked at, the folder a canvas is in (0 for none,
    // or a deleted one), and a canvas's name.
    virtual core::CanvasId CurrentCanvas() const = 0;
    virtual core::FolderId FolderOf(core::CanvasId canvas) const = 0;
    virtual std::string CanvasName(core::CanvasId canvas) const = 0;
    // Every snippet on the live canvases of `folder`, deleted ones
    // included, bottom to top per canvas. Nothing for a folder that is
    // gone.
    virtual std::vector<SnippetFacts> SnippetsIn(core::FolderId folder) const = 0;

    // Words for the card: the key that runs `command`, as bound now -
    // nothing when it is unbound - and the triggers that make snippets
    // with a drag on empty canvas.
    virtual std::optional<std::string> KeyLabel(CommandId command) const = 0;
    virtual core::CreationTrigger ScreenshotTrigger() const = 0;
    virtual core::CreationTrigger DrawingTrigger() const = 0;
};

}  // namespace sz::ui::tutorial
