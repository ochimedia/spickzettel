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

// The pointer a resize handle wears.
ImGuiMouseCursor ResizeHandleCursor(ResizeHandle handle) {
    switch (handle) {
        case ResizeHandle::NW:
        case ResizeHandle::SE:
            return ImGuiMouseCursor_ResizeNWSE;
        case ResizeHandle::NE:
        case ResizeHandle::SW:
            return ImGuiMouseCursor_ResizeNESW;
        case ResizeHandle::N:
        case ResizeHandle::S:
            return ImGuiMouseCursor_ResizeNS;
        case ResizeHandle::E:
        case ResizeHandle::W:
            return ImGuiMouseCursor_ResizeEW;
    }
    return ImGuiMouseCursor_Arrow;
}

ImVec2 Im(platform::Vec2 v) { return ImVec2(v.x, v.y); }
}  // namespace

std::optional<ImVec2> OverlayApp::SelectionBarButtonCenter(ChromeButton button) const {
    const std::optional<platform::Vec2> center = editor_.SelectionBarButtonCenter(button);
    return center.has_value() ? std::optional<ImVec2>(Im(*center)) : std::nullopt;
}

void OverlayApp::RenderItems(float displayW, float displayH) {
    // Reset every frame - see the member's own doc comment. Cleared up
    // front so a frame where the mouse has moved off every handle (or
    // there are no items at all) shows "none" rather than whatever handle
    // happened to be hovered last.
    debugHoveredResizeHandle_.clear();

    // A note edit ended by a command from elsewhere - the session ends
    // whatever gesture is open before any other (see
    // Session::EndOpenGesture) - has its editor put away with it, rather
    // than typing on into an edit that is over.
    editor_.ForgetNoteEditEndedElsewhere();
    const Canvas* canvasPtr = Manager().CurrentOrNull();
    if (!canvasPtr) {
        return;  // no canvas, so no items to render
    }
    const Canvas& canvas = *canvasPtr;
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
    // (RecognizePress), so what lights up and what a click lands on are
    // one answer - this frame's - with no window hit-test of ImGui's in
    // between to be a frame late.
    const ImGuiIO& io = ImGui::GetIO();
    const PointerTarget target = editor_.ResolvePointerTarget(io.MousePos.x, io.MousePos.y);
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
    const Placement* held = editor_.Input().As<Placement>(Level::Gesture);
    const BarPress* pressed = editor_.Input().As<BarPress>(Level::Gesture);
    const bool resizing = held != nullptr && held->Resizing();
    const bool blocked = resizing || pressed != nullptr;
    const bool hotHandle =
        furnitureHot && target.kind == PointerTarget::Kind::Handle &&
        (!blocked || (resizing && held->Item() == target.item && held->Handle() == target.handle));
    std::optional<ChromeButton> hotButton;
    if (furnitureHot && target.kind == PointerTarget::Kind::Button &&
        (!blocked || (pressed != nullptr && pressed->Pressed() == target.button))) {
        hotButton = target.button;
    }

    // The debug overlay's readout of which handle is live, with a dragging
    // handle reported for as long as it drags, wherever the pointer is.
    if (held != nullptr && held->Handle().has_value()) {
        char handleDebug[64];
        std::snprintf(handleDebug, sizeof(handleDebug), "%s item=%llu (dragging)", ResizeHandleName(*held->Handle()),
                      static_cast<unsigned long long>(held->Item()));
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
    // front of it. The snippet Properties is up for gets the same
    // treatment, so the item a popover is currently showing for doesn't
    // lose its highlight the instant the mouse leaves its rect to go
    // interact with the popover instead.
    std::optional<ItemId> stickyItemId;
    if (held != nullptr) {
        stickyItemId = held->Item();
    } else if (PopupUp(PopupKind::ItemProperties)) {
        stickyItemId = popup_->item;
    }
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
    const Item* editingItem = nullptr;
    ImVec2 editingMin;
    ImVec2 editingMax;
    for (const Item& item : canvas.items) {
        if (item.minimized) {
            continue;  // shown in the dock instead - see RenderDock, called below
        }
        if (Manager().IsDeleted(canvas, item)) {
            continue;  // deleted: hidden until it is restored
        }
        PaintItemBody(drawList, item, editor_.DrawingItem() == item.id, highlightId == item.id, frontmostId == item.id);
        if (itemsInteractive && editor_.EditingNote() == item.id) {
            editingItem = &item;
            editingMin = ImVec2(std::round(item.rect.x), std::round(item.rect.y));
            editingMax = ImVec2(std::round(item.rect.x + item.rect.w), std::round(item.rect.y + item.rect.h));
        }
    }
    // The selection, over every snippet - the primary last, so where two
    // selected snippets overlap the one selected last has its handles on
    // top, which is the one the resolver finds first.
    if (editor_.SelectionLive()) {
        for (const ItemId id : editor_.Selection()) {
            for (const Item& item : canvas.items) {
                if (item.id == id) {
                    PaintSelectionOutline(drawList, item, editor_.DrawingItem() == id);
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
    if (const BoxSelect* selecting = editor_.Input().As<BoxSelect>(Level::Gesture); selecting != nullptr) {
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
    // itself never sees the overlay (see IOverlayWindow::CaptureRegion).
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



// An item's content, the stroke being drawn into it, and its border, into
// `drawList` at the item's own (unrounded) rect.
void OverlayApp::PaintItemBody(ImDrawList* drawList, const Item& item, bool drawing,
                               bool highlighted, bool isFrontmost) {
    const ImVec2 pMin(item.rect.x, item.rect.y);
    const ImVec2 pMax(item.rect.x + item.rect.w, item.rect.y + item.rect.h);
    drawList->PushClipRect(pMin, pMax, true);

    DrawItemContent(drawList, item, pMin, pMax, Cfg().strokeRenderMode, PictureTexture(item),
                    StrokeRasterTextureFor(item.id), /*skipNoteText=*/editor_.EditingNote() == item.id, CanvasMeshSlot(),
                    PictureSampling());

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
        const CanvasState& live = session_.LiveLayer();
        for (const Stroke& stroke : live.Strokes()) {
            DrawStroke(drawList, stroke, LiveStrokeRenderMode(), 0.0f, 0.0f, 1.0f, 1.0f);
        }
        if (live.ActiveStroke().has_value()) {
            DrawStroke(drawList, *live.ActiveStroke(), LiveStrokeRenderMode(), 0.0f, 0.0f, 1.0f, 1.0f);
        }
    }
    // Cut, and waiting for the paste that will move it: faded where it
    // stands, the way a file manager fades a file that has been cut. A
    // cut takes nothing away until it is pasted (see Editor::IsWaitingToBeCut), so
    // without this it looks like the key did nothing at all.
    if (editor_.IsWaitingToBeCut(item.id)) {
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
// it is the recognizer's (RecognizePress).
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
        drawList->AddRectFilled(Im(rect.min), Im(rect.max), ImGui::GetColorU32(theme::kWhite));
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
    const std::optional<Rect> bounds = editor_.SelectionBounds();
    const std::optional<ItemId> primaryId = editor_.PrimarySelection();
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
    for (const ItemId id : editor_.Selection()) {
        const Item* item = Manager().FindItemAnywhere(id);
        allPinned = allPinned && item != nullptr && item->pinned;
    }

    const std::vector<ChromeButton> buttons = editor_.BarButtons();
    if (buttons.empty()) {
        return;  // every button switched off: no pill either, not an empty one
    }
    const BarLayout bar = LayoutBar(*bounds, editor_.DisplayWidth(), editor_.DisplayHeight(), buttons.size());
    drawList->AddRectFilled(Im(bar.min), Im(bar.max), ImGui::GetColorU32(theme::kPanelBg), theme::kRadiusPill);
    drawList->AddRect(Im(bar.min), Im(bar.max), ImGui::GetColorU32(theme::kPanelBorderStrong), theme::kRadiusPill);

    for (const ChromeButton button : buttons) {
        const HitRect rect = BarButtonRect(bar, buttons, button);
        const bool hovered = hotButton == button;
        const BarPress* pressed = editor_.Input().As<BarPress>(Level::Gesture);
        const bool held = pressed != nullptr && pressed->Pressed() == button;
        // Close is the one danger-red button; a pinned selection's Pin
        // wears the accent, the way a selected tool does - and on the
        // drawing bar, the tool in hand does. The rest are plain pills.
        const bool danger = button == ChromeButton::Close;
        const bool active = (button == ChromeButton::Pin && allPinned) ||
                            (button == ChromeButton::Pen && editor_.ActiveTool() == Tool::Draw) ||
                            (button == ChromeButton::Eraser && editor_.ActiveTool() == Tool::Erase) ||
                            (button == ChromeButton::Text && editor_.ActiveTool() == Tool::Text);
        ImVec4 fill = danger ? theme::kDangerSoft : active ? theme::Accent() : theme::kFieldBg;
        if (held && hovered) {
            fill = danger ? theme::kDanger : theme::AccentHover();
        } else if (hovered) {
            fill = danger ? theme::kDanger : active ? theme::AccentHover() : theme::kHoverWash;
        }
        ImGui::RenderFrame(Im(rect.min), Im(rect.max), ImGui::GetColorU32(fill), true, theme::kRadiusPill);

        if (button == ChromeButton::Color) {
            // The color itself, as a swatch, ringed in white so a dark
            // color reads on the pill.
            const ImVec2 center((rect.min.x + rect.max.x) * 0.5f, (rect.min.y + rect.max.y) * 0.5f);
            const float swatchRadius = Px(7.0f);
            drawList->AddCircleFilled(center, swatchRadius, ToImColor(editor_.DrawColorRGBA()));
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
                icon = editor_.PenShape() == DrawShape::Line        ? &icons::kLine
                       : editor_.PenShape() == DrawShape::Rectangle ? &icons::kRectangle
                                                           : &icons::kPen;
                tooltip = editor_.ActiveTool() != Tool::Draw                ? strings::kBarPenTip
                          : editor_.PenShape() == DrawShape::Line          ? strings::kBarLineTip
                          : editor_.PenShape() == DrawShape::Rectangle     ? strings::kBarRectangleTip
                                                                  : strings::kBarPenAgainTip;
                break;
            case ChromeButton::Eraser:
                icon = editor_.EraserShape() == DrawShape::Rectangle ? &icons::kEraserRect : &icons::kEraser;
                tooltip = editor_.ActiveTool() != Tool::Erase                 ? strings::kBarEraserTip
                          : editor_.EraserShape() == DrawShape::Rectangle ? strings::kBarEraserRectTip
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

void OverlayApp::RenderNoteEditor(const Item& item, ImVec2 pMin, ImVec2 pMax) {
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
    if (editor_.TakeNoteEditJustBegun()) {
        ImGui::SetKeyboardFocusHere();
    }
    // Snapshotted *before* the widget call below, not read back out of
    // the editor's buffer afterwards - InputTextMultiline reverts its own
    // buffer internally when Escape is pressed, in the same call that
    // reports the resulting deactivation, so by the time that call returns
    // on an Escape frame the buffer already holds the stale
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
    const std::string preCallBuffer(editor_.NoteEditBuffer());
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // The widget edits the string's own storage and asks for more through
    // the resize callback - what ImGui's misc/cpp/imgui_stdlib does, done
    // here so the std::string is the buffer rather than a fixed array that
    // would cut a long note off at its end.
    std::string& buffer = editor_.NoteEditBuffer();
    const bool typed = ImGui::InputTextMultiline("##notetext", buffer.data(), buffer.capacity() + 1, avail,
                                                 ImGuiInputTextFlags_CallbackResize, &ResizeStringForInputText, &buffer);
    if (typed) {
        // The note is what has been typed so far - see
        // Session::PreviewText - so that whatever ends the edit keeps it.
        session_.PreviewText(buffer);
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemDeactivated()) {
        // Covers both a real commit (Enter/Tab/click-away after typing)
        // and Escape - Escape here means "stop editing," not "undo my
        // typing" (the app's own Undo covers that), so both paths commit;
        // only the source of the text differs (see preCallBuffer's own
        // comment above).
        editor_.EndEditingNote(preCallBuffer);
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
    const Canvas* canvasPtr = Manager().CurrentOrNull();
    if (!canvasPtr) {
        return;  // nothing can be minimized when there's no canvas
    }
    const Canvas& canvas = *canvasPtr;
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
        DrawItemContent(dl, *item, contentMin, contentMax, Cfg().strokeRenderMode, PictureTexture(*item),
                        StrokeRasterTextureFor(item->id), /*skipNoteText=*/false, CanvasMeshSlot(),
                        PictureSampling());
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
        session_.SetMinimized({*restoreId}, false);
    }
}


}  // namespace sz::ui
