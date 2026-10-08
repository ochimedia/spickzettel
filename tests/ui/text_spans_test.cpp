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

// A profile's name goes in whole, however long, where an array of 256
// cut it through a character.
TEST(TextSpansTest, AValueGoesInWholeHoweverLong) {
    std::string name;
    for (int i = 0; i < 100; ++i) {
        name += "æ¼¢";
    }
    const std::vector<TextSpan> spans = MarkedSpans("Profile {ui:%s} is active", name);
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[1].text, name);
    ASSERT_EQ(MarkedSpans("No value", name).size(), 1u);
    EXPECT_EQ(MarkedSpans("No value", name)[0].text, "No value");
}

// The user's words put in after the names are found: a brace in them is
// shown as typed, and marked or not as the place they go is.
TEST(TextSpansTest, AnArgumentIsNeverReadForNames) {
    const std::vector<TextSpan> spans = MarkedSpans("Editing behavior for profile {ui:%s}", "a}b{ui:c");
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].text, "Editing behavior for profile ");
    EXPECT_FALSE(spans[0].marked);
    EXPECT_EQ(spans[1].text, "a}b{ui:c");
    EXPECT_TRUE(spans[1].marked);

    const std::vector<TextSpan> plain = MarkedSpans("Hello %s.", "{ui:x}");
    ASSERT_EQ(plain.size(), 1u);
    EXPECT_EQ(plain[0].text, "Hello {ui:x}.");
    EXPECT_FALSE(plain[0].marked);
}

}  // namespace
}  // namespace sz::ui
