#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "core/config/display_choice.h"
#include "ui/icons_generated.h"
#include "ui/settings_widgets.h"

#include <imgui.h>
// For the combo preview the edit-target picker draws a profile's name in -
// see RenderEditTargetPicker.
#include <imgui_internal.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Overview: canvas switcher / manager / move-copy picker =================


void OverlayApp::RenderOverview(float displayW, float displayH) {
    if (!IsOverviewOpen()) {
        return;
    }
    // A click on the backdrop closes it - after this frame, which draws it
    // whole, as every action waits for the draw to finish (see Act).
    if (PanelBackdrop("##overview_backdrop", displayW, displayH)) {
        Act(action::ClosePanel{PanelKind::Overview});
    }

    constexpr float kPanelMarginFrac = 0.08f;
    const ImVec2 panelMin(displayW * kPanelMarginFrac, displayH * kPanelMarginFrac);
    const ImVec2 panelSize(displayW * (1.0f - 2.0f * kPanelMarginFrac), displayH * (1.0f - 2.0f * kPanelMarginFrac));
    ImGui::SetNextWindowPos(panelMin);
    ImGui::SetNextWindowSize(panelSize);
    ImGui::Begin("##overview_panel", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                      ImGuiWindowFlags_NoMove);

    RenderOverviewHeader();
    ImGui::Separator();
    BeginOverviewPreviewFrame();

    // Both the Canvases body below and the Settings page render
    // fine even while picking (pickerItemId_ set) - the tab strip is only
    // hidden there, not the tab logic - but picking a move/copy
    // destination is exactly the "browse canvases" task, so force the
    // Canvases body regardless of whatever overviewTab_ happens to still
    // hold from a previous, non-picker visit.
    const bool showCanvasesBody = pickerItemId_.has_value() || overviewTab_ == OverviewTab::Canvases;

    ImGui::BeginChild("##overview_body", ImVec2(0.0f, -Px(40.0f)), ImGuiChildFlags_None);
    // A new page starts at its beginning. All three tabs and both About
    // pages share this one scrolling child, so without this, opening the
    // licenses from halfway down the About text drops you halfway down the
    // licenses - and the buttons that ask for the switch are in the footer,
    // outside this child, where SetScrollY would move the wrong window.
    if (overviewBodyScrollToTop_) {
        ImGui::SetScrollY(0.0f);
        overviewBodyScrollToTop_ = false;
    }
    if (!showCanvasesBody && overviewTab_ == OverviewTab::About) {
        RenderOverviewAboutPanel();
    } else if (!showCanvasesBody) {
        settingsPage_.Draw();
    } else {
        SettleDeletedFolderShown();
        RenderFolderSidebar();
        ImGui::SameLine();
        RenderCanvasGrid(displayW, displayH);
    }
    ImGui::EndChild();  // ##overview_body

    RenderOverviewFooter(showCanvasesBody);

    ImGui::End();
}

void OverlayApp::RenderOverviewHeader() {
    // Null with an empty library (see CanvasManager's class comment) -
    // the Overview is exactly the screen that has to keep working then,
    // since it's where a new canvas or folder comes from.
    const Canvas* currentCanvas = Manager().CurrentOrNull();
    if (pickerItemId_.has_value() && currentCanvas) {
        const auto it = std::find_if(currentCanvas->items.begin(), currentCanvas->items.end(),
                                      [&](const Item& i) { return i.id == *pickerItemId_; });
        const std::string itemName = it != currentCanvas->items.end() ? it->name : strings::kMoveCopyItemWord;
        ImGui::TextColored(theme::Accent(), strings::kMoveCopyPrompt, pickerIsCopy_ ? strings::kMoveCopyCopy : strings::kMoveCopyMove, itemName.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(Labeled(strings::kMoveCopyCancel, "pickercancel"))) {
            Act(action::ClosePanel{PanelKind::Overview});
        }
        return;
    }

    // Picker mode (above) never shows these - it has its own
    // single-purpose header, and Settings has no business being
    // reachable mid-pick (see overviewTab_'s own doc comment).
    if (TabButton("overviewtabcanvases", strings::kOverviewTabCanvases, overviewTab_ == OverviewTab::Canvases)) {
        SwitchOverviewTab(OverviewTab::Canvases);
    }
    ImGui::SameLine();
    if (TabButton("overviewtabsettings", strings::kOverviewTabSettings, overviewTab_ == OverviewTab::Settings)) {
        SwitchOverviewTab(OverviewTab::Settings);
    }
    ImGui::SameLine();
    if (TabButton("overviewtababout", strings::kOverviewTabAbout, overviewTab_ == OverviewTab::About)) {
        SwitchOverviewTab(OverviewTab::About);
    }
    if (overviewTab_ != OverviewTab::Canvases) {
        return;
    }

    // What the canvas thumbnails may draw, right-aligned on the tab row -
    // the reason to reach for either is being unable to tell two canvases
    // apart, so they belong next to the canvases rather than in Settings.
    // Only while a canvas grid is showing: a preview toggle over the About
    // text is a control with nothing to act on.
    //
    // What is deleted is shown in the same folders and grid, where it was,
    // rather than on a page of its own - so this is a way of looking at
    // them, beside the other two, and says how much there is to see.
    char deletedLabel[96];
    const size_t deletedCount = Manager().DeletedFolderAndCanvasCount();
    if (deletedCount > 0) {
        std::snprintf(deletedLabel, sizeof(deletedLabel), strings::kOverviewShowDeletedCount, deletedCount);
    } else {
        std::snprintf(deletedLabel, sizeof(deletedLabel), "%s", strings::kOverviewShowDeleted);
    }
    const float checkboxWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float gapBeforePreviews = ImGui::GetStyle().ItemSpacing.x * 4.0f;
    const float controlsWidth = checkboxWidth + ImGui::CalcTextSize(deletedLabel).x +
                                gapBeforePreviews + ImGui::CalcTextSize(strings::kOverviewPreviewsLabel).x +
                                ImGui::GetStyle().ItemSpacing.x + checkboxWidth +
                                ImGui::CalcTextSize(strings::kOverviewPreviewsVector).x +
                                ImGui::GetStyle().ItemSpacing.x + checkboxWidth +
                                ImGui::CalcTextSize(strings::kOverviewPreviewsBitmap).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                          std::max(0.0f, ImGui::GetContentRegionAvail().x - controlsWidth));
    // Vertically centered against the tab buttons, which are taller
    // than a checkbox's own frame.
    const float rowCenterOffset = (ImGui::GetItemRectSize().y - ImGui::GetFrameHeight()) * 0.5f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, rowCenterOffset));
    if (ImGui::Checkbox(Labeled(deletedLabel, "showdeleted"), &showDeleted_)) {
        SettleDeletedFolderShown();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kOverviewShowDeletedHelp);
    }
    ImGui::SameLine(0.0f, gapBeforePreviews);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kGraphite300, "%s", strings::kOverviewPreviewsLabel);
    ImGui::SameLine();
    // "Vector", not "Strokes": in bitmap mode a stroke *is* pixels, so a
    // box labeled Strokes that leaves them showing when it is unchecked
    // reads as a bug rather than as the two halves of the drawing model.
    bool strokes = settings_.Get(setting::kOverviewShowsStrokes);
    if (ImGui::Checkbox(Labeled(strings::kOverviewPreviewsVector, "prevvector"), &strokes)) {
        settings_.Set(setting::kOverviewShowsStrokes, strokes);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kOverviewPreviewsVectorHelp);
    }
    ImGui::SameLine();
    bool bitmaps = settings_.Get(setting::kOverviewShowsBitmaps);
    if (ImGui::Checkbox(Labeled(strings::kOverviewPreviewsBitmap, "prevbitmap"), &bitmaps)) {
        settings_.Set(setting::kOverviewShowsBitmaps, bitmaps);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", strings::kOverviewPreviewsBitmapHelp);
    }
}

void OverlayApp::RenderFolderSidebar() {
    constexpr float kFolderRowHeight = 34.0f;
    constexpr float kFolderRowGap = 4.0f;
    const std::vector<Folder>& folders = Manager().Folders();
    const FolderId currentFolderId = OverviewFolderId();
    const bool showingDeleted = ShowingDeleted();
    const std::time_t now = std::time(nullptr);

    ImGui::BeginChild("##folder_sidebar", ImVec2(OverviewSidebarWidth(), 0.0f), ImGuiChildFlags_None);
    if (!showingDeleted &&
        std::all_of(folders.begin(), folders.end(), [&](const Folder& f) { return Manager().IsDeleted(f); })) {
        // Every folder has been deleted - legal now (see CanvasManager's
        // class comment), and reachable in one step from a library with a
        // single folder in it. "New folder" in the footer is still right
        // there, and so is "New canvas", which mints a folder to put it in.
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kOverviewNoFolders);
        ImGui::PopTextWrapPos();
    }
    for (size_t fi = 0; fi < folders.size(); ++fi) {
        const Folder& f = folders[fi];
        const bool deleted = Manager().IsDeleted(f);
        // Skipped here rather than filtered out beforehand, so that `fi`
        // stays the folder's place in Folders(), which ReorderFolder takes.
        if (deleted && !showingDeleted) {
            continue;
        }
        // With Show deleted on, a folder that is deleted or holds a deleted
        // canvas is marked out, with the two buttons that act on what is
        // deleted in it, and every other folder is dimmed.
        const bool marked = showingDeleted && Manager().HoldsDeleted(f);
        const bool dimmed = showingDeleted && !marked;
        const bool isCurrentFolder = f.id == currentFolderId;
        const bool isRenamingThis = renamingFolderId_ == f.id;
        ImGui::PushID(static_cast<int>(f.id));
        if (dimmed) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * kDimmedAlpha);
        }

        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        // Room for the buttons is always reserved: the last folder is
        // deletable like any other.
        const float buttonsWidth = marked ? Px(kPillButtonSize) * 2.0f + Px(kDeletedButtonGap) : Px(kPillButtonSize);
        const float selectWidth = rowWidth - buttonsWidth - Px(6.0f);
        const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + Px(kFolderRowHeight));
        ImDrawList* sidebarDrawList = ImGui::GetWindowDrawList();
        // Through GetColorU32, which a dimmed row's alpha applies to.
        if (isCurrentFolder) {
            const ImVec4 fill = marked ? ImVec4(theme::kDanger.x, theme::kDanger.y, theme::kDanger.z, 0.32f)
                                       : ImVec4(theme::Accent().x, theme::Accent().y, theme::Accent().z, 0.2f);
            sidebarDrawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(fill), Px(theme::kRadiusSm));
        } else if (marked) {
            sidebarDrawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(theme::kDangerSoft),
                                           Px(theme::kRadiusSm));
        }

        if (isRenamingThis) {
            // No InvisibleButton/drag-drop this frame: it would sit right
            // under the InputText drawn below at the same screen rect, and
            // (per the "overlapping widgets resolve first-submitted-wins"
            // gotcha - see docs/ARCHITECTURE.md) would win every click,
            // leaving the input field unclickable.
            ImGui::Dummy(ImVec2(selectWidth, Px(kFolderRowHeight)));
        } else {
            if (ImGui::InvisibleButton("##folderrow", ImVec2(selectWidth, Px(kFolderRowHeight)))) {
                // A deleted folder is looked into, not browsed - see
                // deletedFolderShown_.
                if (deleted) {
                    Act(action::ShowDeletedFolder{f.id});
                } else {
                    Act(action::SwitchFolder{f.id});
                }
            }
            const bool rowHovered = ImGui::IsItemHovered();
            if (rowHovered && !deleted && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                BeginRenaming(f.id, std::nullopt, f.name);
            }
            if (rowHovered && !isCurrentFolder) {
                sidebarDrawList->AddRectFilled(rowMin, ImVec2(rowMin.x + selectWidth, rowMax.y),
                                                ImGui::GetColorU32(theme::kHoverWash), Px(theme::kRadiusSm));
            }
            if (rowHovered && marked) {
                if (deleted && Cfg().purgeDeleted) {
                    ImGui::SetTooltip("%s\n%s", DeletedWhen(f.deletedAt, now).c_str(),
                                      GoesOn(f.deletedAt, Cfg().purgeDeletedAfterDays).c_str());
                } else if (deleted) {
                    ImGui::SetTooltip("%s", DeletedWhen(f.deletedAt, now).c_str());
                } else {
                    const size_t count = Manager().MarkedCanvasesIn(f.id).size();
                    ImGui::SetTooltip(count == 1 ? strings::kDeletedHoldsOne : strings::kDeletedHoldsMany, count);
                }
            }
            // Nothing is dragged out of or into what is deleted: it stays
            // where it was until it is restored.
            if (!deleted) {
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("HB_FOLDER_REORDER", &f.id, sizeof(FolderId));
                    ImGui::TextUnformatted(f.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HB_FOLDER_REORDER")) {
                        const FolderId draggedId = *static_cast<const FolderId*>(payload->Data);
                        if (draggedId != f.id) {
                            Act(action::ReorderFolder{draggedId, fi});
                        }
                    }
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HB_CANVAS_REORDER")) {
                        const CanvasId draggedCanvasId = *static_cast<const CanvasId*>(payload->Data);
                        Act(action::MoveCanvasToFolder{draggedCanvasId, f.id});
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        }

        if (isRenamingThis) {
            ImGui::SetCursorScreenPos(
                ImVec2(rowMin.x + Px(6.0f), rowMin.y + (Px(kFolderRowHeight) - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::SetNextItemWidth(selectWidth - Px(12.0f));
            if (renameJustFocused_) {
                ImGui::SetKeyboardFocusHere();
                renameJustFocused_ = false;
            }
            ImGui::InputText("##renamefolder", renameBuffer_, sizeof(renameBuffer_));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                Act(action::RenameFolder{f.id, std::string(renameBuffer_)});
                renamingFolderId_.reset();
                window_->ReleaseTextInput();
            } else if (ImGui::IsItemDeactivated()) {
                renamingFolderId_.reset();
                window_->ReleaseTextInput();
            }
        } else {
            const ImVec4& ink = marked && !isCurrentFolder ? theme::kDeletedInk
                                : isCurrentFolder         ? theme::kWhite
                                                          : theme::kGraphite200;
            const ImVec2 textPos(rowMin.x + Px(10.0f),
                                 rowMin.y + (Px(kFolderRowHeight) - ImGui::GetTextLineHeight()) * 0.5f);
            sidebarDrawList->PushClipRect(rowMin, ImVec2(rowMin.x + selectWidth - Px(4.0f), rowMax.y), true);
            sidebarDrawList->AddText(textPos, ImGui::GetColorU32(ink), f.name.c_str());
            sidebarDrawList->PopClipRect();
        }

        if (!isRenamingThis) {
            const float buttonY = rowMin.y + (Px(kFolderRowHeight) - Px(kPillButtonSize)) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + selectWidth + Px(4.0f), buttonY));
            if (marked) {
                // Both act on what is deleted in the folder: all of it back,
                // or all of it gone for good - the folder with it only if
                // the folder is what was deleted.
                switch (DeletedButtons(strings::kDeletedRestoreFolderTip,
                                       deleted ? strings::kDeletedDeleteForGoodTip
                                               : strings::kDeletedDeleteDeletedInFolderTip)) {
                    case DeletedButton::Restore:
                        Act(action::Restore{f.id});
                        break;
                    case DeletedButton::DeleteForGood:
                        AskToDelete(DeleteTarget{deleted ? DeleteTarget::Kind::Folder
                                                         : DeleteTarget::Kind::DeletedCanvasesIn,
                                                 f.id, f.name, /*forGood=*/true});
                        break;
                    case DeletedButton::None:
                        break;
                }
            } else {
                // Not while picking: see SendPickedItemTo.
                ImGui::BeginDisabled(pickerItemId_.has_value());
                const bool deletePressed = DangerIconButton("##delfolder", icons::kTrash);
                ImGui::EndDisabled();
                if (deletePressed) {
                    AskToDelete(DeleteTarget{DeleteTarget::Kind::Folder, f.id, f.name});
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", strings::kOverviewDeleteFolder);
                }
            }
        }

        // A just-made folder is the last row, which may be below the fold -
        // bring it into view once (see overviewScrollToFolderId_). By
        // position rather than ImGui::SetScrollHereY, which measures the
        // last item submitted: that is this row's delete button, not the
        // row, and the two are different heights.
        if (overviewScrollToFolderId_ == f.id) {
            ImGui::SetScrollFromPosY(rowMin.y - ImGui::GetWindowPos().y, 0.5f);
        }
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y + Px(kFolderRowGap)));
        if (dimmed) {
            ImGui::PopStyleVar();
        }
        ImGui::PopID();
    }
    // Whether or not it was found, so a stale id can't keep pulling the
    // sidebar around on later frames - same as the canvas grid's own.
    overviewScrollToFolderId_.reset();
    // Closes out the last row's SetCursorScreenPos boundary extension above -
    // ImGui asserts if a window/child ends right after a manual cursor move
    // with no item submitted afterward to confirm growing to that position.
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndChild();
}

void OverlayApp::RenderCanvasGrid(float displayW, float displayH) {
    const ImVec2 tileSize = Px(200.0f, 130.0f);
    constexpr float kSpacing = 14.0f;
    const std::vector<Canvas>& canvases = Manager().Canvases();
    const CanvasId currentCanvasId = Manager().CurrentCanvasId();  // 0 when there is none
    const FolderId currentFolderId = OverviewFolderId();
    const bool showingDeleted = ShowingDeleted();
    const std::time_t now = std::time(nullptr);

    // Canvases belonging to the shown folder that aren't deleted (or all of
    // them, with Show deleted on), in Canvases() order, and beside each its
    // place among *all* of that folder's canvases - which is what
    // ReorderCanvas expects for `newIndex` (it's scoped within the moved
    // canvas's own folder, hidden ones and all).
    std::vector<size_t> folderCanvasIndices;
    std::vector<size_t> folderCanvasPlaces;
    size_t placeInFolder = 0;
    for (size_t i = 0; i < canvases.size(); ++i) {
        if (canvases[i].folderId != currentFolderId) {
            continue;
        }
        if (showingDeleted || !Manager().IsDeleted(canvases[i])) {
            folderCanvasIndices.push_back(i);
            folderCanvasPlaces.push_back(placeInFolder);
        }
        ++placeInFolder;
    }

    ImGui::BeginChild("##overview_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    const float availW = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((availW + Px(kSpacing)) / (tileSize.x + Px(kSpacing))));

    // Built once for the whole grid rather than per tile.
    const PreviewTextureFn previewTexture = PreviewTextureLookup();

    if (folderCanvasIndices.empty()) {
        // A folder holding nothing has always been possible (a freshly
        // created one starts out that way); what's new is that the
        // *library* can be in the same state, with no canvas current at
        // all - so the two get separate wording, since only the second
        // one means there's nothing being edited behind this panel.
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(theme::kGraphite200, "%s",
                           Manager().HasCurrentCanvas() ? strings::kOverviewFolderEmpty : strings::kOverviewLibraryEmpty);
        ImGui::PopTextWrapPos();
    }
    for (size_t idxInFolder = 0; idxInFolder < folderCanvasIndices.size(); ++idxInFolder) {
        const Canvas& c = canvases[folderCanvasIndices[idxInFolder]];
        if (idxInFolder % static_cast<size_t>(columns) != 0) {
            ImGui::SameLine(0.0f, Px(kSpacing));
        }
        ImGui::PushID(static_cast<int>(c.id));
        // Only ever with Show deleted on: deleted on its own or with its
        // folder, and marked out either way. Everything else is dimmed.
        const bool deleted = Manager().IsDeleted(c);
        const bool dimmed = showingDeleted && !deleted;
        if (dimmed) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * kDimmedAlpha);
        }
        ImGui::BeginGroup();

        const ImVec2 thumbMin = ImGui::GetCursorScreenPos();
        const ImVec2 thumbMax(thumbMin.x + tileSize.x, thumbMin.y + tileSize.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        DrawCanvasPreview(drawList, c, thumbMin, thumbMax, displayW, displayH, Cfg().strokeRenderMode,
                           Cfg().overviewShowsStrokes, previewTexture, PreviewMeshSlot(), PictureSampling());
        const bool isActive = c.id == currentCanvasId;
        if (deleted) {
            drawList->AddRectFilled(thumbMin, thumbMax, ImGui::GetColorU32(theme::kDangerSoft), Px(4.0f));
            drawList->AddRect(thumbMin, thumbMax, ImGui::GetColorU32(theme::kDanger), Px(4.0f), ImDrawFlags_None,
                              PxWhole(2.0f));
        } else {
            // The preview's own pictures are drawn at full strength whatever
            // the style's alpha, so a dimmed tile is dimmed by a veil.
            if (dimmed) {
                drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(14, 16, 20, 150), Px(4.0f));
            }
            drawList->AddRect(thumbMin, thumbMax,
                               ImGui::GetColorU32(isActive ? theme::Accent() : ImVec4(0.275f, 0.298f, 0.345f, 1.0f)),
                               Px(4.0f), ImDrawFlags_None, isActive ? PxWhole(2.0f) : 1.0f);
        }

        // A tile is where a snippet being sent somewhere goes, or else the
        // canvas to go to, with the Overview's job done.
        if (ImGui::InvisibleButton("##tile", tileSize) && !deleted) {
            if (pickerItemId_.has_value()) {
                Act(action::SendPicked{c.id});
            } else {
                Act(action::SwitchCanvas{c.id});
                Act(action::ClosePanel{PanelKind::Overview});
            }
        }
        if (deleted && ImGui::IsItemHovered()) {
            // Its own stamp, or its folder's when it went with the folder.
            const Folder* folder = Manager().FindFolder(c.folderId);
            const int64_t stamp = c.deletedAt != 0 ? c.deletedAt : folder != nullptr ? folder->deletedAt : 0;
            if (Cfg().purgeDeleted) {
                ImGui::SetTooltip("%s\n%s\n%s", DeletedWhen(stamp, now).c_str(),
                                  GoesOn(stamp, Cfg().purgeDeletedAfterDays).c_str(), strings::kDeletedRestoreToOpen);
            } else {
                ImGui::SetTooltip("%s\n%s", DeletedWhen(stamp, now).c_str(), strings::kDeletedRestoreToOpen);
            }
        }
        if (!deleted && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("HB_CANVAS_REORDER", &c.id, sizeof(CanvasId));
            ImGui::TextUnformatted(c.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (!deleted && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HB_CANVAS_REORDER")) {
                const CanvasId draggedId = *static_cast<const CanvasId*>(payload->Data);
                if (draggedId != c.id) {
                    Act(action::ReorderCanvas{draggedId, folderCanvasPlaces[idxInFolder]});
                }
            }
            ImGui::EndDragDropTarget();
        }

        const bool isRenamingThisCanvas = renamingCanvasId_ == c.id;
        if (isRenamingThisCanvas) {
            ImGui::SetNextItemWidth(tileSize.x);
            if (renameJustFocused_) {
                ImGui::SetKeyboardFocusHere();
                renameJustFocused_ = false;
            }
            ImGui::InputText("##renamecanvas", renameBuffer_, sizeof(renameBuffer_));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                Act(action::RenameCanvas{c.id, std::string(renameBuffer_)});
                renamingCanvasId_.reset();
                window_->ReleaseTextInput();
            } else if (ImGui::IsItemDeactivated()) {
                renamingCanvasId_.reset();
                window_->ReleaseTextInput();
            }
        } else {
            ImGui::TextColored(deleted ? theme::kDeletedInk : theme::kWhite, "%s", c.name.c_str());
            if (!deleted && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                BeginRenaming(std::nullopt, c.id, c.name);
            }
        }
        // No "...and this folder holds more than one" condition: the last
        // canvas in a folder is deletable, and a button that is simply not
        // drawn reads as a bug. See CanvasManager's class comment for the
        // invariant behind it, now gone.
        if (deleted) {
            ImGui::SameLine(tileSize.x - Px(kPillButtonSize) * 2.0f - Px(kDeletedButtonGap));
            // Out of a deleted folder the folder comes back to hold it, and
            // the rest of what went with the folder stays deleted - see
            // CanvasManager::Restore.
            const Folder* folder = Manager().FindFolder(c.folderId);
            const bool folderDeleted = folder != nullptr && Manager().IsDeleted(*folder);
            switch (DeletedButtons(folderDeleted ? strings::kDeletedRestoreCanvasAndFolderTip
                                                 : strings::kDeletedRestoreCanvasTip,
                                   strings::kDeletedDeleteForGoodTip)) {
                case DeletedButton::Restore:
                    Act(action::Restore{c.id});
                    break;
                case DeletedButton::DeleteForGood:
                    AskToDelete(DeleteTarget{DeleteTarget::Kind::Canvas, c.id, c.name, /*forGood=*/true});
                    break;
                case DeletedButton::None:
                    break;
            }
        } else if (!isRenamingThisCanvas) {
            ImGui::SameLine(tileSize.x - Px(kPillButtonSize));
            // Not while picking: see SendPickedItemTo.
            ImGui::BeginDisabled(pickerItemId_.has_value());
            const bool deletePressed = DangerIconButton("##delcanvas", icons::kTrash);
            ImGui::EndDisabled();
            if (deletePressed) {
                AskToDelete(DeleteTarget{DeleteTarget::Kind::Canvas, c.id, c.name});
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", strings::kOverviewDeleteCanvas);
            }
        }

        ImGui::EndGroup();
        if (dimmed) {
            ImGui::PopStyleVar();
        }
        // A just-created canvas is at the end of its folder, which may be
        // past the bottom of this list - scroll it into view once (see
        // overviewScrollToCanvasId_). Centered rather than merely made
        // visible: the tile that just appeared should be the one you are
        // looking at.
        if (overviewScrollToCanvasId_ == c.id) {
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::PopID();
    }
    // Whether or not it was found - the target may be in another folder,
    // or already gone - so a stale id can't keep yanking the list around
    // on later frames.
    overviewScrollToCanvasId_.reset();
    ImGui::EndChild();
}

void OverlayApp::RenderOverviewFooter(bool showCanvasesBody) {
    if (!showCanvasesBody && overviewTab_ == OverviewTab::About) {
        // In the footer rather than at the end of the text it belongs to:
        // the licenses run to a couple of hundred lines, and a way out
        // that has to be scrolled back to is a way out you stop using.
        // The footer is the panel's own row of verbs - the Canvases tab
        // keeps New folder / New canvas here - so this is where a reader
        // already looks for one.
        if (aboutShowsNotices_) {
            if (ImGui::Button(Labeled(strings::kAboutBackToAbout, "noticesback"))) {
                aboutShowsNotices_ = false;
                overviewBodyScrollToTop_ = true;
            }
        } else if (ImGui::Button(Labeled(strings::kAboutThirdPartyLicenses, "noticesopen"))) {
            aboutShowsNotices_ = true;
            overviewBodyScrollToTop_ = true;
        }
        return;
    }
    if (!showCanvasesBody) {
        return;
    }

    // Not while picking, since it switches to the canvas it makes: see
    // SendPickedItemTo. New canvas stays, as a destination to send to.
    ImGui::BeginDisabled(pickerItemId_.has_value());
    const bool newFolderPressed = PrimaryButton("##newfolder", icons::kPlus, strings::kOverviewNewFolder);
    ImGui::EndDisabled();
    if (newFolderPressed) {
        Act(action::NewFolder{});
    }
    // Lined up with the canvas grid above it, which starts past the
    // window's padding, the sidebar and the gap after it. SameLine counts
    // from the window's edge rather than from inside its padding, so the
    // padding is added here - without it the button sat that far left of
    // the tiles.
    ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + OverviewSidebarWidth() + ImGui::GetStyle().ItemSpacing.x);
    // Not into a deleted folder, which is the one on show: a new canvas goes
    // to the folder being browsed, and that would be somewhere else.
    const bool showsDeletedFolder = ShowingDeleted() && deletedFolderShown_.has_value();
    ImGui::BeginDisabled(showsDeletedFolder);
    const bool newCanvasPressed = PrimaryButton("##newcanvas", icons::kPlus, strings::kOverviewNewCanvas);
    ImGui::EndDisabled();
    if (newCanvasPressed) {
        Act(action::NewCanvas{});
    }
}

void OverlayApp::SendPickedItemTo(CanvasId target) {
    if (!pickerItemId_.has_value()) {
        return;
    }
    const ItemId item = *pickerItemId_;
    const bool isCopy = pickerIsCopy_;
    const CanvasId source = Manager().CurrentCanvasId();
    // The item is sent from the current canvas, so nothing in the picker
    // may change which canvas that is: New folder and the delete buttons
    // are off while it is open. And what happened is checked rather than
    // assumed: a move that did nothing said "Moved to" all the same, and
    // threw away the history of a snippet that had not gone anywhere.
    if (target != source) {
        const Canvas* targetCanvas = Manager().FindCanvas(target);
        const std::string targetName = targetCanvas ? targetCanvas->name : strings::kDeleteConfirmCanvasWord;
        const Session::Placed sent = session_.SendItemsTo({item}, target, isCopy);
        if (sent.items.empty()) {
            pickerItemId_.reset();
            return;
        }
        ShowActionToast(sent.pictureLost ? std::string(strings::kToastCopiedWithoutPicture)
                                         : std::string(isCopy ? strings::kToastCopiedToPrefix
                                                              : strings::kToastMovedToPrefix) +
                                               targetName);
    }
    // Out of picker mode (the move/copy is already done), with the Overview
    // itself left open - the user can click the target canvas to switch to
    // it, or close the Overview themselves; closing it here would make the
    // whole panel vanish the instant a canvas was picked, with nothing but
    // the toast to explain what had just happened.
    pickerItemId_.reset();
}

namespace {
// What this tab shows: the component and the license it is under, in the
// order they matter to someone glancing at it. The full texts are in
// THIRD-PARTY-NOTICES.md, which is where they have to be complete - this
// list is the summary, and is the reason most people never open the other.
struct BuiltWithRow {
    const char* component;
    const char* license;
};
const BuiltWithRow kBuiltWith[] = {
    {strings::kAboutComponentImgui, strings::kAboutLicenseMit},
    {strings::kAboutComponentJson, strings::kAboutLicenseMit},
    {strings::kAboutComponentStb, strings::kAboutLicenseMitOrPublicDomain},
    {strings::kAboutComponentQoi, strings::kAboutLicenseMit},
    {strings::kAboutComponentManrope, strings::kAboutLicenseOfl},
    {strings::kAboutComponentIcons, strings::kAboutLicenseIscMit},
};

// ABOUT.md and THIRD-PARTY-NOTICES.md, rendered with just enough Markdown
// awareness to look like prose rather than like a file someone forgot to
// format: headings and bullets, nothing else. A real Markdown renderer
// would be a project of its own (ImGui has none built in), and the
// alternative - dumping the raw text with its '#' and '-' prefixes intact -
// looks like a bug. Whoever edits either file gets a live preview by
// opening this tab, so the limits are self-evident rather than needing to
// be documented somewhere they'd be missed.
//
// Rendered by *paragraph*, not by line. Both files are hard-wrapped at
// around 76 columns like any readable source file, so drawing a line at a
// time would reproduce those breaks verbatim - a narrow column stranded in
// a wide panel, re-wrapped at the wrong width whenever the panel is a
// different size than the author's editor was. Consecutive lines are
// joined back into one string and handed to ImGui to wrap, which is also
// what makes a bullet's second line line up under its first: it's one
// wrapped item rather than two unrelated ones.
void RenderMarkdownSubset(std::string_view document) {
    constexpr float kBulletIndent = 16.0f;
    std::string paragraph;
    bool paragraphIsBullet = false;
    const auto flush = [&]() {
        if (paragraph.empty()) {
            return;
        }
        if (paragraphIsBullet) {
            ImGui::Indent(Px(kBulletIndent));
            ImGui::Bullet();
            // Bullet() already ends flush against whatever follows it, so
            // this gap is the whole separation between the dot and the
            // word - 4px read as the two touching.
            ImGui::SameLine(0.0f, Px(10.0f));
            ImGui::TextUnformatted(paragraph.data(), paragraph.data() + paragraph.size());
            ImGui::Unindent(Px(kBulletIndent));
        } else {
            ImGui::TextUnformatted(paragraph.data(), paragraph.data() + paragraph.size());
        }
        paragraph.clear();
        paragraphIsBullet = false;
    };

    size_t pos = 0;
    while (pos <= document.size()) {
        const size_t eol = document.find('\n', pos);
        std::string_view line = document.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? document.size() + 1 : eol + 1;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        std::string_view trimmed = line;
        while (!trimmed.empty() && trimmed.front() == ' ') {
            trimmed.remove_prefix(1);
        }

        if (trimmed.empty()) {
            // A blank line is Markdown's paragraph break, and the only
            // thing that ends one here.
            flush();
            ImGui::Spacing();
            continue;
        }
        size_t hashes = 0;
        while (hashes < trimmed.size() && trimmed[hashes] == '#') {
            ++hashes;
        }
        if (hashes > 0 && hashes < trimmed.size() && trimmed[hashes] == ' ') {
            flush();
            const std::string heading(trimmed.substr(hashes + 1));
            ImGui::Spacing();
            // Only the top level gets larger type; deeper ones stay at body
            // size and lean on color alone, so a document with four levels
            // doesn't turn into four competing sizes.
            if (hashes == 1) {
                ImGui::PushFont(nullptr, 22.0f);
            }
            ImGui::TextColored(theme::Accent(), "%s", heading.c_str());
            if (hashes == 1) {
                ImGui::PopFont();
            }
            ImGui::Spacing();
            continue;
        }
        if (trimmed.size() > 2 && trimmed[0] == '-' && trimmed[1] == ' ') {
            flush();
            paragraphIsBullet = true;
            paragraph.assign(trimmed.substr(2));
            continue;
        }
        // Anything else continues whatever is being built - a bullet's
        // indented second line, or the next line of a paragraph.
        if (!paragraph.empty()) {
            paragraph += ' ';
        }
        paragraph.append(trimmed);
    }
    flush();
}
}  // namespace

void OverlayApp::SwitchOverviewTab(OverviewTab tab) {
    if (overviewTab_ == tab) {
        return;
    }
    overviewTab_ = tab;
    overviewBodyScrollToTop_ = true;
    // Leaving About also leaves its license page: coming back to a tab
    // that is still showing somebody else's MIT text, several tabs later,
    // is not a place anyone meant to return to.
    aboutShowsNotices_ = false;
}

void OverlayApp::RenderOverviewAboutPanel() {
    // 0.0f means "wrap at the window's own right edge", which is what this
    // wants. PushTextWrapPos takes a window-local X *position*, not a
    // width, so passing GetContentRegionAvail().x - the obvious-looking
    // thing, and what the Settings panel happens to get away with - wraps
    // at roughly half the width once the panel is wide.
    ImGui::PushTextWrapPos(0.0f);

    if (aboutShowsNotices_) {
        // The licenses, in the same panel and the same scroll region as
        // the About text rather than in a popup: this is a page you read,
        // not a thing you act on, and the Overview has enough windows
        // stacked over it already (see OverlayApp::StackSurfaces). The way back
        // is in the panel's footer, which does not scroll.
        RenderMarkdownSubset(build::NoticesText());
        ImGui::PopTextWrapPos();
        return;
    }

    // Which build this is, first and plainly - it's the one thing someone
    // opens this tab to find, and the reason the tab is worth having at all
    // when a tester needs to say which binary they were using.
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kAboutVersion);
    ImGui::SameLine(Px(90.0f));
    const std::string version = build::VersionLine();
    ImGui::TextUnformatted(version.c_str());
    // Whose it is, right under what it is. This one stays in plain sight
    // rather than behind the button below: it is the app saying who owns
    // it, which is a different job from reproducing other people''s terms.
    ImGui::TextColored(theme::kGraphite200, "%s", strings::kAboutCopyrightLabel);
    ImGui::SameLine(Px(90.0f));
    ImGui::TextUnformatted(strings::kAboutCopyright);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    RenderMarkdownSubset(build::AboutText());

    // What is inside this binary that somebody else wrote. The list is
    // short enough to read at a glance and is the part most people want;
    // the licenses themselves are long enough that they would bury the
    // rest of this tab, so they are one click away.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushFont(nullptr, 22.0f);
    ImGui::TextColored(theme::Accent(), "%s", strings::kAboutBuiltWith);
    ImGui::PopFont();
    ImGui::Spacing();
    for (const BuiltWithRow& row : kBuiltWith) {
        ImGui::TextUnformatted(row.component);
        ImGui::SameLine(Px(260.0f));
        ImGui::TextColored(theme::kGraphite300, "%s", row.license);
    }

    ImGui::PopTextWrapPos();
}

void OverlayApp::BeginRenaming(std::optional<FolderId> folder, std::optional<CanvasId> canvas,
                               const std::string& name) {
    renamingFolderId_ = folder;
    renamingCanvasId_ = canvas;
    std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s", name.c_str());
    renameJustFocused_ = true;
    if (window_) {
        window_->RequestTextInput();
    }
    // Every key is the field's until it lets go - see NameEdit.
    editor_.Input().Push(std::make_unique<NameEdit>(
                             [this] { return renamingFolderId_.has_value() || renamingCanvasId_.has_value(); },
                             [this] {
                                 // A rename field gives the keyboard back when ImGui
                                 // deactivates it (see the InputText sites in
                                 // RenderOverview). Stopped from outside - the Overview
                                 // closed between frames - that frame never comes, and the
                                 // keyboard stayed borrowed from the game for the rest of
                                 // the session. ReleaseTextInput is idempotent.
                                 const bool wasRenaming = renamingFolderId_.has_value() || renamingCanvasId_.has_value();
                                 renamingFolderId_.reset();
                                 renamingCanvasId_.reset();
                                 if (wasRenaming && window_) {
                                     window_->ReleaseTextInput();
                                 }
                             }),
                         Event{});
}

void OverlayApp::AskToDelete(DeleteTarget target) {
    // Not asked at all where Settings > Behavior says not to: deleted after
    // the draw, as the confirmation's own Delete would be.
    const bool forGood = target.forGood || target.kind == DeleteTarget::Kind::DeletedCanvasesIn;
    if (!(forGood ? Cfg().confirmDeleteForGood : Cfg().confirmDelete)) {
        Act(action::Delete{std::move(target)});
        return;
    }
    // Opened at the next frame's Open even when asked from inside one: the
    // Overview's buttons ask from within its PushID nesting, and the popup
    // belongs at the top level.
    PopupRecord popup;
    popup.kind = PopupKind::ConfirmDelete;
    popup.deleteTarget = std::move(target);
    OpenPopup(std::move(popup));
}

void OverlayApp::RenderConfirmDeletePopover() {
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const bool open = ImGui::BeginPopup(kConfirmDeletePopupId);
    PopupDrawn(PopupKind::ConfirmDelete, open);
    if (!open) {
        return;
    }
    if (!PopupUp(PopupKind::ConfirmDelete)) {
        // Ended from outside this frame, before ImGui heard of it.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const DeleteTarget target = *popup_->deleteTarget;
    const bool isFolder = target.kind == DeleteTarget::Kind::Folder;
    const bool deletedIn = target.kind == DeleteTarget::Kind::DeletedCanvasesIn;
    const char* word = isFolder ? strings::kDeleteConfirmFolderWord : strings::kDeleteConfirmCanvasWord;
    // A delete marks the thing, which can be restored, and says so; a delete
    // of something deleted already is for good, and says that.
    const bool forGood = target.forGood || deletedIn;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(220.0f));
    if (deletedIn) {
        ImGui::Text(strings::kDeleteConfirmPromptDeletedIn, target.name.c_str());
    } else {
        ImGui::Text(forGood ? strings::kDeleteConfirmPromptForGood : strings::kDeleteConfirmPrompt, word,
                    target.name.c_str());
    }
    if (isFolder) {
        ImGui::TextColored(theme::kDanger, "%s", strings::kDeleteConfirmAlsoCanvases);
    }
    // With the retention period on, "can be restored" has an end, and says
    // when: the dialog is where a person decides how much that matters.
    if (forGood) {
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kDeleteConfirmCannotUndo);
    } else if (Cfg().purgeDeleted) {
        ImGui::TextColored(theme::kGraphite200, strings::kDeleteConfirmRestorableFor, Cfg().purgeDeletedAfterDays);
    } else {
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kDeleteConfirmRestorable);
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const bool cancelPressed = ImGui::Button(Labeled(strings::kDeleteConfirmCancel, strings::kMoveCopyCancel));
    ImGui::SameLine();
    const bool deletePressed = DangerButton("##confirmdelete", icons::kTrash,
                                            forGood ? strings::kDeleteConfirmDeleteForGood : strings::kDeleteConfirmDelete);
    // CloseCurrentPopup must be called while this popup is still current -
    // i.e. before EndPopup, not after (it operates on the popup ID stack,
    // which EndPopup pops). The delete itself is an action, done once the
    // frame is drawn.
    if (cancelPressed || deletePressed) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();

    if (deletePressed) {
        Act(action::Delete{target});
    }
}

void OverlayApp::PerformDelete(const DeleteTarget& target) {
    editor_.Settle(Scope::Canvas);  // a command - see Scope
    const bool forGood = target.forGood || target.kind == DeleteTarget::Kind::DeletedCanvasesIn;
    const bool deletedIn = target.kind == DeleteTarget::Kind::DeletedCanvasesIn;
    // Its textures go as it leaves the screen, either way (see
    // TextureCache), and its history only with the thing itself, for good.
    if (forGood) {
        if (deletedIn ? session_.DeleteMarkedCanvasesPermanently(target.id) : session_.DeletePermanently(target.id)) {
            ShowActionToast(strings::kToastDeletedForGood);
        }
    } else if (session_.Delete(target.id)) {
        ShowActionToast(strings::kToastDeleted);
    }
}

void OverlayApp::RenderActionToast() {
    if (actionToastText_.empty() || ImGui::GetTime() >= actionToastExpireAtSeconds_) {
        return;
    }
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(actionToastText_.c_str());
    constexpr float kPaddingX = 16.0f;
    constexpr float kPaddingY = 9.0f;
    const ImVec2 boxSize(textSize.x + Px(kPaddingX) * 2.0f, textSize.y + Px(kPaddingY) * 2.0f);
    const ImVec2 boxMin((ImGui::GetIO().DisplaySize.x - boxSize.x) * 0.5f, Px(22.0f));
    const ImVec2 boxMax(boxMin.x + boxSize.x, boxMin.y + boxSize.y);
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(18, 20, 26, 235), 999.0f);
    drawList->AddText(ImVec2(boxMin.x + Px(kPaddingX), boxMin.y + Px(kPaddingY)), IM_COL32(240, 242, 245, 255),
                       actionToastText_.c_str());
}

std::string OverlayApp::PersistenceWarning() const {
    std::string warning;
    char line[1024];
    if (session_.LastWriteFailed() && session_.Store() != nullptr) {
        std::snprintf(line, sizeof(line), strings::kStatusWriteFailed, session_.Store()->File().string().c_str());
        warning = line;
    }
    if (configWriteFailedPath_.has_value()) {
        std::snprintf(line, sizeof(line), strings::kStatusConfigWriteFailed, configWriteFailedPath_->c_str());
        if (!warning.empty()) {
            warning += "\n";
        }
        warning += line;
    }
    return warning;
}

void OverlayApp::RenderPersistenceWarning() {
    const std::string warning = PersistenceWarning();
    if (warning.empty()) {
        return;
    }
    // Along the bottom, out from under the canvas bar's own reveal zone and
    // away from the toast at the top, in the toast's own colors but with a
    // warning tint behind the text: this one does not go away by itself.
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(warning.c_str());
    constexpr float kPaddingX = 16.0f;
    constexpr float kPaddingY = 9.0f;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 boxSize(textSize.x + Px(kPaddingX) * 2.0f, textSize.y + Px(kPaddingY) * 2.0f);
    const ImVec2 boxMin((display.x - boxSize.x) * 0.5f, display.y - boxSize.y - Px(64.0f));
    const ImVec2 boxMax(boxMin.x + boxSize.x, boxMin.y + boxSize.y);
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(92, 40, 20, 235), Px(8.0f));
    drawList->AddText(ImVec2(boxMin.x + Px(kPaddingX), boxMin.y + Px(kPaddingY)), IM_COL32(255, 232, 210, 255),
                       warning.c_str());
}

void OverlayApp::OpenOverview() {
    pickerItemId_.reset();
    if (!IsOverviewOpen()) {
        editor_.Input().Push(std::make_unique<Panel>(PanelKind::Overview), Event{});
    }
    overviewTab_ = OverviewTab::Canvases;
    showDeleted_ = false;
    deletedFolderShown_.reset();
    settingsPage_.OnPanelOpened();
}

void OverlayApp::OpenSettings() {
    OpenOverview();
    SwitchOverviewTab(OverviewTab::Settings);
}

void OverlayApp::AskToDeleteCanvas(CanvasId canvas) {
    const Canvas* found = Manager().FindCanvas(canvas);
    AskToDelete(DeleteTarget{DeleteTarget::Kind::Canvas, canvas, found != nullptr ? found->name : std::string()});
}

void OverlayApp::OpenPicker(ItemId itemId, bool isCopy) {
    pickerItemId_ = itemId;
    pickerIsCopy_ = isCopy;
    if (!IsOverviewOpen()) {
        editor_.Input().Push(std::make_unique<Panel>(PanelKind::Overview), Event{});
    }
}

void OverlayApp::CloseOverview() {
    pickerItemId_.reset();
    // A name being edited, or a key being captured, is the Overview's: a
    // row left armed would sit silently waiting, capturing whatever key is
    // pressed next time Settings reopens.
    if (editor_.Input().At(Level::Text) != nullptr && editor_.Input().As<TypingNote>(Level::Text) == nullptr) {
        editor_.Input().End(Level::Text);
    }
    // Nothing, when the machine has ended it already - see Panel.
    if (IsOverviewOpen()) {
        editor_.Input().End(Level::Panel);
    }
}

void OverlayApp::ToggleCheatSheet() {
    if (IsCheatSheetOpen()) {
        editor_.Input().End(Level::Panel);
    } else {
        editor_.Input().Push(std::make_unique<Panel>(PanelKind::CheatSheet), Event{});
    }
}

void OverlayApp::ClosePanel(PanelKind kind) {
    switch (kind) {
        case PanelKind::Overview:
            CloseOverview();
            return;
        case PanelKind::CheatSheet:
            return;  // nothing of its own to put away
    }
}

void OverlayApp::ShowActionToast(std::string text) {
    // Every other call site runs from inside RenderOverview, always mid-
    // frame with a live context - but QuickCapture can now call this
    // before the overlay has ever been shown at all this session (e.g.
    // the quick-capture hotkey pressed first, while still hidden), and a
    // backend's EnsureCreated() isn't contractually guaranteed to have
    // created one yet either. ImGui::GetTime() dereferences the current
    // context unconditionally, so calling it with none set is a crash, not
    // a graceful no-op - the toast just wouldn't be visible yet anyway.
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    actionToastText_ = std::move(text);
    actionToastExpireAtSeconds_ = ImGui::GetTime() + 2.2;
}



}  // namespace sz::ui
