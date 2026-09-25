#include "platform/win32/win32_crash_dump.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace sz::platform::win32 {
namespace {

class Win32CrashDumpTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               (std::string("spickzettel_crash_dump_test_") +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::vector<std::filesystem::path> Dumps() const {
        std::vector<std::filesystem::path> dumps;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
            if (entry.path().extension() == ".dmp") {
                dumps.push_back(entry.path());
            }
        }
        return dumps;
    }

    std::filesystem::path dir_;
};

TEST_F(Win32CrashDumpTest, WritesADumpOfTheProcessAsItStands) {
    std::filesystem::create_directories(dir_);
    const std::filesystem::path file = dir_ / "now.dmp";
    ASSERT_TRUE(WriteMiniDump(file.wstring(), nullptr));
    EXPECT_GT(std::filesystem::file_size(file), 1024u);
}

// A crash before the app's own folder exists still has somewhere to put
// its dump: every missing folder on the way is made.
TEST_F(Win32CrashDumpTest, TheDumpFolderIsMadeWithEveryFolderAboveIt) {
    std::wstring path = (dir_ / "Spickzettel" / "crashes").wstring();
    std::vector<wchar_t> buffer(path.begin(), path.end());
    buffer.push_back(L'\0');
    EXPECT_TRUE(CreateDirectoryChain(buffer.data()));
    EXPECT_TRUE(std::filesystem::is_directory(dir_ / "Spickzettel" / "crashes"));
    EXPECT_EQ(std::wstring(buffer.data()), path) << "left as it was";
    EXPECT_TRUE(CreateDirectoryChain(buffer.data())) << "and there already is fine";
}

TEST_F(Win32CrashDumpTest, ThePrefixIsAlwaysAFileName) {
    EXPECT_EQ(CrashDumpPrefix("0.1.0 (v0.1.0-3-gabc1234-dirty) - demo, prerelease"),
              L"Spickzettel-0.1.0_(v0.1.0-3-gabc1234-dirty)_-_demo__prerelease-");
    EXPECT_EQ(CrashDumpPrefix("1/2\\3:4"), L"Spickzettel-1_2_3_4-");
}

TEST_F(Win32CrashDumpTest, PruningKeepsTheNewest) {
    std::filesystem::create_directories(dir_);
    const auto now = std::filesystem::file_time_type::clock::now();
    for (int i = 0; i < 5; ++i) {
        const std::filesystem::path file = dir_ / ("d" + std::to_string(i) + ".dmp");
        std::ofstream(file) << i;
        std::filesystem::last_write_time(file, now - std::chrono::hours(5 - i));
    }
    std::ofstream(dir_ / "notes.txt") << "not a dump";

    PruneCrashDumps(dir_, 2);
    EXPECT_EQ(Dumps().size(), 2u);
    EXPECT_TRUE(std::filesystem::exists(dir_ / "d3.dmp"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "d4.dmp"));
    EXPECT_TRUE(std::filesystem::exists(dir_ / "notes.txt")) << "only dumps are pruned";
}

// The real thing, in a child process: a crash ends the process with a dump
// in the folder, which installing the writer did not create. One death
// test per test: the child runs the whole test body again, and a check
// between two of them would fail there.
TEST_F(Win32CrashDumpTest, ACrashLeavesADump) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            InstallCrashDumpWriter(dir_, "test");
            if (std::filesystem::exists(dir_)) {
                std::exit(1);
            }
            // On a thread of its own: googletest catches SEH exceptions on
            // the one running the test, and would report this one as a
            // failure instead of leaving it unhandled.
            std::thread([] { *static_cast<volatile int*>(nullptr) = 1; }).join();
        },
        ::testing::ExitedWithCode(3), "");
    ASSERT_EQ(Dumps().size(), 1u);
    EXPECT_EQ(Dumps()[0].filename().wstring().rfind(L"Spickzettel-test-", 0), 0u);
}

// abort() - which std::terminate ends in - is not an SEH exception, and
// has its own way into the writer.
TEST_F(Win32CrashDumpTest, AnAbortLeavesADump) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            InstallCrashDumpWriter(dir_, "test");
            std::abort();
        },
        ::testing::ExitedWithCode(3), "");
    EXPECT_EQ(Dumps().size(), 1u);
}

}  // namespace
}  // namespace sz::platform::win32
