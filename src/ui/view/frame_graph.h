#pragma once

// The frame graph - see AppConfig::showFrameGraph: the last ten seconds of
// core::Timeline, drawn in the top right corner of the display.
//
// Above, a bar per frame for the time since the frame before it - green
// within a refresh or so, amber within two, red past that, cut off at
// 50 ms with the time written over it - and over it, pale, the time the
// overlay spent building it. A frame paced idle (view mode, the pinned
// view) has only the second, over a gray line along the bottom: its gap is
// the pacing's. Where the overlay was hidden, the time is shaded instead.
//
// Below, a lane per kind of work that can make a frame late (see
// core::TimelineMark), with a mark where it ran - as long as it took, and a
// line up through the frames from any that took a millisecond or more, so
// that a late frame and what made it late line up. The header says the last
// frame's time, the worst frame and the worst build in the ten seconds, and
// the WAL's size after the last commit or checkpoint.

#include <imgui.h>

#include "core/diagnostics/timeline.h"

namespace sz::ui {

// `now` on the timeline's clock (see core::Timeline::Now) is the graph's
// right edge.
void DrawFrameGraph(ImDrawList* drawList, float displayW, const core::Timeline& timeline, double now);

}  // namespace sz::ui
