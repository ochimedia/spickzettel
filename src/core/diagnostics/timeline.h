#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sz::core {

// What the frame graph draws, recorded as it happens: every frame the
// overlay draws, and the work on the app thread that can make one late -
// see AppConfig::showFrameGraph, and docs/PERF.md, "Instrument 4".
//
// One for the process, reached through Instance: the work it marks is in
// the store, the session and the config file, and threading a recorder
// through each of them would be plumbing for a debugging aid. The app
// thread's alone - everything it marks runs there - so nothing is locked.
// Records nothing until recording is on, which the overlay sets from the
// setting at every frame: off, a mark costs a branch.

// The kinds of work marked, each in a lane of its own in the graph.
enum class TimelineMark : uint8_t {
    Commit,       // a library write, from its BEGIN to its COMMIT
    Encode,       // a picture encoded for the library - inside a commit
    Checkpoint,   // the WAL moved into the file, and both flushed
    Capture,      // the screen read back, for a snippet or a frozen screen
    ReadPicture,  // a picture read from the library and decoded
    ConfigWrite,  // config.json written
};
inline constexpr size_t kTimelineMarkCount = 6;

// Seconds on the recorder's clock (see Timeline::Now).
struct TimelineSpan {
    double start = 0.0;
    double seconds = 0.0;
    TimelineMark mark = TimelineMark::Commit;
    // What it moved, where that says something: for a commit the WAL's
    // size after it, for a checkpoint what it moved into the file, for a
    // picture its encoded size. -1 for nothing said.
    int64_t bytes = -1;
};

struct TimelineFrame {
    double start = 0.0;
    // The time in the overlay's own frame, from its start to its last
    // draw call - not the time presenting it, which waits for the display.
    double buildSeconds = 0.0;
    // Whether the frame after this one was paced idle, a few times a second
    // rather than at every refresh (see IOverlayWindow::SetFramePacing):
    // the gap up to it is the pacing's, not a hitch.
    bool idleAfter = false;
    // The first frame since the overlay was put away (see
    // Timeline::BreakFrames): the gap up to it is time hidden, not a frame.
    bool afterBreak = false;
};

// A fixed-size log that overwrites its oldest entry, read oldest first.
template <typename T, size_t N>
class TimelineRing {
public:
    void Push(const T& value) {
        entries_[next_] = value;
        next_ = (next_ + 1) % N;
        count_ = count_ < N ? count_ + 1 : N;
    }
    size_t Size() const { return count_; }
    // 0 is the oldest.
    const T& operator[](size_t index) const { return entries_[(next_ + N - count_ + index) % N]; }
    void Clear() { next_ = count_ = 0; }

private:
    std::array<T, N> entries_{};
    size_t next_ = 0;
    size_t count_ = 0;
};

class Timeline {
public:
    // Frames at up to 360 Hz for the graph's ten seconds, and spans enough
    // for a write per frame over the same.
    static constexpr size_t kFrameCapacity = 4096;
    static constexpr size_t kSpanCapacity = 1024;
    using Frames = TimelineRing<TimelineFrame, kFrameCapacity>;
    using Spans = TimelineRing<TimelineSpan, kSpanCapacity>;

    static Timeline& Instance();
    // Seconds since the first call, on a clock that only goes forward.
    static double Now();

    // Turned off, what was recorded goes: turned on again, the graph starts
    // from nothing rather than across a gap it knows nothing about.
    void SetRecording(bool on);
    bool Recording() const { return recording_; }

    void AddFrame(const TimelineFrame& frame);
    // The overlay went away, and draws no frames until it is back: the
    // next frame is marked as the first after a break.
    void BreakFrames() { breakPending_ = true; }
    void AddSpan(const TimelineSpan& span);
    const Frames& RecordedFrames() const { return frames_; }
    const Spans& RecordedSpans() const { return spans_; }

private:
    bool recording_ = false;
    bool breakPending_ = false;
    Frames frames_;
    Spans spans_;
};

// Marks the work from its construction to its destruction, when recording.
class TimelineScope {
public:
    explicit TimelineScope(TimelineMark mark);
    ~TimelineScope();
    TimelineScope(const TimelineScope&) = delete;
    TimelineScope& operator=(const TimelineScope&) = delete;

    // See TimelineSpan::bytes.
    void SetBytes(int64_t bytes) { span_.bytes = bytes; }

private:
    TimelineSpan span_;
    bool recording_;
};

}  // namespace sz::core
