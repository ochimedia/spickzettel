#include "ui/item_painting.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/drawing/stroke_mesh.h"
#include "ui/theme.h"

namespace sz::ui {

using namespace ::sz::core;

// Tessellated here rather than handed to ImGui's AddPolyline, which offsets
// each point along the average of its two adjacent segment normals and
// rescales by 1/cos^2 of half the turn - a miter clamped only at 100x the
// half width, so a near-reversal threw a spike out the side of a wide pen.
// It also has no cap but a flat one and no round join at all. See
// stroke_mesh.h for the geometry this builds instead, and why one connected
// mesh with no overlapping triangles is what a translucent stroke needs.
//
// The fringe is one pixel in *screen* space, so it stays a pixel wide
// whatever the item is scaled to - see kStrokeFringePx, which both this and
// the mesh cache take it from.
//
// The scale is baked into the geometry and the offset is not: the mesh comes
// out around the origin and is translated as its vertices are written. That
// is what makes a mesh worth keeping between frames - see StrokeMeshSlot.
void DrawStroke(ImDrawList* drawList, const Stroke& stroke, StrokeRenderMode rendering, float offsetX,
                 float offsetY, float scaleX, float scaleY, float opacity, StrokeMeshSlot meshSlot) {
    if (stroke.points.empty() || opacity <= 0.0f) {
        return;
    }
    const float scaleAvg = (std::abs(scaleX) + std::abs(scaleY)) / 2.0f;
    const float halfWidth = stroke.width * scaleAvg * 0.5f;
    const ImU32 color = ToImColor(stroke.colorRGBA, opacity);

    if (rendering == StrokeRenderMode::Polyline) {
        // Reused between calls rather than allocated per stroke - and,
        // unlike the tessellated path below, positioned rather than merely
        // scaled: AddPolyline takes screen coordinates and there is nothing
        // here to translate afterwards.
        static std::vector<StrokePoint> screenPoints;
        screenPoints.clear();
        screenPoints.reserve(stroke.points.size());
        for (const StrokePoint& p : stroke.points) {
            screenPoints.push_back(StrokePoint{offsetX + p.x * scaleX, offsetY + p.y * scaleY});
        }
        // Kept switchable to compare the tessellator against - see
        // StrokeRenderMode::Polyline. ImGui's AddPolyline
        // leaves flat ends, so a disc goes on each one: the caps are not
        // what is being compared, and without them the two renderers differ
        // in an obvious way that has nothing to do with the tessellation.
        //
        // Those discs do overlap the line they cap, which a translucent
        // stroke shows as a darker blob at each end - a fair part of what
        // the tessellated path exists to avoid, and the reason it can't
        // simply be done this way.
        if (screenPoints.size() >= 2) {
            static std::vector<ImVec2> polyline;
            polyline.clear();
            polyline.reserve(screenPoints.size());
            for (const StrokePoint& p : screenPoints) {
                polyline.push_back(ImVec2(p.x, p.y));
            }
            drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), color,
                                   stroke.width * scaleAvg);
        }
        drawList->AddCircleFilled(ImVec2(screenPoints.front().x, screenPoints.front().y), halfWidth, color);
        drawList->AddCircleFilled(ImVec2(screenPoints.back().x, screenPoints.back().y), halfWidth, color);
        return;
    }

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

    drawList->PrimReserve(static_cast<int>(mesh->indices.size()), static_cast<int>(mesh->vertices.size()));
    // After PrimReserve, which is what may start a fresh draw command (and
    // reset this) when a mesh crosses the 16-bit index ceiling.
    const unsigned int base = drawList->_VtxCurrentIdx;
    for (const StrokeVertex& v : mesh->vertices) {
        drawList->PrimWriteVtx(ImVec2(offsetX + v.x, offsetY + v.y), uv, v.coverage >= 1.0f ? color : transparent);
    }
    for (const uint32_t index : mesh->indices) {
        drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
    }
}

// An item's fill (its captured image/placeholder gradient/plain color
// fill, at backgroundOpacity) plus its baked strokes (at
// foregroundOpacity), into `pMin..pMax` - the shared core of both
// RenderItems' per-item interactive window, RenderViewOnly's flat
// read-only pass, and the dock's own thumbnail chips (see RenderDock).
// Caller owns clipping (PushClipRect/PopClipRect) around this.
void DrawPicture(ImDrawList* drawList, uint64_t texture, ImVec2 pMin, ImVec2 pMax, ImU32 tint,
                  ImageSampling sampling) {
    const bool filtered = sampling.apply != nullptr && sampling.filter != platform::ImageFilter::Bilinear;
    if (filtered) {
        drawList->AddCallback(sampling.apply, reinterpret_cast<void*>(static_cast<intptr_t>(sampling.filter)));
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
                        ImageSampling sampling) {
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
        DrawPicture(drawList, texture, pMin, pMax, ToImColor(picture.tintColorRGBA, picture.opacity), sampling);
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

void DrawItemContent(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, StrokeRenderMode rendering,
                      uint64_t pictureTexture, uint64_t strokeRasterTexture, bool skipNoteText,
                      StrokeMeshSlot meshCache, ImageSampling sampling) {
    DrawSnippetPicture(drawList, item.picture, pMin, pMax, pictureTexture, sampling);

    if (rendering == StrokeRenderMode::Rasterized && strokeRasterTexture != 0) {
        // Every stroke, already drawn into one bitmap and composited here
        // as a single image - which is what makes a stroke that crosses
        // over itself one even color instead of darker at the crossing.
        // The opacity is applied once, to the finished picture, rather than
        // per stroke.
        DrawPicture(drawList, strokeRasterTexture, pMin, pMax, ToImColor(0xFFFFFFFFu, item.foregroundOpacity),
                    sampling);
    } else {
        const float scaleX = item.nativeW != 0.0f ? (pMax.x - pMin.x) / item.nativeW : 1.0f;
        const float scaleY = item.nativeH != 0.0f ? (pMax.y - pMin.y) / item.nativeH : 1.0f;
        // Rasterized with no raster to draw falls back to Tessellated
        // rather than to nothing - see this function's own declaration.
        const StrokeRenderMode perStroke =
            rendering == StrokeRenderMode::Rasterized ? StrokeRenderMode::Tessellated : rendering;
        for (size_t index = 0; index < item.strokes.size(); ++index) {
            DrawStroke(drawList, item.strokes[index], perStroke, pMin.x, pMin.y, scaleX, scaleY,
                        item.foregroundOpacity, meshCache.For(item.id, index));
        }
    }

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
// the same frame - see OverlayApp::previewMeshCache_.
void DrawCanvasPreview(ImDrawList* drawList, const Canvas& canvas, ImVec2 thumbMin, ImVec2 thumbMax, float displayW,
                        float displayH, StrokeRenderMode rendering, bool showStrokes,
                        const PreviewTextureFn& previewTexture,
                        StrokeMeshSlot meshCache, ImageSampling sampling) {
    drawList->PushClipRect(thumbMin, thumbMax, true);
    drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(14, 16, 20, 255));

    if (displayW > 0.0f && displayH > 0.0f) {
        const float scale = std::min((thumbMax.x - thumbMin.x) / displayW, (thumbMax.y - thumbMin.y) / displayH);
        const float offsetX = thumbMin.x + ((thumbMax.x - thumbMin.x) - displayW * scale) * 0.5f;
        const float offsetY = thumbMin.y + ((thumbMax.y - thumbMin.y) - displayH * scale) * 0.5f;

        for (const Item& item : canvas.items) {
            // Deleted on its own: not part of what the canvas shows, and not
            // what restoring a deleted canvas brings back either.
            if (item.deletedAt != 0) {
                continue;
            }
            const ImVec2 pMin(offsetX + item.rect.x * scale, offsetY + item.rect.y * scale);
            const ImVec2 pMax(offsetX + (item.rect.x + item.rect.w) * scale, offsetY + (item.rect.y + item.rect.h) * scale);
            DrawItemPreview(drawList, item, pMin, pMax, rendering, showStrokes, previewTexture, meshCache, sampling);
        }
    }

    drawList->PopClipRect();
}

void DrawItemPreview(ImDrawList* drawList, const Item& item, ImVec2 pMin, ImVec2 pMax, StrokeRenderMode rendering,
                     bool showStrokes,
                     const PreviewTextureFn& previewTexture,
                     StrokeMeshSlot meshCache, ImageSampling sampling) {
    // The picture with whichever texture it can have here: the real one for
    // the current canvas (already loaded), a thumbnail-sized copy for the
    // rest if previews are on, and none at all otherwise - in which case
    // DrawSnippetPicture falls back to the same placeholder gradient or
    // plain fill it uses anywhere else.
    bool drewAnything = false;
    if (item.picture.opacity > 0.0f) {
        // Nothing at all for a picture whose pixels are still being read
        // (see OverlayApp::PicturePreviewTexture, which says so by
        // returning nothing rather than 0). The placeholder gradient means "there is no image
        // here", and a few frames of it in front of an image that *is*
        // there and is on its way reads as the thumbnails being wrong and
        // then correcting themselves. An outlined empty box - what the
        // fall-through below draws - says the same thing quietly.
        const std::optional<uint64_t> texture =
            previewTexture ? previewTexture(item) : std::optional<uint64_t>(0);
        if (texture.has_value()) {
            DrawSnippetPicture(drawList, item.picture, pMin, pMax, *texture, sampling);
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
    // item's native size. pMin is already the item's origin in the preview,
    // so it doubles as DrawStroke's offset.
    const float boxW = pMax.x - pMin.x;
    const float boxH = pMax.y - pMin.y;
    const float strokeScaleX = item.nativeW != 0.0f ? boxW / item.nativeW : (item.rect.w != 0.0f ? boxW / item.rect.w : 1.0f);
    const float strokeScaleY = item.nativeH != 0.0f ? boxH / item.nativeH : (item.rect.h != 0.0f ? boxH / item.rect.h : 1.0f);
    // Rasterized has no bitmap to draw here - a preview keeps no cache of its
    // own, and building one for a thumbnail would cost more than the
    // difference could possibly show at this size.
    const StrokeRenderMode previewMode =
        rendering == StrokeRenderMode::Rasterized ? StrokeRenderMode::Tessellated : rendering;
    for (size_t index = 0; index < item.strokes.size(); ++index) {
        DrawStroke(drawList, item.strokes[index], previewMode, pMin.x, pMin.y, strokeScaleX, strokeScaleY,
                   item.foregroundOpacity, meshCache.For(item.id, index));
    }
}

}  // namespace sz::ui
