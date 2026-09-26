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
    overview_.Open();
    settingsPage_.OnPanelOpened();
}

void OverlayApp::OpenSettings() {
    overview_.OpenSettings();
    settingsPage_.OnPanelOpened();
}

void OverlayApp::AskToDeleteCanvas(CanvasId canvas) {
    const Canvas* found = Manager().FindCanvas(canvas);
    AskToDelete(DeleteTarget{DeleteTarget::Kind::Canvas, canvas, found != nullptr ? found->name : std::string()});
}

void OverlayApp::OpenPicker(ItemId itemId, bool isCopy) { overview_.OpenPicker(itemId, isCopy); }

void OverlayApp::ToggleCheatSheet() { cheatSheet_.Toggle(); }

void OverlayApp::ClosePanel(PanelKind kind) {
    switch (kind) {
        case PanelKind::Overview:
            overview_.Close();
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
