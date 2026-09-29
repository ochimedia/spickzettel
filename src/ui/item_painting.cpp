#include "ui/item_painting.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/canvas/item_geometry.h"
#include "core/drawing/stroke_mesh.h"
#include "ui/theme.h"

namespace sz::ui {

using namespace ::sz::core;

// Tessellated here rather than handed to ImGui's AddPolyline, which offsets
// each point along the average of its two adjacent segment normals and
// rescales by 1/cos^2 of half the turn - a miter clamped only at 100x the
// half width, so a near-reversal threw a spike out the side of a wide pen.
// It also has no cap but a flat one and no round join at all. See
// stroke_mesh.h for the geometry this builds instead, and why it comes
// body first: drawn under the renderer's depth test, the stroke reaches
// each pixel once, wherever it crosses or folds over itself.
//
// The fringe is one pixel in *screen* space, so it stays a pixel wide
// whatever the item is scaled to - see kStrokeFringePx, which both this and
// the mesh cache take it from.
//
// The scale is baked into the geometry and the offset is not: the mesh comes
// out around the origin and is translated as its vertices are written. That
// is what makes a mesh worth keeping between frames - see StrokeMeshSlot.
void DrawStroke(ImDrawList* drawList, const Stroke& stroke, float offsetX, float offsetY, float scaleX, float scaleY,
                 float opacity, StrokeMeshSlot meshSlot, platform::DrawCallback strokeDepth) {
    if (stroke.points.empty() || opacity <= 0.0f) {
        return;
    }
    // The width's own factor - see core::LengthScale, which the pen baked
    // it into the snippet's space with.
    const float widthScale = core::LengthScale(scaleX, scaleY);
    const float halfWidth = stroke.width * widthScale * 0.5f;
    const ImU32 color = ToImColor(stroke.colorRGBA, opacity);

    // The mesh is built around the origin and translated as it is written
    // out below, rather than built at the position it will appear. That is
    // what lets a cached mesh survive its item being dragged: the offset is
    // the only thing a move changes, and it never reaches the tessellator.
    // It also keeps the geometry math at small coordinates, which is where
    // floats behave best.
    const StrokeMesh* mesh = nullptr;
    if (meshSlot.cache != nullptr) {
        mesh = &meshSlot.cache->MeshFor(meshSlot.itemId, meshSlot.strokeIndex, stroke, meshSlot.generation, scaleX,
                                         scaleY, halfWidth);
    } else {
        // Nothing to cache it in - the stroke being drawn right now, which
        // grows every frame, or a caller that keeps no cache. Static rather
        // than local so the two buffers are reused frame to frame; this is
        // the render thread, and nothing here re-enters.
        static StrokeMesh uncached;
        static std::vector<StrokePoint> scaledPoints;
        scaledPoints.clear();
        scaledPoints.reserve(stroke.points.size());
        for (const StrokePoint& p : stroke.points) {
            scaledPoints.push_back(StrokePoint{p.x * scaleX, p.y * scaleY});
        }
        BuildStrokeMesh(scaledPoints, halfWidth, kStrokeFringePx, uncached);
        mesh = &uncached;
    }
    if (mesh->indices.empty()) {
        return;
    }

    // Coverage is only ever 0 or 1 out of the builder - solid inside, and the
    // outer edge of the anti-aliasing fringe - so two colors cover it.
    const ImU32 transparent = color & ~IM_COL32_A_MASK;
    const ImVec2 uv = drawList->_Data->TexUvWhitePixel;

    // Under the depth test the first fragment on a pixel is the one that
    // stays, which is why the mesh comes body first: an edge drawn first
    // would take a pixel the stroke's own body covers fully, and leave a
    // faint line across every crossing (see StrokeMesh).
    if (strokeDepth != nullptr) {
        drawList->AddCallback(strokeDepth, reinterpret_cast<void*>(intptr_t{1}));
    }

    // One reservation for the whole mesh, which a stroke drawn for half a
    // minute without lifting the pen takes past 65536 vertices: ImGui's
    // indices are 32 bits here for that (see cmake/FetchImGui.cmake).
    drawList->PrimReserve(static_cast<int>(mesh->indices.size()), static_cast<int>(mesh->vertices.size()));
    // After PrimReserve, which may start a fresh draw command and reset it.
    const unsigned int base = drawList->_VtxCurrentIdx;
    for (const StrokeVertex& v : mesh->vertices) {
        drawList->PrimWriteVtx(ImVec2(offsetX + v.x, offsetY + v.y), uv, v.coverage >= 1.0f ? color : transparent);
    }
    for (const uint32_t index : mesh->indices) {
        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
    }
    if (strokeDepth != nullptr) {
        drawList->AddCallback(strokeDepth, nullptr);
    }
}

namespace {

// Opens what a snippet's strokes are drawn into at `opacity`, the snippet's:
// below full opacity and with a layer to be had, a layer of its own, and the
// strokes go in at full strength - the opacity is the layer's, applied once
// by CloseStrokeLayer. Otherwise nothing, and each stroke takes the opacity
// itself: fully opaque, the layer laid down would change nothing drawing
// straight onto the frame does not, since source-over is associative.
// Returns what to draw each stroke at.
bool UsesLayer(float opacity, const PaintHooks& hooks) { return hooks.strokeLayer != nullptr && opacity < 1.0f; }

float OpenStrokeLayer(ImDrawList* drawList, float opacity, const PaintHooks& hooks) {
    if (!UsesLayer(opacity, hooks)) {
        return opacity;
    }
    platform::StrokeLayerStep step;
    step.open = true;
    drawList->AddCallback(hooks.strokeLayer, &step, sizeof(step));
    return 1.0f;
}

void CloseStrokeLayer(ImDrawList* drawList, float opacity, const PaintHooks& hooks) {
    if (!UsesLayer(opacity, hooks)) {
        return;
    }
    platform::StrokeLayerStep step;
    step.open = false;
    step.opacity = opacity;
    drawList->AddCallback(hooks.strokeLayer, &step, sizeof(step));
    // Null only where no renderer backend is set up, which is also where
    // nothing is drawn.
    if (const ImDrawCallback reset = ImGui::GetPlatformIO().DrawCallback_ResetRenderState) {
        drawList->AddCallback(reset, nullptr);
    }
}

// A snippet's strokes, native space into `pMin..pMax`, as one layer - see
// DrawItemContent.
void DrawStrokeLayer(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, StrokeMeshSlot meshCache,
                     const PaintHooks& hooks, const std::function<void(float opacity)>& moreStrokes) {
    const float boxW = pMax.x - pMin.x;
    const float boxH = pMax.y - pMin.y;
    const float scaleX = item.nativeW != 0.0f ? boxW / item.nativeW : (item.rect.w != 0.0f ? boxW / item.rect.w : 1.0f);
    const float scaleY = item.nativeH != 0.0f ? boxH / item.nativeH : (item.rect.h != 0.0f ? boxH / item.rect.h : 1.0f);
    const float strokeOpacity = OpenStrokeLayer(drawList, item.foregroundOpacity, hooks);
    for (size_t index = 0; index < item.strokes.size(); ++index) {
        DrawStroke(drawList, item.strokes[index], pMin.x, pMin.y, scaleX, scaleY, strokeOpacity,
                   meshCache.For(item.id, index), hooks.strokeDepth);
    }
    if (moreStrokes) {
        moreStrokes(strokeOpacity);
    }
    CloseStrokeLayer(drawList, item.foregroundOpacity, hooks);
}

}  // namespace

void DrawPicture(ImDrawList* drawList, uint64_t texture, ImVec2 pMin, ImVec2 pMax, ImU32 tint, PaintHooks hooks) {
    const bool filtered = hooks.applyFilter != nullptr && hooks.filter != platform::ImageFilter::Bilinear;
    if (filtered) {
        drawList->AddCallback(hooks.applyFilter, reinterpret_cast<void*>(static_cast<intptr_t>(hooks.filter)));
    }
    drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(texture)), pMin, pMax, ImVec2(0.0f, 0.0f),
                        ImVec2(1.0f, 1.0f), tint);
    if (filtered) {
        // Null only where no renderer backend is set up, which is also
        // where nothing is drawn.
        if (const ImDrawCallback reset = ImGui::GetPlatformIO().DrawCallback_ResetRenderState) {
            drawList->AddCallback(reset, nullptr);
        }
    }
}

void DrawSnippetPicture(ImDrawList* drawList, const Picture& picture, ImVec2 pMin, ImVec2 pMax, uint64_t texture,
                        PaintHooks hooks) {
    if (picture.opacity <= 0.0f) {
        return;
    }
    if (texture != 0) {
        // Real pixels - AddImage stretches the whole texture to fill
        // pMin..pMax on its own, the same way the gradient/fill below fills
        // whatever rect the item currently has, so resizing the item needs
        // no extra handling here. The tint multiplies the sampled texture
        // (see Picture::tintColorRGBA) - white, the default, leaves a
        // capture unmodified; any other color mixes into it.
        DrawPicture(drawList, texture, pMin, pMax, ToImColor(picture.tintColorRGBA, picture.opacity), hooks);
    } else if (picture.showsPlaceholder) {
        // No pixels to show (the OS-level capture failed, or the picture
        // cannot be read) - a placeholder gradient, faded by the same
        // opacity a real capture would use.
        const ImU32 top = ImColor::HSV(picture.placeholderHue / 360.0f, 0.38f, 0.55f, picture.opacity);
        const ImU32 bottom = ImColor::HSV(picture.placeholderHue / 360.0f, 0.24f, 0.82f, picture.opacity);
        drawList->AddRectFilledMultiColor(pMin, pMax, top, top, bottom, bottom);
    } else {
        // A picture given a solid color instead - just the color itself, no
        // image to tint.
        drawList->AddRectFilled(pMin, pMax, ToImColor(picture.tintColorRGBA, picture.opacity));
    }
}

void DrawItemContent(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, uint64_t pictureTexture,
                     bool skipNoteText, StrokeMeshSlot meshCache, PaintHooks hooks,
                     const std::function<void(float opacity)>& moreStrokes) {
    DrawSnippetPicture(drawList, item.picture, pMin, pMax, pictureTexture, hooks);
    DrawStrokeLayer(drawList, item, pMin, pMax, meshCache, hooks, moreStrokes);

    // Text (Item::noteText, see its own doc comment) is a caption layered
    // on top of whatever's already here - a background image, strokes, or
    // both - rather than replacing either, so a snippet's drawing/
    // screenshot content and its text can freely coexist on one item. A
    // band pinned to the content's own top edge, sized to fit the wrapped
    // text, not the whole item, so it reads as a caption
    // rather than swallowing whatever it's sitting on. Skipped for
    // whichever item skipNoteText names - the one currently open in
    // RenderNoteEditor's own window, which paints its own
    // matching backing panel over the same rect - to avoid double-drawing
    // the same text under that window's own InputTextMultiline.
    if (!item.noteText.empty() && !skipNoteText) {
        // No panel fill behind the text (kept transparent on purpose - see
        // Item::noteText's own doc comment: a user who wants a backdrop
        // already has the item's own background color/opacity controls,
        // so this doesn't need to impose one automatically). Just clip to
        // the content rect so wrapped text can't bleed past it.
        // Color and size both come off the item itself now (see
        // Item::noteTextColorRGBA/noteTextSizePx) rather than being fixed
        // at white-and-whatever-the-UI-font-is - RenderNoteEditor's live
        // editor reads the exact same two fields, so switching between
        // editing and not doesn't change how the text looks.
        const float wrapWidth = std::max(0.0f, (pMax.x - pMin.x) - 2.0f * theme::kNoteTextPad);
        drawList->PushClipRect(pMin, pMax, true);
        drawList->AddText(ImGui::GetFont(), item.noteTextSizePx,
                           ImVec2(pMin.x + theme::kNoteTextPad, pMin.y + theme::kNoteTextPad),
                           ToImColor(item.noteTextColorRGBA), item.noteText.c_str(), nullptr, wrapWidth);
        drawList->PopClipRect();
    }
}

// Renders a scaled-down snapshot of `canvas`'s items into `thumbMin..thumbMax`
// - uniform scale (never stretched), letterboxed/centered, computed from how
// the real overlay's own dimensions compare to the thumbnail box.
//
// `meshCache` must be a different one from the canvas's own: a tile draws
// the same items the canvas behind it does, at a wholly different scale, in
// the same frame - see CanvasView::previewMeshCache_.
void DrawCanvasPreview(ImDrawList* drawList, const Canvas& canvas, ImVec2 thumbMin, ImVec2 thumbMax, float displayW,
                       float displayH, bool showStrokes, const PreviewTextureFn& previewTexture,
                       StrokeMeshSlot meshCache, PaintHooks hooks) {
    drawList->PushClipRect(thumbMin, thumbMax, true);
    drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(14, 16, 20, 255));

    if (displayW > 0.0f && displayH > 0.0f) {
        const float scale = std::min((thumbMax.x - thumbMin.x) / displayW, (thumbMax.y - thumbMin.y) / displayH);
        const float offsetX = thumbMin.x + ((thumbMax.x - thumbMin.x) - displayW * scale) * 0.5f;
        const float offsetY = thumbMin.y + ((thumbMax.y - thumbMin.y) - displayH * scale) * 0.5f;

        for (const Item& item : canvas.items) {
            // Deleted on its own: not part of what the canvas shows, and not
            // what restoring a deleted canvas brings back either. Minimized:
            // not on the canvas either, but a chip on the dock.
            if (item.deletedAt != 0 || item.minimized) {
                continue;
            }
            const ImVec2 pMin(offsetX + item.rect.x * scale, offsetY + item.rect.y * scale);
            const ImVec2 pMax(offsetX + (item.rect.x + item.rect.w) * scale, offsetY + (item.rect.y + item.rect.h) * scale);
            DrawItemPreview(drawList, item, pMin, pMax, showStrokes, previewTexture, meshCache, hooks);
        }
    }

    drawList->PopClipRect();
}

void DrawItemPreview(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, bool showStrokes,
                     const PreviewTextureFn& previewTexture, StrokeMeshSlot meshCache, PaintHooks hooks) {
    // The picture with whichever texture it can have here: the real one for
    // the current canvas (already loaded), a thumbnail-sized copy for the
    // rest if previews are on, and none at all otherwise - in which case
    // DrawSnippetPicture falls back to the same placeholder gradient or
    // plain fill it uses anywhere else.
    bool drewAnything = false;
    if (item.picture.opacity > 0.0f) {
        // Nothing at all for a picture whose pixels are still being read
        // (see CanvasView::PicturePreviewTexture, which says so by
        // returning nothing rather than 0). The placeholder gradient means "there is no image
        // here", and a few frames of it in front of an image that *is*
        // there and is on its way reads as the thumbnails being wrong and
        // then correcting themselves. An outlined empty box - what the
        // fall-through below draws - says the same thing quietly.
        const std::optional<uint64_t> texture =
            previewTexture ? previewTexture(item) : std::optional<uint64_t>(0);
        if (texture.has_value()) {
            DrawSnippetPicture(drawList, item.picture, pMin, pMax, *texture, hooks);
            drewAnything = true;
        }
    }
    if (!drewAnything) {
        drawList->AddRect(pMin, pMax, IM_COL32(90, 96, 110, 180));
    }
    if (!showStrokes) {
        return;
    }

    // Native -> preview is one scale factor per axis: the box over the
    // item's native size, the same as on the canvas.
    DrawStrokeLayer(drawList, item, pMin, pMax, meshCache, hooks, {});
}

}  // namespace sz::ui
