// The popovers and overlays that sit over the canvas: the properties
// popover a snippet's More button opens, the colour chooser the drawing
// bar's colour button opens, and the two drag previews - the frame a
// region capture is dragging out, and the rectangle the rectangle eraser
// is about to take away. All of them are ordinary ImGui windows, submitted
// from OnFrame after the items so they sit above every snippet.
#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "ui/icons_generated.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace sz::ui {

using namespace overlay_detail;

namespace {
// A round color swatch button - the .swatch equivalent (a plain colored
// circle, ringed in white while selected, in the panel border color while
// merely hovered). Returns true the frame it's clicked.
bool ColorSwatchButton(ImU32 fillColor, bool selected) {
    constexpr float kDiameter = 22.0f;
    ImGui::InvisibleButton("##swatch", ImVec2(kDiameter + 6.0f, kDiameter + 6.0f));
    const bool pressed = ImGui::IsItemClicked();
    const ImVec2 pMin = ImGui::GetItemRectMin();
    const ImVec2 pMax = ImGui::GetItemRectMax();
    const ImVec2 center((pMin.x + pMax.x) * 0.5f, (pMin.y + pMax.y) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, kDiameter * 0.5f, fillColor);
    // Same reasoning as PillColorButton's rim: black, and any dark custom
    // color, needs an edge of its own to read as a swatch on a dark panel.
    dl->AddCircle(center, kDiameter * 0.5f, ImGui::ColorConvertFloat4ToU32(theme::kPanelBorderStrong), 0, 1.0f);
    if (selected) {
        dl->AddCircle(center, kDiameter * 0.5f + 2.0f, ImGui::ColorConvertFloat4ToU32(theme::kWhite), 0, 1.5f);
    } else if (ImGui::IsItemHovered()) {
        dl->AddCircle(center, kDiameter * 0.5f + 2.0f, ImGui::ColorConvertFloat4ToU32(theme::kPanelBorderStrong), 0,
                       1.5f);
    }
    return pressed;
}

}  // namespace

// ================= Canvases =================

CanvasId OverlayApp::CreateCanvasInCurrentFolder() {
    const CanvasId id = Manager().AddCanvas(TimestampName());
    // It lands at the end of the folder, which in a long folder is off the
    // bottom of the Overview's grid - see overviewScrollToCanvasId_.
    overviewScrollToCanvasId_ = id;
    return id;
}

CanvasId OverlayApp::CreateCanvasBesideCurrent() {
    // The browsed folder and the current canvas's are deliberately
    // decoupled (see CanvasManager's class comment); browsing elsewhere
    // without opening anything is what tells them apart. A switch to the
    // new canvas re-syncs the browsed folder anyway.
    if (const Canvas* current = Manager().CurrentOrNull()) {
        Manager().SwitchToFolder(current->folderId);
    }
    return CreateCanvasInCurrentFolder();
}

void OverlayApp::CreateAndSwitchToNewCanvas() {
    // Settled like any other switch: the shortcut can land mid-gesture.
    // Nothing to settle when the new canvas is already current - the
    // library had none, and the press that asked for a snippet is what is
    // in flight (see EnsureCanvasForNewItem).
    SwitchToCanvasSettled(CreateCanvasBesideCurrent());
}

// A new canvas that the selected snippets come along to - "these belong
// somewhere of their own", which otherwise takes a new canvas, a switch
// back, a cut, a switch forward and a paste.
//
// With nothing selected this is exactly CreateAndSwitchToNewCanvas, and
// says so by doing nothing else: an empty selection is not a reason to
// refuse the canvas.
//
// The moves happen before the switch, because MoveOrCopyItemToCanvas
// takes snippets off the *current* canvas (see its header note), and the
// switch happens even if every move failed - there is a new canvas either
// way, and leaving the app on the old one would make the shortcut look
// like it had done nothing.
//
// The hand is settled before the moves rather than at the switch: a
// shortcut can land mid-gesture, and a stroke or a drag in flight on a
// selected snippet has to end on the canvas it started on, before the
// snippet leaves it.
void OverlayApp::MoveSelectionToNewCanvas() {
    SettleHand();
    const CanvasId source = Manager().CurrentCanvasId();
    const CanvasId target = CreateCanvasBesideCurrent();
    std::vector<ItemId> moved;
    for (const ItemId id : selection_) {
        if (Manager().IsItemDeleted(id)) {
            continue;  // deleted since it was selected
        }
        Manager().MoveOrCopyItemToCanvas(id, target, /*copy=*/false);
        // A move returns 0 whether it moved the snippet or found nothing
        // to move - the caller already knows the id - so what happened is
        // read from where the snippet is now.
        if (Manager().CanvasHoldingItem(id) != target) {
            continue;
        }
        // Its history is filed under the canvas it has left, where an undo
        // would now edit a snippet living somewhere else - see
        // Session::ForgetHistoryOfItem.
        session_.ForgetHistoryOfItem(source, id);
        moved.push_back(id);
    }
    Manager().SwitchToCanvas(target);
    // What arrived is what is selected, so it can be arranged straight
    // away - and the canvas bar is over it rather than over nothing.
    selection_ = moved;
    if (!moved.empty()) {
        const Canvas* canvas = Manager().FindCanvas(target);
        ShowActionToast(std::string(strings::kToastMovedToPrefix) +
                         (canvas != nullptr ? canvas->name : std::string()));
    }
}

// ================= The properties popover =================

void OverlayApp::RenderItemPropertiesPopover() {
    // Consume the deferred-open request first - see
    // itemPropertiesPopoverRequested_'s own doc comment.
    if (itemPropertiesPopoverRequested_) {
        itemPropertiesPopoverRequested_ = false;
        ImGui::OpenPopup("##item_properties_popover");
    }

    // Anchored just below the "More" button that opened it (see
    // itemPropertiesPopoverAnchor_'s own doc comment) rather than ImGui's
    // default near-mouse placement. Pivot (1, 0): the anchor point is the
    // popover's own top-right corner, not top-left - keeps it from
    // running off the right edge of the screen when that button sits
    // near it (which it usually does - every cluster is right-aligned to
    // its own item, and an item can sit anywhere up to the screen edge).
    ImGui::SetNextWindowPos(itemPropertiesPopoverAnchor_, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    if (!ImGui::BeginPopup("##item_properties_popover")) {
        // Not open (never triggered this frame, or the user closed it -
        // click-outside, Escape) - drop the sticky reference used by
        // RenderItems' highlight resolution along with it. A
        // harmless no-op on every ordinary frame where it was already
        // unset.
        itemPropertiesPopoverItemId_.reset();
        return;
    }
    KeepPopoverInFront();
    if (!itemPropertiesPopoverItemId_.has_value()) {
        ImGui::EndPopup();
        return;
    }
    Canvas* canvasPtr = Manager().CurrentOrNull();
    if (!canvasPtr) {
        // The canvas this popover's item lived on was deleted out from
        // under it - same outcome as the item itself going away, below.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    Canvas& canvas = *canvasPtr;
    const auto it = std::find_if(canvas.items.begin(), canvas.items.end(),
                                  [&](const Item& i) { return i.id == *itemPropertiesPopoverItemId_; });
    // Gone, or deleted while the popover was open.
    if (it == canvas.items.end() || Manager().IsDeleted(canvas, *it)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    RenderItemOpacity(*it);
    // Every item has a picture layer (see Item::ImageLayer) - the guard is
    // for the hypothetical one that does not, which shows its foreground
    // opacity and nothing else.
    if (Layer* picture = it->ImageLayer()) {
        RenderItemBackgroundColour(*picture);
        RenderItemTextStyle(*it);
        // After the background-colour ColorEdit3 swatch, not before - see
        // KeepChildPopupsInFront.
        KeepChildPopupsInFront();
    }
    ImGui::EndPopup();
}

void OverlayApp::RenderItemOpacity(Item& item) {
    // Foreground (strokes) and background (captured image / color fill)
    // opacity are independent - see Item::foregroundOpacity/
    // Layer::opacity's own doc comments. Background can go all the way
    // to 0 (invisible) unlike foreground, which bottoms out at 10% - a
    // fully invisible drawing surface still has strokes to see, but there
    // being nothing left to *tell* whether it's an item at all is only a
    // real state for the background.
    int foregroundPct = static_cast<int>(std::round(item.foregroundOpacity * 100.0f));
    ImGui::SetNextItemWidth(160.0f);
    // An id of its own, not shared with the background slider below: both
    // are visible at once on any snippet with a picture in it, and ###
    // hashes only the id, so one spelling for the two of them made them one
    // widget as far as ImGui is concerned - which is an ID conflict it
    // warns about, and a drag it can attribute to the wrong slider.
    if (ImGui::SliderInt(Labeled(strings::kPopoverForeground, "opacityfg"), &foregroundPct, 10, 100, strings::kFormatPercent)) {
        item.foregroundOpacity = static_cast<float>(foregroundPct) / 100.0f;
        Manager().MarkChanged();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kPopoverForegroundTip);
    }

    Layer* picture = item.ImageLayer();
    if (!picture) {
        return;
    }
    int backgroundPct = static_cast<int>(std::round(picture->opacity * 100.0f));
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt(Labeled(strings::kPopoverBackground, "opacitybg"), &backgroundPct, 0, 100, strings::kFormatPercent)) {
        picture->opacity = static_cast<float>(backgroundPct) / 100.0f;
        Manager().MarkChanged();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", picture->textureHandle != 0 ? strings::kPopoverBackgroundShotTip
                                                            : strings::kPopoverBackgroundFillTip);
    }
}

void OverlayApp::RenderItemBackgroundColour(Layer& picture) {
    // White, and the picker for everything else. White gets a swatch of
    // its own because it is the one colour with a meaning here: a no-op
    // multiply tint on a real capture (see Layer::tintColorRGBA), the way
    // back to the picture as it was - and hitting exact white in a picker
    // takes aim. Nothing else is preset: the picker does the whole job.
    ImGui::PushID("##bg_color_section");
    ImGui::TextUnformatted(strings::kPopoverBackgroundColor);
    constexpr uint32_t kWhiteBackground = 0xFFFFFFFFu;
    if (ColorSwatchButton(IM_COL32(255, 255, 255, 255), picture.tintColorRGBA == kWhiteBackground)) {
        picture.tintColorRGBA = kWhiteBackground;
        Manager().MarkChanged();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kPopoverBackgroundWhiteTip);
    }
    ImGui::SameLine(0.0f, 6.0f);
    float rgb[3];
    ColorRGBAToFloats(picture.tintColorRGBA, rgb);
    if (ImGui::ColorEdit3("##bgcolor", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        picture.tintColorRGBA = FloatsToColorRGBA(rgb, static_cast<uint8_t>(0xFF));
        Manager().MarkChanged();
    }
    ImGui::PopID();
}

void OverlayApp::RenderItemTextStyle(Item& item) {
    // Per item rather than app-wide (see Item::noteTextColorRGBA/
    // noteTextSizePx for why). Shown whether or not this item currently
    // has any text: the alternative - appearing only once something's been
    // typed - makes the popover's own height jump around depending on
    // which item opened it, and rules out setting up a caption's look
    // before writing it.
    ImGui::Spacing();
    ImGui::PushID("##note_text_section");
    ImGui::TextUnformatted(strings::kPopoverText);
    if (item.noteText.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kPopoverTextNoneYet);
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat(Labeled(strings::kPopoverTextSize, "notetextsize"), &item.noteTextSizePx, kNoteTextSizeMin, kNoteTextSizeMax,
                            strings::kFormatPixels)) {
        Manager().MarkChanged();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kPopoverTextSizeTip);
    }
    // ColorEdit4, not the ColorEdit3 the background colour uses: unlike
    // the background colour, Item::noteTextColorRGBA's own alpha byte is
    // live (text has no separate opacity field), so a caption can be faded
    // from here.
    float rgba[4];
    ColorRGBAToFloats(item.noteTextColorRGBA, rgba);
    rgba[3] = static_cast<float>(item.noteTextColorRGBA & 0xFFu) / 255.0f;
    if (ImGui::ColorEdit4("##notetextcolor", rgba,
                           ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar |
                               ImGuiColorEditFlags_AlphaPreview)) {
        item.noteTextColorRGBA =
            FloatsToColorRGBA(rgba, static_cast<uint8_t>(std::clamp(rgba[3], 0.0f, 1.0f) * 255.0f + 0.5f));
        Manager().MarkChanged();
    }
    ImGui::PopID();
}

// ================= The context menu =================

void OverlayApp::OpenItemContextMenu(ItemId itemId, ImVec2 at) {
    itemContextMenuItemId_ = itemId;
    itemContextMenu_.RequestOpenAt(at);
}

void OverlayApp::RenderItemContextMenu() {
    // Found again every frame rather than held by pointer across them:
    // Duplicate grows the canvas's item vector and the two z-order rows
    // reorder it, either of which moves every Item in it.
    Item* item = nullptr;
    if (itemContextMenuItemId_.has_value()) {
        if (Canvas* canvas = Manager().CurrentOrNull()) {
            for (Item& candidate : canvas->items) {
                if (candidate.id == *itemContextMenuItemId_ && !Manager().IsDeleted(*canvas, candidate)) {
                    item = &candidate;
                    break;
                }
            }
        }
    }
    // No snippet - deleted while the menu was up, or its canvas switched
    // out from under it - leaves the rows empty, which is how the menu is
    // told to close itself (see ContextMenu::Render).
    const std::optional<int> chosen = itemContextMenu_.Render([&](std::vector<ContextMenuEntry>& rows) {
        if (item != nullptr) {
            BuildItemContextMenuRows(*item, rows);
        }
    });
    if (chosen.has_value()) {
        RunItemMenuAction(static_cast<ItemMenuAction>(*chosen), *itemContextMenuItemId_);
    }
    if (!itemContextMenu_.IsOpen()) {
        itemContextMenuItemId_.reset();
    }
}

void OverlayApp::BuildItemContextMenuRows(Item& item, std::vector<ContextMenuEntry>& rows) {
    const ItemId itemId = item.id;
    // Same rule as the popover's Clear button: painted pixels count as
    // something to clear, or the row is greyed out over a snippet that
    // visibly has ink on it.
    const Layer* painted = Session::FindPaintedLayer(item);
    const bool nothingToClear = item.strokes.empty() && (painted == nullptr || !painted->HasPaintedPixels());

    // By value rather than by reference into `rows`: a push_back that
    // reallocates would leave a reference handed back from an earlier one
    // dangling, and a menu is exactly the kind of list that grows a row.
    const auto add = [&rows](ItemMenuAction action, const char* id, const Icon& icon, const char* label,
                              bool enabled = true, std::string shortcut = {}, bool separatorAbove = false) {
        rows.push_back(ContextMenuEntry{static_cast<int>(action), id, &icon, label, std::move(shortcut), enabled,
                                         separatorAbove});
    };

    // Size first, then what is on the snippet, then where it lives - the
    // order the popover's own row of buttons is in, which is the order a
    // hand has already learned.
    add(ItemMenuAction::ToggleFullscreen, "##menu_fullscreen",
        item.isFullscreen ? icons::kRestore : icons::kMaximize,
        item.isFullscreen ? strings::kMenuRestoreSize : strings::kMenuFullscreen);
    add(ItemMenuAction::ResetSize, "##menu_reset_size", icons::kTarget, strings::kMenuOriginalSize);
    add(ItemMenuAction::ClearDrawing, "##menu_clear_drawing", icons::kEraser, strings::kMenuClearDrawing,
        /*enabled=*/!nothingToClear);

    // Duplicate is the selection's, not this one snippet's. A right-click
    // has already made this snippet part of the selection (see
    // HandleItemGesture), so the two agree whenever only it is selected,
    // and where they differ the shortcut shown beside the row is the honest
    // answer: Ctrl+D does the whole selection, so the row that names Ctrl+D
    // has to as well.
    add(ItemMenuAction::Duplicate, "##menu_duplicate", icons::kCopy, strings::kMenuDuplicate, /*enabled=*/true,
        MenuShortcutLabel(ShortcutAction::Duplicate), /*separatorAbove=*/true);
    // Disabled when nothing *overlapping* this snippet is in that
    // direction, rather than at the ends of the stack - see
    // CanvasManager::MoveItemLayer for why that is the useful rule.
    add(ItemMenuAction::SendBackward, "##menu_send_backward", icons::kLayerDown, strings::kMenuSendBackward,
        Manager().CanMoveItemLayer(itemId, -1));
    add(ItemMenuAction::BringForward, "##menu_bring_forward", icons::kLayerUp, strings::kMenuBringForward,
        Manager().CanMoveItemLayer(itemId, 1));

    add(ItemMenuAction::MoveToCanvas, "##menu_move_to_canvas", icons::kMove, strings::kMenuMoveToCanvas,
        Manager().Canvases().size() > 1, /*shortcut=*/{}, /*separatorAbove=*/true);
    // The selection again, and for the same reason as Duplicate: this is
    // the row for the Ctrl+Shift+N beside it.
    add(ItemMenuAction::MoveToNewCanvas, "##menu_move_to_new_canvas", icons::kPlus, strings::kMenuMoveToNewCanvas,
        /*enabled=*/true, MenuShortcutLabel(ShortcutAction::NewCanvasWithSelection));
}

std::string OverlayApp::MenuShortcutLabel(ShortcutAction action) const {
    // Live(), not Stored(): what is bound right now, profile and all, is
    // what the row has to promise.
    const platform::KeyCombo& combo = settings_.Live().shortcuts[ShortcutActionIndex(action)];
    return combo.key == 0 ? std::string() : FormatKeyComboLabel(combo);
}

void OverlayApp::RunItemMenuAction(ItemMenuAction action, ItemId itemId) {
    const ImGuiIO& io = ImGui::GetIO();
    switch (action) {
        case ItemMenuAction::ToggleFullscreen:
            Manager().ToggleFullscreen(itemId, io.DisplaySize.x, io.DisplaySize.y, io.KeyShift);
            return;
        case ItemMenuAction::ResetSize:
            Manager().ResetItemToNativeSize(itemId);
            return;
        case ItemMenuAction::ClearDrawing:
            ClearItemDrawing(itemId);
            return;
        case ItemMenuAction::Duplicate:
            DuplicateSelection();
            return;
        case ItemMenuAction::SendBackward:
            Manager().MoveItemLayer(itemId, -1);
            return;
        case ItemMenuAction::BringForward:
            Manager().MoveItemLayer(itemId, 1);
            return;
        case ItemMenuAction::MoveToCanvas:
            // Opens the Overview in picker mode to choose a destination -
            // the menu has already closed itself by the time this runs (a
            // chosen row closes the popup), so there is nothing left here
            // to dismiss.
            OpenPicker(itemId, /*isCopy=*/false);
            return;
        case ItemMenuAction::MoveToNewCanvas:
            MoveSelectionToNewCanvas();
            return;
    }
}

// ================= The colour chooser =================

void OverlayApp::OpenColorChooser(ImVec2 from) {
    // Only asked for here: the bar's colour button fires from the raw
    // mouse callback between frames, where there is no window for
    // ImGui::OpenPopup to belong to.
    colorChooserRequested_ = true;
    colorChooserAnchor_ = from;
}

void OverlayApp::RenderColorChooser(float displayW, float displayH) {
    constexpr const char* kPopupId = "##color_chooser";
    if (colorChooserRequested_) {
        colorChooserRequested_ = false;
        ImGui::OpenPopup(kPopupId);
    }
    // Beside the point it was asked from, on whichever side has room, so
    // a bar near an edge of the screen does not have its chooser placed
    // off it.
    constexpr float kGap = 20.0f;
    const bool above = colorChooserAnchor_.y > displayH * 0.5f;
    const bool toTheLeft = colorChooserAnchor_.x > displayW * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(colorChooserAnchor_.x + (toTheLeft ? -kGap : kGap),
                                   colorChooserAnchor_.y + (above ? -kGap : kGap)),
                            ImGuiCond_Appearing, ImVec2(toTheLeft ? 1.0f : 0.0f, above ? 1.0f : 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    const bool open = ImGui::BeginPopup(kPopupId);
    ImGui::PopStyleVar();
    if (!open) {
        if (colorChooserOpen_) {
            // Closed since the last frame. What it was left on is the
            // colour from now on, and the next time the app starts.
            colorChooserOpen_ = false;
            if (settings_.Stored().strokeColorRGBA != drawColorRGBA_) {
                settings_.Mutable().strokeColorRGBA = drawColorRGBA_;
                settings_.Commit();
            }
        }
        return;
    }
    colorChooserOpen_ = true;
    // Items re-assert themselves to the front every frame; a popup has to
    // as well, or the first snippet it overlaps covers it.
    KeepPopoverInFront();
    float rgb[3];
    ColorRGBAToFloats(drawColorRGBA_, rgb);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::ColorPicker3("##picker", rgb,
                            ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_NoLabel)) {
        SetDrawColor(FloatsToColorRGBA(rgb, 0xFF));
    }
    ImGui::EndPopup();
}

// ================= Drag previews =================

void OverlayApp::RenderRegionCaptureOverlay() {
    if (!creation_.has_value() || !creation_->dragTo.has_value()) {
        return;
    }
    const CreationGesture& gesture = *creation_;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 pMin(std::min(gesture.downX, gesture.dragTo->x), std::min(gesture.downY, gesture.dragTo->y));
    const ImVec2 pMax(std::max(gesture.downX, gesture.dragTo->x), std::max(gesture.downY, gesture.dragTo->y));
    drawList->AddRectFilled(pMin, pMax, theme::AccentU32(40));
    drawList->AddRect(pMin, pMax, theme::AccentU32(255), 0.0f, 2.0f, ImDrawFlags_None);
    char dims[32];
    std::snprintf(dims, sizeof(dims), strings::kFormatSizeWidthByHeight, pMax.x - pMin.x, pMax.y - pMin.y);
    drawList->AddText(ImVec2(pMin.x, pMin.y - 18.0f), IM_COL32(255, 255, 255, 255), dims);
}

void OverlayApp::RenderRectEraserOverlay() {
    if (!rectErase_.has_value()) {
        return;
    }
    // Same visual language as RenderRegionCaptureOverlay, in a cool tone
    // instead of that one's warm orange - erasing is a destructive
    // preview, not a placement one, and the two shouldn't read as the
    // same affordance at a glance.
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const RectErase& r = *rectErase_;
    const ImVec2 pMin(std::min(r.x0, r.x1), std::min(r.y0, r.y1));
    const ImVec2 pMax(std::max(r.x0, r.x1), std::max(r.y0, r.y1));
    drawList->AddRectFilled(pMin, pMax, IM_COL32(120, 170, 255, 40));
    drawList->AddRect(pMin, pMax, IM_COL32(120, 170, 255, 255), 0.0f, 2.0f, ImDrawFlags_None);
}

}  // namespace sz::ui
