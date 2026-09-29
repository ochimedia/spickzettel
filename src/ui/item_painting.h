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
#include "platform/i_overlay_window.h"

namespace sz::ui {

// What the renderer lends the painting: how pictures are resampled (the
// setting, AppConfig::imageFilter, and the callback that applies it), and
// the two callbacks strokes are drawn between (see DrawStroke and
// DrawItemContent). The callbacks are null where nothing renders, and the
// painting then does without them: pictures keep ImGui's own sampler,
// strokes are drawn untested and each at the snippet's opacity.
struct PaintHooks {
    platform::ImageFilter filter = platform::ImageFilter::Bilinear;
    // IOverlayWindow::ImageFilterCallback.
    platform::DrawCallback applyFilter = nullptr;
    // IOverlayWindow::StrokeDepthCallback.
    platform::DrawCallback strokeDepth = nullptr;
    // IOverlayWindow::StrokeLayerCallback.
    platform::DrawCallback strokeLayer = nullptr;
};

// Draws one stroke, transformed from its own native/stroke space into
// screen space via (offsetX/Y, scaleX/Y). The scale reaches the tessellator;
// the offset is applied to the finished vertices, which is what lets a
// cached mesh outlive its item being moved - see StrokeMeshSlot.
// Between two `strokeDepth` calls, so the stroke reaches each pixel once
// however it crosses itself - see IOverlayWindow::StrokeDepthCallback.
void DrawStroke(ImDrawList* drawList, const core::Stroke& stroke, float offsetX, float offsetY, float scaleX,
                float scaleY, float opacity = 1.0f, core::StrokeMeshSlot meshSlot = {},
                platform::DrawCallback strokeDepth = nullptr);

// `texture` stretched to fill `pMin..pMax`, tinted by `tint`, resampled
// the way `hooks` say - every snippet picture is drawn through here.
// Bilinear is ImGui's own sampler and adds nothing to the draw list; any
// other filter is the picture between two callbacks, the renderer's and
// ImGui's reset.
void DrawPicture(ImDrawList* drawList, uint64_t texture, ImVec2 pMin, ImVec2 pMax, ImU32 tint, PaintHooks hooks);

// A snippet's picture stretched to fill `pMin..pMax`, at its own opacity:
// its pixels from `texture`, or, for 0, its placeholder gradient or its
// plain fill (see picture.h). Nothing is drawn at zero opacity. `texture`
// is the full-size picture's or, in the Overview's previews, a
// thumbnail-sized copy of it. `hooks` say how the pixels, if there are
// some, are resampled.
void DrawSnippetPicture(ImDrawList* drawList, const core::Picture& picture, ImVec2 pMin, ImVec2 pMax,
                        uint64_t texture, PaintHooks hooks = {});

// An item's picture, from `pictureTexture` (see DrawSnippetPicture), plus
// its baked strokes and its text, into `pMin..pMax` - shared by the items
// layer, the view-only layer and the dock's chips. Caller owns clipping
// (PushClipRect/PopClipRect) around this; the clip rectangle is also the
// part of the stroke layer used (see below). `skipNoteText`, true only for
// the note being typed into, skips just the read-only text so it doesn't
// double up with the editor's own on top of it.
//
// The strokes are one layer: each blends over the ones before it by its own
// ink's alpha, and the snippet's opacity fades the finished layer once, so
// opaque strokes stay flat against each other however faded the snippet.
// Below full opacity that takes the renderer's stroke layer (see
// IOverlayWindow::StrokeLayerCallback); at full opacity, drawing straight
// onto the frame comes to the same, since source-over is associative.
//
// `meshCache` is passed straight down to each stroke - see StrokeMeshSlot.
// `moreStrokes`, when given, draws strokes that are not in the item yet -
// the one being drawn - after its own and into the same layer, at the
// opacity it is handed.
void DrawItemContent(ImDrawList* drawList, const core::Item& item, ImVec2 pMin, ImVec2 pMax, uint64_t pictureTexture,
                     bool skipNoteText = false, core::StrokeMeshSlot meshCache = {}, PaintHooks hooks = {},
                     const std::function<void(float opacity)>& moreStrokes = {});

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
// which for a deleted canvas is also what restoring it brings back. So is
// a minimized one, which the canvas shows only as a chip.
void DrawCanvasPreview(ImDrawList* drawList, const core::Canvas& canvas, ImVec2 thumbMin, ImVec2 thumbMax,
                       float displayW, float displayH, bool showStrokes, const PreviewTextureFn& previewTexture,
                       core::StrokeMeshSlot meshCache, PaintHooks hooks);
// One item as a preview draws it into `pMin..pMax`: its picture with the
// texture `previewTexture` has for it, then its strokes scaled from the
// item's native size to the box. What DrawCanvasPreview draws per item.
void DrawItemPreview(ImDrawList* drawList, const core::Item& item, ImVec2 pMin, ImVec2 pMax, bool showStrokes,
                     const PreviewTextureFn& previewTexture, core::StrokeMeshSlot meshCache, PaintHooks hooks);

}  // namespace sz::ui
