#include "core/diagnostics/timeline.h"

#include <gtest/gtest.h>

namespace sz::core {
namespace {

// The process's one timeline, recording for the test and let go of after.
class TimelineTest : public ::testing::Test {
protected:
    void SetUp() override { timeline_.SetRecording(false); }
    void TearDown() override { timeline_.SetRecording(false); }
    Timeline& timeline_ = Timeline::Instance();
};

TEST(TimelineRingTest, KeepsTheNewestOldestFirst) {
    TimelineRing<int, 3> ring;
    EXPECT_EQ(ring.Size(), 0u);
    for (int i = 1; i <= 5; ++i) {
        ring.Push(i);
    }
    ASSERT_EQ(ring.Size(), 3u);
    EXPECT_EQ(ring[0], 3);
    EXPECT_EQ(ring[1], 4);
    EXPECT_EQ(ring[2], 5);
    ring.Clear();
    EXPECT_EQ(ring.Size(), 0u);
    ring.Push(6);
    EXPECT_EQ(ring[0], 6);
}

TEST_F(TimelineTest, NothingIsRecordedWhileRecordingIsOff) {
    timeline_.AddFrame(TimelineFrame{1.0, 0.001, false});
    { const TimelineScope marked(TimelineMark::Commit); }
    EXPECT_EQ(timeline_.RecordedFrames().Size(), 0u);
    EXPECT_EQ(timeline_.RecordedSpans().Size(), 0u);
}

TEST_F(TimelineTest, AScopeIsRecordedAsTheSpanItCovers) {
    timeline_.SetRecording(true);
    const double before = Timeline::Now();
    {
        TimelineScope marked(TimelineMark::Checkpoint);
        marked.SetBytes(4096);
    }
    const double after = Timeline::Now();
    ASSERT_EQ(timeline_.RecordedSpans().Size(), 1u);
    const TimelineSpan& span = timeline_.RecordedSpans()[0];
    EXPECT_EQ(span.mark, TimelineMark::Checkpoint);
    EXPECT_EQ(span.bytes, 4096);
    EXPECT_GE(span.start, before);
    EXPECT_LE(span.start + span.seconds, after);
}

// A scope that began while recording was off is not recorded when it ends
// after recording came on: it would say it started when nothing knew.
TEST_F(TimelineTest, AScopeBegunBeforeRecordingIsNotRecorded) {
    {
        const TimelineScope marked(TimelineMark::Commit);
        timeline_.SetRecording(true);
    }
    EXPECT_EQ(timeline_.RecordedSpans().Size(), 0u);
}

TEST_F(TimelineTest, TurningRecordingOffLetsGoOfWhatWasRecorded) {
    timeline_.SetRecording(true);
    timeline_.AddFrame(TimelineFrame{1.0, 0.001, false});
    { const TimelineScope marked(TimelineMark::Commit); }
    timeline_.SetRecording(false);
    timeline_.SetRecording(true);
    EXPECT_EQ(timeline_.RecordedFrames().Size(), 0u);
    EXPECT_EQ(timeline_.RecordedSpans().Size(), 0u);
}

// The time the overlay was away is not a late frame: the first frame after
// it says so.
TEST_F(TimelineTest, TheFirstFrameAfterABreakIsMarked) {
    timeline_.SetRecording(true);
    timeline_.AddFrame(TimelineFrame{1.0, 0.001, false});
    timeline_.BreakFrames();
    timeline_.AddFrame(TimelineFrame{5.0, 0.001, false});
    timeline_.AddFrame(TimelineFrame{5.016, 0.001, false});
    ASSERT_EQ(timeline_.RecordedFrames().Size(), 3u);
    EXPECT_FALSE(timeline_.RecordedFrames()[0].afterBreak);
    EXPECT_TRUE(timeline_.RecordedFrames()[1].afterBreak);
    EXPECT_FALSE(timeline_.RecordedFrames()[2].afterBreak);
}

}  // namespace
}  // namespace sz::core
