#include "ui/view/cheat_sheet.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/session/actions.h"
#include "generated/ui_strings.h"
#include "ui/interaction/levels.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace sz::ui {

using namespace ::sz::core;

namespace {

// What goes in front of a gesture for the modifier a creation trigger
// names - nothing for a plain press, and no row at all for Off.
std::optional<std::string> TriggerPrefix(CreationTrigger trigger) {
    switch (trigger) {
        case CreationTrigger::Plain:
            return std::string();
        case CreationTrigger::Ctrl:
            return std::string("Ctrl+");
        case CreationTrigger::Alt:
            return std::string("Alt+");
        case CreationTrigger::Off:
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

std::vector<CheatSheetSection> BuildCheatSheet(const AppConfig& config, const ShortcutBindings& shortcuts) {
    std::vector<CheatSheetSection> sections;
    const auto section = [&sections](const char* title) -> std::vector<CheatSheetRow>& {
        sections.push_back(CheatSheetSection{title, {}});
        return sections.back().rows;
    };
    // A command's keys, as the command table has them bound right now (see
    // KeysFor): the first, or every one of them.
    const auto firstKey = [&](CommandId id) {
        const std::vector<platform::KeyCombo> keys = KeysFor(id, config, shortcuts);
        return keys.empty() ? std::string() : FormatKeyComboLabel(keys.front());
    };
    const auto everyKey = [&](CommandId id) {
        std::string text;
        for (const platform::KeyCombo& key : KeysFor(id, config, shortcuts)) {
            text += (text.empty() ? "" : ", ") + FormatKeyComboLabel(key);
        }
        return text;
    };
    // A row for a command no key may reach: none, then, rather than one
    // that promises "(none)".
    const auto shortcut = [&](std::vector<CheatSheetRow>& rows, CommandId id, const char* what) {
        if (std::string keys = firstKey(id); !keys.empty()) {
            rows.push_back({std::move(keys), what});
        }
    };

    std::vector<CheatSheetRow>& general = section(strings::kCheatSheetGeneral);
    shortcut(general, CommandId::ToggleEditMode, strings::kCheatSheetShowHide);
    shortcut(general, CommandId::ToggleViewMode, strings::kCheatSheetViewOnly);
    shortcut(general, CommandId::QuickCapture, strings::kCheatSheetQuickCapture);
    shortcut(general, CommandId::SilentCapture, strings::kCheatSheetSilentCapture);
    shortcut(general, CommandId::CheatSheet, strings::kCheatSheetSelf);
    general.push_back({firstKey(CommandId::Undo) + ", " + firstKey(CommandId::Redo), strings::kCheatSheetUndo});
    general.push_back({firstKey(CommandId::PutDown), strings::kCheatSheetEsc});
    general.push_back({strings::kCheatSheetRightClickKey, strings::kCheatSheetRightClick});

    // The presses on empty canvas, as the triggers in Settings have them.
    std::vector<CheatSheetRow>& making = section(strings::kCheatSheetMaking);
    making.push_back({"", strings::kCheatSheetOnEmptyCanvas});
    const auto creation = [&making](CreationTrigger trigger, const char* area, const char* full) {
        if (const std::optional<std::string> prefix = TriggerPrefix(trigger)) {
            making.push_back({*prefix + strings::kCheatSheetDrag, area});
            making.push_back({*prefix + strings::kCheatSheetDoubleClickOrHold, full});
        }
    };
    creation(config.screenshotTrigger, strings::kCheatSheetScreenshotArea, strings::kCheatSheetScreenshotFull);
    creation(config.drawingTrigger, strings::kCheatSheetDrawingArea, strings::kCheatSheetDrawingFull);
    const size_t beforeKeys = making.size();
    shortcut(making, CommandId::NewScreenshotTool, strings::kCheatSheetScreenshotTool);
    shortcut(making, CommandId::NewDrawingTool, strings::kCheatSheetDrawingTool);
    shortcut(making, CommandId::NewCanvas, strings::kCheatSheetNewCanvas);
    if (making.size() > beforeKeys) {
        making.insert(making.begin() + static_cast<std::ptrdiff_t>(beforeKeys),
                      CheatSheetRow{"", strings::kCheatSheetAnywhere});
    }

    std::vector<CheatSheetRow>& arranging = section(strings::kCheatSheetArranging);
    arranging.push_back({strings::kCheatSheetSelectKeys, strings::kCheatSheetSelect});
    arranging.push_back({strings::kCheatSheetBoxKeys, strings::kCheatSheetBox});
    arranging.push_back({strings::kCheatSheetDrag, strings::kCheatSheetMove});
    arranging.push_back({strings::kCheatSheetResizeKeys, strings::kCheatSheetResize});
    arranging.push_back({strings::kCheatSheetNudgeKeys, strings::kCheatSheetNudge});
    arranging.push_back({strings::kCheatSheetWheelKey, strings::kCheatSheetScale});
    arranging.push_back({strings::kCheatSheetOpacityKeys, strings::kCheatSheetOpacity});
    arranging.push_back({strings::kCheatSheetSnippetMenuKey, strings::kCheatSheetSnippetMenu});

    std::vector<CheatSheetRow>& drawing = section(strings::kCheatSheetDrawing);
    drawing.push_back({strings::kCheatSheetDoubleClickOrHold, strings::kCheatSheetStartDrawing});
    shortcut(drawing, CommandId::DrawTool, strings::kCheatSheetPen);
    shortcut(drawing, CommandId::EraseTool, strings::kCheatSheetEraser);
    shortcut(drawing, CommandId::TextTool, strings::kCheatSheetText);
    shortcut(drawing, CommandId::SelectTool, strings::kCheatSheetSelectTool);
    drawing.push_back({strings::kCheatSheetShapeKeys, strings::kCheatSheetShape});
    drawing.push_back({strings::kCheatSheetShapeMenuKey, strings::kCheatSheetShapeMenu});
    drawing.push_back({strings::kCheatSheetRightDragKey, strings::kCheatSheetRightDrag});
    drawing.push_back({strings::kCheatSheetEraseRectKeys, strings::kCheatSheetEraseRect});
    drawing.push_back({strings::kCheatSheetWheelKey, strings::kCheatSheetToolSize});
    drawing.push_back({strings::kCheatSheetAltKeys, strings::kCheatSheetAlt});
    drawing.push_back({strings::kCheatSheetStopKeys, strings::kCheatSheetStop});

    std::vector<CheatSheetRow>& clipboard = section(strings::kCheatSheetClipboard);
    shortcut(clipboard, CommandId::Copy, strings::kCheatSheetCopy);
    shortcut(clipboard, CommandId::Cut, strings::kCheatSheetCut);
    shortcut(clipboard, CommandId::Paste, strings::kCheatSheetPaste);
    shortcut(clipboard, CommandId::PasteInPlace, strings::kCheatSheetPasteInPlace);
    shortcut(clipboard, CommandId::Duplicate, strings::kCheatSheetDuplicate);
    shortcut(clipboard, CommandId::NewCanvasWithSelection, strings::kCheatSheetToNewCanvas);
    clipboard.push_back({everyKey(CommandId::DeleteSelection), strings::kCheatSheetDelete});

    std::vector<CheatSheetRow>& canvases = section(strings::kCheatSheetCanvases);
    canvases.push_back({strings::kCheatSheetAltWheelKey, strings::kCheatSheetAltWheel});
    canvases.push_back({strings::kCheatSheetBottomEdgeKey, strings::kCheatSheetBottomEdge});
    canvases.push_back({"", strings::kCheatSheetInOverview});
    canvases.push_back({strings::kCheatSheetRenameKey, strings::kCheatSheetRename});
    canvases.push_back({strings::kCheatSheetDragTileKey, strings::kCheatSheetDragTile});
    return sections;
}


namespace {

// Splits the sections, in order, into `columns` runs with the tallest run
// as short as it can be. Six sections and at most three columns: trying
// every split is cheaper than being clever.
std::vector<size_t> BalancedColumnStarts(const std::vector<float>& heights, size_t columns) {
    const size_t n = heights.size();
    std::vector<size_t> best{0};
    if (columns <= 1 || n <= 1) {
        return best;
    }
    const auto sum = [&heights](size_t from, size_t to) {
        float total = 0.0f;
        for (size_t i = from; i < to; ++i) {
            total += heights[i];
        }
        return total;
    };
    float bestTallest = sum(0, n);
    for (size_t a = 1; a < n; ++a) {
        if (columns == 2) {
            const float tallest = std::max(sum(0, a), sum(a, n));
            if (tallest < bestTallest) {
                bestTallest = tallest;
                best = {0, a};
            }
            continue;
        }
        for (size_t b = a + 1; b < n; ++b) {
            const float tallest = std::max({sum(0, a), sum(a, b), sum(b, n)});
            if (tallest < bestTallest) {
                bestTallest = tallest;
                best = {0, a, b};
            }
        }
    }
    return best;
}

}  // namespace

CheatSheet::CheatSheet(Settings& settings, Editor& editor, ViewHost& host)
    : settings_(settings), editor_(editor), host_(host) {}

bool CheatSheet::IsOpen() const {
    const Panel* panel = editor_.Input().As<Panel>(Level::Panel);
    return panel != nullptr && panel->Kind() == PanelKind::CheatSheet;
}

void CheatSheet::Toggle() {
    if (IsOpen()) {
        editor_.Input().End(Level::Panel);
    } else {
        editor_.Input().Push(std::make_unique<Panel>(PanelKind::CheatSheet), Event{});
    }
}

void CheatSheet::Draw(float displayW, float displayH) {
    if (!IsOpen()) {
        return;
    }
    // Escape and its own key close it too, as its interaction's (see
    // Panel).
    if (PanelBackdrop("##cheat_sheet_backdrop", displayW, displayH)) {
        host_.Act(action::ClosePanel{PanelKind::CheatSheet});
    }

    const std::vector<CheatSheetSection> sections = BuildCheatSheet(settings_.Stored(), settings_.Live().shortcuts);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float sectionGap = lineH * 0.9f;
    const float keysGap = Px(20.0f);
    const float columnGap = Px(40.0f);

    // One keys column and one description column, the same widths
    // throughout, so every group lines up with every other.
    float keysW = 0.0f;
    float whatW = 0.0f;
    std::vector<float> heights;
    for (const CheatSheetSection& section : sections) {
        whatW = std::max(whatW, ImGui::CalcTextSize(section.title).x);
        for (const CheatSheetRow& row : section.rows) {
            if (row.keys.empty()) {
                continue;  // a context line runs across both columns
            }
            keysW = std::max(keysW, ImGui::CalcTextSize(row.keys.c_str()).x);
            whatW = std::max(whatW, ImGui::CalcTextSize(row.what.c_str()).x);
        }
        heights.push_back(lineH * static_cast<float>(1 + section.rows.size()) + sectionGap);
    }
    const float columnW = keysW + keysGap + whatW;

    // As many columns as fit in the Overview's own margins, up to three.
    constexpr float kMarginFrac = 0.08f;
    const float maxW = displayW * (1.0f - 2.0f * kMarginFrac);
    const float maxH = displayH * (1.0f - 2.0f * kMarginFrac);
    size_t columns = 3;
    while (columns > 1 &&
           static_cast<float>(columns) * columnW + static_cast<float>(columns - 1) * columnGap +
                   2.0f * style.WindowPadding.x >
               maxW) {
        --columns;
    }
    std::vector<size_t> starts = BalancedColumnStarts(heights, columns);
    starts.push_back(sections.size());
    float bodyH = 0.0f;
    for (size_t c = 0; c + 1 < starts.size(); ++c) {
        float h = 0.0f;
        for (size_t i = starts[c]; i < starts[c + 1]; ++i) {
            h += heights[i];
        }
        bodyH = std::max(bodyH, h - sectionGap);
    }
    const size_t usedColumns = starts.size() - 1;
    const float headerH = lineH + style.ItemSpacing.y * 2.0f + 1.0f;
    const ImVec2 size(std::min(maxW, static_cast<float>(usedColumns) * columnW +
                                         static_cast<float>(usedColumns - 1) * columnGap +
                                         2.0f * style.WindowPadding.x),
                      std::min(maxH, headerH + bodyH + 2.0f * style.WindowPadding.y + style.ItemSpacing.y));
    ImGui::SetNextWindowPos(ImVec2((displayW - size.x) * 0.5f, (displayH - size.y) * 0.5f));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##cheat_sheet_panel", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(theme::kWhite, "%s", strings::kCheatSheetTitle);
    // How to get out again, right-aligned on the title's line.
    const platform::KeyCombo& ownKey = settings_.Live().shortcuts[ShortcutActionIndex(ShortcutAction::CheatSheet)];
    char closeHint[96];
    if (ownKey.key != 0) {
        std::snprintf(closeHint, sizeof(closeHint), strings::kCheatSheetClose, FormatKeyComboLabel(ownKey).c_str());
    } else {
        std::snprintf(closeHint, sizeof(closeHint), "%s", strings::kCheatSheetCloseNoKey);
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(closeHint).x));
    ImGui::TextColored(theme::kGraphite300, "%s", closeHint);
    ImGui::Separator();

    // Scrolls only on a screen too small for it all.
    ImGui::BeginChild("##cheat_sheet_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    const ImVec2 origin = ImGui::GetCursorPos();
    for (size_t c = 0; c < usedColumns; ++c) {
        const float x = origin.x + static_cast<float>(c) * (columnW + columnGap);
        float y = origin.y;
        for (size_t i = starts[c]; i < starts[c + 1]; ++i) {
            const CheatSheetSection& section = sections[i];
            ImGui::SetCursorPos(ImVec2(x, y));
            ImGui::TextColored(theme::Accent(), "%s", section.title);
            y += lineH;
            for (const CheatSheetRow& row : section.rows) {
                ImGui::SetCursorPos(ImVec2(x, y));
                if (row.keys.empty()) {
                    ImGui::TextColored(theme::kGraphite400, "%s", row.what.c_str());
                } else {
                    ImGui::TextColored(theme::kWhite, "%s", row.keys.c_str());
                    ImGui::SetCursorPos(ImVec2(x + keysW + keysGap, y));
                    ImGui::TextColored(theme::kGraphite200, "%s", row.what.c_str());
                }
                y += lineH;
            }
            y += sectionGap;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace sz::ui
