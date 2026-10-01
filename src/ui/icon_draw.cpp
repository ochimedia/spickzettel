#include "ui/icon_draw.h"

namespace sz::ui {

namespace {
constexpr float kIconSpace = 24.0f;  // matches assets/icons/*.svg's own viewBox
}

void DropRepeatedPathPoints(ImVector<ImVec2>& path) {
    // ImGui's anti-aliased stroker offsets each point along the average of
    // the two segment normals meeting there, then divides that average by
    // its own squared length (IM_FIXNORMAL2F) - the standard mitre, which
    // widens a corner exactly enough to keep the outer edges meeting.
    //
    // A zero-length segment has no direction, so IM_NORMALIZE2F_OVER_ZERO
    // leaves its normal at (0,0), and averaging that with the real normal
    // next to it halves the vector - which the mitre step then reads as a
    // very sharp corner and doubles the stroke to compensate. The result is
    // a lump twice the width of the line, sticking out sideways at a place
    // where the path does not actually turn at all.
    //
    // Icons produce those points honestly: DrawIcon emits a MoveTo, and
    // PathArcTo then emits its own start point at the same place (see the
    // ArcTo case below), so any subpath that opens with an arc carries the
    // same point twice. Dropping them here rather than at each call site
    // covers the other ways it can happen too - two arcs meeting exactly,
    // a segment the source SVG authored with zero length.
    if (path.Size < 2) {
        return;
    }
    // Well under a pixel, so this can only ever remove a segment too short
    // to have painted anything, and far above the rounding in the tape's
    // own four-decimal angles.
    constexpr float kSamePointEpsilonSq = 0.01f * 0.01f;
    int kept = 1;
    for (int i = 1; i < path.Size; ++i) {
        const float dx = path[i].x - path[kept - 1].x;
        const float dy = path[i].y - path[kept - 1].y;
        if (dx * dx + dy * dy > kSamePointEpsilonSq) {
            path[kept++] = path[i];
        }
    }
    path.resize(kept);
}

void DrawIcon(ImDrawList* drawList, const Icon& icon, ImVec2 pos, float size, ImU32 color, float thickness) {
    const float scale = size / kIconSpace;
    const float scaledThickness = thickness * scale;
    const auto Pt = [&](float x, float y) { return ImVec2(pos.x + x * scale, pos.y + y * scale); };

    // Defensive, not load-bearing under normal use: every subpath this
    // function itself opens is closed via EndSubpath before returning, so
    // there's nothing of ours left in the path buffer between calls -
    // this only guards against unrelated code elsewhere having left a
    // stray PathLineTo/etc. uncommitted on the same drawlist.
    drawList->PathClear();

    bool pathOpen = false;
    for (int i = 0; i < icon.count; ++i) {
        const IconCmd& cmd = icon.cmds[i];
        switch (cmd.op) {
            case IconOp::MoveTo:
                drawList->PathLineTo(Pt(cmd.a, cmd.b));
                pathOpen = true;
                break;
            case IconOp::LineTo:
                drawList->PathLineTo(Pt(cmd.a, cmd.b));
                break;
            case IconOp::CubicTo:
                drawList->PathBezierCubicCurveTo(Pt(cmd.a, cmd.b), Pt(cmd.c, cmd.d), Pt(cmd.e, cmd.f));
                break;
            case IconOp::ArcTo:
                // cmd.c is a 24-space radius, so it needs the same `scale`
                // as every point - Pt() only applies to point pairs, hence
                // scaling it separately here.
                //
                // This appends the arc's own start point (exactly, or as
                // the nearest entry in ImGui's 48-sample table when that
                // lands on it), which is the point the MoveTo before it
                // already put there - see DropRepeatedPathPoints, which is
                // what stops the two of them from becoming a lump.
                drawList->PathArcTo(Pt(cmd.a, cmd.b), cmd.c * scale, cmd.d, cmd.e);
                break;
            case IconOp::EndSubpath:
                if (pathOpen) {
                    DropRepeatedPathPoints(drawList->_Path);
                    drawList->PathStroke(color, scaledThickness);
                    pathOpen = false;
                }
                break;
            case IconOp::Circle:
                // A stroked ring only reads as a ring once its inner edge
                // (radius - thickness/2) is actually open - once the
                // stroke is as wide as the circle's own diameter, SVG
                // renders no visible hole at all, just a solid dot (the
                // source SVGs' own three-dot "more vertical" icon relies
                // on exactly this: r="1" stroke-width="2"). AddCircle
                // draws a true ring instead, which at r=1 reads as no icon
                // at all - so match the SVG's own rendering once the
                // stroke would engulf the disk.
                // Compared unscaled (cmd.c/thickness, not the *scale'd
                // pixel values) since the ratio between them - which one
                // this icon set actually authors - is scale-invariant.
                if (thickness >= 2.0f * cmd.c) {
                    drawList->AddCircleFilled(Pt(cmd.a, cmd.b), cmd.c * scale, color);
                } else {
                    drawList->AddCircle(Pt(cmd.a, cmd.b), cmd.c * scale, color, 0, scaledThickness);
                }
                break;
            case IconOp::RoundedRect:
                // AddRect's parameter order is (rounding, thickness, flags);
                // a flag in the thickness slot compiles and draws garbage.
                drawList->AddRect(Pt(cmd.a, cmd.b), Pt(cmd.c, cmd.d), color, cmd.e * scale, scaledThickness,
                                   ImDrawFlags_None);
                break;
        }
    }
}

}  // namespace sz::ui
