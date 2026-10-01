#include "ui/view/pointer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "core/config/settings_catalog.h"
#include "generated/ui_strings.h"
#include "platform/pen_glyph.h"
#include "ui/icons_generated.h"
#include "ui/interaction/gestures.h"
#include "ui/interaction/levels.h"
#include "ui/theme.h"

#include <imgui.h>

namespace sz::ui {

using namespace ::sz::core;

namespace {
// How long the mouse wheel's own size preview stays up after the last
// step, and how much of that tail it spends fading. Long enough to read
// the number and judge the dot without turning into something that sits
// on screen after you've moved on - see RenderBrushSizePreview.
constexpr double kSizePreviewHoldSeconds = 1.1;
constexpr double kSizePreviewFadeSeconds = 0.35;
}  // namespace

Pointer::Pointer(Settings& settings, Editor& editor, ViewHost& host)
    : settings_(settings), editor_(editor), host_(host) {}

bool Pointer::PanelOpen() const { return editor_.Input().At(Level::Panel) != nullptr; }

void Pointer::DrawOverCanvas() {
    RenderRegionCaptureOverlay();
    RenderRectEraserOverlay();
    RenderBrushSizePreview();
    RenderToolModifierBadge();
}

void Pointer::Draw() {
    ApplyPointerShape();
    DrawSoftwareCursor();
}

void Pointer::Apply() {
    // The preview gone, the width the wheel settled on is kept - once, not
    // per notch.
    if (ImGui::GetTime() >= sizePreviewExpireAtSeconds_) {
        KeepPenWidth();
    }
}

void Pointer::ToolSized(bool pen) {
    if (pen) {
        drawWidthDirty_ = true;
    }
    sizePreviewExpireAtSeconds_ = ImGui::GetTime() + kSizePreviewHoldSeconds;
}

// Which shape the OS cursor should wear, decided here at the end of the
// frame rather than at the start - because ImGui only knows what it wants
// once every widget has run, and this must not overrule it. A resize
// handle's directional arrow, the dock's hand and the note editor's I-beam
// are all ImGui's to install; asking the platform for a crosshair on top of
// them is what made them flash and vanish.
//
// Asks for Default while the software pointer is drawn rather than worn by
// the OS: the OS cursor is hidden then, so this would be shaping something
// invisible.
void Pointer::ApplyPointerShape() {
    if (!host_.Window()) {
        return;
    }
    const ImGuiMouseCursor imguiCursor = ImGui::GetMouseCursor();
    const bool imguiOwnsIt = imguiCursor != ImGuiMouseCursor_Arrow;
    const platform::CursorShape wanted =
        (imguiOwnsIt || settings_.Live().InputOptions().SoftwarePointerDrawn()) ? platform::CursorShape::Default
                                                               : WantedPointerShape();

    // Re-asserted when either half of the decision moves, and not otherwise.
    //
    // Pushing it every frame was the previous version, and the note here
    // said it "costs one call that does nothing on the shape it is already
    // wearing". Measured, it cost 82 us of CPU per frame - a quarter of the
    // whole frame on an idle overlay - because SetCursorShape answers "is
    // the cursor still over one of my windows" with WindowFromPoint, a
    // system-wide hit test, and then installs the shape again.
    //
    // What "moved" has to mean is the whole of the difficulty, and getting it
    // wrong is not subtle: it puts the arrow back over the canvas and leaves
    // it there.
    //
    // ImGui's backend installs its own cursor from NewFrame when ImGui's
    // wanted shape differs from the one it installed last - and it reads that
    // shape *before* NewFrame resets it, so its install lands one frame after
    // the change. Watching `ImGui::GetMouseCursor()` for a change is
    // therefore a frame too early: on the frame the backend actually takes
    // the cursor away, ImGui's own answer has already been Arrow since the
    // previous frame, and a key built on it says nothing changed. That is
    // exactly how the pen stopped coming back after a resize handle. So the
    // re-assert has to cover the frame after a change as well as the frame of
    // one - hence two frames of history rather than one.
    //
    // A moved pointer re-asserts too. It costs nothing where it matters (an
    // overlay nobody is touching is the case being optimized, and there the
    // pointer is still by definition), and it covers everything that can only
    // happen while the pointer moves - including SetCursorShape declining to
    // act because its own hit test said the cursor had left our window, which
    // would otherwise be recorded as applied and never retried.
    const ImVec2 pointer = ImGui::GetMousePos();
    const bool pointerMoved = pointer.x != lastPointerX_ || pointer.y != lastPointerY_;
    const bool imguiTouchedTheCursor =
        imguiCursor != lastImGuiCursor_ || lastImGuiCursor_ != previousImGuiCursor_;
    previousImGuiCursor_ = lastImGuiCursor_;
    lastImGuiCursor_ = imguiCursor;
    lastPointerX_ = pointer.x;
    lastPointerY_ = pointer.y;

    if (!pointerMoved && !imguiTouchedTheCursor && appliedPointerShape_ == wanted) {
        return;
    }
    appliedPointerShape_ = wanted;
    host_.Window()->SetCursorShape(wanted);
}

// Draws the pointer at ImGui's mouse position, whatever put it there - which
// is what makes this work in both configurations. Under a mouse grab that
// position is the grab's own accumulated one, so by construction the drawn
// pointer can't drift from what a click actually hits; with only
// useSoftwarePointer on, it's the real cursor's position and this is purely a
// change of appearance. Either way the OS cursor is hidden over this window
// (see Win32Dx11Renderer::NewFrame) so there is exactly one pointer visible.
//
// Gated on the settings rather than on the grab really running, which core
// has no way to ask: a backend without an input grab (the Linux dev harness)
// would otherwise draw a second, redundant pointer.
// The one place that decides what the pointer means, shared by both of them
// so they cannot disagree.
//
// An arrow unless something more specific applies - not a crosshair
// whenever a drawing tool is in hand, which put a crosshair over empty
// desktop where nothing would be drawn.

platform::CursorShape Pointer::WantedPointerShape() const {
    // Everything below is about what the pointer is *over*. Over the
    // overview, a popover or the selection's handles and bar, ImGui owns the
    // pointer and this must not argue with it - the arrow here is only
    // what's left when ImGui wants nothing more specific, which
    // ApplyPointerShape has already checked before this answer is used at
    // all.
    if (ImGui::GetIO().WantCaptureMouse || PanelOpen()) {
        return platform::CursorShape::Arrow;
    }
    // Placing a snippet: the click puts a corner somewhere exact. Not over
    // a panel, which the click is for instead: asked first, a menu or the
    // Overview wore the crosshair.
    if (editor_.ArmedCreation().has_value()) {
        return platform::CursorShape::Crosshair;
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    const PointerTarget target = editor_.ResolvePointerTarget(mouse.x, mouse.y);
    if (target.kind != PointerTarget::Kind::Body) {
        // Open canvas, a handle or the bar: nothing here for the tool to
        // mark. A handle has already asked ImGui for its own shape
        // (RenderItems), which ApplyPointerShape lets win; a button gets
        // the arrow, as a button does.
        return platform::CursorShape::Arrow;
    }
    // A marking tool marks only the snippets in drawing mode, and not with
    // Alt held, when the press picks the snippet up instead.
    if (!editor_.IsDrawingOn(target.item) || editor_.PressPicksUp()) {
        return platform::CursorShape::Arrow;
    }
    switch (editor_.ActiveTool()) {
        case Tool::Draw:
            return platform::CursorShape::Pen;
        case Tool::Erase:
            // No eraser glyph in either cursor set, and the exact point is
            // what matters - the brush-size ring already says how much will
            // come away.
            return platform::CursorShape::Crosshair;
        case Tool::Text:
        case Tool::Select:
        case Tool::NewDrawing:
        case Tool::NewScreenshot:
            break;
    }
    return platform::CursorShape::Arrow;
}

void Pointer::RenderToolModifierBadge() {
    if (PanelOpen() || editor_.ArmedCreation().has_value() ||
        (editor_.ActiveTool() != Tool::Draw && editor_.ActiveTool() != Tool::Erase)) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const Marking* stroke = editor_.Input().As<Marking>(Level::Gesture);
    const bool dragging = stroke != nullptr;
    if (!dragging) {
        // Only where a press would make one: over a snippet in drawing
        // mode, not a panel, and not with Alt held.
        const PointerTarget target = editor_.ResolvePointerTarget(io.MousePos.x, io.MousePos.y);
        if (io.WantCaptureMouse || target.kind != PointerTarget::Kind::Body || !editor_.IsDrawingOn(target.item) ||
            editor_.PressPicksUp()) {
            return;
        }
    }
    // Mid-drag, what the gesture is making; before one, what a press would
    // make now - the modifiers held, or the shape picked from the tool's
    // menu.
    const Icon* icon = nullptr;
    if (editor_.ActiveTool() == Tool::Draw) {
        const DrawShape shape = dragging ? stroke->Shape() : editor_.ShapeForPress();
        if (dragging && stroke->GetKind() != Marking::Kind::Shape) {
            return;  // freehand, which needs no saying
        }
        icon = shape == DrawShape::Rectangle ? &icons::kRectangle
               : shape == DrawShape::Line    ? &icons::kLine
                                             : nullptr;
    } else if (dragging ? stroke->GetKind() == Marking::Kind::EraseRect
                        : editor_.ShapeForPress() == DrawShape::Rectangle) {
        icon = &icons::kEraserRect;
    }
    if (icon == nullptr) {
        return;
    }
    // Down and to the right of the hotspot, clear of the pen glyph and the
    // brush-size dot, on a disc of the panel color so it reads over any
    // snippet.
    const float offset = Px(18.0f);
    const float size = Px(22.0f);
    const float inset = Px(4.0f);
    const ImVec2 min(io.MousePos.x + offset, io.MousePos.y + offset);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(min, ImVec2(min.x + size, min.y + size), ImGui::ColorConvertFloat4ToU32(theme::kPanelBg),
                      Px(theme::kRadiusSm));
    DrawIcon(dl, *icon, ImVec2(min.x + inset, min.y + inset), size - inset * 2.0f,
             ImGui::ColorConvertFloat4ToU32(theme::kWhite), 1.5f);
}

void Pointer::DrawSoftwareCursor() const {
    if (!settings_.Live().InputOptions().SoftwarePointerDrawn()) {
        return;
    }
    // Whatever ImGui asked for, if it asked for anything: its atlas already
    // carries the resize arrows, the hand, the I-beam and NotAllowed, and
    // io.MouseDrawCursor makes it draw them at the same position this
    // pointer would be. Drawing over them instead is what made a resize
    // handle's arrow flash and turn back into a crosshair.
    if (ImGui::GetMouseCursor() != ImGuiMouseCursor_Arrow) {
        return;
    }

    // Nothing more specific, so this pointer is the one being drawn. Stops
    // ImGui drawing its own on top: the backend's io.MouseDrawCursor - the
    // flag that reliably hides the OS cursor - also asks ImGui to render a
    // pointer from its font atlas, and ImGui::EndFrame skips that only
    // while the wanted cursor is None. Set here, after every widget has had
    // its say for this frame, so nothing reinstates a shape afterwards.
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    // Whole pixels, every frame - the same integer position ImGui hit-tests
    // against, so the pointer is drawn exactly where clicks land, and crisp.
    // The grab keeps the pointer's position as a float internally (that is
    // what keeps slow movement from vanishing under a pixel) and floors it
    // once; on identical input its integer steps match the OS cursor's, so
    // there is nothing left for fractional drawing to smooth over. It was
    // tried, and it traded a crisp pointer for a soft one to hide a stepping
    // problem that turned out to be a wrong speed multiplier.
    const ImVec2 at = ImGui::GetMousePos();
    if (at.x < 0.0f || at.y < 0.0f) {
        return;
    }

    constexpr ImU32 kFill = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kEdge = IM_COL32(20, 24, 32, 235);
    constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 70);
    // At Windows' scale for the display rather than the interface scale:
    // this stands in for the system pointer, which Windows draws larger on
    // a scaled display whatever this app's own setting says.
    const float scale = host_.Window() != nullptr ? static_cast<float>(host_.Window()->ScalePercent()) / 100.0f : 1.0f;

    const platform::CursorShape shape = WantedPointerShape();

    if (shape == platform::CursorShape::Pen) {
        // A pen, for a snippet a drawing tool would mark - the shared
        // outline (see platform::pen_glyph), placed with its nib at the
        // pointer's own position so it points at the pixel it will draw
        // on. Same two-pass outline and fill as the arrow below, and the
        // same reason: it has to stay legible over a bright game and a
        // dark one alike.
        const auto place = [&at, scale](platform::Vec2 p) { return ImVec2(at.x + p.x * scale, at.y + p.y * scale); };
        ImVec2 nibShape[std::size(platform::pen_glyph::kNib)];
        ImVec2 body[std::size(platform::pen_glyph::kBody)];
        ImVec2 shadow[std::size(platform::pen_glyph::kBody)];
        for (size_t i = 0; i < std::size(nibShape); ++i) {
            nibShape[i] = place(platform::pen_glyph::kNib[i]);
        }
        for (size_t i = 0; i < std::size(body); ++i) {
            body[i] = place(platform::pen_glyph::kBody[i]);
            shadow[i] = ImVec2(body[i].x + 1.5f * scale, body[i].y + 1.5f * scale);
        }
        const float outline = platform::pen_glyph::kOutlineWidth * scale;
        drawList->AddConvexPolyFilled(shadow, static_cast<int>(std::size(shadow)), kShadow);
        drawList->AddConvexPolyFilled(body, static_cast<int>(std::size(body)), kFill);
        drawList->AddConvexPolyFilled(nibShape, static_cast<int>(std::size(nibShape)), kFill);
        drawList->AddPolyline(body, static_cast<int>(std::size(body)), kEdge, outline, ImDrawFlags_Closed);
        drawList->AddPolyline(nibShape, static_cast<int>(std::size(nibShape)), kEdge, outline, ImDrawFlags_Closed);
        return;
    }

    if (shape == platform::CursorShape::Crosshair) {
        // A crosshair for the tools where the exact point matters and an
        // arrow's body would sit on top of it. The gap in the middle leaves
        // the target pixel itself visible.
        const float arm = 11.0f * scale;
        const float gap = 3.0f * scale;
        const ImVec2 spans[4][2] = {
            {ImVec2(at.x - arm, at.y), ImVec2(at.x - gap, at.y)},
            {ImVec2(at.x + gap, at.y), ImVec2(at.x + arm, at.y)},
            {ImVec2(at.x, at.y - arm), ImVec2(at.x, at.y - gap)},
            {ImVec2(at.x, at.y + gap), ImVec2(at.x, at.y + arm)},
        };
        for (const auto& span : spans) {
            drawList->AddLine(span[0], span[1], kEdge, 3.0f * scale);
            drawList->AddLine(span[0], span[1], kFill, scale);
        }
        return;
    }

    // The ordinary arrow, hotspot at its tip so it points at what it is
    // over rather than near it. Drawn white with a dark outline (and a
    // slight shadow) so it stays legible over a bright game and a dark one
    // alike - the same reason the OS cursor is shaped this way.
    constexpr ImVec2 kArrow[7] = {
        ImVec2(0.0f, 0.0f),   ImVec2(0.0f, 17.0f),  ImVec2(4.2f, 12.8f),  ImVec2(7.0f, 18.6f),
        ImVec2(10.0f, 17.2f), ImVec2(7.2f, 11.6f),  ImVec2(12.2f, 11.6f),
    };
    ImVec2 arrow[7];
    ImVec2 shadow[7];
    for (int i = 0; i < 7; ++i) {
        arrow[i] = ImVec2(at.x + kArrow[i].x * scale, at.y + kArrow[i].y * scale);
        shadow[i] = ImVec2(arrow[i].x + 1.5f * scale, arrow[i].y + 1.5f * scale);
    }
    drawList->AddConvexPolyFilled(shadow, 7, kShadow);
    drawList->AddConvexPolyFilled(arrow, 7, kFill);
    drawList->AddPolyline(arrow, 7, kEdge, 1.4f * scale, ImDrawFlags_Closed);
}

// Feedback for the mouse wheel's size change, which otherwise altered the
// tool silently and left "how big is it now?" to be answered by drawing a
// test stroke and undoing it. Deliberately shows the *size itself* - a dot
// of exactly the diameter the tool will mark at, under the cursor - rather
// than only a number somewhere else on screen: the question is about a
// size, so the answer should be one. The number rides along for the cases
// where the dot alone is hard to judge (1px vs 2px).
void Pointer::RenderBrushSizePreview() {
    const double now = ImGui::GetTime();
    if (now >= sizePreviewExpireAtSeconds_) {
        return;  // and the width it showed is kept - see Apply
    }
    if (PanelOpen()) {
        return;
    }
    const std::optional<float> diameter = editor_.ActiveToolSizePx();
    if (!diameter.has_value()) {
        return;
    }
    // Holds at full strength, then fades over the last stretch rather than
    // blinking out - a hard disappearance at the end reads as a glitch.
    const float alpha =
        static_cast<float>(std::clamp((sizePreviewExpireAtSeconds_ - now) / kSizePreviewFadeSeconds, 0.0, 1.0));

    // Follows the live cursor rather than freezing where the wheel was
    // turned: adjusting the size and then moving to where you're about to
    // draw is one continuous motion, and a preview left behind at the old
    // spot would be answering the question in the wrong place.
    const ImVec2 center = ImGui::GetIO().MousePos;
    const float radius = *diameter * 0.5f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const auto fade = [alpha](ImU32 color) {
        const ImU32 a = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };

    if (editor_.ActiveTool() == Tool::Erase) {
        // Hollow, and in RenderRectEraserOverlay's own cool blue rather
        // than the warm placement tone: the eraser takes ink away, and a
        // filled dot would read as something about to be painted.
        dl->AddCircleFilled(center, radius, fade(IM_COL32(120, 170, 255, 40)));
        dl->AddCircle(center, radius, fade(IM_COL32(120, 170, 255, 255)), 0, 2.0f);
    } else {
        // The pen's actual color, so this previews the mark itself and not
        // just its footprint.
        dl->AddCircleFilled(center, radius, fade(ToImColor(editor_.DrawColorRGBA())));
        // A hairline at exactly `radius` (not outside it - the ring must
        // not make the dot look bigger than it is) keeps a dark color, or
        // a 1px width, findable against whatever is underneath.
        dl->AddCircle(center, radius, fade(IM_COL32(255, 255, 255, 200)), 0, 1.0f);
    }

    char label[16];
    std::snprintf(label, sizeof(label), strings::kFormatPixels, *diameter);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    constexpr float kLabelPadX = 7.0f;
    constexpr float kLabelPadY = 3.0f;
    constexpr float kLabelGap = 10.0f;
    // Clear of the dot at every size in both ranges (1..24 and 8..64), so
    // the two never overlap and the label doesn't jump sides.
    const ImVec2 boxMin(center.x + radius + Px(kLabelGap), center.y - textSize.y * 0.5f - Px(kLabelPadY));
    const ImVec2 boxMax(boxMin.x + textSize.x + Px(kLabelPadX) * 2.0f, boxMin.y + textSize.y + Px(kLabelPadY) * 2.0f);
    // Same pill as the toast (see Messages) - over arbitrary game content, plain
    // text has no guaranteed contrast to sit against.
    dl->AddRectFilled(boxMin, boxMax, fade(IM_COL32(18, 20, 26, 235)), theme::kRadiusPill);
    dl->AddText(ImVec2(boxMin.x + Px(kLabelPadX), boxMin.y + Px(kLabelPadY)), fade(IM_COL32(240, 242, 245, 255)),
                label);
}

void Pointer::KeepPenWidth() {
    if (!drawWidthDirty_) {
        return;
    }
    drawWidthDirty_ = false;
    if (settings_.Get(setting::kStrokeWidth) != editor_.DrawWidth()) {
        settings_.Set(setting::kStrokeWidth, editor_.DrawWidth());
    }
}

// ================= Drag previews =================

void Pointer::RenderRegionCaptureOverlay() {
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

void Pointer::RenderRectEraserOverlay() {
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
