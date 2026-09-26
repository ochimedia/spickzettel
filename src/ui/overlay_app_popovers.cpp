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

void OverlayApp::KeepPenWidth() {
    if (!drawWidthDirty_) {
        return;
    }
    drawWidthDirty_ = false;
    if (settings_.Get(setting::kStrokeWidth) != editor_.DrawWidth()) {
        settings_.Set(setting::kStrokeWidth, editor_.DrawWidth());
    }
}

void OverlayApp::KeepPen() {
    KeepPenWidth();
    popups_.Settle();
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
