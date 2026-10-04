#pragma once

// The canvas view - docs/VIEW_LAYER.md, section 7: the canvas layer (the
// frozen screen, the debug readout), the items layer (every snippet, the
// selection's border and bar, a box being dragged), the note
// editor, the dock of minimized snippets, and in the read-only modes the
// view-only layer. What it keeps of its own is what drawing a canvas is
// made of: both mesh caches, the previews' reading budgets, and the debug
// readout of the resize band's edge under the pointer.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "core/canvas/item.h"
#include "core/drawing/stroke.h"
#include "core/drawing/stroke_mesh_cache.h"
#include "core/session/session.h"
#include "core/session/settings.h"
#include "ui/editor.h"
#include "ui/item_painting.h"
#include "ui/view/view_host.h"

namespace sz::ui {

// How much of itself every snippet keeps while a new one is being made -
// see CanvasView::ItemsFadedForCreation. Enough to tell where things are,
// little enough that what is being framed is what is seen.
inline constexpr float kCreationFadeAlpha = 0.2f;

class CanvasView {
public:
    CanvasView(core::Session& session, core::Settings& settings, Editor& editor, ViewHost& host);

    // A frame opened and closed on both mesh caches: what a frame did not
    // draw is dropped at its end (see StrokeMeshCache).
    void BeginFrame();
    void EndFrame();
    // Stage 1: what the last frame drew and this one has not asked for yet
    // let go of (see TextureCache::BeginFrame), and the current canvas's
    // pictures asked for, which is what keeps them.
    void KeepTextures();
    // Stage 2: the canvas layer, then every item on the current canvas,
    // back to front, into one layer, the selection's border and bar over
    // all of them, and the note editor and the dock above it.
    // `propertiesItem` is the snippet Properties is up for, which keeps its
    // highlight while the pointer is over the popover; `bottomPanelsTop` is
    // what the dock's chips stay above.
    void Draw(float displayW, float displayH, std::optional<core::ItemId> propertiesItem, float bottomPanelsTop);
    // The read-only modes' whole canvas: the current canvas's items at their
    // real screen positions - in the pinned view only the pinned ones - with
    // no selection and nothing to click, and `chrome` drawn last into the
    // same layer.
    void DrawViewOnly(float displayW, float displayH, bool pinnedOnly,
                      const std::function<void(ImDrawList*)>& chrome);

    // What a canvas's preview is drawn with - see ViewHost::Previews. Starts
    // the frame's reading budget over.
    ViewHost::PreviewDrawing Previews();
    // Whether the snippets are faded back so that what a new snippet is
    // made from shows through them: while a creation tool is in hand, and
    // while a region is being dragged out - not for a press that has not
    // moved yet, which may still be a click, and would flicker.
    bool ItemsFadedForCreation() const;
    // Which edge of the resize band (if any) is hovered or dragging - see
    // debugHoveredResizeHandle_ - and let go of, for the read-only modes,
    // which never draw the items that set it.
    const std::string& DebugHoveredResizeHandle() const { return debugHoveredResizeHandle_; }
    void ForgetHoveredHandle() { debugHoveredResizeHandle_.clear(); }
private:
    const core::CanvasManager& Manager() const { return session_.Manager(); }
    core::persistence::LibraryStore* Store() const { return session_.Store(); }
    const core::AppConfig& Cfg() const { return settings_.Stored(); }

    // The frozen screen, if one is held, and the debug readout.
    void RenderCanvasLayer(float displayW, float displayH);
    // The items layer, the note editor and the dock. Decides once, from
    // ResolvePointerTarget, what the pointer is over and so what lights up
    // and which shape it wears; see the definition.
    void RenderItems(float displayW, float displayH, std::optional<core::ItemId> propertiesItem,
                     float bottomPanelsTop);
    // An item's content, its in-progress stroke (`drawing`: it is the
    // snippet in drawing mode, whose stroke in flight is on the live layer)
    // and its border, into the items layer's draw list - see the definition.
    // `selected`: the selection's outline is its border, drawn over every
    // snippet by PaintSelectionOutline, and this one draws none.
    void PaintItemBody(ImDrawList* drawList, const core::Item& item, bool drawing, bool highlighted,
                       bool isFrontmost, bool selected);
    // What a selected snippet wears while the selection is live, *drawn*:
    // its border, in the selection's color.
    // Nothing in it takes input. Which of it is under the pointer is
    // ResolvePointerTarget's answer, what the pointer looks like over it
    // is RenderItems', and a press on any of it is the recognizer's -
    // so nothing over the canvas is hit-tested by ImGui at all - see
    // docs/ARCHITECTURE.md, "Selection", for why that rule is absolute.
    // `drawing`: the snippet is in drawing mode, and wears the halo that
    // says so. `highlighted`: the border is the heavier one, as any
    // snippet's is under the pointer.
    void PaintSelectionOutline(ImDrawList* drawList, const core::Item& item, bool drawing, bool highlighted);
    // The selection bar: its buttons on a small pill floating over
    // the selection's bounding box (see LayoutBar in selection_layout.h
    // for where exactly). `hotButton` is the button the
    // pointer is over and allowed to light up this frame, if any - see
    // RenderItems for what "allowed" means while something is held.
    void PaintSelectionBar(ImDrawList* drawList, const std::optional<ChromeButton>& hotButton);
    // The live text editor over an item whose note is being edited - the
    // one piece of an item that is a real ImGui widget, in a window of its
    // own above the items layer. See the definition.
    void RenderNoteEditor(const core::Item& item, ImVec2 pMin, ImVec2 pMax);
    // Small chips along the bottom of the screen, one per minimized item
    // on the current canvas (see Item::minimized) - click one to restore
    // it (unset minimized, bring to front). A no-op (renders nothing) if
    // nothing's minimized.
    void RenderDock(float displayW, float displayH, float bottomPanelsTop);

    // ===== Textures =====

    // Every texture is the session's TextureCache's, asked for by what it
    // shows as it is about to be drawn, and never held here - see
    // TextureCache for what that settles.
    core::TextureCache& Textures() { return session_.Textures(); }
    // A snippet's picture's texture, read from the library the first time
    // it is asked for: 0 while there are no pixels to show - none stored,
    // or they cannot be read - which draws the placeholder or the fill.
    uint64_t PictureTexture(const core::Item& item);
    // Asks for the textures of every snippet on the current canvas, drawn
    // this frame or not - minimized, or left out of the pinned view - so
    // that none is let go of and read again the moment it is drawn: the
    // current canvas's pictures stay on the GPU, and no other canvas's do.
    void KeepCurrentCanvasTextures();

    // ===== Previews' pictures (AppConfig::overviewShowsBitmaps) =====

    // A picture's pixels, decoded and scaled down to thumbnail size, for a
    // preview - which draws canvases that aren't current and whose
    // full-size pixels are deliberately not in memory. Kept, like every
    // texture, only while something draws it: while a panel shows its
    // canvas, so a library of screenshots costs nothing while it isn't
    // being browsed. The expensive part is the decode, not the memory - a
    // preview is at most LibraryStore::kThumbnailMaxExtent on its long edge,
    // a couple of hundred kilobytes against the eight megabytes it came from.
    //
    // Resets the per-frame decode budget - see Previews.
    void BeginOverviewPreviewFrame();
    // The preview texture for one snippet's picture, loading it if there is
    // budget left this frame and it hasn't already failed. Three answers,
    // and a preview draws each differently: a handle to draw with; 0 for a
    // picture with no pixels to show at all (or whose read failed), which
    // gets the placeholder gradient; and nothing at all for one whose turn
    // to be read hasn't come yet, which gets drawn as an empty tile rather
    // than a stand-in that will be replaced a few frames later. A slow
    // library fills in over those frames rather than stalling the panel.
    std::optional<uint64_t> PicturePreviewTexture(const core::Item& item);
    // How a preview finds a picture's pixels: PicturePreviewTexture while the
    // Overview shows bitmaps (see AppConfig::overviewShowsBitmaps), else
    // nothing - an empty lookup, which DrawCanvasPreview tests for.
    PreviewTextureFn PreviewTextureLookup();

    // Each cache aimed at the current generation, which is all a drawing
    // call needs to be handed - see StrokeMeshSlot.
    core::StrokeMeshSlot CanvasMeshSlot() { return core::StrokeMeshSlot{&strokeMeshCache_, Manager().Generation()}; }
    core::StrokeMeshSlot PreviewMeshSlot() { return core::StrokeMeshSlot{&previewMeshCache_, Manager().Generation()}; }
    // The picture filter from settings, and the window's callbacks for it
    // and for strokes - what every call that paints a snippet is handed.
    PaintHooks Hooks() const;

    core::Session& session_;
    core::Settings& settings_;
    Editor& editor_;
    ViewHost& host_;

    // The tessellated shape of each stroke, kept between frames - see
    // StrokeMeshCache for what that saves and what invalidates an entry.
    //
    // Two of them, not one, because a cached mesh is only valid at the scale
    // it was built at, and the two places strokes are drawn use different
    // ones *in the same frame*: an item on the canvas is drawn at its own
    // size, and the same item appears again, much smaller, in its canvas's
    // preview. One cache would have each rebuild the other's entry every
    // frame - strictly worse than not caching at all. Sharing one between
    // the canvas and the dock is safe by contrast, and they do: RenderItems
    // skips minimized items and the dock draws only those, so no item is
    // ever in both at once.
    core::StrokeMeshCache strokeMeshCache_;
    core::StrokeMeshCache previewMeshCache_;
    // How many previews are still allowed to be read this frame, one budget
    // per cost. Both reset each frame previews are drawn.
    //
    // Full images are the fallback for a picture stored without a
    // thumbnail, and a decode is milliseconds - a handful per frame, so
    // the panel doesn't stall on the frame it opens, which is exactly the
    // frame it must not. Thumbnails are the ordinary path and cost well
    // under a millisecond, so the budget is high enough that a normal
    // library appears at once, and bounded only against a folder holding
    // an unreasonable number of canvases.
    int picturePreviewLoadBudget_ = 0;
    int picturePreviewThumbnailBudget_ = 0;
    // Which edge or corner of the resize band (if any) is currently hovered
    // or dragging, as a
    // short human-readable label ("nw item=3", "e item=5 (dragging)") -
    // empty when none is. Set by RenderItems from the resolver's answer,
    // cleared at the top of every call so it never shows a stale edge from
    // a frame where the mouse has moved off the band since.
    // Exists purely for the debug overlay (gated on AppConfig::
    // showDebugOverlay, same as everything else that setting draws) and
    // the tests - a live readout of exactly what the resize band thinks is
    // under the cursor, since the band is not drawn at all.
    std::string debugHoveredResizeHandle_;
};

}  // namespace sz::ui
