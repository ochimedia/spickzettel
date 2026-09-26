#pragma once

// How a snippet is painted, wherever it is: on the canvas, in the view-only
// layer, as a dock chip, and in a canvas's preview on the Overview's grid
// and the canvas bar. Draw-list calls only - no windows, no widgets, and
// nothing about which snippet is selected or hovered.

#include <cstdint>
#include <functional>
#include <optional>

#include <imgui.h>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "core/drawing/stroke.h"
#include "core/drawing/stroke_mesh_cache.h"
#include "core/drawing/stroke_render_mode.h"
#include "platform/i_overlay_window.h"

namespace sz::ui {

// How a snippet's pictures are resampled: the setting (AppConfig::
// imageFilter) and the renderer's callback that applies it, which is null
// where nothing renders. See DrawPicture.
struct ImageSampling {
    platform::ImageFilter filter = platform::ImageFilter::Bilinear;
    platform::DrawCallback apply = nullptr;
};

// Draws one stroke, transformed from its own native/stroke space into
// screen space via (offsetX/Y, scaleX/Y). The scale reaches the tessellator;
// the offset is applied to the finished vertices, which is what lets a
// cached mesh outlive its item being moved - see StrokeMeshSlot. Which
// renderer draws it is StrokeRenderMode, the setting's own enum.
void DrawStroke(ImDrawList* drawList, const core::Stroke& stroke, core::StrokeRenderMode rendering, float offsetX,
                float offsetY, float scaleX, float scaleY, float opacity = 1.0f, core::StrokeMeshSlot meshSlot = {});

// `texture` stretched to fill `pMin..pMax`, tinted by `tint`, resampled
// the way `sampling` says - every snippet picture is drawn through here.
// Bilinear is ImGui's own sampler and adds nothing to the draw list; any
// other filter is the picture between two callbacks, the renderer's and
// ImGui's reset.
void DrawPicture(ImDrawList* drawList, uint64_t texture, ImVec2 pMin, ImVec2 pMax, ImU32 tint, ImageSampling sampling);

// A snippet's picture stretched to fill `pMin..pMax`, at its own opacity:
// its pixels from `texture`, or, for 0, its placeholder gradient or its
// plain fill (see picture.h). Nothing is drawn at zero opacity. `texture`
// is the full-size picture's or, in the Overview's previews, a
// thumbnail-sized copy of it. `sampling` is how the pixels, if there are
// some, are resampled - see ImageSampling.
void DrawSnippetPicture(ImDrawList* drawList, const core::Picture& picture, ImVec2 pMin, ImVec2 pMax,
                        uint64_t texture, ImageSampling sampling = {});

// An item's picture, from `pictureTexture` (see DrawSnippetPicture), plus
// its baked strokes and its text, into `pMin..pMax` - shared by the items
// layer, the view-only layer and the dock's chips. Caller owns clipping
// (PushClipRect/PopClipRect) around this. `skipNoteText`, true only for
// the note being typed into, skips just the read-only text so it doesn't
// double up with the editor's own on top of it.
// `strokeRasterTexture` is only consulted in StrokeRenderMode::Rasterized:
// it's the item's strokes already drawn into a bitmap, composited in one
// AddImage instead of stroke by stroke. 0 means there isn't one - a
// preview that keeps no cache, or an item whose raster hasn't been built
// yet - and the strokes are drawn tessellated instead, which is the right
// thing to fall back to rather than nothing.
// `meshCache` is passed straight down to each stroke - see StrokeMeshSlot.
void DrawItemContent(ImDrawList* drawList, const core::Item& item, ImVec2 pMin, ImVec2 pMax,
                     core::StrokeRenderMode rendering, uint64_t pictureTexture, uint64_t strokeRasterTexture = 0,
                     bool skipNoteText = false, core::StrokeMeshSlot meshCache = {}, ImageSampling sampling = {});

// A picture's thumbnail-sized pixels, for a preview: a handle to draw with,
// 0 for a picture with none to show, and nothing at all for one whose turn
// to be read hasn't come yet.
using PreviewTextureFn = std::function<std::optional<uint64_t>(const core::Item&)>;

// A scaled-down snapshot of `canvas`'s items in `thumbMin..thumbMax`, the
// way the display shows them - uniform scale, letterboxed. What the
// Overview's tiles and the canvas bar both draw. `previewTexture` supplies
// each picture's thumbnail-sized pixels (empty for none), and `meshCache`
// must not be the canvas's own: a tile draws the same items the canvas
// behind it does, at a wholly different scale, in the same frame. A snippet
// deleted on its own is left out: a preview is of what the canvas holds,
// which for a deleted canvas is also what restoring it brings back.
void DrawCanvasPreview(ImDrawList* drawList, const core::Canvas& canvas, ImVec2 thumbMin, ImVec2 thumbMax,
                       float displayW, float displayH, core::StrokeRenderMode rendering, bool showStrokes,
                       const PreviewTextureFn& previewTexture, core::StrokeMeshSlot meshCache,
                       ImageSampling sampling);
// One item as a preview draws it into `pMin..pMax`: its picture with the
// texture `previewTexture` has for it, then its strokes scaled from the
// item's native size to the box. What DrawCanvasPreview draws per item.
void DrawItemPreview(ImDrawList* drawList, const core::Item& item, ImVec2 pMin, ImVec2 pMax,
                     core::StrokeRenderMode rendering, bool showStrokes, const PreviewTextureFn& previewTexture,
                     core::StrokeMeshSlot meshCache, ImageSampling sampling);

}  // namespace sz::ui
