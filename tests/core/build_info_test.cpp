#include "core/build_info/build_info.h"

#include <gtest/gtest.h>

#include <string>

namespace sz::core::build {
namespace {

TEST(BuildInfoTest, VersionLineStartsWithTheVersion) {
    const std::string line = VersionLine();
    EXPECT_EQ(line.rfind(std::string(kVersion), 0), 0u) << line;
}

TEST(BuildInfoTest, VersionLineNeverRepeatsTheVersionAsTheDescription) {
    // Whatever the tree is at, "0.1.0 (0.1.0)" and "0.1.0 (v0.1.0)" are the
    // two spellings that say nothing and must not appear.
    const std::string line = VersionLine();
    EXPECT_EQ(line.find("(" + std::string(kVersion) + ")"), std::string::npos) << line;
    EXPECT_EQ(line.find("(v" + std::string(kVersion) + ")"), std::string::npos) << line;
}

TEST(BuildInfoTest, VersionLineNamesAPrereleaseBuild) {
    const std::string line = VersionLine();
    EXPECT_EQ(line.find("prerelease") != std::string::npos, kPrereleaseNotice) << line;
}

TEST(BuildInfoTest, EmbeddedTextsArePresentAndLfOnly) {
    EXPECT_FALSE(AboutText().empty());
    EXPECT_FALSE(NoticesText().empty());
    EXPECT_EQ(AboutText().find('\r'), std::string_view::npos);
    EXPECT_EQ(NoticesText().find('\r'), std::string_view::npos);
    EXPECT_NE(NoticesText().find("Dear ImGui"), std::string_view::npos);
}

}  // namespace
}  // namespace sz::core::build
