#include "ui/view/frame_graph.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>

#include "ui/theme.h"

namespace sz::ui {

using core::TimelineMark;

namespace {

constexpr double kWindowSeconds = 10.0;
// The top of the frame bars: three refreshes at 60 Hz. What is taller is
// cut off there and says its time instead - a scale that fit a three-second
// stall would draw every ordinary frame as a line along the bottom.
constexpr double kScaleMs = 50.0;

constexpr ImU32 kText = IM_COL32(150, 158, 172, 255);
constexpr ImU32 kFaint = IM_COL32(255, 255, 255, 22);
constexpr ImU32 kBuild = IM_COL32(226, 230, 238, 170);
constexpr ImU32 kHiddenShade = IM_COL32(255, 255, 255, 24);
constexpr ImU32 kIdle = IM_COL32(150, 158, 172, 160);

struct Lane {
    TimelineMark mark;
    const char* name;
    ImU32 color;
};
// In TimelineMark's order, which is the lanes' top to bottom.
constexpr Lane kLanes[] = {
    {TimelineMark::Commit, "commit", IM_COL32(90, 170, 255, 255)},
    {TimelineMark::Encode, "encode", IM_COL32(170, 130, 255, 255)},
    {TimelineMark::Checkpoint, "checkpoint", IM_COL32(255, 160, 60, 255)},
    {TimelineMark::Capture, "capture", IM_COL32(80, 210, 200, 255)},
    {TimelineMark::ReadPicture, "read picture", IM_COL32(230, 120, 200, 255)},
    {TimelineMark::ConfigWrite, "config", IM_COL32(220, 220, 120, 255)},
};
static_assert(std::size(kLanes) == core::kTimelineMarkCount, "a lane for every mark");

ImU32 WithAlpha(ImU32 color, int alpha) {
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

// Green within a refresh at 60 Hz and a bit, amber within two, red past.
ImU32 IntervalColor(double ms) {
    if (ms <= 20.0) {
        return IM_COL32(90, 214, 130, 255);
    }
    if (ms <= 36.0) {
        return IM_COL32(240, 190, 80, 255);
    }
    return IM_COL32(232, 100, 100, 255);
}

void FormatMs(char* out, size_t size, double seconds) {
    const double ms = seconds * 1000.0;
    std::snprintf(out, size, ms < 10.0 ? "%.1f ms" : "%.0f ms", ms);
}

void FormatBytes(char* out, size_t size, int64_t bytes) {
    if (bytes < (int64_t{1} << 20)) {
        std::snprintf(out, size, "%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(out, size, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
}

}  // namespace

void DrawFrameGraph(ImDrawList* drawList, float displayW, const core::Timeline& timeline, double now) {
    const float pad = Px(10.0f);
    const float lineH = ImGui::GetTextLineHeight();
    const float labelW = Px(92.0f);
    const float graphH = Px(96.0f);
    const float laneH = std::max(Px(17.0f), lineH + Px(2.0f));
    const float width = Px(500.0f);
    // Room under the header for the top label, which is centered on the
    // top of the bars.
    const float headerH = lineH * 1.5f + Px(4.0f);
    const float height = pad * 2.0f + headerH + graphH + Px(6.0f) +
                         laneH * static_cast<float>(core::kTimelineMarkCount);

    const ImVec2 panelMin(displayW - Px(14.0f) - width, Px(14.0f));
    const ImVec2 panelMax(panelMin.x + width, panelMin.y + height);
    const float graphLeft = panelMin.x + pad + labelW;
    const float graphRight = panelMax.x - pad;
    const float graphTop = panelMin.y + pad + headerH;
    const float graphBottom = graphTop + graphH;
    const float lanesTop = graphBottom + Px(6.0f);
    const float lanesBottom = lanesTop + laneH * static_cast<float>(core::kTimelineMarkCount);
    const double windowStart = now - kWindowSeconds;

    const auto xAt = [&](double t) {
        return graphRight - static_cast<float>((now - t) / kWindowSeconds) * (graphRight - graphLeft);
    };
    const auto yAt = [&](double seconds) {
        const double ms = std::min(seconds * 1000.0, kScaleMs);
        return graphBottom - static_cast<float>(ms / kScaleMs) * graphH;
    };

    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(12, 15, 20, 215), Px(6.0f));
    drawList->AddRect(panelMin, panelMax, IM_COL32(255, 255, 255, 40), Px(6.0f));

    // A line a second, through the frames and the lanes alike.
    for (int second = 1; second < static_cast<int>(kWindowSeconds); ++second) {
        const float x = xAt(now - second);
        drawList->AddLine(ImVec2(x, graphTop), ImVec2(x, lanesBottom), kFaint);
    }
    // A refresh at 60 Hz, and two.
    for (const double ms : {1000.0 / 60.0, 2000.0 / 60.0}) {
        const float y = yAt(ms / 1000.0);
        drawList->AddLine(ImVec2(graphLeft, y), ImVec2(graphRight, y), IM_COL32(255, 255, 255, 45));
        char label[16];
        std::snprintf(label, sizeof(label), "%.1f ms", ms);
        drawList->AddText(ImVec2(panelMin.x + pad, y - lineH * 0.5f), kText, label);
    }
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f+ ms", kScaleMs);
        drawList->AddText(ImVec2(panelMin.x + pad, graphTop - lineH * 0.5f), kText, label);
    }

    drawList->PushClipRect(ImVec2(graphLeft, graphTop - lineH), ImVec2(graphRight, lanesBottom), true);

    // ----- The frames -----
    const core::Timeline::Frames& frames = timeline.RecordedFrames();
    double lastInterval = -1.0;
    double worstInterval = 0.0;
    double worstBuild = 0.0;
    float labelEnd = -1.0e9f;
    for (size_t i = 0; i < frames.Size(); ++i) {
        const core::TimelineFrame& frame = frames[i];
        if (frame.start < windowStart) {
            continue;
        }
        const float x = xAt(frame.start);
        if (i > 0 && frame.afterBreak) {
            drawList->AddRectFilled(ImVec2(std::max(xAt(frames[i - 1].start), graphLeft), graphTop),
                                    ImVec2(x, lanesBottom), kHiddenShade);
        } else if (i > 0 && frames[i - 1].idleAfter) {
            // Up, at the idle pace: a line along the bottom, to tell it
            // from time hidden.
            drawList->AddLine(ImVec2(std::max(xAt(frames[i - 1].start), graphLeft), graphBottom - Px(1.0f)),
                              ImVec2(x, graphBottom - Px(1.0f)), kIdle, Px(2.0f));
        } else if (i > 0) {
            const double interval = frame.start - frames[i - 1].start;
            const double ms = interval * 1000.0;
            drawList->AddLine(ImVec2(x, graphBottom), ImVec2(x, yAt(interval)), IntervalColor(ms), Px(1.0f));
            if (ms > kScaleMs && x > labelEnd) {
                char label[16];
                FormatMs(label, sizeof(label), interval);
                const ImVec2 at(x + Px(2.0f), graphTop);
                drawList->AddText(at, IntervalColor(ms), label);
                labelEnd = at.x + ImGui::CalcTextSize(label).x + Px(4.0f);
            }
            lastInterval = interval;
            worstInterval = std::max(worstInterval, interval);
        }
        drawList->AddLine(ImVec2(x, graphBottom), ImVec2(x, yAt(frame.buildSeconds)), kBuild, Px(1.0f));
        worstBuild = std::max(worstBuild, frame.buildSeconds);
    }

    // ----- The work, a lane each -----
    const core::Timeline::Spans& spans = timeline.RecordedSpans();
    std::array<float, core::kTimelineMarkCount> laneLabelEnd;
    laneLabelEnd.fill(-1.0e9f);
    // The WAL's size after the last commit or checkpoint: what a checkpoint
    // would move, and what a power cut could take.
    int64_t walBytes = -1;
    for (size_t i = 0; i < spans.Size(); ++i) {
        const core::TimelineSpan& span = spans[i];
        if (span.mark == TimelineMark::Commit && span.bytes >= 0) {
            walBytes = span.bytes;
        } else if (span.mark == TimelineMark::Checkpoint) {
            walBytes = 0;
        }
        if (span.start + span.seconds < windowStart) {
            continue;
        }
        const size_t lane = static_cast<size_t>(span.mark);
        const ImU32 color = kLanes[lane].color;
        const float top = lanesTop + laneH * static_cast<float>(lane);
        const float x0 = std::max(xAt(span.start), graphLeft);
        const float x1 = std::max(xAt(span.start + span.seconds), x0 + Px(2.0f));
        drawList->AddRectFilled(ImVec2(x0, top + Px(4.0f)), ImVec2(x1, top + laneH - Px(4.0f)), color);
        if (span.seconds >= 0.001) {
            drawList->AddLine(ImVec2(x0, graphTop), ImVec2(x0, top), WithAlpha(color, 110));
        }
        if (span.seconds >= 0.002 && x1 > laneLabelEnd[lane]) {
            char label[16];
            FormatMs(label, sizeof(label), span.seconds);
            const ImVec2 at(x1 + Px(3.0f), top + (laneH - lineH) * 0.5f);
            drawList->AddText(at, color, label);
            laneLabelEnd[lane] = at.x + ImGui::CalcTextSize(label).x + Px(4.0f);
        }
    }

    drawList->PopClipRect();

    for (size_t lane = 0; lane < core::kTimelineMarkCount; ++lane) {
        const float top = lanesTop + laneH * static_cast<float>(lane);
        drawList->AddText(ImVec2(panelMin.x + pad, top + (laneH - lineH) * 0.5f), kLanes[lane].color,
                          kLanes[lane].name);
    }

    // ----- The header -----
    char last[16] = "idle";
    if (lastInterval >= 0.0 && !(frames.Size() > 0 && frames[frames.Size() - 1].idleAfter)) {
        FormatMs(last, sizeof(last), lastInterval);
    }
    char worst[16];
    FormatMs(worst, sizeof(worst), worstInterval);
    char build[16];
    FormatMs(build, sizeof(build), worstBuild);
    char wal[24] = "-";
    if (walBytes >= 0) {
        FormatBytes(wal, sizeof(wal), walBytes);
    }
    char header[128];
    std::snprintf(header, sizeof(header), "frame %s   worst %s   build worst %s   WAL %s", last, worst, build,
                  wal);
    drawList->AddText(ImVec2(panelMin.x + pad, panelMin.y + pad), kText, header);
}

}  // namespace sz::ui
