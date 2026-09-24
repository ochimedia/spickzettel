#include "ui/context_menu.h"

#include <algorithm>
#include <cfloat>
#include <cstddef>

#include <imgui_internal.h>

#include "ui/ui_scale.h"

namespace sz::ui {

namespace {

// The row's shape, in pixels at 100% - each goes through Px, as the font
// and the icons are scaled too (see UiScale). Nothing here is themed: a
// menu takes its colors from the style (see the header) but its metrics
// are its own, so every menu in the app has rows of the same height
// however it is colored.
constexpr float kIconSize = 16.0f;
constexpr float kIconGap = 10.0f;       // icon column to the label
constexpr float kShortcutGap = 32.0f;   // the gap a shortcut is never closer than
constexpr float kRowPadX = 10.0f;
constexpr float kRowPadY = 5.0f;
constexpr float kMenuPad = 6.0f;        // the popup's own padding, all round
constexpr float kSeparatorPadY = 5.0f;  // above and below a divider line

// Everything about the menu's size that has to be known before the popup
// is positioned: it is placed by a pivot (see Render), and a pivot needs
// the size up front rather than after the window has laid itself out.
struct Metrics {
    float rowHeight = 0.0f;
    // 0 when no row has an icon at all - a menu of plain actions doesn't
    // pay for a column nothing is ever drawn in.
    float iconColumn = 0.0f;
    float shortcutColumn = 0.0f;
    // What one row spans: the window's content width, inside its padding.
    float innerWidth = 0.0f;
    float height = 0.0f;
};

Metrics Measure(const std::vector<ContextMenuEntry>& entries) {
    Metrics metrics;
    metrics.rowHeight = ImGui::GetTextLineHeight() + Px(kRowPadY) * 2.0f;
    float labelColumn = 0.0f;
    bool anyIcon = false;
    size_t separators = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const ContextMenuEntry& entry = entries[i];
        anyIcon = anyIcon || entry.icon != nullptr;
        if (entry.label != nullptr) {
            labelColumn = std::max(labelColumn, ImGui::CalcTextSize(entry.label).x);
        }
        if (!entry.shortcut.empty()) {
            metrics.shortcutColumn = std::max(metrics.shortcutColumn, ImGui::CalcTextSize(entry.shortcut.c_str()).x);
        }
        if (entry.separatorAbove && i != 0) {
            ++separators;
        }
    }
    metrics.iconColumn = anyIcon ? Px(kIconSize) + Px(kIconGap) : 0.0f;
    // The shortcut column and its gap only exist if something is bound:
    // a menu whose actions all lack shortcuts is as narrow as its labels.
    metrics.innerWidth = Px(kRowPadX) * 2.0f + metrics.iconColumn + labelColumn +
                          (metrics.shortcutColumn > 0.0f ? Px(kShortcutGap) + metrics.shortcutColumn : 0.0f);
    metrics.height = Px(kMenuPad) * 2.0f + static_cast<float>(entries.size()) * metrics.rowHeight +
                      static_cast<float>(separators) * (Px(kSeparatorPadY) * 2.0f + 1.0f);
    return metrics;
}

}  // namespace

void ContextMenu::RequestOpenAt(ImVec2 screenPos) {
    requested_ = true;
    anchor_ = screenPos;
}

std::optional<int> ContextMenu::Render(const Builder& build) {
    if (requested_) {
        requested_ = false;
        ImGui::OpenPopup(popupId_);
    }
    // Asked before the rows are built so that a menu nobody opened costs
    // one lookup a frame and nothing else. IsPopupOpen, OpenPopup and
    // BeginPopup all hash the id against the current window, so all three
    // have to be reached at the same nesting level - which for every menu
    // in this app is the top level of a frame.
    if (!ImGui::IsPopupOpen(popupId_)) {
        open_ = false;
        return std::nullopt;
    }

    std::vector<ContextMenuEntry> entries;
    build(entries);
    const Metrics metrics = Measure(entries);

    // At the point it was asked for, turned back onto the screen when it
    // would otherwise run off the right edge or the bottom - the corner
    // the anchor *is* flips rather than the menu being nudged, so a menu
    // opened in a corner never covers the thing it was opened on.
    //
    // Then held on the screen, for a menu that fits neither way round: one
    // opened halfway down, taller than either half - which a menu of a
    // dozen rows is at a large interface scale. Flipped, it ran off the
    // top instead of the bottom. And one taller than the whole screen is
    // held to its height, and scrolls.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float width = metrics.innerWidth + Px(kMenuPad) * 2.0f;
    const float height = std::min(metrics.height, display.y);
    const float x = anchor_.x + width > display.x ? anchor_.x - width : anchor_.x;
    const float y = anchor_.y + height > display.y ? anchor_.y - height : anchor_.y;
    ImGui::SetNextWindowPos(ImVec2(std::clamp(x, 0.0f, std::max(0.0f, display.x - width)),
                                   std::clamp(y, 0.0f, std::max(0.0f, display.y - height))),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, display.y));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(kMenuPad), Px(kMenuPad)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    const bool open = ImGui::BeginPopup(popupId_);
    ImGui::PopStyleVar(2);
    if (!open) {
        open_ = false;
        return std::nullopt;
    }
    open_ = true;
    // Every item re-asserts itself to the front on every frame it is
    // drawn, so a popup has to as well or the first snippet it overlaps
    // covers it - the same per-frame reassertion OverlayApp's own popovers
    // make (see KeepPopoverInFront), done here directly so this file needs
    // to know nothing about the app.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    if (entries.empty()) {
        // Nothing left to act on - the snippet it was opened over has been
        // deleted while it was up, say. An empty panel would be a dead end
        // the user has to dismiss; closing is what they would do anyway.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return std::nullopt;
    }

    // Selectable paints its hover in the Header colors, which this app's
    // style leaves at ImGui's own blue - borrowed from the frame colors
    // here, which are themed, so the wash under a row matches every other
    // hover in the app without this file naming a color.
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, style.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, style.Colors[ImGuiCol_FrameBgActive]);

    std::optional<int> chosen;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < entries.size(); ++i) {
        const ContextMenuEntry& entry = entries[i];
        if (entry.separatorAbove && i != 0) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(metrics.innerWidth, Px(kSeparatorPadY) * 2.0f + 1.0f));
            drawList->AddLine(ImVec2(at.x, at.y + Px(kSeparatorPadY)),
                               ImVec2(at.x + metrics.innerWidth, at.y + Px(kSeparatorPadY)),
                               ImGui::GetColorU32(ImGuiCol_Separator));
        }
        // BeginDisabled rather than ImGuiSelectableFlags_Disabled: it both
        // refuses the click and lowers style.Alpha, which the GetColorU32
        // calls below pick up - so the icon and the text of an unavailable
        // row dim with it instead of staying bright over a dead row.
        ImGui::BeginDisabled(!entry.enabled);
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        // The label is drawn below rather than passed in: Selectable would
        // put it at the left edge, where the icon goes. What it is given
        // is the row's id alone (see ContextMenuEntry::id), which shows
        // nothing.
        if (ImGui::Selectable(entry.id, false, ImGuiSelectableFlags_None,
                               ImVec2(metrics.innerWidth, metrics.rowHeight))) {
            chosen = entry.action;
        }
        const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
        if (entry.icon != nullptr) {
            DrawIcon(drawList, *entry.icon,
                      ImVec2(rowMin.x + Px(kRowPadX), rowMin.y + (metrics.rowHeight - Px(kIconSize)) * 0.5f),
                      Px(kIconSize), ink);
        }
        if (entry.label != nullptr) {
            drawList->AddText(ImVec2(rowMin.x + Px(kRowPadX) + metrics.iconColumn, rowMin.y + Px(kRowPadY)), ink,
                               entry.label);
        }
        if (!entry.shortcut.empty()) {
            // Right-aligned to the menu's edge rather than to a column of
            // its own: the shortcuts read as one block down the right-hand
            // side, which is what makes them skimmable.
            const float shortcutWidth = ImGui::CalcTextSize(entry.shortcut.c_str()).x;
            drawList->AddText(ImVec2(rowMin.x + metrics.innerWidth - Px(kRowPadX) - shortcutWidth,
                                     rowMin.y + Px(kRowPadY)),
                               ImGui::GetColorU32(ImGuiCol_TextDisabled), entry.shortcut.c_str());
        }
        ImGui::EndDisabled();
    }

    ImGui::PopStyleColor(2);
    ImGui::EndPopup();
    return chosen;
}

}  // namespace sz::ui
