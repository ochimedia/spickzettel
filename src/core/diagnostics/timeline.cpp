#include "core/diagnostics/timeline.h"

#include <chrono>

namespace sz::core {

Timeline& Timeline::Instance() {
    static Timeline timeline;
    return timeline;
}

double Timeline::Now() {
    static const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

void Timeline::SetRecording(bool on) {
    if (!on) {
        frames_.Clear();
        spans_.Clear();
    }
    recording_ = on;
}

void Timeline::AddFrame(const TimelineFrame& frame) {
    if (recording_) {
        TimelineFrame marked = frame;
        marked.afterBreak = marked.afterBreak || breakPending_;
        frames_.Push(marked);
    }
    breakPending_ = false;
}

void Timeline::AddSpan(const TimelineSpan& span) {
    if (recording_) {
        spans_.Push(span);
    }
}

TimelineScope::TimelineScope(TimelineMark mark) : recording_(Timeline::Instance().Recording()) {
    if (recording_) {
        span_.mark = mark;
        span_.start = Timeline::Now();
    }
}

TimelineScope::~TimelineScope() {
    if (recording_) {
        span_.seconds = Timeline::Now() - span_.start;
        Timeline::Instance().AddSpan(span_);
    }
}

}  // namespace sz::core
