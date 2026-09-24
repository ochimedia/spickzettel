#include "core/util/file_system.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

namespace sz::core {
namespace {

class RealFileSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               (std::string("spickzettel_file_system_test_") +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static std::string ReadBack(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    static void Put(const std::filesystem::path& path, const std::string& text) {
        std::ofstream(path, std::ios::binary) << text;
    }

    RealFileSystem fs_;
    std::filesystem::path dir_;
};

TEST_F(RealFileSystemTest, ListsWhatADirectoryHoldsByNameAndKind) {
    Put(dir_ / "a.json", "{}");
    std::filesystem::create_directories(dir_ / "sub");

    auto entries = fs_.List(dir_);
    ASSERT_TRUE(entries.has_value());
    std::sort(entries->begin(), entries->end(), [](const auto& l, const auto& r) { return l.name < r.name; });
    ASSERT_EQ(entries->size(), 2u);
    EXPECT_EQ((*entries)[0].name, "a.json");
    EXPECT_EQ((*entries)[0].kind, FileSystem::Kind::File);
    EXPECT_EQ((*entries)[1].name, "sub");
    EXPECT_EQ((*entries)[1].kind, FileSystem::Kind::Directory);
}

// Nothing listed is a different answer from an empty directory.
TEST_F(RealFileSystemTest, ADirectoryThatCannotBeListedIsNoListingAtAll) {
    EXPECT_FALSE(fs_.List(dir_ / "missing").has_value());
    Put(dir_ / "file", "x");
    EXPECT_FALSE(fs_.List(dir_ / "file").has_value());
    ASSERT_TRUE(fs_.List(dir_).has_value());
}

TEST_F(RealFileSystemTest, ReadsAWholeFileButNothingPastItsBudget) {
    Put(dir_ / "record.json", "0123456789");
    EXPECT_EQ(fs_.Read(dir_ / "record.json", 10), "0123456789");
    EXPECT_FALSE(fs_.Read(dir_ / "record.json", 9).has_value());
    EXPECT_FALSE(fs_.Read(dir_ / "missing.json", 100).has_value());
}

// Whatever is at the name is left exactly as it was - never opened, so
// never truncated.
TEST_F(RealFileSystemTest, WritingANewFileOverAnExistingOneLeavesItAlone) {
    Put(dir_ / "taken", "somebody's");
    EXPECT_EQ(fs_.WriteNewFile(dir_ / "taken", "mine", 4), FileSystem::WriteResult::NameTaken);
    EXPECT_EQ(ReadBack(dir_ / "taken"), "somebody's");

    EXPECT_EQ(fs_.WriteNewFile(dir_ / "fresh", "mine", 4), FileSystem::WriteResult::Written);
    EXPECT_EQ(ReadBack(dir_ / "fresh"), "mine");
    EXPECT_EQ(fs_.WriteNewFile(dir_ / "nowhere" / "fresh", "mine", 4), FileSystem::WriteResult::Failed);
}

TEST_F(RealFileSystemTest, CreatesADirectoryWithItsParentsUnlessAFileIsInTheWay) {
    EXPECT_TRUE(fs_.CreateDirectories(dir_ / "a" / "b" / "c"));
    EXPECT_EQ(fs_.Status(dir_ / "a" / "b" / "c"), FileSystem::Kind::Directory);
    EXPECT_TRUE(fs_.CreateDirectories(dir_ / "a" / "b")) << "already there is there";

    Put(dir_ / "file", "x");
    EXPECT_FALSE(fs_.CreateDirectories(dir_ / "file" / "d"));
}

TEST_F(RealFileSystemTest, RemovesATreeAndReportsWhatWasNeverThereAsGone) {
    std::filesystem::create_directories(dir_ / "tree" / "deeper");
    Put(dir_ / "tree" / "deeper" / "leaf", "x");
    EXPECT_TRUE(fs_.RemoveAll(dir_ / "tree"));
    EXPECT_EQ(fs_.LinkStatus(dir_ / "tree"), FileSystem::Kind::None);
    EXPECT_TRUE(fs_.RemoveAll(dir_ / "tree"));
    EXPECT_TRUE(fs_.Remove(dir_ / "never"));
}

// A link inside a tree names something that may be anywhere: removing the
// tree takes the link and leaves what it points at. A junction on Windows -
// what "mklink /J" makes, with no privilege needed - and a symlink elsewhere.
TEST_F(RealFileSystemTest, RemovingATreeNeverFollowsALinkInIt) {
    std::filesystem::create_directories(dir_ / "elsewhere");
    Put(dir_ / "elsewhere" / "precious", "keep me");
    std::filesystem::create_directories(dir_ / "tree");
#if defined(_WIN32)
    const std::string command =
        "mklink /J \"" + (dir_ / "tree" / "link").string() + "\" \"" + (dir_ / "elsewhere").string() + "\" >nul";
    ASSERT_EQ(std::system(command.c_str()), 0) << "could not create the junction";
#else
    std::filesystem::create_directory_symlink(dir_ / "elsewhere", dir_ / "tree" / "link");
#endif
    EXPECT_EQ(fs_.LinkStatus(dir_ / "tree" / "link"), FileSystem::Kind::Link);
    EXPECT_EQ(fs_.Status(dir_ / "tree" / "link"), FileSystem::Kind::Directory);

    EXPECT_TRUE(fs_.RemoveAll(dir_ / "tree"));
    EXPECT_EQ(ReadBack(dir_ / "elsewhere" / "precious"), "keep me");
}

}  // namespace
}  // namespace sz::core
