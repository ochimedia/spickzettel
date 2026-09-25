#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "ui/icons_generated.h"
#include "core/canvas/item_geometry.h"

#include <imgui.h>
// For ImGui::RenderFrame - the selection bar's buttons are drawn, not
// widgets (see PaintSelectionBar), and this is what ImGui::Button draws its
// own frame with, so they look exactly as ImGui's buttons do.
#include <imgui_internal.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Items =================

namespace {
// kItemMinWidth/kItemMinHeight/kGrabMarginPx/ClampRectToViewport live in
// core/canvas/item_geometry.h - CanvasManager::SyncItemsToDisplaySize needs
// the exact same floors/never-fully-offscreen guarantee this file's own
// move/resize handling already enforces, so they're shared rather than
// kept as two copies that could drift apart.

// ----- Selection geometry -----
//
// Where a selected snippet's handles and the selection bar are, in pixels,
// from the rects alone. This is what ResolvePointerTarget reads to say
// what is under a point; PaintSelectionOutline and PaintSelectionBar draw
// to the same rects.

// Half-open on the far edges, the way ImGui's own ImRect::Contains is, so a
// pixel exactly on the seam between two adjacent rects belongs to exactly
// one of them.
struct HitRect {
    ImVec2 min;
    ImVec2 max;
    bool Contains(float x, float y) const { return x >= min.x && y >= min.y && x < max.x && y < max.y; }
};

// A handle is a small square centered *on* the border - a corner or the
// middle of an edge - the way a drawing program draws them. It covers a
// few of the snippet's own pixels, which is fine: handles show only while
// the selection is live, when nothing can be drawn anyway. Its hit rect
// reaches a little past what is drawn, so it needn't be hit dead on.
constexpr float kHandleSizePx = 8.0f;
constexpr float kHandleHitSlopPx = 3.0f;

// One of the 8 handles: where its center is, the pointer it wears, and its
// compass name as the debug overlay prints it.
struct HandleSpec {
    ResizeHandle handle;
    const char* name;
    ImVec2 center;
    ImGuiMouseCursor cursor;
};

// The 8 handles of a rect, corners first, at whole pixels: every one is
// tested against a whole-pixel pointer position, and a fractional edge
// left a sub-pixel column that belonged to nothing.
std::array<HandleSpec, 8> HandleSpecs(const Rect& r) {
    const float x0 = std::round(r.x);
    const float y0 = std::round(r.y);
    const float x1 = std::round(r.x + r.w);
    const float y1 = std::round(r.y + r.h);
    const float xm = std::round((x0 + x1) * 0.5f);
    const float ym = std::round((y0 + y1) * 0.5f);
    return {{
        {ResizeHandle::NW, "nw", ImVec2(x0, y0), ImGuiMouseCursor_ResizeNWSE},
        {ResizeHandle::NE, "ne", ImVec2(x1, y0), ImGuiMouseCursor_ResizeNESW},
        {ResizeHandle::SE, "se", ImVec2(x1, y1), ImGuiMouseCursor_ResizeNWSE},
        {ResizeHandle::SW, "sw", ImVec2(x0, y1), ImGuiMouseCursor_ResizeNESW},
        {ResizeHandle::N, "n", ImVec2(xm, y0), ImGuiMouseCursor_ResizeNS},
        {ResizeHandle::S, "s", ImVec2(xm, y1), ImGuiMouseCursor_ResizeNS},
        {ResizeHandle::E, "e", ImVec2(x1, ym), ImGuiMouseCursor_ResizeEW},
        {ResizeHandle::W, "w", ImVec2(x0, ym), ImGuiMouseCursor_ResizeEW},
    }};
}

HitRect HandleDrawRect(ImVec2 center) {
    const float half = std::round(Px(kHandleSizePx) * 0.5f);
    return HitRect{ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half)};
}

HitRect HandleHitRect(ImVec2 center) {
    const float half = std::round(Px(kHandleSizePx) * 0.5f) + std::round(Px(kHandleHitSlopPx));
    return HitRect{ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half)};
}

// The compass name the debug overlay prints for a handle, and the pointer
// it wears - both straight off the same table the resolver walks.
const char* ResizeHandleName(ResizeHandle handle) {
    for (const HandleSpec& h : HandleSpecs(Rect{})) {
        if (h.handle == handle) {
            return h.name;
        }
    }
    return "?";
}
ImGuiMouseCursor ResizeHandleCursor(ResizeHandle handle) {
    for (const HandleSpec& h : HandleSpecs(Rect{})) {
        if (h.handle == handle) {
            return h.cursor;
        }
    }
    return ImGuiMouseCursor_Arrow;
}

// The selection bar: [Pin][More][Minimize][Maximize/Restore][Close] - or,
// in drawing mode, [Pen][Eraser][Text][Color] - buttons of this size, this
// far apart, on a pill this much bigger than them, floating just above the
// selection's bounding box - or below it when there is no room above, or
// inside its top edge when there is no room either way (a fullscreen
// snippet). Centered on the box and kept on screen.
constexpr float kBarButtonSize = 28.0f;
constexpr float kBarButtonGap = 2.0f;
constexpr float kBarPad = 6.0f;
constexpr float kBarGapPx = 8.0f;  // between the box and the bar
constexpr float kBarHeight = kBarButtonSize + 2.0f * kBarPad;

float BarWidth(size_t buttonCount) {
    const auto n = static_cast<float>(buttonCount);
    return n * Px(kBarButtonSize) + (n - 1.0f) * Px(kBarButtonGap) + 2.0f * Px(kBarPad);
}

struct BarLayout {
    ImVec2 min;
    ImVec2 max;
};

BarLayout LayoutBar(const Rect& bounds, float displayW, float displayH, size_t buttonCount) {
    const float width = BarWidth(buttonCount);
    float x = std::round(bounds.x + bounds.w * 0.5f - width * 0.5f);
    x = std::clamp(x, 0.0f, std::max(0.0f, displayW - width));
    const auto at = [&](float y) { return BarLayout{ImVec2(x, y), ImVec2(x + width, y + Px(kBarHeight))}; };
    const BarLayout above = at(std::round(bounds.y) - Px(kBarGapPx) - Px(kBarHeight));
    if (above.min.y >= 0.0f) {
        return above;
    }
    const BarLayout below = at(std::round(bounds.y + bounds.h) + Px(kBarGapPx));
    if (below.max.y <= displayH) {
        return below;
    }
    return at(std::max(0.0f, std::round(bounds.y) + Px(kBarGapPx)));
}

HitRect BarButtonRect(const BarLayout& bar, const std::vector<ChromeButton>& buttons, ChromeButton button) {
    float slot = 0.0f;
    for (const ChromeButton candidate : buttons) {
        if (candidate == button) {
            break;
        }
        slot += 1.0f;
    }
    const float x = bar.min.x + Px(kBarPad) + slot * (Px(kBarButtonSize) + Px(kBarButtonGap));
    const float y = bar.min.y + Px(kBarPad);
    return HitRect{ImVec2(x, y), ImVec2(x + Px(kBarButtonSize), y + Px(kBarButtonSize))};
}
}  // namespace

// Which buttons this bar is carrying, in the order they are drawn: the
// settings' own list, minus whatever is switched off (see
// AppConfig::snippetBar and Settings > Interaction). Empty is a legal
// answer - everything switched off - and means no bar is drawn at all.
std::vector<ChromeButton> OverlayApp::BarButtons() const {
    const BarButtonList& configured = drawingItem_.has_value() ? Cfg().drawingBar : Cfg().snippetBar;
    std::vector<ChromeButton> shown;
    shown.reserve(configured.size());
    for (const BarButtonSetting& entry : configured) {
        if (entry.shown) {
            shown.push_back(entry.button);
        }
    }
    return shown;
}

// ----- The selection -----

bool OverlayApp::IsSelected(ItemId id) const {
    return std::find(selection_.begin(), selection_.end(), id) != selection_.end();
}

bool OverlayApp::SelectionLive() const { return !ArmedCreation().has_value(); }

void OverlayApp::SelectOnly(ItemId id) {
    selection_.clear();
    selection_.push_back(id);
}

void OverlayApp::ToggleSelected(ItemId id) {
    const auto it = std::find(selection_.begin(), selection_.end(), id);
    if (it != selection_.end()) {
        selection_.erase(it);
    } else {
        selection_.push_back(id);
    }
}

void OverlayApp::ClearSelection() { selection_.clear(); }

void OverlayApp::PruneSelection() {
    if (selection_.empty()) {
        return;
    }
    const Canvas* canvas = Manager().CurrentOrNull();
    const auto onScreen = [&](ItemId id) {
        if (canvas == nullptr) {
            return false;
        }
        for (const Item& item : canvas->items) {
            if (item.id == id) {
                return !item.minimized && !Manager().IsDeleted(*canvas, item);
            }
        }
        return false;
    };
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](ItemId id) { return !onScreen(id); }),
                     selection_.end());
    // Drawing mode is on a selected snippet, so it goes the same way: a
    // canvas switch, a delete, a minimize.
    if (drawingItem_.has_value() && !IsSelected(*drawingItem_)) {
        ExitDrawingMode();
    }
}

std::optional<ItemId> OverlayApp::PrimarySelection() const {
    if (selection_.empty()) {
        return std::nullopt;
    }
    return selection_.back();
}

std::optional<Rect> OverlayApp::SelectionBounds() const {
    const Canvas* canvas = Manager().CurrentOrNull();
    if (canvas == nullptr) {
        return std::nullopt;
    }
    std::optional<Rect> bounds;
    for (const Item& item : canvas->items) {
        if (!IsSelected(item.id)) {
            continue;
        }
        if (!bounds.has_value()) {
            bounds = item.rect;
            continue;
        }
        const float x0 = std::min(bounds->x, item.rect.x);
        const float y0 = std::min(bounds->y, item.rect.y);
        const float x1 = std::max(bounds->x + bounds->w, item.rect.x + item.rect.w);
        const float y1 = std::max(bounds->y + bounds->h, item.rect.y + item.rect.h);
        bounds = Rect{x0, y0, x1 - x0, y1 - y0};
    }
    return bounds;
}

// ----- The clipboard -----

void OverlayApp::CopySelectionToClipboard(bool cut) {
    if (selection_.empty()) {
        return;  // nothing selected is nothing to copy, and says so by
                 // leaving whatever is on the clipboard alone
    }
    clipboard_ = selection_;
    clipboardIsCut_ = cut;
    ShowActionToast(cut ? strings::kToastCut : strings::kToastCopied);
}

bool OverlayApp::IsWaitingToBeCut(ItemId id) const {
    return clipboardIsCut_ && std::find(clipboard_.begin(), clipboard_.end(), id) != clipboard_.end();
}

// What is on the clipboard, onto the canvas being looked at: copies of it,
// or - after a Cut - the snippets themselves, moved here.
//
// Every snippet is looked up as this runs rather than trusted from when it
// was copied: one deleted since is skipped, and a paste that finds nothing
// left says so instead of pasting an empty selection. That check is the
// whole reason the clipboard holds ids.
//
// A copy lands on top of its source when the source is on this canvas, so
// there it is offset the way the Properties popover's own Copy is - far
// enough to see that there are now two. Pasted onto another canvas it
// keeps its place exactly, which is where the eye expects it. A cut pasted
// back onto its own canvas is the snippet itself, and there is nothing to
// tell apart: it stays exactly where it was.
//
// One undo takes the whole paste back: the copies go, and what a cut
// moved here goes back where it came from - see Session::RecordArrivals.
void OverlayApp::PasteFromClipboard() {
    if (clipboard_.empty() || Manager().CurrentOrNull() == nullptr) {
        return;
    }
    const CanvasId here = Manager().CurrentCanvasId();
    const bool cut = clipboardIsCut_;
    std::vector<ItemId> pasted;
    std::vector<Session::Arrival> arrivals;
    bool fromThisCanvas = false;
    bool pictureLost = false;  // a copy whose source's picture could not be read
    for (const ItemId id : clipboard_) {
        const std::optional<CanvasId> from = Manager().CanvasHoldingItem(id);
        if (!from.has_value() || Manager().IsItemDeleted(id)) {
            continue;  // deleted, or deleted for good, since it was copied
        }
        if (cut && *from == here) {
            pasted.push_back(id);  // already here: nothing moves, nothing to undo
            continue;
        }
        if (cut) {
            if (const std::optional<Session::Arrival> moved = session_.MoveItemHere(id)) {
                arrivals.push_back(*moved);
                pasted.push_back(id);
            }
            continue;
        }
        const ItemId placed = Manager().PlaceItemOnCanvas(id, here, /*copy=*/true);
        if (placed == 0) {
            continue;
        }
        // A copy must never share its source's picture file or its
        // texture - see Session::ClonePicturesForCopy.
        pictureLost = !session_.ClonePicturesForCopy(id, placed) || pictureLost;
        fromThisCanvas = fromThisCanvas || *from == here;
        arrivals.push_back(Session::Arrival{placed});
        pasted.push_back(placed);
    }
    if (pasted.empty()) {
        ShowActionToast(strings::kToastNothingToPaste);
        return;
    }
    if (fromThisCanvas) {
        for (const ItemId id : pasted) {
            OffsetCopiedItem(id);
        }
    }
    session_.RecordArrivals(std::move(arrivals), /*duplicate=*/false);
    // Whatever arrived needs a texture now: this is the current canvas,
    // and the only other time the sync runs is a canvas switch.
    session_.SyncTexturesToCurrentCanvas();
    // What was pasted is what is selected, so it can be moved straight
    // away - and, for a cut pasted onto another canvas, so that what
    // arrived is the thing the bar is over.
    selection_ = pasted;
    if (cut) {
        clipboard_.clear();
        clipboardIsCut_ = false;
    }
    ShowActionToast(pictureLost ? strings::kToastCopiedWithoutPicture : strings::kToastPasted);
}

// A copy of every selected snippet, on this canvas, offset the way a
// paste onto its own canvas is - Copy and Paste in one step, and what
// Ctrl+D means everywhere else.
//
// Deliberately not routed through the clipboard: duplicating something is
// not a reason to lose what was copied earlier, and the clipboard holds
// ids rather than snippets (see PasteFromClipboard), so borrowing it here
// would also mean deciding what a later paste of those ids should do.
//
// What was made is what ends up selected, so it can be dragged straight
// off the original - the same rule a paste follows, and the reason the
// copies are offset at all.
void OverlayApp::DuplicateSelection() {
    if (selection_.empty() || Manager().CurrentOrNull() == nullptr) {
        return;
    }
    std::vector<ItemId> made;
    std::vector<Session::Arrival> arrivals;
    bool pictureLost = false;  // a copy whose source's picture could not be read
    for (const ItemId id : selection_) {
        if (Manager().IsItemDeleted(id)) {
            continue;  // deleted since it was selected
        }
        const ItemId copy = Manager().DuplicateItem(id);
        if (copy == 0) {
            continue;
        }
        // A copy must never share its source's picture file or its
        // texture - see Session::ClonePicturesForCopy.
        pictureLost = !session_.ClonePicturesForCopy(id, copy) || pictureLost;
        OffsetCopiedItem(copy);
        made.push_back(copy);
        arrivals.push_back(Session::Arrival{copy});
    }
    if (made.empty()) {
        return;
    }
    session_.RecordArrivals(std::move(arrivals), /*duplicate=*/true);
    // The copies are on the current canvas, so a painted layer's pixels
    // need a texture now rather than at the next canvas switch, which is
    // the only other time the sync runs.
    session_.SyncTexturesToCurrentCanvas();
    selection_ = made;
    ShowActionToast(pictureLost ? strings::kToastCopiedWithoutPicture : strings::kToastDuplicated);
}

void OverlayApp::AddTouchedToSelection(const Rect& box) {
    const Canvas* canvas = Manager().CurrentOrNull();
    if (canvas == nullptr) {
        return;
    }
    // Back to front, so a box over a stack of them leaves the frontmost
    // selected last - which is the one the bar's single-snippet buttons
    // then act on, and the one whose handles are found first.
    for (const Item& item : canvas->items) {
        if (item.minimized || Manager().IsDeleted(*canvas, item) || IsSelected(item.id)) {
            continue;
        }
        // Touched, not enclosed: a box drawn across a row of snippets is
        // meant to have caught the ones it was drawn across, and asking
        // for the whole of a fullscreen snippet to be inside the box
        // would put that one out of reach of any box at all.
        if (RectsOverlap(box, item.rect)) {
            selection_.push_back(item.id);
        }
    }
}

void OverlayApp::DeleteSelection() {
    // Delete pressed mid-drag: the drag stops where it is, rather than go
    // on moving a snippet nobody can see and file that move after the
    // delete - an undo that visibly did nothing, and a snippet restored
    // wherever the hand happened to let go.
    EndGesture();
    // A copy: deleting clears nothing itself, but the toast and the
    // session are free to look at the selection while this runs.
    const std::vector<ItemId> doomed = selection_;
    DeleteItemsWithToast(doomed);
    ClearSelection();
}

void OverlayApp::NudgeSelection(float dx, float dy) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // A run of presses - or a key held down, repeating - is one undo, back
    // to where the run began.
    std::vector<Session::Placement> before = session_.PlacementsOf(selection_);
    for (const ItemId id : selection_) {
        Item* item = Manager().FindItemAnywhere(id);
        if (item == nullptr || item->isFullscreen) {
            continue;  // a fullscreen snippet has nowhere to go
        }
        if (untouchedDrawing_ == id) {
            untouchedDrawing_.reset();  // placed on purpose - see KeepPlacedDrawings
        }
        item->rect = ClampRectToViewport(Rect{item->rect.x + dx, item->rect.y + dy, item->rect.w, item->rect.h},
                                         display.x, display.y);
        Manager().MarkChanged();
        Manager().CommitItemLayout(id);
    }
    RecordPlacementBurst(PlacementBurst::Nudge, std::move(before));
}

// Escape puts the hand down, in stages: a creation tool in hand goes
// back to Select, the hand at rest (see the
// Tool enum), then drawing mode ends, then a cut waiting to be pasted is
// called off, then a selection clears. An open
// popover - the color chooser, a snippet's properties - closes first and
// takes the press. Not while typing, where Escape is the field's, and not
// while a panel is up - the Overview, the cheat sheet - which closes its
// own. The same gates hold for
// Delete and the
// arrows: a note being typed into keeps its own Delete and Backspace - and
// in drawing mode neither acts on the snippet, which is being worked in,
// not on.
void OverlayApp::HandleSelectionKeys() {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !io.WantTextInput && !PanelOpen() && CloseTopmostPopover()) {
        return;
    }
    const bool keysFree = !io.WantTextInput && !PanelOpen() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && keysFree) {
        if (CreationKindFor(activeTool_).has_value()) {
            ClearCreationGesture();
            PickTool(Tool::Select);
        } else if (drawingItem_.has_value()) {
            ExitDrawingMode();
        } else if (clipboardIsCut_ && !clipboard_.empty()) {
            // Never mind the cut: the snippets are still where they were,
            // so this only has to stop them waiting to be moved - the
            // selection they are part of is the next press of Escape's.
            clipboard_.clear();
            clipboardIsCut_ = false;
        } else if (!selection_.empty()) {
            ClearSelection();
        }
    }
    if (!keysFree || selection_.empty() || drawingItem_.has_value()) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace, /*repeat=*/false)) {
        DeleteSelection();
        return;
    }
    // A pixel a press, ten with Shift - the way every drawing program
    // nudges - and repeating while held.
    const float step = io.KeyShift ? 10.0f : 1.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        dx -= step;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        dx += step;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        dy -= step;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        dy += step;
    }
    if (dx != 0.0f || dy != 0.0f) {
        NudgeSelection(dx, dy);
    }
}

std::optional<ImVec2> OverlayApp::SelectionBarButtonCenter(ChromeButton button) const {
    if (!SelectionLive()) {
        return std::nullopt;
    }
    const std::optional<Rect> bounds = SelectionBounds();
    if (!bounds.has_value()) {
        return std::nullopt;
    }
    const std::vector<ChromeButton> buttons = BarButtons();
    if (std::find(buttons.begin(), buttons.end(), button) == buttons.end()) {
        return std::nullopt;  // not on the bar the mode shows
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const HitRect rect =
        BarButtonRect(LayoutBar(*bounds, display.x, display.y, buttons.size()), buttons, button);
    return ImVec2((rect.min.x + rect.max.x) * 0.5f, (rect.min.y + rect.max.y) * 0.5f);
}

void OverlayApp::RenderItems(float displayW, float displayH) {
    // Reset every frame - see the member's own doc comment. Cleared up
    // front so a frame where the mouse has moved off every handle (or
    // there are no items at all) shows "none" rather than whatever handle
    // happened to be hovered last.
    debugHoveredResizeHandle_.clear();

    Canvas* canvasPtr = Manager().CurrentOrNull();
    if (!canvasPtr) {
        return;  // no canvas, so no items to render
    }
    Canvas& canvas = *canvasPtr;
    // While a creation tool is armed, items shouldn't
    // intercept clicks meant for that gesture/menu instead - direct
    // equivalent of the mockup's setInputMode('region') raising the canvas
    // above the item layer. ResolvePointerTarget applies the same rule, so
    // nothing of the selection's is found under the pointer then either.
    const bool itemsInteractive = !ArmedCreation().has_value();

    // What the pointer is over, decided once, before anything is drawn -
    // see ResolvePointerTarget for the walk and the answers it gives. The
    // handles and the bar are drawn from this, and take their presses from
    // the same walk asked of the raw event's own position
    // (HandleItemGesture), so what lights up and what a click lands on are
    // one answer - this frame's - with no window hit-test of ImGui's in
    // between to be a frame late.
    const ImGuiIO& io = ImGui::GetIO();
    const PointerTarget target = ResolvePointerTarget(io.MousePos.x, io.MousePos.y);
    // Whether the selection's furniture may react to the pointer at all
    // this frame: not while a panel of ImGui's own is under it - a
    // popover, the canvas bar, the dock, the Overview, all of which sit above
    // every item - or one of them has a press in flight, which is what
    // io.WantCaptureMouse says; it is also true for as long as any popup is
    // open, which is exactly when ImGui's own widgets refuse to hover.
    const bool furnitureHot = itemsInteractive && !io.WantCaptureMouse;
    // ...and while a resize is under way, or a press on one of the bar's
    // buttons, only that handle or button reacts - the rule ImGui applies
    // to its own widgets while one of them holds ActiveId, so a handle
    // dragged across a bar button doesn't light that button up.
    const ItemGesture* held = GestureIf<ItemGesture>();
    const BarPress* pressed = GestureIf<BarPress>();
    const bool resizing = held != nullptr && held->resize;
    const bool blocked = resizing || pressed != nullptr;
    const bool hotHandle =
        furnitureHot && target.kind == PointerTarget::Kind::Handle &&
        (!blocked || (resizing && held->item == target.item && held->handle == target.handle));
    std::optional<ChromeButton> hotButton;
    if (furnitureHot && target.kind == PointerTarget::Kind::Button &&
        (!blocked || (pressed != nullptr && pressed->button == target.button))) {
        hotButton = target.button;
    }

    // The debug overlay's readout of which handle is live, with a dragging
    // handle reported for as long as it drags, wherever the pointer is.
    if (held != nullptr && held->handle.has_value()) {
        char handleDebug[64];
        std::snprintf(handleDebug, sizeof(handleDebug), "%s item=%llu (dragging)", ResizeHandleName(*held->handle),
                      static_cast<unsigned long long>(held->item));
        debugHoveredResizeHandle_ = handleDebug;
    } else if (hotHandle) {
        char handleDebug[64];
        std::snprintf(handleDebug, sizeof(handleDebug), "%s item=%llu", ResizeHandleName(target.handle),
                      static_cast<unsigned long long>(target.item));
        debugHoveredResizeHandle_ = handleDebug;
    }
    if (hotHandle) {
        ImGui::SetMouseCursor(ResizeHandleCursor(target.handle));
    }

    // Which single item (if any) gets the pronounced hover-tier border
    // this frame. Sticky override: an item mid-drag or mid-resize keeps its
    // own highlight regardless of where the mouse currently is, instead of
    // the hover picking a different item (or none) the moment the cursor
    // strays off its rect mid-gesture, or onto another item stacked in
    // front of it. itemPropertiesPopoverItemId_ gets the same treatment, so
    // the item a popover is currently showing for doesn't lose its
    // highlight the instant the mouse leaves its rect to go interact with
    // the popover instead.
    const std::optional<ItemId> stickyItemId =
        held != nullptr ? std::optional<ItemId>(held->item) : itemPropertiesPopoverItemId_;
    const std::optional<ItemId> highlightId =
        stickyItemId.has_value() ? stickyItemId : (itemsInteractive ? target.body : std::nullopt);

    // The snippet in front: the last one that is actually on screen, since
    // canvas.items is painted in order and a minimized item is painted
    // nowhere. Its border gets its own color (see
    // AppConfig::itemBorderColorFrontRGBA), which is the only thing that
    // says which of a stack of overlapping snippets is on top.
    std::optional<ItemId> frontmostId;
    for (const Item& item : canvas.items) {
        if (!item.minimized && !Manager().IsDeleted(canvas, item)) {
            frontmostId = item.id;
        }
    }

    // One layer for every item, painted back to front in canvas.items
    // order, so z-order is draw order by construction, and the selection's
    // outline, handles and bar over all of them. Nothing in an item is
    // hit-tested by ImGui, so an item needs only a draw list and a place in
    // the stack, and one layer
    // gives every item both at once - the same BeginScreenLayer the canvas
    // layer and the screen chrome use.
    ImDrawList* drawList = BeginScreenLayer("##sz_items_layer", displayW, displayH);
    // The note editor is the one real widget on any item, and goes above
    // the whole layer rather than into it - see below the loop.
    Item* editingItem = nullptr;
    ImVec2 editingMin;
    ImVec2 editingMax;
    for (Item& item : canvas.items) {
        if (item.minimized) {
            continue;  // shown in the dock instead - see RenderDock, called below
        }
        if (Manager().IsDeleted(canvas, item)) {
            continue;  // deleted: hidden until it is restored
        }
        PaintItemBody(drawList, item, canvas, drawingItem_ == item.id, highlightId == item.id, frontmostId == item.id);
        if (itemsInteractive && editingNoteItemId_ == item.id) {
            editingItem = &item;
            editingMin = ImVec2(std::round(item.rect.x), std::round(item.rect.y));
            editingMax = ImVec2(std::round(item.rect.x + item.rect.w), std::round(item.rect.y + item.rect.h));
        }
    }
    // The selection, over every snippet - the primary last, so where two
    // selected snippets overlap the one selected last has its handles on
    // top, which is the one the resolver finds first.
    if (SelectionLive()) {
        for (const ItemId id : selection_) {
            for (const Item& item : canvas.items) {
                if (item.id == id) {
                    PaintSelectionOutline(drawList, item, drawingItem_ == id);
                    break;
                }
            }
        }
        PaintSelectionBar(drawList, hotButton);
    }
    // Over everything, including the selection it is about to add to -
    // the same translucent-fill-and-outline the region capture's own drag
    // preview uses, being the same gesture in a different sense: one
    // frames what is to be made, this one frames what is already there.
    if (const BoxSelection* selecting = GestureIf<BoxSelection>(); selecting != nullptr && selecting->moved) {
        const Rect box = selecting->Bounds();
        const ImVec2 pMin(box.x, box.y);
        const ImVec2 pMax(box.x + box.w, box.y + box.h);
        drawList->AddRectFilled(pMin, pMax, theme::AccentU32(40));
        drawList->AddRect(pMin, pMax, theme::AccentU32(255), 0.0f, PxWhole(2.0f), ImDrawFlags_None);
    }
    // Faded back while a snippet is being made, so the screen it is made
    // from shows through what sits on it - faint rather than gone, so where
    // things are stays in view. Done to the finished vertices, which is the
    // one way to reach every part of a snippet alike: its pictures are
    // drawn in their own colors, not the style's, and PushStyleVar(Alpha)
    // would only reach what asks the style. Only the look: the capture
    // itself never sees the overlay (see IOverlayWindow::
    // CaptureRegionAsTexture).
    if (ItemsFadedForCreation()) {
        for (ImDrawVert& vertex : drawList->VtxBuffer) {
            const ImU32 alpha = (vertex.col >> IM_COL32_A_SHIFT) & 0xFFu;
            const auto faded = static_cast<ImU32>(static_cast<float>(alpha) * kCreationFadeAlpha + 0.5f);
            vertex.col = (vertex.col & ~IM_COL32_A_MASK) | (faded << IM_COL32_A_SHIFT);
        }
    }
    EndScreenLayer();
    // Above the canvas layer, which was created before it: both carry
    // NoBringToFrontOnFocus, so each went in at the back on creation.
    BringToFront("##sz_items_layer");

    // The text editor, while an item's note is being edited: a real ImGui
    // widget, so a window of its own, above every item rather than just
    // its own - it takes keystrokes and shows a caret, and has to be seen
    // while it does, whatever overlaps the item.
    if (editingItem != nullptr) {
        RenderNoteEditor(*editingItem, editingMin, editingMax);
    }

    RenderDock(displayW, displayH);
}


// One walk over the current canvas, answering what a screen point is on -
// see PointerTarget for what each answer is for:
//  - the target: the frontmost thing that would take a press there - a
//    selection bar button, a selected snippet's handle, or an item's body;
//  - `body`: the frontmost item whose content rect holds the point.
// The selection's furniture is drawn over every snippet, so it is asked
// first, the bar before the handles and the snippet selected last before
// the others - the same order it is painted in. It exists only while the
// selection is live (see SelectionLive), and never on a fullscreen
// snippet, which has no handles.
PointerTarget OverlayApp::ResolvePointerTarget(float x, float y) const {
    PointerTarget target;
    const Canvas* canvasPtr = Manager().CurrentOrNull();
    if (!canvasPtr) {
        return target;  // no canvas, so nothing anywhere to hit
    }
    const Canvas& canvas = *canvasPtr;
    for (size_t revIdx = canvas.items.size(); revIdx-- > 0;) {
        const Item& item = canvas.items[revIdx];
        // A minimized item keeps its last on-screen rect (only RenderItems'
        // own skip-if-minimized stops it from actually rendering there -
        // see Item::minimized's own doc comment), so it has to be skipped
        // here too, or a stroke over the patch it last covered would go
        // into the minimized item instead of whatever is visible there. A
        // deleted one is nowhere.
        if (item.minimized || Manager().IsDeleted(canvas, item)) {
            continue;
        }
        const Rect& r = item.rect;
        // Closed on the far edges, unlike the furniture's own rects: the
        // item's outermost pixel column is drawable, and a stroke started
        // exactly there has always landed in the item.
        if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
            target.body = item.id;
            break;
        }
    }
    if (SelectionLive() && !selection_.empty()) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (const std::optional<Rect> bounds = SelectionBounds()) {
            const std::vector<ChromeButton> buttons = BarButtons();
            const BarLayout bar = LayoutBar(*bounds, display.x, display.y, buttons.size());
            for (const ChromeButton button : buttons) {
                if (BarButtonRect(bar, buttons, button).Contains(x, y)) {
                    target.kind = PointerTarget::Kind::Button;
                    target.button = button;
                    return target;
                }
            }
        }
        for (auto it = selection_.rbegin(); it != selection_.rend(); ++it) {
            const Item* item = nullptr;
            for (const Item& candidate : canvas.items) {
                if (candidate.id == *it) {
                    item = &candidate;
                    break;
                }
            }
            if (item == nullptr || item->isFullscreen) {
                continue;
            }
            for (const HandleSpec& h : HandleSpecs(item->rect)) {
                if (HandleHitRect(h.center).Contains(x, y)) {
                    target.kind = PointerTarget::Kind::Handle;
                    target.item = item->id;
                    target.handle = h.handle;
                    return target;
                }
            }
        }
    }
    if (target.body.has_value()) {
        target.kind = PointerTarget::Kind::Body;
        target.item = *target.body;
    }
    return target;
}

// An item's content, the stroke being drawn into it, and its border, into
// `drawList` at the item's own (unrounded) rect.
void OverlayApp::PaintItemBody(ImDrawList* drawList, const Item& item, const Canvas& canvas, bool drawing,
                               bool highlighted, bool isFrontmost) {
    const ImVec2 pMin(item.rect.x, item.rect.y);
    const ImVec2 pMax(item.rect.x + item.rect.w, item.rect.y + item.rect.h);
    drawList->PushClipRect(pMin, pMax, true);

    DrawItemContent(drawList, item, pMin, pMax, Cfg().strokeRenderMode, StrokeRasterTextureFor(item.id),
                    /*skipNoteText=*/editingNoteItemId_ == item.id, CanvasMeshSlot(), PictureSampling());

    if (drawing) {
        // The stroke currently being drawn (not yet baked into
        // item.strokes - see Session::CommitLiveStroke,
        // which only runs on mouse-up) is screen-space, same as
        // item.strokes' own draw call just above but without the
        // native->screen scale factors. Drawn here, after this item's
        // own fill/strokes, rather than in the separate background
        // canvas layer behind every item: a Shot item's opaque
        // gradient/image fill would otherwise completely hide it until
        // the stroke finished, since that layer sits behind items in
        // z-order - a Drawing item's fill-less background just
        // happened to let it show through regardless, which is what
        // made this easy to miss.
        for (const Stroke& stroke : canvas.liveLayer.Strokes()) {
            DrawStroke(drawList, stroke, LiveStrokeRenderMode(), 0.0f, 0.0f, 1.0f, 1.0f);
        }
        if (canvas.liveLayer.ActiveStroke().has_value()) {
            DrawStroke(drawList, *canvas.liveLayer.ActiveStroke(), LiveStrokeRenderMode(), 0.0f, 0.0f, 1.0f, 1.0f);
        }
    }
    // Cut, and waiting for the paste that will move it: faded where it
    // stands, the way a file manager fades a file that has been cut. A
    // cut takes nothing away until it is pasted (see clipboard_), so
    // without this it looks like the key did nothing at all.
    if (IsWaitingToBeCut(item.id)) {
        drawList->AddRectFilled(pMin, pMax, IM_COL32(14, 16, 20, 130));
    }
    drawList->PopClipRect();

    // Border marking the item's bounds - drawn on the items layer with the
    // rest of the item, not the foreground list, where it would paint over
    // the Overview, popovers and another item's handles and leave no way to
    // tell what is in front of what. On the layer it is painted in canvas.items order
    // with everything else of the item, so a later item covers it, and
    // every window that can sit above an item (popovers, the canvas bar,
    // the dock, Overview) is created later in the same OnFrame pass and so
    // naturally paints over it where they overlap. Zero rounding to exactly
    // match the item's own sharp-cornered content rect.
    //
    // The color says which snippet is in front (see
    // AppConfig::itemBorderColorFrontRGBA): the frontmost one gets its own,
    // every other one the resting color - and that one only while
    // AppConfig::showItemBorders is set (on by default), which is what
    // makes a still-empty Drawing item, otherwise rendering nothing at all,
    // visible before it is touched.
    //
    // The *thickness* says where the pointer is: whichever item is
    // highlighted - hovered, or mid-drag/resize/drawing via the sticky
    // override in RenderItems - gets a heavier line in whichever color it
    // was already wearing. Two cues in two channels, deliberately: a
    // brighter color for hover is the channel depth uses, and a hovered
    // snippet at the back would look exactly like the one on top.
    // There's no separate tier for the item being drawn into: arming is
    // just "the cursor happens to be over this item while a tool is
    // selected", so that item is, definitionally, the hovered one. Nothing
    // goes below 2px - on this backend a 1px-thick AddRect rendered nothing
    // at all, while 2px rendered reliably at any alpha.
    //
    // Inset by half the stroke's own thickness on every side - an AddRect
    // stroke is centered on the coordinates it's given, so drawing it at
    // pMin/pMax directly would bleed past them on the outside.
    //
    // A pinned snippet wears a third color instead, and always has a
    // border (see AppConfig::itemBorderColorPinnedRGBA): pins are acted on
    // only when the overlay is put away, so this is what says in edit mode
    // which snippets will stay behind.
    if (item.pinned || isFrontmost || Cfg().showItemBorders || highlighted) {
        const float thickness = PxWhole(highlighted ? 3.0f : 2.0f);
        const float half = thickness * 0.5f;
        const ImVec2 borderMin(pMin.x + half, pMin.y + half);
        const ImVec2 borderMax(pMax.x - half, pMax.y - half);
        const uint32_t colorRGBA = item.pinned    ? Cfg().itemBorderColorPinnedRGBA
                                   : isFrontmost ? Cfg().itemBorderColorFrontRGBA
                                                 : Cfg().itemBorderColorOtherRGBA;
        drawList->AddRect(borderMin, borderMax, ToImColor(colorRGBA), 0.0f, thickness, ImDrawFlags_None);
    }
}

// What a selected snippet wears, drawn: an accent outline over its own
// border, and its eight handles - white squares with an accent edge, so
// they read on a dark screenshot and on a pale note alike. A fullscreen
// snippet has nowhere to be resized to and gets the outline alone. In
// drawing mode the outline is heavier and wears a second, fainter line a
// few pixels out - a halo that says this one is open for drawing, and is
// what a double-click visibly changes. Nothing here takes input: which of
// it is under the pointer is ResolvePointerTarget's answer, and a press on
// it is HandleItemGesture's.
void OverlayApp::PaintSelectionOutline(ImDrawList* drawList, const Item& item, bool drawing) {
    const ImVec2 pMin(std::round(item.rect.x), std::round(item.rect.y));
    const ImVec2 pMax(std::round(item.rect.x + item.rect.w), std::round(item.rect.y + item.rect.h));
    const float outline = PxWhole(drawing ? 3.0f : 2.0f);
    drawList->AddRect(ImVec2(pMin.x + outline * 0.5f, pMin.y + outline * 0.5f),
                      ImVec2(pMax.x - outline * 0.5f, pMax.y - outline * 0.5f), theme::AccentU32(), 0.0f, outline);
    if (drawing) {
        const float haloGap = PxWhole(4.0f);
        drawList->AddRect(ImVec2(pMin.x - haloGap, pMin.y - haloGap), ImVec2(pMax.x + haloGap, pMax.y + haloGap),
                          theme::AccentU32(120), 0.0f, PxWhole(2.0f));
    }
    if (item.isFullscreen) {
        return;
    }
    for (const HandleSpec& h : HandleSpecs(item.rect)) {
        const HitRect rect = HandleDrawRect(h.center);
        drawList->AddRectFilled(rect.min, rect.max, ImGui::GetColorU32(theme::kWhite));
        const float edge = PxWhole(2.0f);
        drawList->AddRect(ImVec2(rect.min.x + edge * 0.5f, rect.min.y + edge * 0.5f),
                          ImVec2(rect.max.x - edge * 0.5f, rect.max.y - edge * 0.5f), theme::AccentU32(), 0.0f, edge);
    }
}

// The selection bar's buttons on their pill, looking exactly as ImGui's
// own buttons do: the same fills for
// rest, hover and press (see IconButton/DangerIconButton), drawn with the
// same RenderFrame ImGui::Button draws its own frame with, so the 1px
// border style.FrameBorderSize asks for is there too. Hover comes from the
// resolver (`hotButton`), and a press shows as pressed only while the
// pointer is still on it, the way a held Button does.
void OverlayApp::PaintSelectionBar(ImDrawList* drawList, const std::optional<ChromeButton>& hotButton) {
    const std::optional<Rect> bounds = SelectionBounds();
    const std::optional<ItemId> primaryId = PrimarySelection();
    if (!bounds.has_value() || !primaryId.has_value()) {
        return;
    }
    const Item* primary = Manager().FindItemAnywhere(*primaryId);
    if (primary == nullptr) {
        return;
    }
    // Pin is the one toggle, and so the one that can be "on": it shows as
    // on when every selected snippet is pinned, since that is what a press
    // would take back.
    bool allPinned = true;
    for (const ItemId id : selection_) {
        const Item* item = Manager().FindItemAnywhere(id);
        allPinned = allPinned && item != nullptr && item->pinned;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const std::vector<ChromeButton> buttons = BarButtons();
    if (buttons.empty()) {
        return;  // every button switched off: no pill either, not an empty one
    }
    const BarLayout bar = LayoutBar(*bounds, display.x, display.y, buttons.size());
    drawList->AddRectFilled(bar.min, bar.max, ImGui::GetColorU32(theme::kPanelBg), theme::kRadiusPill);
    drawList->AddRect(bar.min, bar.max, ImGui::GetColorU32(theme::kPanelBorderStrong), theme::kRadiusPill);

    for (const ChromeButton button : buttons) {
        const HitRect rect = BarButtonRect(bar, buttons, button);
        const bool hovered = hotButton == button;
        const bool held = GestureIf<BarPress>() != nullptr && GestureIf<BarPress>()->button == button;
        // Close is the one danger-red button; a pinned selection's Pin
        // wears the accent, the way a selected tool does - and on the
        // drawing bar, the tool in hand does. The rest are plain pills.
        const bool danger = button == ChromeButton::Close;
        const bool active = (button == ChromeButton::Pin && allPinned) ||
                            (button == ChromeButton::Pen && activeTool_ == Tool::Draw) ||
                            (button == ChromeButton::Eraser && activeTool_ == Tool::Erase) ||
                            (button == ChromeButton::Text && activeTool_ == Tool::Text);
        ImVec4 fill = danger ? theme::kDangerSoft : active ? theme::Accent() : theme::kFieldBg;
        if (held && hovered) {
            fill = danger ? theme::kDanger : theme::AccentHover();
        } else if (hovered) {
            fill = danger ? theme::kDanger : active ? theme::AccentHover() : theme::kHoverWash;
        }
        ImGui::RenderFrame(rect.min, rect.max, ImGui::GetColorU32(fill), true, theme::kRadiusPill);

        if (button == ChromeButton::Color) {
            // The color itself, as a swatch, ringed in white so a dark
            // color reads on the pill.
            const ImVec2 center((rect.min.x + rect.max.x) * 0.5f, (rect.min.y + rect.max.y) * 0.5f);
            const float swatchRadius = Px(7.0f);
            drawList->AddCircleFilled(center, swatchRadius, ToImColor(drawColorRGBA_));
            drawList->AddCircle(center, swatchRadius, ImGui::GetColorU32(theme::kWhite), 0, Px(1.5f));
            if (hovered) {
                ImGui::SetTooltip("%s", strings::kBarColorTip);
            }
            continue;
        }

        const Icon* icon = nullptr;
        const char* tooltip = nullptr;
        switch (button) {
            // The pen's and the eraser's icons show the shape they are
            // cycled to (see ActivateBarButton), so the button reads as
            // what a drag will make.
            case ChromeButton::Pen:
                icon = penShape_ == DrawShape::Line        ? &icons::kLine
                       : penShape_ == DrawShape::Rectangle ? &icons::kRectangle
                                                           : &icons::kPen;
                tooltip = activeTool_ != Tool::Draw                ? strings::kBarPenTip
                          : penShape_ == DrawShape::Line          ? strings::kBarLineTip
                          : penShape_ == DrawShape::Rectangle     ? strings::kBarRectangleTip
                                                                  : strings::kBarPenAgainTip;
                break;
            case ChromeButton::Eraser:
                icon = eraserShape_ == DrawShape::Rectangle ? &icons::kEraserRect : &icons::kEraser;
                tooltip = activeTool_ != Tool::Erase                 ? strings::kBarEraserTip
                          : eraserShape_ == DrawShape::Rectangle ? strings::kBarEraserRectTip
                                                                 : strings::kBarEraserAgainTip;
                break;
            case ChromeButton::Text:
                icon = &icons::kType;
                tooltip = strings::kToolTextTip;
                break;
            case ChromeButton::Color:
                break;  // drawn above
            case ChromeButton::Close:
                icon = &icons::kX;
                tooltip = strings::kItemClose;
                break;
            case ChromeButton::Maximize:
                // One button, icon/action/tooltip swapping on the primary's
                // isFullscreen.
                icon = primary->isFullscreen ? &icons::kRestore : &icons::kMaximize;
                tooltip = primary->isFullscreen ? strings::kItemRestoreSize : strings::kItemMakeFullscreen;
                break;
            case ChromeButton::Minimize:
                icon = &icons::kMinimize;
                tooltip = strings::kItemMinimize;
                break;
            case ChromeButton::More:
                icon = &icons::kMoreVertical;
                tooltip = strings::kItemMoreActions;
                break;
            case ChromeButton::Pin:
                icon = &icons::kPin;
                tooltip = allPinned ? strings::kItemUnpin : strings::kItemPin;
                break;
        }
        if (icon == nullptr) {
            continue;
        }
        const float iconSize = Px(13.0f);
        const ImVec2 iconPos((rect.min.x + rect.max.x - iconSize) * 0.5f, (rect.min.y + rect.max.y - iconSize) * 0.5f);
        DrawIcon(drawList, *icon, iconPos, iconSize, ImGui::GetColorU32(active ? theme::AccentInk() : theme::kWhite));
        if (hovered) {
            ImGui::SetTooltip("%s", tooltip);
        }
    }
}

// What a selection bar button does, on the release that completes its
// press (see HandleItemGesture). Runs from the raw mouse pipeline, between
// frames, so nothing here has to be deferred past an item loop that is
// iterating canvas.items by index.
void OverlayApp::ActivateBarButton(ChromeButton button) {
    const std::optional<ItemId> primaryId = PrimarySelection();
    if (!primaryId.has_value()) {
        return;
    }
    switch (button) {
        case ChromeButton::Close:
            DeleteSelection();
            break;
        case ChromeButton::Maximize:
            ToggleFullscreenUndoably(*primaryId, /*stretch=*/false);
            break;
        case ChromeButton::Minimize:
            for (const ItemId id : selection_) {
                Item* item = Manager().FindItemAnywhere(id);
                if (item == nullptr) {
                    continue;
                }
                item->minimized = true;
                Manager().MarkChanged();
            }
            // Off the screen, so out of the selection - PruneSelection would
            // do it next frame; doing it now keeps the bar from showing
            // over nothing for a frame.
            ClearSelection();
            break;
        case ChromeButton::Pin: {
            // Nothing happens on screen until the overlay is put away - see
            // TrayController::PutAway, which is where a pin is acted on.
            // Not an undo step, the same as Minimize: it changes where the
            // snippet is shown, not what it holds. One press pins the whole
            // selection, or unpins it when every one of it is pinned.
            bool allPinned = true;
            for (const ItemId id : selection_) {
                const Item* item = Manager().FindItemAnywhere(id);
                allPinned = allPinned && item != nullptr && item->pinned;
            }
            for (const ItemId id : selection_) {
                if (Item* item = Manager().FindItemAnywhere(id)) {
                    item->pinned = !allPinned;
                }
            }
            Manager().MarkChanged();
            break;
        }
        case ChromeButton::More: {
            // A request flag rather than ImGui::OpenPopup directly - see
            // colorChooserRequested_'s own doc comment: this runs outside
            // any frame, with no current window for a popup to be scoped
            // to.
            itemPropertiesPopoverItemId_ = primaryId;
            itemPropertiesPopoverRequested_ = true;
            // The button's own bottom-right corner, a little below it -
            // paired with the pivot RenderItemPropertiesPopover opens with,
            // so the popover's right edge (not its left) tracks this point
            // regardless of how wide it ends up being, since the bar can
            // sit near the screen's right edge.
            if (const std::optional<ImVec2> center = SelectionBarButtonCenter(ChromeButton::More)) {
                itemPropertiesPopoverAnchor_ =
                    ImVec2(center->x + Px(kBarButtonSize) * 0.5f, center->y + Px(kBarButtonSize) * 0.5f + Px(6.0f));
            }
            break;
        }
        // The drawing bar: the tool to draw with, and the color. The tool
        // already in hand is cycled through its shapes instead - pen, line,
        // rectangle; eraser, rectangle eraser - so a plain drag makes them,
        // for a hand with no modifier key to hold (see penShape_).
        case ChromeButton::Pen:
            if (activeTool_ != Tool::Draw) {
                PickTool(Tool::Draw);
            } else {
                penShape_ = penShape_ == DrawShape::Freehand ? DrawShape::Line
                            : penShape_ == DrawShape::Line   ? DrawShape::Rectangle
                                                             : DrawShape::Freehand;
            }
            break;
        case ChromeButton::Eraser:
            if (activeTool_ != Tool::Erase) {
                PickTool(Tool::Erase);
            } else {
                eraserShape_ = eraserShape_ == DrawShape::Rectangle ? DrawShape::Freehand : DrawShape::Rectangle;
            }
            break;
        case ChromeButton::Text:
            PickTool(Tool::Text);
            break;
        case ChromeButton::Color:
            // The chooser opens next to the button - asked for here, opened
            // on the next frame (see OpenColorChooser).
            if (const std::optional<ImVec2> center = SelectionBarButtonCenter(ChromeButton::Color)) {
                OpenColorChooser(*center);
            }
            break;
    }
}

// Which edges the handle moves - a corner two, an edge one.
void OverlayApp::ResizeHandleEdges(ResizeHandle handle, bool& left, bool& right, bool& top, bool& bottom) {
    left = handle == ResizeHandle::NW || handle == ResizeHandle::SW || handle == ResizeHandle::W;
    right = handle == ResizeHandle::NE || handle == ResizeHandle::SE || handle == ResizeHandle::E;
    top = handle == ResizeHandle::NW || handle == ResizeHandle::NE || handle == ResizeHandle::N;
    bottom = handle == ResizeHandle::SW || handle == ResizeHandle::SE || handle == ResizeHandle::S;
}

// Text editing (Tool::Text, any item - see its own doc comment): the one
// piece of an item that is a genuine ImGui widget, because it has to
// receive keystrokes and a caret, which nothing drawn can. Positioned
// exactly over the content rect, both fullscreen and windowed (pMin/pMax
// are already correct for either case), so editing works the same
// regardless of mode - deliberately the *whole* content rect rather than
// just the compact caption band the read-only look (DrawItemContent)
// renders once editing ends, so there's room to actually write more than
// one short line comfortably. Being an input-capturing window, it makes
// io.WantCaptureMouse true over the item while editing, which is what
// keeps a stroke from starting under the caret.
namespace {
// ImGuiInputTextFlags_CallbackResize's contract, against a std::string:
// the widget reports the length it needs, the string is resized to hold
// it, and the widget is pointed at the (possibly moved) storage.
int ResizeStringForInputText(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}
}  // namespace

void OverlayApp::RenderNoteEditor(Item& item, ImVec2 pMin, ImVec2 pMax) {
    constexpr ImGuiWindowFlags kNoteWindowFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    char noteWinName[40];
    std::snprintf(noteWinName, sizeof(noteWinName), "##noteedit%llu", static_cast<unsigned long long>(item.id));
    ImGui::SetNextWindowPos(pMin);
    ImGui::SetNextWindowSize(ImVec2(pMax.x - pMin.x, pMax.y - pMin.y));
    // WindowPadding is theme::kNoteTextPad, not an arbitrary value - this
    // has to land the widget's content area at exactly the same offset
    // DrawItemContent's read-only band uses (see kNoteTextPad's own doc
    // comment), or the text visibly jumps the moment editing starts or
    // ends.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kNoteTextPad, theme::kNoteTextPad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    // No backing panel while editing either - transparent background is
    // the point (see Item::noteText's own doc comment), the same as the
    // read-only band; this window is NoBackground and nothing here fills
    // it.
    ImGui::Begin(noteWinName, nullptr, kNoteWindowFlags);
    ImGui::PopStyleVar(3);
    // The item's own text size and color (see Item::noteTextSizePx /
    // noteTextColorRGBA), pushed around the whole editor so what's being
    // typed matches what DrawItemContent will paint the instant editing
    // ends - same reason theme::kNoteTextPad is shared between the two.
    // PushFont(nullptr, size) keeps the current font and only changes its
    // size. The value is a *base* size, which ImGui multiplies by the
    // interface scale - and a note's text is content, drawn at its own
    // size whatever the interface scale (DrawItemContent passes it to
    // AddText as it is), so the scale is divided back out here.
    ImGui::PushFont(nullptr, item.noteTextSizePx / UiScale());
    ImGui::PushStyleColor(ImGuiCol_Text, ToImColor(item.noteTextColorRGBA));
    if (noteEditJustFocused_) {
        ImGui::SetKeyboardFocusHere();
        noteEditJustFocused_ = false;
    }
    // Snapshotted *before* the widget call below, not read back out of
    // noteEditBuffer_ afterwards - InputTextMultiline reverts its own
    // buffer internally when Escape is pressed, in the same call that
    // reports the resulting deactivation, so by the time that call returns
    // on an Escape frame noteEditBuffer_ already holds the stale
    // pre-edit-session text again. This snapshot is always exactly what
    // the user had typed as of the start of this frame, which is what
    // EndEditingNote below actually needs to commit - see its own doc
    // comment. FrameBg is cleared here too, scoped to just this one
    // widget, so no default input-field fill shows through the transparent
    // window behind it either.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    const std::string preCallBuffer(noteEditBuffer_);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // The widget edits the string's own storage and asks for more through
    // the resize callback - what ImGui's misc/cpp/imgui_stdlib does, done
    // here so the std::string is the buffer rather than a fixed array that
    // would cut a long note off at its end.
    ImGui::InputTextMultiline("##notetext", noteEditBuffer_.data(), noteEditBuffer_.capacity() + 1, avail,
                              ImGuiInputTextFlags_CallbackResize, &ResizeStringForInputText, &noteEditBuffer_);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemDeactivated()) {
        // Covers both a real commit (Enter/Tab/click-away after typing)
        // and Escape - Escape here means "stop editing," not "undo my
        // typing" (the app's own Undo covers that), so both paths commit;
        // only the source of the text differs (see preCallBuffer's own
        // comment above).
        EndEditingNote(preCallBuffer);
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::End();
    BringToFront(noteWinName);
}

// One square thumbnail tile per minimized item on the current canvas (see
// Item::minimized) - a bottom-center dock, with no setting for its place
// yet. Every tile always shows a border, deliberately, even though a
// blank Drawing's content alone would otherwise render as nothing at all -
// the border is what tells the user there's an item there to restore, not
// the thumbnail content, which the user's own request specifically didn't
// want carrying a type badge/label of any kind (name is tooltip-only).
// Click a tile to restore it (bring it back exactly where it was, and to
// the front) - a no-op (renders nothing) if nothing's minimized.
void OverlayApp::RenderDock(float displayW, float displayH) {
    Canvas* canvasPtr = Manager().CurrentOrNull();
    if (!canvasPtr) {
        return;  // nothing can be minimized when there's no canvas
    }
    Canvas& canvas = *canvasPtr;
    std::vector<ItemId> minimizedIds;
    for (const Item& item : canvas.items) {
        if (item.minimized && !Manager().IsDeleted(canvas, item)) {
            minimizedIds.push_back(item.id);
        }
    }
    if (minimizedIds.empty()) {
        return;
    }

    const float chipSize = Px(56.0f);
    const float gap = Px(8.0f);
    const float bottomMargin = Px(16.0f);
    const float totalW =
        static_cast<float>(minimizedIds.size()) * chipSize + static_cast<float>(minimizedIds.size() - 1) * gap;
    // Above whatever is out on the bottom edge - the canvas bar, a strip
    // docked there - rather than under it.
    const float chipsBottom = std::min(displayH - bottomMargin, bottomPanelsTop_ - gap);
    const ImVec2 dockMin((displayW - totalW) * 0.5f, chipsBottom - chipSize);

    ImGui::SetNextWindowPos(dockMin);
    ImGui::SetNextWindowSize(ImVec2(totalW, chipSize));
    ImGui::Begin("##dock", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::optional<ItemId> restoreId;
    for (size_t i = 0; i < minimizedIds.size(); ++i) {
        const Item* item = nullptr;
        for (const Item& it : canvas.items) {
            if (it.id == minimizedIds[i]) {
                item = &it;
                break;
            }
        }
        if (item == nullptr) {
            continue;
        }
        const ImVec2 chipMin(dockMin.x + static_cast<float>(i) * (chipSize + gap), dockMin.y);
        const ImVec2 chipMax(chipMin.x + chipSize, chipMin.y + chipSize);
        dl->AddRectFilled(chipMin, chipMax, ImGui::ColorConvertFloat4ToU32(theme::kPanelBg), Px(theme::kRadiusSm));

        // The item's own content, letterboxed/centered to preserve its
        // aspect ratio within the square tile (same fitting math as a
        // fullscreen item's own aspect-preserving mode - see
        // FitAspectRatioIntoViewport) rather than stretched to fill it -
        // falls back to filling the whole tile for a not-yet-sized item
        // (nativeW/H still 0).
        const float aspect = (item->nativeW > 0.0f && item->nativeH > 0.0f) ? item->nativeW / item->nativeH : 1.0f;
        const Rect fitted = FitAspectRatioIntoViewport(aspect, chipSize, chipSize);
        const ImVec2 contentMin(chipMin.x + fitted.x, chipMin.y + fitted.y);
        const ImVec2 contentMax(contentMin.x + fitted.w, contentMin.y + fitted.h);
        dl->PushClipRect(chipMin, chipMax, true);
        // Safe to share the canvas's own cache although a chip draws at a
        // very different scale: RenderItems skips minimized items and this
        // draws only those, so no item is ever in both in one frame - see
        // strokeMeshCache_'s own doc comment.
        DrawItemContent(dl, *item, contentMin, contentMax, Cfg().strokeRenderMode, StrokeRasterTextureFor(item->id),
                         /*skipNoteText=*/false, CanvasMeshSlot(), PictureSampling());
        dl->PopClipRect();

        dl->AddRect(chipMin, chipMax, ImGui::ColorConvertFloat4ToU32(theme::kPanelBorderStrong), Px(theme::kRadiusSm));

        char btnId[32];
        std::snprintf(btnId, sizeof(btnId), "##dockchip%llu", static_cast<unsigned long long>(item->id));
        ImGui::SetCursorScreenPos(chipMin);
        if (ImGui::InvisibleButton(btnId, ImVec2(chipSize, chipSize))) {
            restoreId = item->id;
        }
        if (ImGui::IsItemHovered()) {
            const std::string label = item->name.empty() ? strings::kMoveCopyItemWord : item->name;
            ImGui::SetTooltip("Restore \"%s\"", label.c_str());
        }
    }
    ImGui::End();
    BringToFront("##dock");

    if (restoreId.has_value()) {
        for (Item& it : canvas.items) {
            if (it.id == *restoreId) {
                it.minimized = false;
                Manager().MarkChanged();
                break;
            }
        }
    }
}


}  // namespace sz::ui
