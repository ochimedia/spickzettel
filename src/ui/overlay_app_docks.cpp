// The canvas bar: the panel that hides against the bottom edge of the
// screen and slides out when it is wanted.
//
// Where it is this frame, and how far out, is decided once, before
// anything is drawn - UpdateEdgePanels - so that the minimized snippets'
// chips that have to clear it and the bar's own window read the same
// answer.
#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/icons_generated.h"

#include <imgui.h>

namespace sz::ui {

using namespace overlay_detail;

namespace {
// How close to an edge the pointer has to come for what is docked there to
// slide out. Small, so that working near an edge doesn't keep summoning
// things; the edge itself is a target a pointer stops at without aiming.
constexpr float kRevealZonePx = 3.0f;
// How far past a panel the pointer may stray and still be pointing at it -
// enough not to lose it on the way to a button at its very edge.
constexpr float kHoverSlackPx = 16.0f;
// The bar's distance from its edge once out.
constexpr float kDockMarginPx = 10.0f;
// How long the bar comes out on its own: when the overlay comes up, so it
// is seen where it is; and whenever the canvas changes, so a switch shows
// where it landed.
constexpr double kShownFlashSeconds = 1.6;
constexpr double kCanvasChangeFlashSeconds = 1.2;

constexpr float kBarTileHeight = 64.0f;
constexpr float kBarPadding = 8.0f;
constexpr float kBarGap = 8.0f;
constexpr float kCanvasBarButtonSize = 28.0f;
// The bar never runs closer than this to the sides of the screen; past
// that its tiles scroll.
constexpr float kBarSideMarginPx = 96.0f;

// Starts and ends gently - a linear slide reads as mechanical.
float Ease(float t) { return t * t * (3.0f - 2.0f * t); }

bool Near(const std::optional<Rect>& rect, ImVec2 point, float slack) {
    return rect.has_value() && point.x >= rect->x - slack && point.x <= rect->x + rect->w + slack &&
           point.y >= rect->y - slack && point.y <= rect->y + rect->h + slack;
}

// A thumbnail is the screen in small, so it takes the display's shape -
// held to sensible bounds for very wide or tall ones.
float TileWidth(float displayW, float displayH) {
    const float aspect = displayH > 0.0f ? std::clamp(displayW / displayH, 1.0f, 2.4f) : 16.0f / 9.0f;
    return Px(kBarTileHeight) * aspect;
}
}  // namespace

void EdgeReveal::Update(bool wanted, double now, float deltaSeconds) {
    if (wanted) {
        lastWanted = now;
    }
    const bool out = wanted || now < holdUntil || now - lastWanted < kEdgeRevealLingerSeconds;
    const float step = deltaSeconds / kEdgeRevealSlideSeconds;
    amount = out ? std::min(1.0f, amount + step) : std::max(0.0f, amount - step);
}

std::vector<CanvasId> OverlayApp::CanvasBarCanvases() const {
    // The folder the *current canvas* is in, as Alt+wheel steps through -
    // not the one the Overview happens to be browsing.
    const Canvas* current = Manager().CurrentOrNull();
    const FolderId folder = current != nullptr ? current->folderId : Manager().CurrentFolderId();
    std::vector<CanvasId> ids;
    for (const Canvas& canvas : Manager().Canvases()) {
        if (canvas.folderId == folder && !Manager().IsDeleted(canvas)) {
            ids.push_back(canvas.id);
        }
    }
    return ids;
}

void OverlayApp::UpdateEdgePanels(float displayW, float displayH) {
    const ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    const AppConfig& cfg = Cfg();

    if (edgePanelsFlashPending_) {
        edgePanelsFlashPending_ = false;
        canvasBarReveal_.Flash(now, kShownFlashSeconds);
        canvasBarScrollToCurrent_ = true;
    }
    const CanvasId currentCanvas = Manager().CurrentCanvasId();
    if (canvasBarLastCanvas_ != currentCanvas) {
        if (canvasBarLastCanvas_.has_value()) {
            canvasBarReveal_.Flash(now, kCanvasChangeFlashSeconds);
        }
        canvasBarLastCanvas_ = currentCanvas;
        canvasBarScrollToCurrent_ = true;
    }


    // Wanted: the pointer at the bottom edge, or on the bar (last frame's
    // rect, which is where it was drawn). Not while the hand is busy with
    // something else - a stroke or a drag run into the bottom of the
    // screen must not pull the bar out from under it - and not while the
    // Overview is up, which covers it.
    const bool pointerKnown = ImGui::IsMousePosValid(&io.MousePos);
    const ImVec2 pointer = io.MousePos;
    const bool busy = PanelOpen() || !editor_.HandAtRest();
    const bool atBottom = pointerKnown && pointer.y >= displayH - Px(kRevealZonePx);
    const bool onBar = pointerKnown && Near(canvasBarRect_, pointer, Px(kHoverSlackPx));
    // And while a tile's context menu is up: the pointer has left the bar
    // for the menu, and a menu hanging over the panel it was opened from
    // having slid away would be a puzzle. The menu closes itself on the
    // click that chooses or dismisses it, so this cannot hold the bar out.
    const bool barWanted =
        cfg.showCanvasBar && !busy && (atBottom || onBar || canvasContextMenu_.IsOpen());
    canvasBarReveal_.Update(barWanted, now, io.DeltaTime);
    if (!cfg.showCanvasBar) {
        canvasBarReveal_ = EdgeReveal{};
    }

    // ----- The canvas bar: centered on the bottom edge -----
    canvasBarRect_.reset();
    float bottomTop = displayH;  // the top of whatever is out on the bottom edge
    if (cfg.showCanvasBar && canvasBarReveal_.amount > 0.0f && !PanelOpen()) {
        const size_t count = CanvasBarCanvases().size();
        const float tiles = count == 0 ? 0.0f
                                       : static_cast<float>(count) * TileWidth(displayW, displayH) +
                                             static_cast<float>(count - 1) * Px(kBarGap);
        // Two buttons at the right end: a new canvas, and the Overview.
        const float buttons = Px(kCanvasBarButtonSize) * 2.0f + Px(kBarGap);
        const float content = Px(kBarPadding) * 2.0f + tiles + (count > 0 ? Px(kBarGap) : 0.0f) + buttons;
        const float narrowest = Px(kBarPadding) * 4.0f + buttons + TileWidth(displayW, displayH);
        const float width = std::min(content, std::max(narrowest, displayW - Px(kBarSideMarginPx) * 2.0f));
        const float height = Px(kBarTileHeight) + Px(kBarPadding) * 2.0f;
        const float outY = displayH - Px(kDockMarginPx) - height;
        const float y = displayH + (outY - displayH) * Ease(canvasBarReveal_.amount);
        canvasBarRect_ = Rect{(displayW - width) * 0.5f, y, width, height};
        bottomTop = y;
    }

    bottomPanelsTop_ = bottomTop;
}

void OverlayApp::RenderCanvasBar(float displayW, float displayH) {
    if (!canvasBarRect_.has_value()) {
        return;
    }
    const Rect bar = *canvasBarRect_;
    const ImGuiIO& io = ImGui::GetIO();
    const std::vector<CanvasId> ids = CanvasBarCanvases();
    const CanvasId currentId = Manager().CurrentCanvasId();
    const float tileW = TileWidth(displayW, displayH);

    ImGui::SetNextWindowPos(ImVec2(bar.x, bar.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(bar.w, bar.h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(theme::kRadiusMd));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme::kPanelBorder);
    ImGui::Begin("##canvas_bar", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The tiles, before the buttons, scrolled when there are more than fit.
    // The wheel over the bar scrolls it (Alt+wheel still switches canvas,
    // as it does everywhere); a change of canvas brings the current one
    // into view.
    const float regionMinX = bar.x + Px(kBarPadding);
    const float regionMaxX = bar.x + bar.w - Px(kBarPadding) - (Px(kCanvasBarButtonSize) * 2.0f + Px(kBarGap)) - Px(kBarGap);
    const float regionW = std::max(0.0f, regionMaxX - regionMinX);
    const float contentW =
        ids.empty() ? 0.0f : static_cast<float>(ids.size()) * tileW + static_cast<float>(ids.size() - 1) * Px(kBarGap);
    const float maxScroll = std::max(0.0f, contentW - regionW);
    if (canvasBarScrollToCurrent_) {
        canvasBarScrollToCurrent_ = false;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] != currentId) {
                continue;
            }
            const float tileX = static_cast<float>(i) * (tileW + Px(kBarGap));
            if (tileX < canvasBarScroll_) {
                canvasBarScroll_ = tileX;
            } else if (tileX + tileW > canvasBarScroll_ + regionW) {
                canvasBarScroll_ = tileX + tileW - regionW;
            }
        }
    }
    if (ImGui::IsWindowHovered() && !io.KeyAlt && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
        canvasBarScroll_ -= (io.MouseWheel + io.MouseWheelH) * (tileW + Px(kBarGap));
    }
    canvasBarScroll_ = std::clamp(canvasBarScroll_, 0.0f, maxScroll);

    // The same thumbnails the Overview draws, from the same cache - the two
    // are never out at once (the bar stays in while the Overview is open).
    BeginOverviewPreviewFrame();
    const PreviewTextureFn previewTexture = PreviewTextureLookup();

    // Each tile's place among *all* its folder's canvases, deleted ones
    // included - what ReorderCanvas counts in, as the Overview's grid does.
    std::vector<size_t> placeInFolder(ids.size(), 0);
    {
        const Canvas* current = Manager().CurrentOrNull();
        const FolderId folder = current != nullptr ? current->folderId : Manager().CurrentFolderId();
        size_t place = 0;
        for (const Canvas& c : Manager().Canvases()) {
            if (c.folderId != folder) {
                continue;
            }
            const auto at = std::find(ids.begin(), ids.end(), c.id);
            if (at != ids.end()) {
                placeInFolder[static_cast<size_t>(at - ids.begin())] = place;
            }
            ++place;
        }
    }
    const bool dragging = ImGui::GetDragDropPayload() != nullptr;

    std::optional<CanvasId> clicked;
    std::optional<CanvasId> rightClicked;
    // A tile dragged onto another takes that one's place, the rest shifting
    // along - the order Alt+wheel walks. Applied after the loop, which
    // reads the list the move changes.
    std::optional<std::pair<CanvasId, size_t>> reorder;
    ImGui::PushClipRect(ImVec2(regionMinX, bar.y), ImVec2(regionMaxX, bar.y + bar.h), true);
    for (size_t i = 0; i < ids.size(); ++i) {
        const float x = regionMinX + static_cast<float>(i) * (tileW + Px(kBarGap)) - canvasBarScroll_;
        if (x + tileW < regionMinX || x > regionMaxX) {
            continue;  // scrolled out of view
        }
        const Canvas* canvas = nullptr;
        for (const Canvas& c : Manager().Canvases()) {
            if (c.id == ids[i]) {
                canvas = &c;
            }
        }
        if (canvas == nullptr) {
            continue;
        }
        const ImVec2 tileMin(x, bar.y + Px(kBarPadding));
        const ImVec2 tileMax(x + tileW, tileMin.y + Px(kBarTileHeight));
        DrawCanvasPreview(dl, *canvas, tileMin, tileMax, displayW, displayH, Cfg().strokeRenderMode,
                          Cfg().overviewShowsStrokes, previewTexture, PreviewMeshSlot(), PictureSampling());

        char tileId[48];
        std::snprintf(tileId, sizeof(tileId), "##canvasbar_tile_%zu", i);
        ImGui::SetCursorScreenPos(tileMin);
        if (ImGui::InvisibleButton(tileId, ImVec2(tileW, Px(kBarTileHeight)))) {
            clicked = canvas->id;
        }
        const bool hovered = ImGui::IsItemHovered();
        // The tile's InvisibleButton answers the left button alone, so the
        // right one is read off the hover - and deliberately does not also
        // switch to the canvas: a menu is opened to act on something, not
        // to go to it.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            rightClicked = canvas->id;
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("SZ_CANVAS_BAR_TILE", &canvas->id, sizeof(CanvasId));
            ImGui::TextUnformatted(canvas->name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SZ_CANVAS_BAR_TILE")) {
                const CanvasId draggedId = *static_cast<const CanvasId*>(payload->Data);
                if (draggedId != canvas->id) {
                    reorder = {draggedId, placeInFolder[i]};
                }
            }
            ImGui::EndDragDropTarget();
        }
        const bool isCurrent = canvas->id == currentId;
        dl->AddRect(tileMin, tileMax,
                    ImGui::GetColorU32(isCurrent ? theme::Accent() : hovered ? theme::kGraphite200 : theme::kGraphite500),
                    Px(3.0f), 0, isCurrent ? Px(2.0f) : 1.0f);
        // Not while a tile is being dragged, whose own name follows the
        // pointer instead.
        if (hovered && !dragging) {
            ImGui::SetTooltip(strings::kCanvasBarTileTip, canvas->name.c_str(), static_cast<int>(i + 1),
                              static_cast<int>(ids.size()));
        }
    }
    ImGui::PopClipRect();

    // A new canvas, at the end of the row it will join.
    const float buttonY = bar.y + (bar.h - Px(kCanvasBarButtonSize)) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(regionMaxX + Px(kBarGap), buttonY));
    const bool makeNew = PillIconButton("##canvasbar_new", icons::kPlus, false);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kCanvasBarNewCanvasTip);
    }
    // And the Overview - every canvas, every folder, the settings. The bar
    // is the one panel that is always there (out of the bottom edge), so
    // this is the way to the Overview that needs nothing on screen and
    // nothing switched on.
    ImGui::SetCursorScreenPos(ImVec2(regionMaxX + Px(kBarGap) + Px(kCanvasBarButtonSize) + Px(kBarGap), buttonY));
    const bool openOverview = PillIconButton("##canvasbar_overview", icons::kLayoutGrid, false);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kCanvasBarOverviewTip);
    }

    ImGui::End();
    // Over the snippets, which re-assert themselves to the front every
    // frame; under the popovers, which are submitted after it.
    BringToFront("##canvas_bar");
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    if (rightClicked.has_value()) {
        OpenCanvasContextMenu(*rightClicked, io.MousePos);
    }
    if (reorder.has_value()) {
        session_.ReorderCanvas(reorder->first, reorder->second);
    }
    if (clicked.has_value()) {
        editor_.SwitchToCanvasSettled(*clicked);
    }
    if (makeNew) {
        // Into the folder the bar is showing - the current canvas's, which
        // need not be the one the Overview last browsed.
        editor_.SwitchToCanvasSettled(editor_.CreateCanvasBesideCurrent());
    }
    if (openOverview) {
        OpenOverview();
    }
}

// ================= A tile's context menu =================

void OverlayApp::OpenCanvasContextMenu(CanvasId canvasId, ImVec2 at) {
    PushPopup(PopupKind::CanvasMenu);
    canvasContextMenuCanvasId_ = canvasId;
    Queue(Effect{Effect::Kind::OpenCanvasMenu, at});
}

void OverlayApp::RenderCanvasContextMenu() {
    // Looked up afresh every frame, like the snippet menu's: a canvas can
    // be deleted or moved to another folder from elsewhere while this is
    // up, and either takes it off the bar.
    const Canvas* canvas = nullptr;
    if (canvasContextMenuCanvasId_.has_value()) {
        const Canvas* found = Manager().FindCanvas(*canvasContextMenuCanvasId_);
        if (found != nullptr && !Manager().IsDeleted(*found)) {
            canvas = found;
        }
    }
    const std::optional<int> chosen = canvasContextMenu_.Render([&](std::vector<ContextMenuEntry>& rows) {
        if (canvas != nullptr) {
            BuildCanvasContextMenuRows(*canvas, rows);
        }
    });
    if (chosen.has_value()) {
        Command command{static_cast<CommandId>(*chosen)};
        command.canvas = *canvasContextMenuCanvasId_;
        Dispatch(command);
    }
    if (!canvasContextMenu_.IsOpen()) {
        canvasContextMenuCanvasId_.reset();
    }
}

void OverlayApp::BuildCanvasContextMenuRows(const Canvas& canvas, std::vector<ContextMenuEntry>& rows) const {
    // Deliberately short. The last canvas of a folder is deletable like
    // any other - see the Overview's own delete button for why there is no
    // "and this folder holds more than one" condition on it.
    Command deleteCanvas{CommandId::DeleteCanvas};
    deleteCanvas.canvas = canvas.id;
    rows.push_back(MenuRow(deleteCanvas, "##canvasmenu_delete", &icons::kTrash, strings::kMenuDeleteCanvas));
}

}  // namespace sz::ui
