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

// ================= The properties popover =================

void OverlayApp::RenderItemPropertiesPopover() {
    // Anchored just below the "More" button that opened it rather than
    // ImGui's default near-mouse placement. Pivot (1, 0): the anchor point
    // is the popover's own top-right corner, not top-left - keeps it from
    // running off the right edge of the screen when that button sits
    // near it (which it usually does - every cluster is right-aligned to
    // its own item, and an item can sit anywhere up to the screen edge).
    const bool up = PopupUp(PopupKind::ItemProperties);
    ImGui::SetNextWindowPos(up ? popup_->at : ImVec2(0.0f, 0.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    const bool open = ImGui::BeginPopup(kItemPropertiesPopupId);
    PopupDrawn(PopupKind::ItemProperties, open);
    if (!open) {
        return;
    }
    KeepPopoverInFront();
    if (!up) {
        // Ended from outside this frame, before ImGui heard of it.
        ImGui::CloseCurrentPopup();
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
                                  [&](const Item& i) { return i.id == popup_->item; });
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
    PopupRecord popup;
    popup.kind = PopupKind::ItemMenu;
    popup.item = itemId;
    popup.at = at;
    OpenPopup(std::move(popup));
}

void OverlayApp::RenderItemContextMenu() {
    // Found again every frame rather than held by pointer across them:
    // Duplicate grows the canvas's item vector and the two z-order rows
    // reorder it, either of which moves every Item in it.
    const bool up = PopupUp(PopupKind::ItemMenu);
    const ItemId itemId = up ? popup_->item : 0;
    const Item* item = nullptr;
    if (up) {
        if (const Canvas* canvas = Manager().CurrentOrNull()) {
            for (const Item& candidate : canvas->items) {
                if (candidate.id == itemId && !Manager().IsDeleted(*canvas, candidate)) {
                    item = &candidate;
                    break;
                }
            }
        }
    }
    // No snippet - deleted while the menu was up, its canvas switched out
    // from under it, or the menu ended from outside - leaves the rows
    // empty, which is how the menu is told to close itself (see
    // ContextMenu::Render).
    const ContextMenu::Drawn drawn =
        itemContextMenu_.Render(up ? popup_->at : ImVec2(0.0f, 0.0f), [&](std::vector<ContextMenuEntry>& rows) {
            if (item != nullptr) {
                BuildItemContextMenuRows(*item, rows);
            }
        });
    PopupDrawn(PopupKind::ItemMenu, drawn.up);
    if (drawn.chosen.has_value()) {
        // Fullscreen with Shift held stretches to the screen rather than
        // keeping the snippet's shape - the row's other half.
        CommandId id = static_cast<CommandId>(*drawn.chosen);
        if (id == CommandId::ToggleFullscreen && ImGui::GetIO().KeyShift) {
            id = CommandId::ToggleFullscreenStretched;
        }
        Dispatch(Command{id, itemId});
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
    // (see RecognizePress), so the two agree whenever only it is
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

void OverlayApp::OpenEmptyCanvasMenu(platform::Vec2 at) {
    PopupRecord popup;
    popup.kind = PopupKind::EmptyCanvasMenu;
    popup.at = ImVec2(at.x, at.y);
    OpenPopup(std::move(popup));
}

void OverlayApp::RenderEmptyCanvasMenu() {
    const bool up = PopupUp(PopupKind::EmptyCanvasMenu);
    const ContextMenu::Drawn drawn =
        emptyCanvasMenu_.Render(up ? popup_->at : ImVec2(0.0f, 0.0f), [&](std::vector<ContextMenuEntry>& rows) {
            if (up) {
                BuildEmptyCanvasMenuRows(rows);
            }
        });
    PopupDrawn(PopupKind::EmptyCanvasMenu, drawn.up);
    if (drawn.chosen.has_value()) {
        Dispatch(Command{static_cast<CommandId>(*drawn.chosen)});
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
    const auto same = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& queued) {
        const bool aboutAPopup = effect.kind == Effect::Kind::OpenPopup || effect.kind == Effect::Kind::ClosePopup;
        return queued.kind == effect.kind && (!aboutAPopup || queued.popup == effect.popup);
    });
    if (same != effects_.end()) {
        effects_.erase(same);
    }
    effects_.push_back(effect);
}

namespace {
// The popup's ImGui id, as its render function begins it.
const char* PopupId(PopupKind kind) {
    switch (kind) {
        case PopupKind::ItemMenu:
            return kItemContextMenuId;
        case PopupKind::CanvasMenu:
            return kCanvasContextMenuId;
        case PopupKind::EmptyCanvasMenu:
            return kEmptyCanvasMenuId;
        case PopupKind::ItemProperties:
            return kItemPropertiesPopupId;
        case PopupKind::ColorChooser:
            return kColorChooserPopupId;
        case PopupKind::ConfirmDelete:
            return kConfirmDeletePopupId;
    }
    return "";
}
}  // namespace

// ================= The popup that is up =================

// Asked for, it is up - including the frames before one draws it, which
// is what the machine's Popup asks on every tick (see Popup::Offer).
bool OverlayApp::PopupShowing(PopupKind kind) const { return PopupUp(kind); }

void OverlayApp::OpenPopup(PopupRecord popup) {
    // Ending the one there first: its closing is done now, against its own
    // record, before this one takes the record's place.
    editor_.Input().Push(std::make_unique<Popup>(popup.kind), Event{});
    const PopupKind kind = popup.kind;
    popup.drawn = false;
    popup_ = std::move(popup);
    Effect effect{Effect::Kind::OpenPopup};
    effect.popup = kind;
    Queue(effect);
}

void OverlayApp::PopupClosed(PopupKind kind) {
    if (!PopupUp(kind)) {
        return;
    }
    switch (kind) {
        case PopupKind::ItemProperties:
            // A style being dragged when it closed ends with it.
            session_.EndStyleEdit();
            break;
        case PopupKind::ColorChooser:
            KeepPenColor();
            break;
        case PopupKind::ItemMenu:
        case PopupKind::CanvasMenu:
        case PopupKind::EmptyCanvasMenu:
        case PopupKind::ConfirmDelete:
            break;  // what it was about goes with the record
    }
    popup_.reset();
}

void OverlayApp::PopupDrawn(PopupKind kind, bool drawn) {
    if (!PopupUp(kind)) {
        return;
    }
    if (drawn) {
        popup_->drawn = true;
    } else if (popup_->drawn) {
        PopupClosed(kind);  // closed by itself
    }
}

// Ended from outside - by the machine, as a command's scope or view-only
// mode ends it (see Popup::Interrupt): its closing is done at once, since
// the frame that would notice may never come - the overlay put away, the
// app exiting. ImGui's half waits for the next frame, where there is one.
void OverlayApp::ClosePopup(PopupKind kind) {
    PopupClosed(kind);
    Effect effect{Effect::Kind::ClosePopup};
    effect.popup = kind;
    Queue(effect);
}

void OverlayApp::KeepPenWidth() {
    if (!drawWidthDirty_) {
        return;
    }
    drawWidthDirty_ = false;
    if (settings_.Get(setting::kStrokeWidth) != editor_.DrawWidth()) {
        settings_.Set(setting::kStrokeWidth, editor_.DrawWidth());
    }
}

void OverlayApp::KeepPenColor() {
    if (settings_.Get(setting::kStrokeColor) != editor_.DrawColorRGBA()) {
        settings_.Set(setting::kStrokeColor, editor_.DrawColorRGBA());
    }
}

void OverlayApp::KeepPen() {
    KeepPenWidth();
    // Up, the chooser keeps it as it closes; kept now for an overlay that
    // settles with it still up - put away, which leaves a popup up for the
    // next showing, and perhaps never shown again.
    if (PopupUp(PopupKind::ColorChooser)) {
        KeepPenColor();
    }
}

void OverlayApp::CloseInnermostPopup() { Queue(Effect{Effect::Kind::CloseInnermostPopup}); }

// At the top level of the frame, in the order asked: a popup opened
// replaces the one open before it, so the one asked for last is the one
// that stays up.
void OverlayApp::ApplyEffects() {
    std::vector<Effect> effects;
    effects.swap(effects_);
    for (const Effect& effect : effects) {
        switch (effect.kind) {
            case Effect::Kind::OpenPopup:
                // Not one asked for and ended since.
                if (!PopupUp(effect.popup)) {
                    break;
                }
                switch (effect.popup) {
                    case PopupKind::ItemMenu:
                        itemContextMenu_.Open();
                        break;
                    case PopupKind::CanvasMenu:
                        canvasContextMenu_.Open();
                        break;
                    case PopupKind::EmptyCanvasMenu:
                        emptyCanvasMenu_.Open();
                        break;
                    case PopupKind::ItemProperties:
                    case PopupKind::ColorChooser:
                        ImGui::OpenPopup(PopupId(effect.popup));
                        break;
                    case PopupKind::ConfirmDelete:
                        OpenConfirmDelete();
                        break;
                }
                break;
            case Effect::Kind::ClosePopup: {
                // It and whatever is open inside it; nothing, if it is gone
                // already - a row chosen, a click outside.
                ImGuiContext& g = *ImGui::GetCurrentContext();
                const ImGuiID id = ImGui::GetID(PopupId(effect.popup));
                for (int level = 0; level < g.OpenPopupStack.Size; ++level) {
                    if (g.OpenPopupStack[level].PopupId == id) {
                        ImGui::ClosePopupToLevel(level, /*restore_focus_to_window_under_popup=*/true);
                        break;
                    }
                }
                break;
            }
            case Effect::Kind::CloseInnermostPopup: {
                // A help popover, a dropdown: Escape's, since ImGui closes
                // its own popups on Escape only with keyboard nav on, which
                // this app leaves off. From outside any popup's Begin/End,
                // where ImGui::CloseCurrentPopup does nothing: closing to
                // one fewer than are open is ImGui's way of saying "just the
                // innermost", and focus goes back to the panel under it.
                ImGuiContext& g = *ImGui::GetCurrentContext();
                if (g.OpenPopupStack.Size > 0) {
                    ImGui::ClosePopupToLevel(g.OpenPopupStack.Size - 1,
                                             /*restore_focus_to_window_under_popup=*/true);
                }
                break;
            }
            case Effect::Kind::LetGoOfWidget:
                ImGui::ClearActiveID();
                ImGui::ClearDragDrop();
                break;
        }
    }
}

// ================= The color chooser =================

void OverlayApp::OpenColorChooser(platform::Vec2 from) {
    // Only asked for here: the bar's color button fires from the input
    // stream between frames - see Effect.
    PopupRecord popup;
    popup.kind = PopupKind::ColorChooser;
    popup.at = ImVec2(from.x, from.y);
    OpenPopup(std::move(popup));
}

void OverlayApp::OpenItemProperties(ItemId item, std::optional<platform::Vec2> at) {
    // Opened on the next frame - see Effect: a bar button fires outside
    // any frame. Asked for from nowhere in particular, it opens by the
    // selection bar's More button, which is where it is asked from.
    PopupRecord popup;
    popup.kind = PopupKind::ItemProperties;
    popup.item = item;
    if (at.has_value()) {
        popup.at = ImVec2(at->x, at->y);
    } else if (const std::optional<platform::Vec2> more = editor_.SelectionBarButtonCenter(ChromeButton::More)) {
        popup.at = ImVec2(more->x, more->y);
    }
    OpenPopup(std::move(popup));
}

void OverlayApp::RenderColorChooser(float displayW, float displayH) {
    // Beside the point it was asked from, on whichever side has room, so
    // a bar near an edge of the screen does not have its chooser placed
    // off it.
    const bool up = PopupUp(PopupKind::ColorChooser);
    const ImVec2 anchor = up ? popup_->at : ImVec2(0.0f, 0.0f);
    const float gap = Px(20.0f);
    const bool above = anchor.y > displayH * 0.5f;
    const bool toTheLeft = anchor.x > displayW * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(anchor.x + (toTheLeft ? -gap : gap), anchor.y + (above ? -gap : gap)),
                            ImGuiCond_Appearing, ImVec2(toTheLeft ? 1.0f : 0.0f, above ? 1.0f : 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(8.0f, 8.0f));
    const bool open = ImGui::BeginPopup(kColorChooserPopupId);
    ImGui::PopStyleVar();
    // Closed since the last frame, what it was left on is the color from
    // now on, and the next time the app starts - see PopupClosed.
    PopupDrawn(PopupKind::ColorChooser, open);
    if (!open) {
        return;
    }
    // Items re-assert themselves to the front every frame; a popup has to
    // as well, or the first snippet it overlaps covers it.
    KeepPopoverInFront();
    if (!up) {
        // Ended from outside this frame, before ImGui heard of it.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    float rgb[3];
    ColorRGBAToFloats(editor_.DrawColorRGBA(), rgb);
    ImGui::SetNextItemWidth(Px(220.0f));
    if (ImGui::ColorPicker3("##picker", rgb,
                            ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_NoLabel)) {
        editor_.SetDrawColor(FloatsToColorRGBA(rgb, 0xFF));
    }
    ImGui::EndPopup();
}

// ================= Drag previews =================

void OverlayApp::RenderRegionCaptureOverlay() {
    const Framing* framing = editor_.Input().As<Framing>(Level::Gesture);
    if (framing == nullptr) {
        return;
    }
    const Rect frame = framing->Frame();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 pMin(frame.x, frame.y);
    const ImVec2 pMax(frame.x + frame.w, frame.y + frame.h);
    drawList->AddRectFilled(pMin, pMax, theme::AccentU32(40));
    drawList->AddRect(pMin, pMax, theme::AccentU32(255), 0.0f, PxWhole(2.0f), ImDrawFlags_None);
    char dims[32];
    std::snprintf(dims, sizeof(dims), strings::kFormatSizeWidthByHeight, pMax.x - pMin.x, pMax.y - pMin.y);
    drawList->AddText(ImVec2(pMin.x, pMin.y - ImGui::GetTextLineHeight() - Px(1.0f)), IM_COL32(255, 255, 255, 255),
                      dims);
}

void OverlayApp::RenderRectEraserOverlay() {
    const Marking* stroke = editor_.Input().As<Marking>(Level::Gesture);
    if (stroke == nullptr || stroke->GetKind() != Marking::Kind::EraseRect) {
        return;
    }
    // Same visual language as RenderRegionCaptureOverlay, in a cool tone
    // instead of that one's warm orange - erasing is a destructive
    // preview, not a placement one, and the two shouldn't read as the
    // same affordance at a glance.
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const Rect box = stroke->EraseBox();
    const ImVec2 pMin(box.x, box.y);
    const ImVec2 pMax(box.x + box.w, box.y + box.h);
    drawList->AddRectFilled(pMin, pMax, IM_COL32(120, 170, 255, 40));
    drawList->AddRect(pMin, pMax, IM_COL32(120, 170, 255, 255), 0.0f, PxWhole(2.0f), ImDrawFlags_None);
}

}  // namespace sz::ui
