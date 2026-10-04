#include "ui/view/popups.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/config/settings_catalog.h"
#include "generated/ui_strings.h"
#include "ui/icons_generated.h"
#include "ui/interaction/command.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace sz::ui {

using namespace ::sz::core;

Popups::Popups(Session& session, Settings& settings, Editor& editor, ViewHost& host)
    : session_(session), settings_(settings), editor_(editor), host_(host) {}

void Popups::OpenConfirmDelete(DeleteTarget target) {
    // Opened at the next frame's Open even when asked from inside one: the
    // Overview's buttons ask from within its PushID nesting, and the popup
    // belongs at the top level.
    PopupRecord popup;
    popup.kind = PopupKind::ConfirmDelete;
    popup.deleteTarget = std::move(target);
    Open(std::move(popup));
}

// Stage 4, in the order they sit in the stack's row of them.
void Popups::DrawOverCanvas(float displayW, float displayH) {
    RenderItemPropertiesPopover();
    RenderItemContextMenu();
    RenderCanvasContextMenu();
    RenderEmptyCanvasMenu();
    RenderColorChooser(displayW, displayH);
    RenderShapeMenu();
}

void Popups::OpenLibraryReminder(int64_t bytes) {
    PopupRecord popup;
    popup.kind = PopupKind::LibraryReminder;
    popup.bytes = bytes;
    Open(std::move(popup));
}

void Popups::DrawConfirmDelete() { RenderConfirmDeletePopover(); }

void Popups::DrawLibraryReminder() { RenderLibraryReminderPopover(); }

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
    // A permanent hairline rim: black, and any dark custom color, needs an
    // edge of its own to read as a swatch on a dark panel rather than as a
    // hole in it.
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

void Popups::RenderItemPropertiesPopover() {
    // Anchored just below the "More" button that opened it rather than
    // ImGui's default near-mouse placement. Pivot (1, 0): the anchor point
    // is the popover's own top-right corner, not top-left - keeps it from
    // running off the right edge of the screen when that button sits
    // near it (which it usually does - every cluster is right-aligned to
    // its own item, and an item can sit anywhere up to the screen edge).
    const bool up = Up(PopupKind::ItemProperties);
    ImGui::SetNextWindowPos(up ? popup_->at : ImVec2(0.0f, 0.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    const bool open = ImGui::BeginPopup(kItemPropertiesPopupId);
    Drawn(PopupKind::ItemProperties, open);
    if (!open) {
        return;
    }
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
    PopoverItem item{it->id, ItemStyle::Of(*it), it->picture.stored, !it->noteText.empty()};
    RenderItemOpacity(item);
    bool keepAspect = item.style.keepAspect;
    if (ImGui::Checkbox(Labeled(strings::kPopoverKeepAspect, "keepaspect"), &keepAspect)) {
        ItemStyle style = item.style;
        style.keepAspect = keepAspect;
        PreviewPopoverStyle(item, style);
    }
    if (ImGui::IsItemHovered()) {
        HelpTooltip("%s", strings::kPopoverKeepAspectTip);
    }
    RenderItemBackgroundColor(item);
    RenderItemTextStyle(item);
    // A change of style is one step for as long as the hand is on it - a
    // slider dragged, the color picker's square - and ends when it lets go.
    if (!ImGui::IsAnyItemActive()) {
        session_.EndStyleEdit();
    }
    ImGui::EndPopup();
}

void Popups::PreviewPopoverStyle(PopoverItem& item, const ItemStyle& style) {
    session_.PreviewStyle(item.id, style);
    item.style = style;
}

void Popups::RenderItemOpacity(PopoverItem& item) {
    // Foreground (strokes) and background (captured image / color fill)
    // opacity are independent - see Item::foregroundOpacity/
    // Picture::opacity's own doc comments. Background can go all the way
    // to 0 (invisible) unlike foreground, which bottoms out at 10% - a
    // fully invisible drawing surface still has strokes to see, but there
    // being nothing left to *tell* whether it's an item at all is only a
    // real state for the background.
    int foregroundPct = static_cast<int>(std::round(item.style.foregroundOpacity * 100.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    // An id of its own, not shared with the background slider below: both
    // are visible at once on any snippet with a picture in it, and ###
    // hashes only the id, so one spelling for the two of them made them one
    // widget as far as ImGui is concerned - which is an ID conflict it
    // warns about, and a drag it can attribute to the wrong slider.
    if (ImGui::SliderInt(Labeled(strings::kPopoverForeground, "opacityfg"), &foregroundPct, 10, 100, strings::kFormatPercent, ImGuiSliderFlags_AlwaysClamp)) {
        ItemStyle style = item.style;
        style.foregroundOpacity = static_cast<float>(foregroundPct) / 100.0f;
        PreviewPopoverStyle(item, style);
    }
    if (ImGui::IsItemHovered()) {
        HelpTooltip("%s", strings::kPopoverForegroundTip);
    }

    int backgroundPct = static_cast<int>(std::round(item.style.pictureOpacity * 100.0f));
    ImGui::SetNextItemWidth(Px(160.0f));
    if (ImGui::SliderInt(Labeled(strings::kPopoverBackground, "opacitybg"), &backgroundPct, 0, 100, strings::kFormatPercent, ImGuiSliderFlags_AlwaysClamp)) {
        ItemStyle style = item.style;
        style.pictureOpacity = static_cast<float>(backgroundPct) / 100.0f;
        PreviewPopoverStyle(item, style);
    }
    if (ImGui::IsItemHovered()) {
        HelpTooltip("%s", item.pictureStored ? strings::kPopoverBackgroundShotTip
                                                   : strings::kPopoverBackgroundFillTip);
    }
}

void Popups::RenderItemBackgroundColor(PopoverItem& item) {
    // White, and the picker for everything else. White gets a swatch of
    // its own because it is the one color with a meaning here: a no-op
    // multiply tint on a real capture (see Picture::tintColorRGBA), the way
    // back to the picture as it was - and hitting exact white in a picker
    // takes aim. Nothing else is preset: the picker does the whole job.
    ImGui::PushID("##bg_color_section");
    ImGui::TextUnformatted(strings::kPopoverBackgroundColor);
    constexpr uint32_t kWhiteBackground = 0xFFFFFFFFu;
    const auto setTint = [&](uint32_t tintRGBA) {
        ItemStyle style = item.style;
        style.pictureTintRGBA = tintRGBA;
        PreviewPopoverStyle(item, style);
    };
    if (ColorSwatchButton(IM_COL32(255, 255, 255, 255), item.style.pictureTintRGBA == kWhiteBackground)) {
        setTint(kWhiteBackground);
    }
    if (ImGui::IsItemHovered()) {
        HelpTooltip("%s", strings::kPopoverBackgroundWhiteTip);
    }
    ImGui::SameLine(0.0f, Px(6.0f));
    float rgb[3];
    ColorRGBAToFloats(item.style.pictureTintRGBA, rgb);
    if (ImGui::ColorEdit3("##bgcolor", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        setTint(FloatsToColorRGBA(rgb, static_cast<uint8_t>(0xFF)));
    }
    ImGui::PopID();
}

void Popups::RenderItemTextStyle(PopoverItem& item) {
    // Per item rather than app-wide (see Item::noteTextColorRGBA/
    // noteTextSizePx for why). Shown whether or not this item currently
    // has any text: the alternative - appearing only once something's been
    // typed - makes the popover's own height jump around depending on
    // which item opened it, and rules out setting up a caption's look
    // before writing it.
    ImGui::Spacing();
    ImGui::PushID("##note_text_section");
    ImGui::TextUnformatted(strings::kPopoverText);
    if (!item.hasNote) {
        ImGui::SameLine();
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kPopoverTextNoneYet);
    }
    ImGui::SetNextItemWidth(Px(160.0f));
    float textSizePx = item.style.noteTextSizePx;
    if (ImGui::SliderFloat(Labeled(strings::kPopoverTextSize, "notetextsize"), &textSizePx, kNoteTextSizeMin, kNoteTextSizeMax,
                            strings::kFormatPixels, ImGuiSliderFlags_AlwaysClamp)) {
        ItemStyle style = item.style;
        style.noteTextSizePx = textSizePx;
        PreviewPopoverStyle(item, style);
    }
    if (ImGui::IsItemHovered()) {
        HelpTooltip("%s", strings::kPopoverTextSizeTip);
    }
    // ColorEdit4, not the ColorEdit3 the background color uses: unlike
    // the background color, Item::noteTextColorRGBA's own alpha byte is
    // live (text has no separate opacity field), so a caption can be faded
    // from here.
    float rgba[4];
    ColorRGBAToFloats(item.style.noteTextColorRGBA, rgba);
    rgba[3] = static_cast<float>(item.style.noteTextColorRGBA & 0xFFu) / 255.0f;
    if (ImGui::ColorEdit4("##notetextcolor", rgba,
                           ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar)) {
        ItemStyle style = item.style;
        style.noteTextColorRGBA =
            FloatsToColorRGBA(rgba, static_cast<uint8_t>(std::clamp(rgba[3], 0.0f, 1.0f) * 255.0f + 0.5f));
        PreviewPopoverStyle(item, style);
    }
    ImGui::PopID();
}

// ================= The context menu =================

namespace {
// A row's command as it is run: Paste at the point the menu was opened at,
// which is where the right click was - not wherever the pointer has gone
// to choose the row.
Command MenuCommand(Command command, ImVec2 openedAt) {
    if (command.id == CommandId::Paste) {
        command.at = platform::Vec2{openedAt.x, openedAt.y};
    }
    return command;
}
}  // namespace

void Popups::OpenItemMenu(ItemId itemId, ImVec2 at) {
    PopupRecord popup;
    popup.kind = PopupKind::ItemMenu;
    popup.item = itemId;
    popup.at = at;
    Open(std::move(popup));
}

void Popups::RenderItemContextMenu() {
    // Found again every frame rather than held by pointer across them:
    // Duplicate grows the canvas's item vector and the two z-order rows
    // reorder it, either of which moves every Item in it.
    const bool up = Up(PopupKind::ItemMenu);
    const ItemId itemId = up ? popup_->item : 0;
    const ImVec2 openedAt = up ? popup_->at : ImVec2(0.0f, 0.0f);
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
    Drawn(PopupKind::ItemMenu, drawn.up);
    if (drawn.chosen.has_value()) {
        // Fullscreen with Shift held stretches to the screen rather than
        // keeping the snippet's shape - the row's other half.
        CommandId id = static_cast<CommandId>(*drawn.chosen);
        if (id == CommandId::ToggleFullscreen && ImGui::GetIO().KeyShift) {
            id = CommandId::ToggleFullscreenStretched;
        }
        host_.Act(action::RunCommand{MenuCommand(Command{id, itemId}, openedAt)});
    }
}

void Popups::BuildItemContextMenuRows(const Item& item, std::vector<ContextMenuEntry>& rows) {
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
    // names Ctrl+D has to as well - and does, being the same command. The
    // two pastes are here as on empty canvas, Paste at the point the menu
    // was opened at: over a snippet is as good a place to put something as
    // beside it, and a hand that has a snippet under it should not have to
    // find a gap first.
    add(CommandId::Copy, "##menu_copy", icons::kCopy, strings::kMenuCopy, /*separatorAbove=*/true);
    add(CommandId::Cut, "##menu_cut", icons::kScissors, strings::kMenuCut);
    add(CommandId::Paste, "##menu_paste", icons::kClipboard, strings::kMenuPaste);
    add(CommandId::PasteInPlace, "##menu_paste_in_place", icons::kClipboard, strings::kMenuPasteInPlace);
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

void Popups::OpenEmptyCanvasMenu(ImVec2 at) {
    PopupRecord popup;
    popup.kind = PopupKind::EmptyCanvasMenu;
    popup.at = at;
    Open(std::move(popup));
}

void Popups::RenderEmptyCanvasMenu() {
    const bool up = Up(PopupKind::EmptyCanvasMenu);
    const ImVec2 openedAt = up ? popup_->at : ImVec2(0.0f, 0.0f);
    const ContextMenu::Drawn drawn =
        emptyCanvasMenu_.Render(up ? popup_->at : ImVec2(0.0f, 0.0f), [&](std::vector<ContextMenuEntry>& rows) {
            if (up) {
                BuildEmptyCanvasMenuRows(rows);
            }
        });
    Drawn(PopupKind::EmptyCanvasMenu, drawn.up);
    if (drawn.chosen.has_value()) {
        host_.Act(action::RunCommand{MenuCommand(Command{static_cast<CommandId>(*drawn.chosen)}, openedAt)});
    }
}

void Popups::BuildEmptyCanvasMenuRows(std::vector<ContextMenuEntry>& rows) const {
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
    add(CommandId::PasteInPlace, "##emptymenu_paste_in_place", &icons::kClipboard, strings::kMenuPasteInPlace);
    add(CommandId::SelectAll, "##emptymenu_select_all", &icons::kSelect, strings::kMenuSelectAll);

    // The Overview last, where the row used most is found without reading.
    add(CommandId::CheatSheet, "##emptymenu_cheat_sheet", &icons::kKeyboard, strings::kMenuCheatSheet,
        /*separatorAbove=*/true);
    add(CommandId::Settings, "##emptymenu_settings", &icons::kSettings, strings::kMenuSettings);
    add(CommandId::Overview, "##emptymenu_overview", &icons::kLayoutGrid, strings::kMenuOverview);
}

// ================= Effects =================

void Popups::Queue(const Effect& effect) {
    const auto same = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& queued) {
        const bool aboutAPopup = effect.kind == Effect::Kind::OpenPopup || effect.kind == Effect::Kind::ClosePopup;
        return queued.kind == effect.kind && (!aboutAPopup || queued.popup == effect.popup);
    });
    if (same != effects_.end()) {
        effects_.erase(same);
    }
    effects_.push_back(effect);
}

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
        case PopupKind::ShapeMenu:
            return kShapeMenuId;
        case PopupKind::ConfirmDelete:
            return kConfirmDeletePopupId;
        case PopupKind::LibraryReminder:
            return kLibraryReminderPopupId;
    }
    return "";
}

// ================= The popup that is up =================

void Popups::Open(PopupRecord popup) {
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

void Popups::Closed(PopupKind kind) {
    if (!Up(kind)) {
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
        case PopupKind::ShapeMenu:
        case PopupKind::ConfirmDelete:
        case PopupKind::LibraryReminder:
            break;  // what it was about goes with the record
    }
    popup_.reset();
}

void Popups::Drawn(PopupKind kind, bool drawn) {
    if (!Up(kind)) {
        return;
    }
    if (drawn) {
        popup_->drawn = true;
    } else if (popup_->drawn) {
        Closed(kind);  // closed by itself
    }
}

void Popups::Close(PopupKind kind) {
    Closed(kind);
    Effect effect{Effect::Kind::ClosePopup};
    effect.popup = kind;
    Queue(effect);
}

void Popups::KeepPenColor() {
    if (settings_.Get(setting::kStrokeColor) != editor_.DrawColorRGBA()) {
        settings_.Set(setting::kStrokeColor, editor_.DrawColorRGBA());
    }
}

void Popups::CloseInnermost() { Queue(Effect{Effect::Kind::CloseInnermostPopup}); }

void Popups::LetGoOfWidget() { Queue(Effect{Effect::Kind::LetGoOfWidget}); }

void Popups::KeepChooserColor() {
    if (Up(PopupKind::ColorChooser)) {
        KeepPenColor();
    }
}

// At the top level of the frame, in the order asked: a popup opened
// replaces the one open before it, so the one asked for last is the one
// that stays up.
void Popups::ApplyEffects() {
    std::vector<Effect> effects;
    effects.swap(effects_);
    std::optional<PopupKind> opened;
    for (const Effect& effect : effects) {
        switch (effect.kind) {
            case Effect::Kind::OpenPopup:
                // Not one asked for and ended since.
                if (!Up(effect.popup)) {
                    break;
                }
                opened = effect.popup;
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
                    case PopupKind::ShapeMenu:
                        shapeMenu_.Open();
                        break;
                    case PopupKind::ItemProperties:
                    case PopupKind::ColorChooser:
                    case PopupKind::ConfirmDelete:
                    case PopupKind::LibraryReminder:
                        ImGui::OpenPopup(PopupId(effect.popup));
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
    // Opened and closed again in this one pass - an Escape in the same gap
    // between frames as the click that asked for it - it is not open at its
    // first draw, which reads that as not drawn yet rather than closed (see
    // Drawn). Closed here, or its record stayed, and with it the machine's
    // Popup level, claiming every key and click after it.
    if (opened.has_value() && Up(*opened) && !popup_->drawn && !ImGui::IsPopupOpen(PopupId(*opened))) {
        Closed(*opened);
    }
}

// ================= The color chooser =================

void Popups::OpenColorChooser(ImVec2 from) {
    // Only asked for here: the bar's color button fires from the input
    // stream between frames - see Effect.
    PopupRecord popup;
    popup.kind = PopupKind::ColorChooser;
    popup.at = from;
    Open(std::move(popup));
}

void Popups::OpenItemProperties(ItemId item, std::optional<platform::Vec2> at) {
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
    Open(std::move(popup));
}

void Popups::RenderColorChooser(float displayW, float displayH) {
    // Beside the point it was asked from, on whichever side has room, so
    // a bar near an edge of the screen does not have its chooser placed
    // off it.
    const bool up = Up(PopupKind::ColorChooser);
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
    // now on, and the next time the app starts - see Closed.
    Drawn(PopupKind::ColorChooser, open);
    if (!open) {
        return;
    }
    if (!up) {
        // Ended from outside this frame, before ImGui heard of it.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    // With the ink's own alpha, the stroke's opacity: a stroke at half
    // strength blends with the ones under it, where the snippet's opacity
    // fades all of them together (see DrawItemContent).
    float rgba[4];
    ColorRGBAToFloats4(editor_.DrawColorRGBA(), rgba);
    ImGui::SetNextItemWidth(Px(220.0f));
    if (ImGui::ColorPicker4("##picker", rgba,
                            ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar)) {
        editor_.SetDrawColor(FloatsToColorRGBA4(rgba));
    }
    ImGui::EndPopup();
}

// ================= The shape menu =================

void Popups::OpenShapeMenu(Tool tool, ImVec2 at) {
    PopupRecord popup;
    popup.kind = PopupKind::ShapeMenu;
    popup.tool = tool;
    popup.at = at;
    Open(std::move(popup));
}

void Popups::RenderShapeMenu() {
    const bool up = Up(PopupKind::ShapeMenu);
    const Tool tool = up ? popup_->tool : Tool::Draw;
    const ContextMenu::Drawn drawn =
        shapeMenu_.Render(up ? popup_->at : ImVec2(0.0f, 0.0f), [&](std::vector<ContextMenuEntry>& rows) {
            if (up) {
                BuildShapeMenuRows(tool, rows);
            }
        });
    Drawn(PopupKind::ShapeMenu, drawn.up);
    if (drawn.chosen.has_value()) {
        host_.Act(action::RunCommand{Command{static_cast<CommandId>(*drawn.chosen)}});
    }
}

void Popups::BuildShapeMenuRows(Tool tool, std::vector<ContextMenuEntry>& rows) const {
    // The tool's shape is marked whether or not it is in hand: it is kept
    // (see Editor::PenShape), and the button wears its icon either way.
    const DrawShape shape = tool == Tool::Draw ? editor_.PenShape() : editor_.EraserShape();
    const auto add = [&](CommandId id, const char* widgetId, const Icon* icon, const char* label, DrawShape drawn,
                         const char* keys) {
        ContextMenuEntry row = MenuRow(Command{id}, widgetId, icon, label);
        row.shortcut = keys;
        row.current = shape == drawn;
        rows.push_back(std::move(row));
    };
    if (tool == Tool::Draw) {
        add(CommandId::PickPen, "##shapemenu_pen", &icons::kPen, strings::kMenuPen, DrawShape::Freehand, "");
        add(CommandId::PickLine, "##shapemenu_line", &icons::kLine, strings::kMenuLine, DrawShape::Line,
            strings::kMenuLineKeys);
        add(CommandId::PickRectangle, "##shapemenu_rectangle", &icons::kRectangle, strings::kMenuRectangle,
            DrawShape::Rectangle, strings::kMenuRectangleKeys);
    } else {
        add(CommandId::PickEraser, "##shapemenu_eraser", &icons::kEraser, strings::kMenuEraser, DrawShape::Freehand,
            "");
        add(CommandId::PickRectangleEraser, "##shapemenu_rectangle_eraser", &icons::kEraserRect,
            strings::kMenuRectangleEraser, DrawShape::Rectangle, strings::kMenuRectangleKeys);
    }
}

// ================= A tile's context menu =================

void Popups::OpenCanvasMenu(CanvasId canvasId, ImVec2 at) {
    PopupRecord popup;
    popup.kind = PopupKind::CanvasMenu;
    popup.canvas = canvasId;
    popup.at = at;
    Open(std::move(popup));
}

void Popups::RenderCanvasContextMenu() {
    // Looked up afresh every frame, like the snippet menu's: a canvas can
    // be deleted or moved to another folder from elsewhere while this is
    // up, and either takes it off the bar.
    const bool up = Up(PopupKind::CanvasMenu);
    const CanvasId canvasId = up ? popup_->canvas : 0;
    const Canvas* canvas = nullptr;
    if (up) {
        const Canvas* found = Manager().FindCanvas(canvasId);
        if (found != nullptr && !Manager().IsDeleted(*found)) {
            canvas = found;
        }
    }
    const ContextMenu::Drawn drawn =
        canvasContextMenu_.Render(up ? popup_->at : ImVec2(0.0f, 0.0f), [&](std::vector<ContextMenuEntry>& rows) {
            if (canvas != nullptr) {
                BuildCanvasContextMenuRows(*canvas, rows);
            }
        });
    Drawn(PopupKind::CanvasMenu, drawn.up);
    if (drawn.chosen.has_value()) {
        Command command{static_cast<CommandId>(*drawn.chosen)};
        command.canvas = canvasId;
        host_.Act(action::RunCommand{command});
    }
}

void Popups::BuildCanvasContextMenuRows(const Canvas& canvas, std::vector<ContextMenuEntry>& rows) const {
    // Deliberately short. The last canvas of a folder is deletable like
    // any other - see the Overview's own delete button for why there is no
    // "and this folder holds more than one" condition on it.
    Command deleteCanvas{CommandId::DeleteCanvas};
    deleteCanvas.canvas = canvas.id;
    rows.push_back(MenuRow(deleteCanvas, "##canvasmenu_delete", &icons::kTrash, strings::kMenuDeleteCanvas));
}

// ================= Menu rows =================

std::string Popups::MenuShortcutLabel(CommandId id) const {
    // Live, not Stored: what is bound right now, profile and all, is what
    // the row has to promise.
    const std::vector<platform::KeyCombo> keys = KeysFor(id, Cfg(), settings_.Live().shortcuts);
    return keys.empty() ? std::string() : FormatKeyComboLabel(keys.front());
}

ContextMenuEntry Popups::MenuRow(const Command& command, const char* id, const Icon* icon, const char* label,
                                     bool separatorAbove) const {
    return ContextMenuEntry{static_cast<int>(command.id), id, icon, label, MenuShortcutLabel(command.id),
                            editor_.Available(command), separatorAbove};
}

void Popups::RenderConfirmDeletePopover() {
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const bool open = ImGui::BeginPopup(kConfirmDeletePopupId);
    Drawn(PopupKind::ConfirmDelete, open);
    if (!open) {
        return;
    }
    if (!Up(PopupKind::ConfirmDelete)) {
        // Ended from outside this frame, before ImGui heard of it.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const DeleteTarget target = *popup_->deleteTarget;
    const bool isFolder = target.kind == DeleteTarget::Kind::Folder;
    const bool deletedIn = target.kind == DeleteTarget::Kind::DeletedCanvasesIn;
    const bool trash = target.kind == DeleteTarget::Kind::Trash;
    const char* word = isFolder ? strings::kDeleteConfirmFolderWord : strings::kDeleteConfirmCanvasWord;
    // A delete marks the thing, which can be restored, and says so; a delete
    // of something deleted already is for good, and says that.
    const bool forGood = target.forGood || deletedIn || trash;
    // Several folders at once - the tutorial's, at its Done - are named
    // each, and spoken of as several.
    const bool several = isFolder && !target.alsoFolders.empty();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(220.0f));
    if (trash) {
        // Counted as Show deleted counts it, so the two numbers agree.
        ImGui::TextUnformatted(strings::kDeleteConfirmPromptTrash);
        const size_t count = Manager().DeletedFolderAndCanvasCount();
        if (count == 1) {
            ImGui::TextColored(theme::kDanger, "%s", strings::kDeleteConfirmTrashContentsOne);
        } else {
            ImGui::TextColored(theme::kDanger, strings::kDeleteConfirmTrashContentsMany, count);
        }
    } else if (deletedIn) {
        ImGui::Text(strings::kDeleteConfirmPromptDeletedIn, target.name.c_str());
    } else if (several) {
        std::vector<std::string> names{target.name};
        for (const uint64_t also : target.alsoFolders) {
            const Folder* found = Manager().FindFolder(also);
            names.push_back(found != nullptr ? found->name : std::string());
        }
        std::string list;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0) {
                list += i + 1 == names.size() ? strings::kDeleteConfirmAnd : ", ";
            }
            list += "\"" + names[i] + "\"";
        }
        ImGui::Text(strings::kDeleteConfirmPromptFolders, list.c_str());
    } else {
        ImGui::Text(forGood ? strings::kDeleteConfirmPromptForGood : strings::kDeleteConfirmPrompt, word,
                    target.name.c_str());
    }
    if (isFolder) {
        ImGui::TextColored(theme::kDanger, "%s",
                           several ? strings::kDeleteConfirmAlsoCanvasesMany : strings::kDeleteConfirmAlsoCanvases);
    }
    // With the retention period on, "can be restored" has an end, and says
    // when: the dialog is where a person decides how much that matters.
    if (forGood) {
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kDeleteConfirmCannotUndo);
    } else if (Cfg().purgeDeleted) {
        ImGui::TextColored(theme::kGraphite200,
                           several ? strings::kDeleteConfirmRestorableForMany : strings::kDeleteConfirmRestorableFor,
                           Cfg().purgeDeletedAfterDays);
    } else {
        ImGui::TextColored(theme::kGraphite200, "%s",
                           several ? strings::kDeleteConfirmRestorableMany : strings::kDeleteConfirmRestorable);
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const bool cancelPressed = ImGui::Button(Labeled(strings::kDeleteConfirmCancel, strings::kMoveCopyCancel));
    ImGui::SameLine();
    const bool deletePressed =
        DangerButton("##confirmdelete", icons::kTrash,
                     trash     ? strings::kDeleteConfirmEmptyTrash
                     : forGood ? strings::kDeleteConfirmDeleteForGood
                               : strings::kDeleteConfirmDelete);
    // CloseCurrentPopup must be called while this popup is still current -
    // i.e. before EndPopup, not after (it operates on the popup ID stack,
    // which EndPopup pops). The delete itself is an action, done once the
    // frame is drawn.
    if (cancelPressed || deletePressed) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();

    if (deletePressed) {
        host_.Act(action::Delete{target});
    }
}

// Where the library stands and what makes it smaller, at the middle of the
// screen as the delete confirmation is: the trash, which only ever grows
// with retention off, and the setting that empties it - each a button that
// goes there.
void Popups::RenderLibraryReminderPopover() {
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const bool open = ImGui::BeginPopup(kLibraryReminderPopupId);
    Drawn(PopupKind::LibraryReminder, open);
    if (!open) {
        return;
    }
    if (!Up(PopupKind::LibraryReminder)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    // In Explorer's units, as the setting is: megabytes up to a gigabyte,
    // then gigabytes with a decimal.
    const double megabytes = static_cast<double>(popup_->bytes) / (1024.0 * 1024.0);
    char size[32];
    if (megabytes < 1024.0) {
        std::snprintf(size, sizeof(size), "%.0f MB", megabytes);
    } else {
        std::snprintf(size, sizeof(size), "%.1f GB", megabytes / 1024.0);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(320.0f));
    ImGui::Text(strings::kLibraryReminderText, size);
    ImGui::Spacing();
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kLibraryReminderHint);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const bool trashPressed = ImGui::Button(Labeled(strings::kLibraryReminderShowTrash, "reminder_trash"));
    ImGui::SameLine();
    const bool settingsPressed = ImGui::Button(Labeled(strings::kLibraryReminderSettings, "reminder_settings"));
    ImGui::SameLine();
    const bool okPressed = ImGui::Button(Labeled(strings::kLibraryReminderOk, "reminder_ok"));
    if (trashPressed || settingsPressed || okPressed) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();

    if (trashPressed) {
        host_.Act(action::ShowTrash{});
    } else if (settingsPressed) {
        host_.Act(action::ShowTrashSettings{});
    }
}

}  // namespace sz::ui
