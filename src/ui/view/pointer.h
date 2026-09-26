#pragma once

// The pointer - docs/VIEW_LAYER.md, section 7: the shape the OS cursor
// wears, the software pointer drawn while the mouse grab hides the OS one,
// and what follows the pointer over the canvas - the drag previews (a
// region being framed, the rectangle eraser), the wheel's size preview and
// the modifier badge - all on ImGui's foreground list. What it keeps of its
// own is the shape last applied and the history that says when to apply it
// again, when the size preview expires, and the pen's width owed.

#include <optional>

#include <imgui.h>

#include "core/session/settings.h"
#include "platform/platform_types.h"
#include "ui/editor.h"
#include "ui/view/view_host.h"

namespace sz::ui {

class Pointer {
public:
    Pointer(core::Settings& settings, Editor& editor, ViewHost& host);

    // The wheel sized the tool in hand: its size preview comes up, and a
    // pen's width is owed to the settings once the preview has faded, so a
    // burst of notches is one write.
    void ToolSized(bool pen);
    // Stage 5: the drag previews, the size preview and the badge.
    void DrawOverCanvas();
    // Stage 9: the shape, then the software pointer - after everything
    // else is drawn, since ImGui only knows what it wants once every widget
    // has run.
    void Draw();
    // Stage 10: the pen's width, once its size preview has faded.
    void Apply();
    // The pen's width owed, kept now - the overlay settling.
    void KeepPenWidth();
    // The overlay has just been put back on screen: while it was away,
    // whatever is underneath owned the pointer and will have installed its
    // own shape, so what was last asked for says nothing about what is on
    // screen now.
    void OnOverlayShown() { appliedPointerShape_.reset(); }

private:
    // Which pointer the canvas itself asks for, from what is under the mouse
    // and what the tools would do there. Says nothing about what ImGui wants
    // over its own windows and widgets; ApplyPointerShape settles that.
    platform::CursorShape WantedPointerShape() const;
    // Hands WantedPointerShape() to the platform window, or Default when
    // something else owns the cursor this frame. Only actually pushes when
    // the answer has moved - see its definition for what "moved" has to
    // mean.
    void ApplyPointerShape();
    // The overlay's own pointer, drawn while the mouse grab is taking input
    // away from a game - which also means the OS cursor is hidden and left
    // wherever that game is holding it. See the definition.
    void DrawSoftwareCursor() const;
    // The frame a region being framed is dragging out, and the rectangle the
    // rectangle eraser is about to take away - the same visual language, a
    // translucent fill and an outline, kept apart since the two follow
    // different gestures.
    void RenderRegionCaptureOverlay();
    void RenderRectEraserOverlay();
    // The mouse wheel's own feedback: a dot the exact size the active tool
    // will draw (or erase) at, under the cursor, plus the number. See
    // sizePreviewExpireAtSeconds_ for why it's transient.
    void RenderBrushSizePreview();
    // Beside the pointer while Draw or Erase is in hand over a snippet and
    // a modifier changes what a press would make - a line or a rectangle -
    // a small glyph of it, so the modifiers are not a secret.
    void RenderToolModifierBadge();
    // Whether a panel covering the canvas is up.
    bool PanelOpen() const;

    core::Settings& settings_;
    Editor& editor_;
    ViewHost& host_;

    // What the last push asked for. nullopt means "nothing known" - a fresh
    // start, or the overlay having just been shown (see OnOverlayShown).
    std::optional<platform::CursorShape> appliedPointerShape_;
    // Two frames of ImGui's wanted cursor, not one, and the pointer position.
    // See ApplyPointerShape for why the history has to be two deep - a
    // one-frame view misses the frame ImGui's backend actually installs on.
    ImGuiMouseCursor lastImGuiCursor_ = ImGuiMouseCursor_Arrow;
    ImGuiMouseCursor previousImGuiCursor_ = ImGuiMouseCursor_Arrow;
    float lastPointerX_ = 0.0f;
    float lastPointerY_ = 0.0f;
    // Transient "this is how big it is now" preview at the cursor, armed
    // by a mouse-wheel size change and expiring on its own shortly after
    // - ImGui::GetTime() past this means nothing to draw. Deliberately
    // transient rather than a permanent brush-outline cursor: this overlay
    // sits on top of a game, where a ring that follows the pointer forever
    // is exactly the sort of always-there element that makes an overlay
    // feel busy. No companion size/tool field - it reads the live editor's
    // tool and its size, so a burst of wheel steps shows the current value
    // throughout rather than a snapshot of the first.
    double sizePreviewExpireAtSeconds_ = 0.0;
    // Whether the wheel has changed the pen's width since it was last
    // saved - see ToolSized.
    bool drawWidthDirty_ = false;
};

}  // namespace sz::ui
