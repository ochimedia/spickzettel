#include "ui/text_spans.h"

#include <gtest/gtest.h>

namespace sz::ui {
namespace {

// A {ui:} name a run of its own, marked, and the text around it plain;
// other braces stay as they are.
TEST(TextSpansTest, MarksTheNamesInRuns) {
    const std::vector<TextSpan> spans = MarkedSpans("Turn on {ui:Show deleted} and press {ui:Empty trash}. {canvas}");
    ASSERT_EQ(spans.size(), 5u);
    const std::vector<std::pair<std::string, bool>> expected = {
        {"Turn on ", false}, {"Show deleted", true}, {" and press ", false}, {"Empty trash", true}, {". {canvas}", false}};
    for (size_t i = 0; i < spans.size(); ++i) {
        EXPECT_EQ(spans[i].text, expected[i].first) << i;
        EXPECT_EQ(spans[i].marked, expected[i].second) << i;
    }
}

// A text without names is one plain run; one cut off at the end shows as
// typed.
TEST(TextSpansTest, TextWithoutANameIsOnePlainRun) {
    ASSERT_EQ(MarkedSpans("No names here.").size(), 1u);
    EXPECT_FALSE(MarkedSpans("No names here.")[0].marked);
    const std::vector<TextSpan> open = MarkedSpans("Press {ui:Next");
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0].text, "Press {ui:Next");
    EXPECT_TRUE(MarkedSpans("").empty());
}

}  // namespace
}  // namespace sz::ui
