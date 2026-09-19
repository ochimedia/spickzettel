#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/canvas/item_geometry.h"
#include "ui/icons_generated.h"

#include <imgui.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Recently deleted =================
//
// What the Canvases tab shows in place of the folders and the grid while
// recentlyDeletedOpen_ is set: every deleted thing with a mark of its own
// (see CanvasManager::DeletedThings), newest first - a preview on the left,
// what it is on the right, and Restore and Delete permanently at the end of
// its row. Nothing in the list is opened or looked into: restoring is how a
// thing is seen again, so a deleted folder is one row rather than a tree to
// browse, and nothing deleted is ever on the canvas to be guarded against.

namespace {

// "1 snippet", "3 snippets".
std::string Counted(size_t count, const char* one, const char* many) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), count == 1 ? one : many, count);
    return buf;
}

std::string Joined(const std::vector<std::string>& parts) {
    std::string joined;
    for (const std::string& part : parts) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += part;
    }
    return joined;
}

// What kind of snippet this is, by what it holds rather than by its name -
// a name is only what it was called when it was made.
const char* SnippetKind(const Item& item) {
    if (item.hasBackground) {
        return strings::kDeletedKindScreenshot;
    }
    if (!item.noteText.empty()) {
        return strings::kDeletedKindNote;
    }
    return strings::kDeletedKindDrawing;
}

// What has been put into a snippet: its strokes, its painting, its text. A
// screenshot's picture goes without saying - its kind says it - so one with
// nothing added is its kind alone, where anything else is "nothing in it".
std::string SnippetContent(const Item& item) {
    std::vector<std::string> parts;
    if (!item.strokes.empty()) {
        parts.push_back(Counted(item.strokes.size(), strings::kDeletedStrokeOne, strings::kDeletedStrokeMany));
    }
    // By its file as well as its pixels: a snippet off the current canvas has
    // its painted pixels on disk, not in memory.
    const bool painted = std::any_of(item.layers.begin(), item.layers.end(), [](const Layer& layer) {
        return layer.kind == LayerKind::Painted && (!layer.imageFile.empty() || layer.HasPaintedPixels());
    });
    if (painted) {
        parts.push_back(strings::kDeletedPainting);
    }
    if (!item.noteText.empty()) {
        parts.push_back(strings::kDeletedText);
    }
    if (parts.empty()) {
        return item.hasBackground ? std::string() : std::string(strings::kDeletedNothingInIt);
    }
    return Joined(parts);
}

// How many canvases and snippets restoring a folder or canvas brings back:
// what went with it, not what was deleted on its own before.
std::string ContainerContent(size_t canvases, size_t snippets, bool isFolder) {
    std::vector<std::string> parts;
    if (isFolder && canvases > 0) {
        parts.push_back(Counted(canvases, strings::kDeletedCanvasOne, strings::kDeletedCanvasMany));
    }
    if (snippets > 0) {
        parts.push_back(Counted(snippets, strings::kDeletedSnippetOne, strings::kDeletedSnippetMany));
    }
    return parts.empty() ? std::string(strings::kDeletedNothingInIt) : Joined(parts);
}

size_t LiveSnippetCount(const Canvas& canvas) {
    return static_cast<size_t>(std::count_if(canvas.items.begin(), canvas.items.end(),
                                              [](const Item& item) { return item.deletedAt == 0; }));
}

// A local calendar time, copied out of the buffer std::localtime shares -
// see TimestampName for why that spelling, and the pragma.
std::tm LocalTime(std::time_t when) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const std::tm* local = std::localtime(&when);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return local != nullptr ? *local : std::tm{};
}

// "Deleted today, 14:05 - 32 min ago": the day in words while that is
// shorter than a date, and how long ago while that is the quicker thing to
// read - which is what "I deleted something half an hour ago" is looking for.
std::string DeletedWhen(int64_t deletedAt, std::time_t now) {
    const std::tm at = LocalTime(static_cast<std::time_t>(deletedAt));
    const std::tm today = LocalTime(now);
    const std::tm yesterday = LocalTime(now - 24 * 60 * 60);
    const auto sameDay = [](const std::tm& a, const std::tm& b) {
        return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
    };
    char clock[16] = "";
    std::strftime(clock, sizeof(clock), "%H:%M", &at);
    char day[64] = "";
    if (sameDay(at, today)) {
        std::snprintf(day, sizeof(day), strings::kDeletedToday, clock);
    } else if (sameDay(at, yesterday)) {
        std::snprintf(day, sizeof(day), strings::kDeletedYesterday, clock);
    } else {
        std::strftime(day, sizeof(day), "%Y-%m-%d %H:%M", &at);
    }
    constexpr int64_t kMinute = 60;
    constexpr int64_t kHour = 60 * kMinute;
    constexpr int64_t kDay = 24 * kHour;
    const int64_t seconds = static_cast<int64_t>(now) - deletedAt;
    char ago[48] = "";
    if (seconds >= 0 && seconds < kMinute) {
        std::snprintf(ago, sizeof(ago), "%s", strings::kDeletedJustNow);
    } else if (seconds >= kMinute && seconds < kHour) {
        std::snprintf(ago, sizeof(ago), strings::kDeletedMinutesAgo, static_cast<int>(seconds / kMinute));
    } else if (seconds >= kHour && seconds < kDay) {
        std::snprintf(ago, sizeof(ago), strings::kDeletedHoursAgo, static_cast<int>(seconds / kHour));
    }
    char line[160];
    if (ago[0] != '\0') {
        std::snprintf(line, sizeof(line), strings::kDeletedAtAgo, day, ago);
    } else {
        std::snprintf(line, sizeof(line), strings::kDeletedAt, day);
    }
    return line;
}

// A container's name, saying so when restoring the thing in it would bring
// that container back too.
std::string PlaceName(const std::string& name, bool deleted) {
    return deleted ? name + " " + strings::kDeletedAlsoDeleted : name;
}

// What a row says and what its preview draws, found by id - the list
// itself is only ids and stamps.
struct DeletedRow {
    std::string name;
    const char* kind = strings::kDeletedKindFolder;
    std::string content;
    std::string where;
    // A canvas as its tile draws it, a folder as its first canvas, a
    // snippet on its own; at most one of the two is set.
    const Canvas* previewCanvas = nullptr;
    const Item* previewItem = nullptr;
};

// Nullopt when nothing by the thing's id is there to describe.
std::optional<DeletedRow> DescribeDeletedThing(const CanvasManager& manager, const DeletedThing& thing) {
    DeletedRow row;
    switch (thing.kind) {
        case DeletedThing::Kind::Folder: {
            const Folder* folder = manager.FindFolder(thing.id);
            if (folder == nullptr) {
                return std::nullopt;
            }
            row.name = folder->name;
            row.kind = strings::kDeletedKindFolder;
            size_t canvasCount = 0;
            size_t snippetCount = 0;
            for (const Canvas& canvas : manager.Canvases()) {
                if (canvas.folderId != folder->id || canvas.deletedAt != 0) {
                    continue;
                }
                ++canvasCount;
                snippetCount += LiveSnippetCount(canvas);
                // Its first canvas stands for it.
                if (row.previewCanvas == nullptr) {
                    row.previewCanvas = &canvas;
                }
            }
            row.content = ContainerContent(canvasCount, snippetCount, /*isFolder=*/true);
            return row;
        }
        case DeletedThing::Kind::Canvas: {
            const Canvas* canvas = manager.FindCanvas(thing.id);
            if (canvas == nullptr) {
                return std::nullopt;
            }
            row.name = canvas->name;
            row.kind = strings::kDeletedKindCanvas;
            row.content = ContainerContent(0, LiveSnippetCount(*canvas), /*isFolder=*/false);
            row.previewCanvas = canvas;
            if (const Folder* folder = manager.FindFolder(canvas->folderId)) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), strings::kDeletedInFolder,
                              PlaceName(folder->name, folder->deletedAt != 0).c_str());
                row.where = buf;
            }
            return row;
        }
        case DeletedThing::Kind::Snippet: {
            const Canvas* holder = nullptr;
            for (const Canvas& canvas : manager.Canvases()) {
                for (const Item& item : canvas.items) {
                    if (item.id == thing.id) {
                        holder = &canvas;
                        row.previewItem = &item;
                    }
                }
            }
            if (row.previewItem == nullptr) {
                return std::nullopt;
            }
            row.name = row.previewItem->name;
            row.kind = SnippetKind(*row.previewItem);
            row.content = SnippetContent(*row.previewItem);
            const Folder* folder = manager.FindFolder(holder->folderId);
            char buf[512];
            std::snprintf(buf, sizeof(buf), strings::kDeletedOnCanvas,
                          PlaceName(holder->name, holder->deletedAt != 0).c_str(),
                          folder != nullptr ? PlaceName(folder->name, folder->deletedAt != 0).c_str() : "");
            row.where = buf;
            return row;
        }
    }
    return std::nullopt;
}

}  // namespace

void OverlayApp::RenderRecentlyDeleted(float displayW, float displayH) {
    const std::vector<DeletedThing> things = Manager().DeletedThings();
    if (things.empty()) {
        ImGui::TextColored(theme::kGraphite200, "%s", strings::kDeletedEmpty);
        return;
    }

    // A canvas tile's proportions, a little smaller: the list is read down,
    // not scanned across.
    constexpr float kPreviewW = 160.0f;
    constexpr float kPreviewH = 104.0f;
    constexpr float kPadding = 10.0f;
    constexpr float kRowGap = 6.0f;
    constexpr float kTextGap = 16.0f;
    constexpr float kIconButtonSize = 28.0f;  // PillIconButton's and DangerIconButton's own
    const float rowHeight = kPreviewH + kPadding * 2.0f;
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImGuiStyle& style = ImGui::GetStyle();

    const PreviewTextureFn previewTexture = PreviewTextureLookup();
    const std::time_t now = std::time(nullptr);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // Deferred past the walk, which reads the very records a restore changes.
    std::optional<uint64_t> restoreId;

    for (const DeletedThing& thing : things) {
        const std::optional<DeletedRow> row = DescribeDeletedThing(Manager(), thing);
        if (!row.has_value()) {
            continue;
        }

        ImGui::PushID(static_cast<int>(thing.id));
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + rowHeight);
        drawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(theme::kFieldBg), theme::kRadiusSm);

        const ImVec2 previewMin(rowMin.x + kPadding, rowMin.y + kPadding);
        const ImVec2 previewMax(previewMin.x + kPreviewW, previewMin.y + kPreviewH);
        DrawDeletedPreview(drawList, row->previewCanvas, row->previewItem, previewMin, previewMax, displayW, displayH,
                           previewTexture);

        // Restore and Delete permanently at the row's end, centred on it.
        const float restoreWidth =
            style.FramePadding.x * 2.0f + 15.0f + 7.0f + ImGui::CalcTextSize(strings::kDeletedRestore).x;
        const float buttonsX = rowMax.x - kPadding - kIconButtonSize - style.ItemSpacing.x - restoreWidth;
        const float rowCenterY = rowMin.y + rowHeight * 0.5f;

        // The words, clipped short of the buttons.
        const float textX = previewMax.x + kTextGap;
        float textY = previewMin.y + 4.0f;
        drawList->PushClipRect(ImVec2(textX, rowMin.y), ImVec2(buttonsX - kTextGap, rowMax.y), true);
        const auto line = [&](const std::string& text, const ImVec4& color) {
            drawList->AddText(ImVec2(textX, textY), ImGui::GetColorU32(color), text.c_str());
            textY += lineHeight;
        };
        line(row->name.empty() ? std::string(strings::kMoveCopyItemWord) : row->name, theme::kWhite);
        char kindLine[256];
        if (row->content.empty()) {
            std::snprintf(kindLine, sizeof(kindLine), "%s", row->kind);
        } else {
            std::snprintf(kindLine, sizeof(kindLine), strings::kDeletedKindAndContent, row->kind, row->content.c_str());
        }
        line(kindLine, theme::kGraphite200);
        if (!row->where.empty()) {
            line(row->where, theme::kGraphite300);
        }
        line(DeletedWhen(thing.deletedAt, now), theme::kGraphite300);
        drawList->PopClipRect();

        ImGui::SetCursorScreenPos(ImVec2(buttonsX, rowCenterY - ImGui::GetFrameHeight() * 0.5f));
        if (PrimaryButton("##restoredeleted", icons::kUndo, strings::kDeletedRestore)) {
            restoreId = thing.id;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", strings::kDeletedRestoreTip);
        }
        ImGui::SetCursorScreenPos(
            ImVec2(buttonsX + restoreWidth + style.ItemSpacing.x, rowCenterY - kIconButtonSize * 0.5f));
        if (DangerIconButton("##deleteforgood", icons::kTrash)) {
            confirmDeleteTarget_ = ConfirmDeleteTarget{ConfirmKindOf(thing.kind), thing.id, row->name, /*forGood=*/true};
            confirmDeletePopoverRequested_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", strings::kDeletedDeleteForGoodTip);
        }

        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y + kRowGap));
        ImGui::PopID();
    }
    // Closes out the last row's cursor move, as the folder sidebar's does.
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    if (restoreId.has_value() && session_.Restore(*restoreId)) {
        ShowActionToast(strings::kToastRestored);
    }
}

void OverlayApp::DrawDeletedPreview(ImDrawList* drawList, const Canvas* canvas, const Item* item, ImVec2 previewMin,
                                    ImVec2 previewMax, float displayW, float displayH,
                                    const PreviewTextureFn& previewTexture) {
    if (canvas != nullptr) {
        DrawCanvasPreview(drawList, *canvas, previewMin, previewMax, displayW, displayH, Cfg().strokeRenderMode,
                          Cfg().overviewShowsStrokes, previewTexture, PreviewMeshSlot());
    } else {
        drawList->AddRectFilled(previewMin, previewMax, IM_COL32(14, 16, 20, 255));
        if (item != nullptr) {
            const float aspect = item->nativeW > 0.0f && item->nativeH > 0.0f ? item->nativeW / item->nativeH
                                 : item->rect.h > 0.0f                        ? item->rect.w / item->rect.h
                                                                              : 1.0f;
            const Rect fitted = FitAspectRatioIntoViewport(aspect, previewMax.x - previewMin.x, previewMax.y - previewMin.y);
            const ImVec2 itemMin(previewMin.x + fitted.x, previewMin.y + fitted.y);
            const ImVec2 itemMax(itemMin.x + fitted.w, itemMin.y + fitted.h);
            drawList->PushClipRect(previewMin, previewMax, true);
            DrawItemPreview(drawList, *item, itemMin, itemMax, Cfg().strokeRenderMode, Cfg().overviewShowsStrokes,
                            previewTexture, PreviewMeshSlot());
            drawList->PopClipRect();
        }
    }
    drawList->AddRect(previewMin, previewMax, IM_COL32(70, 76, 88, 255), 4.0f);
}

OverlayApp::ConfirmDeleteTarget::Kind OverlayApp::ConfirmKindOf(DeletedThing::Kind kind) {
    switch (kind) {
        case DeletedThing::Kind::Folder:
            return ConfirmDeleteTarget::Kind::Folder;
        case DeletedThing::Kind::Canvas:
            return ConfirmDeleteTarget::Kind::Canvas;
        case DeletedThing::Kind::Snippet:
            break;
    }
    return ConfirmDeleteTarget::Kind::Snippet;
}

}  // namespace sz::ui
