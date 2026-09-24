#include "support/faulty_file_system.h"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "support/memory_file_system.h"

namespace sz::core::fakes {
namespace {

std::filesystem::path Root() { return std::filesystem::temp_directory_path() / "spickzettel_faulty"; }

TEST(FaultyFileSystemTest, AFailureHitsOnlyWhatItMatchesAndOnlyAsOftenAsAsked) {
    MemoryFileSystem disk;
    FaultyFileSystem fs(disk);
    ASSERT_TRUE(fs.CreateDirectories(Root()));
    fs.FailWhen(FaultyFileSystem::Op::WriteNewFile,
                [](const std::filesystem::path& path) { return path.filename() == "a"; }, 1);

    EXPECT_EQ(fs.WriteNewFile(Root() / "b", "x", 1), FileSystem::WriteResult::Written);
    EXPECT_EQ(fs.WriteNewFile(Root() / "a", "x", 1), FileSystem::WriteResult::Failed);
    EXPECT_EQ(disk.Status(Root() / "a"), FileSystem::Kind::None) << "a failure has no effect";
    EXPECT_EQ(fs.WriteNewFile(Root() / "a", "x", 1), FileSystem::WriteResult::Written) << "once only";

    fs.FailWhen(FaultyFileSystem::Op::List, [](const std::filesystem::path&) { return true; });
    EXPECT_FALSE(fs.List(Root()).has_value());
    EXPECT_FALSE(fs.List(Root()).has_value()) << "every time, until cleared";
    fs.ClearFailures();
    EXPECT_TRUE(fs.List(Root()).has_value());
}

// After the crash nothing changes on the disk, and the write the crash
// interrupted leaves its first half.
TEST(FaultyFileSystemTest, ACrashStopsTheDiskAndTearsTheWriteItInterrupts) {
    MemoryFileSystem disk;
    FaultyFileSystem fs(disk);
    ASSERT_TRUE(fs.CreateDirectories(Root()));
    const size_t before = fs.ChangesAttempted();
    fs.CrashAfter(1);

    EXPECT_EQ(fs.WriteNewFile(Root() / "whole", "12345678", 8), FileSystem::WriteResult::Written);
    EXPECT_FALSE(fs.Crashed());
    EXPECT_EQ(fs.WriteNewFile(Root() / "torn", "12345678", 8), FileSystem::WriteResult::Failed);
    EXPECT_TRUE(fs.Crashed());
    EXPECT_EQ(disk.Read(Root() / "torn", 100), "1234");
    EXPECT_EQ(fs.WriteNewFile(Root() / "later", "x", 1), FileSystem::WriteResult::Failed);
    EXPECT_FALSE(fs.Rename(Root() / "whole", Root() / "moved"));
    EXPECT_FALSE(fs.Remove(Root() / "whole"));
    EXPECT_EQ(disk.Status(Root() / "later"), FileSystem::Kind::None);
    EXPECT_EQ(disk.Read(Root() / "whole", 100), "12345678");
    EXPECT_EQ(fs.ChangesAttempted() - before, 5u) << "every attempt counts, refused or not";
}

}  // namespace
}  // namespace sz::core::fakes
