// The popovers and overlays that sit over the canvas: the properties
// popover a snippet's More button opens, the color chooser the drawing
// bar's color button opens, and the two drag previews - the frame a
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
    const float radius = Px(11.0f);
    ImGui::InvisibleButton("##swatch", Px(28.0f, 28.0f));
    const bool pressed = ImGui::IsItemClicked();
    const ImVec2 pMin = ImGui::GetItemRectMin();
    const ImVec2 pMax = ImGui::GetItemRectMax();
    const ImVec2 center((pMin.x + pMax.x) * 0.5f, (pMin.y + pMax.y) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, radius, fillColor);
    // Same reasoning as PillColorButton's rim: black, and any dark custom
    // color, needs an edge of its own to read as a swatch on a dark panel.
    dl->AddCircle(center, radius, ImGui::ColorConvertFloat4ToU32(theme::kPanelBorderStrong), 0, 1.0f);
    if (selected) {
        dl->AddCircle(center, radius + Px(2.0f), ImGui::ColorConvertFloat4ToU32(theme::kWhite), 0, Px(1.5f));
    } else if (ImGui::IsItemHovered()) {
        dl->AddCircle(center, radius + Px(2.0f), ImGui::ColorConvertFloat4ToU32(theme::kPanelBorderStrong), 0,
                       Px(1.5f));
    }
    return pressed;
}

}  // namespace

// ================= Canvases =================

CanvasId OverlayApp::CreateCanvasInCurrentFolder() {
    const CanvasId id = session_.AddCanvas(TimestampName());
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
        session_.SwitchToFolder(current->folderId);
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
    const CanvasId target = CreateCanvasBesideCurrent();
    const std::vector<ItemId> moved = session_.SendItemsTo(selection_, target, /*copy=*/false).items;
    session_.SwitchToCanvas(target);
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
    // Anchored just below the "More" button that opened it (see
    // itemPropertiesPopoverAnchor_'s own doc comment) rather than ImGui's
    // default near-mouse placement. Pivot (1, 0): the anchor point is the
    // popover's own top-right corner, not top-left - keeps it from
    // running off the right edge of the screen when that button sits
    // near it (which it usually does - every cluster is right-aligned to
    // its own item, and an item can sit anywhere up to the screen edge).
    ImGui::SetNextWindowPos(itemPropertiesPopoverAnchor_, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    if (!ImGui::BeginPopup(kItemPropertiesPopupId)) {
        // Not open (never triggered this frame, or the user closed it -
        // click-outside, Escape) - drop the sticky reference used by
        // RenderItems' highlight resolution along with it. A
        // harmless no-op on every ordinary frame where it was already
        // unset. A style being dragged when it closed ends with it.
        itemPropertiesPopoverItemId_.reset();
        session_.EndStyleEdit();
        return;
    }
    KeepPopoverInFront();
    if (!itemPropertiesPopoverItemId_.has_value()) {
        ImGui::EndPopup();
        return;
    }
    const Canvas* canvasPtr = Manager().CurrentOrNull();
    if (!canvasPtr) {
        // The canvas this popover's item lived on was deleted out from
        // under it - same outcome as the item itself going away, below.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const Canvas& canvas = *canvasPtr;
    const auto it = std::find_if(canvas.items.begin(), canvas.items.end(),
                                  [&](const Item& i) { return i.id == *itemPropertiesPopoverItemId_; });
    // Gone, or deleted while the popover was open.
    if (it == canvas.items.end() || Manager().IsDeleted(canvas, *it)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const Item& item = *it;
    RenderItemOpacity(item);
    bool keepAspect = item.keepAspect;
    if (ImGui::Checkbox(Labeled(strings::kPopoverKeepAspect, "keepaspect"), &keepAspect)) {
        ItemStyle style = ItemStyle::Of(item);
        style.keepAspect = keepAspect;
        session_.PreviewStyle(item.id, style);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kPopoverKeepAspectTip);
    }
    RenderItemBackgroundColor(item);
    RenderItemTextStyle(item);
    // After the background-color ColorEdit3 swatch, not before - see
    // KeepChildPopupsInFront.
    KeepChildPopupsInFront();
    // A change of style is one step for as long as the hand is on it - a
    // slider dragged, the color picker's square - and ends when it lets go.
    if (!ImGui::IsAnyItemActive()) {
        session_.EndStyleEdit();
    }
    ImGui::EndPopup();
}

void OverlayApp::RenderItemOpacity(const Item& item) {
    // Foreground (strokes) and background (captured image / color fill)
    // opacity are independent - see Item::foregroundOpacity/
    // Picture::opacity's own doc comments. Background can go all the way
    // to 0 (invisible) unlike foreground, which bottoms out at 10% - a
    // fully invisible drawing surface still has strokes to see, but there
    // being nothing left to *tell* whether it's an item at all is only a
    // real state for the background.
    int foregroundPct = static_cast<int>(std::round(item.foregroundOpacity * 100.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    // An id of its own, not shared with the background slider below: both
    // are visible at once on any snippet with a picture in it, and ###
    // hashes only the id, so one spelling for the two of them made them one
    // widget as far as ImGui is concerned - which is an ID conflict it
    // warns about, and a drag it can attribute to the wrong slider.
    if (ImGui::SliderInt(Labeled(strings::kPopoverForeground, "opacityfg"), &foregroundPct, 10, 100, strings::kFormatPercent, ImGuiSliderFlags_AlwaysClamp)) {
        ItemStyle style = ItemStyle::Of(item);
        style.foregroundOpacity = static_cast<float>(foregroundPct) / 100.0f;
        session_.PreviewStyle(item.id, style);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kPopoverForegroundTip);
    }

    int backgroundPct = static_cast<int>(std::round(item.picture.opacity * 100.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    if (ImGui::SliderInt(Labeled(strings::kPopoverBackground, "opacitybg"), &backgroundPct, 0, 100, strings::kFormatPercent, ImGuiSliderFlags_AlwaysClamp)) {
        ItemStyle style = ItemStyle::Of(item);
        style.pictureOpacity = static_cast<float>(backgroundPct) / 100.0f;
        session_.PreviewStyle(item.id, style);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", item.picture.stored ? strings::kPopoverBackgroundShotTip
                                                    : strings::kPopoverBackgroundFillTip);
    }
}

void OverlayApp::RenderItemBackgroundColor(const Item& item) {
    // White, and the picker for everything else. White gets a swatch of
    // its own because it is the one color with a meaning here: a no-op
    // multiply tint on a real capture (see Picture::tintColorRGBA), the way
    // back to the picture as it was - and hitting exact white in a picker
    // takes aim. Nothing else is preset: the picker does the whole job.
    ImGui::PushID("##bg_color_section");
    ImGui::TextUnformatted(strings::kPopoverBackgroundColor);
    constexpr uint32_t kWhiteBackground = 0xFFFFFFFFu;
    const auto setTint = [&](uint32_t tintRGBA) {
        ItemStyle style = ItemStyle::Of(item);
        style.pictureTintRGBA = tintRGBA;
        session_.PreviewStyle(item.id, style);
    };
    if (ColorSwatchButton(IM_COL32(255, 255, 255, 255), item.picture.tintColorRGBA == kWhiteBackground)) {
        setTint(kWhiteBackground);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kPopoverBackgroundWhiteTip);
    }
    ImGui::SameLine(0.0f, Px(6.0f));
    float rgb[3];
    ColorRGBAToFloats(item.picture.tintColorRGBA, rgb);
    if (ImGui::ColorEdit3("##bgcolor", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        setTint(FloatsToColorRGBA(rgb, static_cast<uint8_t>(0xFF)));
    }
    ImGui::PopID();
}

void OverlayApp::RenderItemTextStyle(const Item& item) {
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
    ImGui::SetNextItemWidth(Px(160.0f));
    float textSizePx = item.noteTextSizePx;
    if (ImGui::SliderFloat(Labeled(strings::kPopoverTextSize, "notetextsize"), &textSizePx, kNoteTextSizeMin, kNoteTextSizeMax,
                            strings::kFormatPixels, ImGuiSliderFlags_AlwaysClamp)) {
        ItemStyle style = ItemStyle::Of(item);
        style.noteTextSizePx = textSizePx;
        session_.PreviewStyle(item.id, style);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kPopoverTextSizeTip);
    }
    // ColorEdit4, not the ColorEdit3 the background color uses: unlike
    // the background color, Item::noteTextColorRGBA's own alpha byte is
    // live (text has no separate opacity field), so a caption can be faded
    // from here.
    float rgba[4];
    ColorRGBAToFloats(item.noteTextColorRGBA, rgba);
    rgba[3] = static_cast<float>(item.noteTextColorRGBA & 0xFFu) / 255.0f;
    if (ImGui::ColorEdit4("##notetextcolor", rgba,
                           ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar |
                               ImGuiColorEditFlags_AlphaPreview)) {
        ItemStyle style = ItemStyle::Of(item);
        style.noteTextColorRGBA =
            FloatsToColorRGBA(rgba, static_cast<uint8_t>(std::clamp(rgba[3], 0.0f, 1.0f) * 255.0f + 0.5f));
        session_.PreviewStyle(item.id, style);
    }
    ImGui::PopID();
}

// ================= The context menu =================

void OverlayApp::OpenItemContextMenu(ItemId itemId, ImVec2 at) {
    itemContextMenuItemId_ = itemId;
    Queue(Effect{Effect::Kind::OpenItemMenu, at});
}

void OverlayApp::RenderItemContextMenu() {
    // Found again every frame rather than held by pointer across them:
    // Duplicate grows the canvas's item vector and the two z-order rows
    // reorder it, either of which moves every Item in it.
    const Item* item = nullptr;
    if (itemContextMenuItemId_.has_value()) {
        if (const Canvas* canvas = Manager().CurrentOrNull()) {
            for (const Item& candidate : canvas->items) {
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
        // Fullscreen with Shift held stretches to the screen rather than
        // keeping the snippet's shape - the row's other half.
        CommandId id = static_cast<CommandId>(*chosen);
        if (id == CommandId::ToggleFullscreen && ImGui::GetIO().KeyShift) {
            id = CommandId::ToggleFullscreenStretched;
        }
        Dispatch(Command{id, *itemContextMenuItemId_});
    }
    if (!itemContextMenu_.IsOpen()) {
        itemContextMenuItemId_.reset();
    }
}

void OverlayApp::BuildItemContextMenuRows(const Item& item, std::vector<ContextMenuEntry>& rows) {
    const auto add = [&](CommandId id, const char* widgetId, const Icon& icon, const char* label,
                         bool separatorAbove = false) {
        rows.push_back(MenuRow(Command{id, item.id}, widgetId, &icon, label, separatorAbove));
    };

    // Size first, then what is on the snippet, then where it lives - the
    // order the popover's own row of buttons is in, which is the order a
    // hand has already learned.
    add(CommandId::ToggleFullscreen, "##menu_fullscreen", item.isFullscreen ? icons::kRestore : icons::kMaximize,
        item.isFullscreen ? strings::kMenuRestoreSize : strings::kMenuFullscreen);
    add(CommandId::ResetSize, "##menu_reset_size", icons::kTarget, strings::kMenuOriginalSize);
    add(CommandId::ClearDrawing, "##menu_clear_drawing", icons::kEraser, strings::kMenuClearDrawing);

    // Copy, Cut and Duplicate are the selection's, not this one snippet's.
    // A right-click has already made this snippet part of the selection
    // (see HandleItemGesture), so the two agree whenever only it is
    // selected, and where they differ the shortcut shown beside the row is
    // the honest answer: Ctrl+D does the whole selection, so the row that
    // names Ctrl+D has to as well - and does, being the same command. Paste
    // is empty canvas's (see BuildEmptyCanvasMenuRows): what it does has
    // nothing to do with the snippet it would be opened over.
    add(CommandId::Copy, "##menu_copy", icons::kCopy, strings::kMenuCopy, /*separatorAbove=*/true);
    add(CommandId::Cut, "##menu_cut", icons::kScissors, strings::kMenuCut);
    add(CommandId::Duplicate, "##menu_duplicate", icons::kCopy, strings::kMenuDuplicate);
    // Grayed when nothing *overlapping* this snippet is in that direction,
    // rather than at the ends of the stack - see
    // CanvasManager::MoveItemLayer for why that is the useful rule.
    add(CommandId::SendBackward, "##menu_send_backward", icons::kLayerDown, strings::kMenuSendBackward);
    add(CommandId::BringForward, "##menu_bring_forward", icons::kLayerUp, strings::kMenuBringForward);

    add(CommandId::MoveToCanvas, "##menu_move_to_canvas", icons::kMove, strings::kMenuMoveToCanvas,
        /*separatorAbove=*/true);
    // The selection again, and for the same reason as Duplicate: this is
    // the row for the Ctrl+Shift+N beside it.
    add(CommandId::NewCanvasWithSelection, "##menu_move_to_new_canvas", icons::kPlus, strings::kMenuMoveToNewCanvas);
}

// ================= Empty canvas's context menu =================

void OverlayApp::OpenEmptyCanvasMenu(ImVec2 at) { Queue(Effect{Effect::Kind::OpenEmptyCanvasMenu, at}); }

void OverlayApp::RenderEmptyCanvasMenu() {
    const std::optional<int> chosen = emptyCanvasMenu_.Render(
        [&](std::vector<ContextMenuEntry>& rows) { BuildEmptyCanvasMenuRows(rows); });
    if (chosen.has_value()) {
        Dispatch(Command{static_cast<CommandId>(*chosen)});
    }
}

void OverlayApp::BuildEmptyCanvasMenuRows(std::vector<ContextMenuEntry>& rows) const {
    const auto add = [&](CommandId id, const char* widgetId, const Icon* icon, const char* label,
                         bool separatorAbove = false) {
        rows.push_back(MenuRow(Command{id}, widgetId, icon, label, separatorAbove));
    };
    // Every way to make a snippet, whatever the left button has been set
    // to make on its own (see AppConfig::screenshotTrigger): the menu is
    // where the kind a press does not make is still one click away. The
    // two "New" rows are the creation tools' keys, so the next press
    // frames or places it; the fullscreen ones make it at once.
    add(CommandId::NewScreenshotTool, "##emptymenu_new_screenshot", &icons::kCamera, strings::kMenuNewScreenshot);
    add(CommandId::FullscreenScreenshot, "##emptymenu_fullscreen_screenshot", &icons::kMaximize,
        strings::kMenuFullscreenScreenshot);
    add(CommandId::NewDrawingTool, "##emptymenu_new_drawing", &icons::kPen, strings::kMenuNewDrawing);
    add(CommandId::FullscreenDrawing, "##emptymenu_fullscreen_drawing", &icons::kMaximize,
        strings::kMenuFullscreenDrawing);

    add(CommandId::Paste, "##emptymenu_paste", &icons::kClipboard, strings::kMenuPaste, /*separatorAbove=*/true);

    add(CommandId::Overview, "##emptymenu_overview", &icons::kLayoutGrid, strings::kMenuOverview,
        /*separatorAbove=*/true);
    add(CommandId::Settings, "##emptymenu_settings", nullptr, strings::kMenuSettings);
    add(CommandId::CheatSheet, "##emptymenu_cheat_sheet", &icons::kKeyboard, strings::kMenuCheatSheet);
}

// ================= Effects =================

void OverlayApp::Queue(const Effect& effect) {
    const auto same = std::find_if(effects_.begin(), effects_.end(),
                                   [&](const Effect& queued) { return queued.kind == effect.kind; });
    if (same != effects_.end()) {
        *same = effect;
        return;
    }
    effects_.push_back(effect);
}

// At the top level of the frame, in the order asked: a popup opened
// replaces the one open before it, so the one asked for last is the one
// that stays up.
void OverlayApp::ApplyEffects() {
    std::vector<Effect> effects;
    effects.swap(effects_);
    for (const Effect& effect : effects) {
        switch (effect.kind) {
            case Effect::Kind::OpenItemProperties:
                ImGui::OpenPopup(kItemPropertiesPopupId);
                break;
            case Effect::Kind::OpenItemMenu:
                itemContextMenu_.OpenAt(effect.at);
                break;
            case Effect::Kind::OpenCanvasMenu:
                canvasContextMenu_.OpenAt(effect.at);
                break;
            case Effect::Kind::OpenEmptyCanvasMenu:
                emptyCanvasMenu_.OpenAt(effect.at);
                break;
            case Effect::Kind::OpenColorChooser:
                colorChooserAnchor_ = effect.at;
                ImGui::OpenPopup(kColorChooserPopupId);
                break;
            case Effect::Kind::OpenConfirmDelete:
                OpenConfirmDelete();
                break;
        }
    }
}

// ================= The color chooser =================

void OverlayApp::OpenColorChooser(ImVec2 from) {
    // Only asked for here: the bar's color button fires from the input
    // stream between frames - see Effect.
    Queue(Effect{Effect::Kind::OpenColorChooser, from});
}

void OverlayApp::RenderColorChooser(float displayW, float displayH) {
    // Beside the point it was asked from, on whichever side has room, so
    // a bar near an edge of the screen does not have its chooser placed
    // off it.
    const float gap = Px(20.0f);
    const bool above = colorChooserAnchor_.y > displayH * 0.5f;
    const bool toTheLeft = colorChooserAnchor_.x > displayW * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(colorChooserAnchor_.x + (toTheLeft ? -gap : gap),
                                   colorChooserAnchor_.y + (above ? -gap : gap)),
                            ImGuiCond_Appearing, ImVec2(toTheLeft ? 1.0f : 0.0f, above ? 1.0f : 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(8.0f, 8.0f));
    const bool open = ImGui::BeginPopup(kColorChooserPopupId);
    ImGui::PopStyleVar();
    if (!open) {
        if (colorChooserOpen_) {
            // Closed since the last frame. What it was left on is the
            // color from now on, and the next time the app starts.
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
    ImGui::SetNextItemWidth(Px(220.0f));
    if (ImGui::ColorPicker3("##picker", rgb,
                            ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_NoLabel)) {
        SetDrawColor(FloatsToColorRGBA(rgb, 0xFF));
    }
    ImGui::EndPopup();
}

// ================= Drag previews =================

void OverlayApp::RenderRegionCaptureOverlay() {
    const CreationGesture* framing = GestureIf<CreationGesture>();
    if (framing == nullptr || !framing->dragTo.has_value()) {
        return;
    }
    const CreationGesture& gesture = *framing;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 pMin(std::min(gesture.downX, gesture.dragTo->x), std::min(gesture.downY, gesture.dragTo->y));
    const ImVec2 pMax(std::max(gesture.downX, gesture.dragTo->x), std::max(gesture.downY, gesture.dragTo->y));
    drawList->AddRectFilled(pMin, pMax, theme::AccentU32(40));
    drawList->AddRect(pMin, pMax, theme::AccentU32(255), 0.0f, PxWhole(2.0f), ImDrawFlags_None);
    char dims[32];
    std::snprintf(dims, sizeof(dims), strings::kFormatSizeWidthByHeight, pMax.x - pMin.x, pMax.y - pMin.y);
    drawList->AddText(ImVec2(pMin.x, pMin.y - ImGui::GetTextLineHeight() - Px(1.0f)), IM_COL32(255, 255, 255, 255),
                      dims);
}

void OverlayApp::RenderRectEraserOverlay() {
    const StrokeInFlight* stroke = GestureIf<StrokeInFlight>();
    if (stroke == nullptr || stroke->kind != StrokeInFlight::Kind::EraseRect) {
        return;
    }
    // Same visual language as RenderRegionCaptureOverlay, in a cool tone
    // instead of that one's warm orange - erasing is a destructive
    // preview, not a placement one, and the two shouldn't read as the
    // same affordance at a glance.
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const RectErase& r = stroke->rect;
    const ImVec2 pMin(std::min(r.x0, r.x1), std::min(r.y0, r.y1));
    const ImVec2 pMax(std::max(r.x0, r.x1), std::max(r.y0, r.y1));
    drawList->AddRectFilled(pMin, pMax, IM_COL32(120, 170, 255, 40));
    drawList->AddRect(pMin, pMax, IM_COL32(120, 170, 255, 255), 0.0f, PxWhole(2.0f), ImDrawFlags_None);
}

}  // namespace sz::ui
