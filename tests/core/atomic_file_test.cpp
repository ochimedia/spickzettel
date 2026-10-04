#include "core/util/atomic_file.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "support/temp_dir.h"

namespace sz::core {
namespace {

class AtomicFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = sz::test::TempDir() /
               (std::string("spickzettel_atomic_file_test_") +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static std::string Read(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    static void Put(const std::filesystem::path& path, const std::string& text) {
        std::ofstream(path, std::ios::binary) << text;
    }

    std::filesystem::path dir_;
};

TEST_F(AtomicFileTest, WritesTheBytesAndLeavesNoTemporaryBehind) {
    const std::filesystem::path file = dir_ / "deep" / "er" / "record.json";
    ASSERT_TRUE(WriteFileAtomically(file, std::string_view("{\"a\": 1}")));
    EXPECT_EQ(Read(file), "{\"a\": 1}");
    EXPECT_FALSE(std::filesystem::exists(dir_ / "deep" / "er" / "record.json.tmp"));
    ASSERT_TRUE(WriteFileAtomically(file, std::string_view("{\"a\": 2}"))) << "over its previous self";
    EXPECT_EQ(Read(file), "{\"a\": 2}");
}

// A temporary a crash left behind is a plain file with no other name: ours,
// and in the way, so it goes and the name is used.
TEST_F(AtomicFileTest, AStaleTemporaryOfOursIsReplaced) {
    const std::filesystem::path file = dir_ / "record.json";
    Put(file.string() + ".tmp", "half of a record from a process that died");
    ASSERT_TRUE(WriteFileAtomically(file, std::string_view("whole")));
    EXPECT_EQ(Read(file), "whole");
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".tmp"));
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".tmp1"));
}

// A hard link is a second name for a file that may be anybody's. Opening
// it with truncation would empty that file; it is never opened at all.
TEST_F(AtomicFileTest, AHardLinkAtTheTemporaryNameIsNeverOpened) {
    const std::filesystem::path sentinel = dir_ / "somebody-elses.txt";
    Put(sentinel, "precious");
    const std::filesystem::path file = dir_ / "record.json";
    std::error_code ec;
    std::filesystem::create_hard_link(sentinel, file.string() + ".tmp", ec);
    ASSERT_FALSE(ec) << "the filesystem under the temp directory does not do hard links";

    ASSERT_TRUE(WriteFileAtomically(file, std::string_view("the record")));
    EXPECT_EQ(Read(file), "the record");
    EXPECT_EQ(Read(sentinel), "precious") << "written into through its other name";
    EXPECT_EQ(Read(file.string() + ".tmp"), "precious") << "the link itself is left alone too";
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".tmp1")) << "the next name was used and renamed away";
}

// ...and the same for a link to the destination itself, which would have
// truncated the committed file before the rename ever happened.
TEST_F(AtomicFileTest, AHardLinkToTheDestinationAtTheTemporaryNameIsNeverOpened) {
    const std::filesystem::path file = dir_ / "record.json";
    Put(file, "committed");
    std::error_code ec;
    std::filesystem::create_hard_link(file, file.string() + ".tmp", ec);
    ASSERT_FALSE(ec);

    ASSERT_TRUE(WriteFileAtomically(file, std::string_view("replaced")));
    EXPECT_EQ(Read(file), "replaced");
    EXPECT_EQ(Read(file.string() + ".tmp"), "committed") << "the old bytes, untouched, under the other name";
}

TEST_F(AtomicFileTest, ADirectoryAtTheTemporaryNameIsPassedOver) {
    const std::filesystem::path file = dir_ / "record.json";
    std::filesystem::create_directories(file.string() + ".tmp");
    ASSERT_TRUE(WriteFileAtomically(file, std::string_view("around it")));
    EXPECT_EQ(Read(file), "around it");
    EXPECT_TRUE(std::filesystem::is_directory(file.string() + ".tmp"));
}

TEST_F(AtomicFileTest, AWriteThatCannotLandLeavesNothingNew) {
    const std::filesystem::path file = dir_ / "record.json";
    std::filesystem::create_directories(file);  // a directory where the file goes: the rename fails
    EXPECT_FALSE(WriteFileAtomically(file, std::string_view("nowhere")));
    EXPECT_FALSE(std::filesystem::exists(file.string() + ".tmp"));
    EXPECT_TRUE(std::filesystem::is_directory(file));
}

// A flush leaves the file as it was, and makes none where there is none.
TEST_F(AtomicFileTest, AFlushKeepsTheFileAndMakesNone) {
    const std::filesystem::path file = dir_ / "library.db";
    Put(file, "contents");
    EXPECT_TRUE(FlushFileToDisk(file));
    EXPECT_EQ(Read(file), "contents");

    const std::filesystem::path missing = dir_ / "missing.db";
    EXPECT_FALSE(FlushFileToDisk(missing));
    EXPECT_FALSE(std::filesystem::exists(missing));
}

}  // namespace
}  // namespace sz::core
