#include "app/command_line.h"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace sz::app {
namespace {

// No arguments is the ordinary start: nothing set, nothing wrong.
TEST(CommandLineTest, NoArgumentsIsAnOrdinaryStart) {
    const ParsedCommandLine parsed = ParseCommandLine({});
    EXPECT_TRUE(parsed.error.empty());
    EXPECT_FALSE(parsed.options.dataDir.has_value());
}

// The data folder, as the next argument or after an equals sign, made
// absolute; a folder named in any script is the same folder.
TEST(CommandLineTest, TheDataFolderIsTakenEitherWayAndMadeAbsolute) {
    const std::filesystem::path sandbox = std::filesystem::temp_directory_path() / "sandbox";
    const std::string text = reinterpret_cast<const char*>(sandbox.u8string().c_str());
    const ParsedCommandLine spaced = ParseCommandLine({"--data-dir", text});
    ASSERT_TRUE(spaced.error.empty()) << spaced.error;
    ASSERT_TRUE(spaced.options.dataDir.has_value());
    EXPECT_EQ(*spaced.options.dataDir, sandbox.lexically_normal());

    const ParsedCommandLine joined = ParseCommandLine({"--data-dir=" + text});
    ASSERT_TRUE(joined.error.empty()) << joined.error;
    EXPECT_EQ(joined.options.dataDir, spaced.options.dataDir);

    const ParsedCommandLine relative = ParseCommandLine({"--data-dir", "sandbox"});
    ASSERT_TRUE(relative.options.dataDir.has_value());
    EXPECT_TRUE(relative.options.dataDir->is_absolute());

    const ParsedCommandLine named = ParseCommandLine({"--data-dir", "\xE6\xBC\xA2"});  // a kanji, in UTF-8
    ASSERT_TRUE(named.options.dataDir.has_value());
    EXPECT_EQ(named.options.dataDir->filename().u8string(), u8"\u6F22");
}

// Anything not understood is said, naming it - and nothing is set: a start
// with a misspelled option would otherwise be one on the user's own data.
TEST(CommandLineTest, AnythingNotUnderstoodIsAnError) {
    const ParsedCommandLine unknown = ParseCommandLine({"--data-folder", "x"});
    EXPECT_NE(unknown.error.find("--data-folder"), std::string::npos) << unknown.error;
    EXPECT_FALSE(ParseCommandLine({"stray"}).error.empty());
    EXPECT_NE(ParseCommandLine({"--data-dir"}).error.find("--data-dir"), std::string::npos);
    EXPECT_FALSE(ParseCommandLine({"--data-dir="}).error.empty()) << "an empty folder is no folder";
    EXPECT_FALSE(ParseCommandLine({"--data-dir", "a", "--data-dir", "b"}).error.empty()) << "given twice";
}

// The help names every option and what follows it.
TEST(CommandLineTest, TheUsageNamesTheOptions) {
    const std::string usage = CommandLineUsage();
    EXPECT_NE(usage.find("--data-dir <folder>"), std::string::npos) << usage;
}

}  // namespace
}  // namespace sz::app
