#include "core/util/slug.h"

#include <gtest/gtest.h>

namespace sz::core {
namespace {

TEST(SlugTest, LowercasesAndHyphenatesSpaces) {
    EXPECT_EQ(MakeSlug("Boss Room", 12), "boss-room-00000c");
}

TEST(SlugTest, CollapsesRunsOfPunctuationIntoOneHyphen) {
    EXPECT_EQ(MakeSlug("Level 1 -- Sewers!!", 3), "level-1-sewers-000003");
}

TEST(SlugTest, TrimsLeadingAndTrailingSeparators) {
    EXPECT_EQ(MakeSlug("  ~Dungeon~  ", 7), "dungeon-000007");
}

TEST(SlugTest, TwoDifferentIdsWithTheSameNameNeverCollide) {
    EXPECT_NE(MakeSlug("Boss Room", 1), MakeSlug("Boss Room", 2));
}

TEST(SlugTest, EmptyNameFallsBackToUntitled) {
    EXPECT_EQ(MakeSlug("", 5), "untitled-000005");
}

TEST(SlugTest, NameThatIsAllSymbolsFallsBackToUntitled) {
    EXPECT_EQ(MakeSlug("***///", 9), "untitled-000009");
}

TEST(SlugTest, NonAsciiBytesCollapseToSeparatorsRatherThanBreakingTheSlug) {
    // "Café" - the 'é' is a 2-byte UTF-8 sequence; both bytes are treated
    // as one non-ASCII separator run rather than kept verbatim.
    const std::string result = MakeSlug("Caf\xC3\xA9", 4);
    EXPECT_EQ(result, "caf-000004");
}

TEST(SlugTest, LongNameIsTruncated) {
    const std::string longName(200, 'a');
    const std::string result = MakeSlug(longName, 2);
    // 20-char base + "-2".
    EXPECT_EQ(result, std::string(20, 'a') + "-000002");
}

TEST(SlugTest, ADefaultTimestampNameIsKeptWhole) {
    EXPECT_EQ(MakeSlug("2026-09-07 22:36:14", 2), "2026-09-07-22-36-14-000002");
}

TEST(SlugTest, ATruncatedNameNeverEndsInASeparator) {
    // The cut lands just after the space, which became a separator.
    EXPECT_EQ(MakeSlug("abcdefghijklmnopqrs tuvwxyz", 2), "abcdefghijklmnopqrs-000002");
}

TEST(SlugTest, ResultNeverExactlyMatchesAWindowsReservedDeviceName) {
    // The trailing "-<id>" makes an exact match structurally impossible -
    // "con" becomes "con-1", never bare "con".
    EXPECT_EQ(MakeSlug("CON", 1), "con-000001");
    EXPECT_NE(MakeSlug("CON", 1), "con");
}

}  // namespace
}  // namespace sz::core
