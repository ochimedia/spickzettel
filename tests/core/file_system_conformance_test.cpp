// The same expectations, run against the real disk and against the model
// of it the fault tests use (MemoryFileSystem, with FaultyFileSystem for a
// held file) - so that what the fault tests prove about the store is proved
// about the disk it really runs on. Anything the model gets wrong about
// Windows shows up here as the two disagreeing.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/util/file_system.h"
#include "support/faulty_file_system.h"
#include "support/memory_file_system.h"

namespace sz::core {
namespace {

using Kind = FileSystem::Kind;

// A disk to run the expectations against, with the two things a test needs
// that FileSystem itself does not offer: holding a file open, and making a
// link.
class Disk {
public:
    virtual ~Disk() = default;
    virtual FileSystem& Fs() = 0;
    virtual std::filesystem::path Root() const = 0;
    // Whether this disk can hold a file open at all (the real one only on
    // Windows, where holding is what matters).
    virtual bool CanHold() const = 0;
    virtual void Hold(const std::filesystem::path& path) = 0;
    virtual void ReleaseAll() = 0;
    virtual void MakeLink(const std::filesystem::path& link, const std::filesystem::path& target) = 0;
};

class RealDisk final : public Disk {
public:
    RealDisk() {
        std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::replace(name.begin(), name.end(), '/', '_');
        root_ = std::filesystem::temp_directory_path() / ("spickzettel_conformance_" + name);
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }
    ~RealDisk() override {
        open_.clear();
        std::filesystem::remove_all(root_);
    }
    FileSystem& Fs() override { return fs_; }
    std::filesystem::path Root() const override { return root_; }
#if defined(_WIN32)
    bool CanHold() const override { return true; }
#else
    bool CanHold() const override { return false; }
#endif
    // What a picture viewer does: the file open for reading, sharing read
    // and write but not delete - which is how the MSVC runtime opens a
    // stream.
    void Hold(const std::filesystem::path& path) override {
        open_.push_back(std::make_unique<std::ifstream>(path, std::ios::binary));
        ASSERT_TRUE(*open_.back()) << "could not open " << path;
    }
    void ReleaseAll() override { open_.clear(); }
    void MakeLink(const std::filesystem::path& link, const std::filesystem::path& target) override {
#if defined(_WIN32)
        const std::string command = "mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
        ASSERT_EQ(std::system(command.c_str()), 0) << "could not create the junction";
#else
        std::filesystem::create_directory_symlink(target, link);
#endif
    }

private:
    RealFileSystem fs_;
    std::filesystem::path root_;
    std::vector<std::unique_ptr<std::ifstream>> open_;
};

class ModelDisk final : public Disk {
public:
    FileSystem& Fs() override { return faulty_; }
    // Somewhere that would be valid on this machine, never touched on it.
    std::filesystem::path Root() const override {
        return std::filesystem::temp_directory_path() / "spickzettel_memory_disk";
    }
    bool CanHold() const override { return true; }
    void Hold(const std::filesystem::path& path) override { faulty_.Hold(path); }
    void ReleaseAll() override { faulty_.ReleaseAll(); }
    void MakeLink(const std::filesystem::path& link, const std::filesystem::path& target) override {
        ASSERT_TRUE(memory_.MakeLink(link, target));
    }

private:
    fakes::MemoryFileSystem memory_;
    fakes::FaultyFileSystem faulty_{memory_};
};

struct DiskKind {
    const char* name;
    std::function<std::unique_ptr<Disk>()> make;
};
// What ctest names each run by.
void PrintTo(const DiskKind& kind, std::ostream* os) { *os << kind.name; }

class FileSystemConformanceTest : public ::testing::TestWithParam<DiskKind> {
protected:
    void SetUp() override {
        disk_ = GetParam().make();
        root_ = disk_->Root();
        ASSERT_TRUE(Fs().CreateDirectories(root_));
    }
    void TearDown() override { disk_.reset(); }

    FileSystem& Fs() { return disk_->Fs(); }
    void Put(const std::filesystem::path& path, const std::string& text) {
        Fs().CreateDirectories(path.parent_path());
        ASSERT_EQ(Fs().WriteNewFile(path, text.data(), text.size()), FileSystem::WriteResult::Written) << path;
    }
    std::vector<std::string> NamesIn(const std::filesystem::path& dir) {
        std::vector<std::string> names;
        for (const FileSystem::Entry& entry : Fs().List(dir).value_or(std::vector<FileSystem::Entry>{})) {
            names.push_back(entry.name.string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    std::unique_ptr<Disk> disk_;
    std::filesystem::path root_;
};

TEST_P(FileSystemConformanceTest, WritesReadsAndListsFiles) {
    Put(root_ / "a" / "record.json", "{}");
    EXPECT_EQ(Fs().Status(root_ / "a"), Kind::Directory);
    EXPECT_EQ(Fs().Status(root_ / "a" / "record.json"), Kind::File);
    EXPECT_EQ(Fs().Read(root_ / "a" / "record.json", 100), "{}");
    EXPECT_EQ(Fs().FileSize(root_ / "a" / "record.json"), 2u);
    EXPECT_EQ(Fs().HardLinkCount(root_ / "a" / "record.json"), 1u);
    EXPECT_EQ(NamesIn(root_), std::vector<std::string>{"a"});
    EXPECT_EQ(Fs().Status(root_ / "missing"), Kind::None);
    EXPECT_FALSE(Fs().List(root_ / "missing").has_value());
    EXPECT_FALSE(Fs().List(root_ / "a" / "record.json").has_value()) << "a file is not listed";
}

// Whole or not at all: a file within the limit reads whole, however many
// reads that takes; one past it, or a directory, does not read.
TEST_P(FileSystemConformanceTest, ReadsAFileWholeWithinItsLimitAndNothingElse) {
    std::string big(200 * 1024, 'x');
    big.back() = 'y';
    Put(root_ / "big", big);
    EXPECT_EQ(Fs().Read(root_ / "big", big.size()), big);
    EXPECT_FALSE(Fs().Read(root_ / "big", big.size() - 1).has_value());
    Put(root_ / "empty", "");
    EXPECT_EQ(Fs().Read(root_ / "empty", 100), "");
    Fs().MakeDirectory(root_ / "dir");
    EXPECT_FALSE(Fs().Read(root_ / "dir", 100).has_value());
}

TEST_P(FileSystemConformanceTest, WritesANewFileOnlyWhereNothingIsAndItsDirectoryIs) {
    Put(root_ / "taken", "old");
    EXPECT_EQ(Fs().WriteNewFile(root_ / "taken", "new", 3), FileSystem::WriteResult::NameTaken);
    EXPECT_EQ(Fs().Read(root_ / "taken", 100), "old");
    Fs().MakeDirectory(root_ / "dir");
    EXPECT_EQ(Fs().WriteNewFile(root_ / "dir", "new", 3), FileSystem::WriteResult::NameTaken);
    EXPECT_EQ(Fs().WriteNewFile(root_ / "nowhere" / "file", "new", 3), FileSystem::WriteResult::Failed);
}

TEST_P(FileSystemConformanceTest, MakesADirectoryOnlyInOneThatExists) {
    EXPECT_FALSE(Fs().MakeDirectory(root_ / "no" / "parent"));
    EXPECT_TRUE(Fs().MakeDirectory(root_ / "one"));
    EXPECT_TRUE(Fs().MakeDirectory(root_ / "one")) << "already there";
    Put(root_ / "file", "x");
    EXPECT_FALSE(Fs().MakeDirectory(root_ / "file"));
}

TEST_P(FileSystemConformanceTest, RenameReplacesAFileButNeverADirectory) {
    Put(root_ / "from", "new");
    Put(root_ / "to", "old");
    EXPECT_TRUE(Fs().Rename(root_ / "from", root_ / "to"));
    EXPECT_EQ(Fs().Read(root_ / "to", 100), "new");
    EXPECT_EQ(Fs().Status(root_ / "from"), Kind::None);

    Put(root_ / "a" / "x", "1");
    Put(root_ / "b" / "y", "2");
    EXPECT_FALSE(Fs().Rename(root_ / "a", root_ / "b")) << "onto a directory with something in it";
    Fs().MakeDirectory(root_ / "empty");
    EXPECT_FALSE(Fs().Rename(root_ / "a", root_ / "empty")) << "onto an empty directory";
    EXPECT_FALSE(Fs().Rename(root_ / "to", root_ / "empty")) << "a file onto a directory";
    EXPECT_FALSE(Fs().Rename(root_ / "a", root_ / "a" / "inside")) << "under itself";
    EXPECT_FALSE(Fs().Rename(root_ / "missing", root_ / "c"));
    EXPECT_FALSE(Fs().Rename(root_ / "to", root_ / "nowhere" / "to"));

    EXPECT_TRUE(Fs().Rename(root_ / "a", root_ / "b" / "a")) << "a move to another parent";
    EXPECT_EQ(Fs().Read(root_ / "b" / "a" / "x", 100), "1");
}

TEST_P(FileSystemConformanceTest, RemovesAFileOrAnEmptyDirectoryAndCallsWhatIsNotThereGone) {
    Put(root_ / "d" / "f", "x");
    EXPECT_FALSE(Fs().Remove(root_ / "d"));
    EXPECT_TRUE(Fs().Remove(root_ / "d" / "f"));
    EXPECT_TRUE(Fs().Remove(root_ / "d"));
    EXPECT_TRUE(Fs().Remove(root_ / "d"));
    EXPECT_EQ(Fs().LinkStatus(root_ / "d"), Kind::None);
}

TEST_P(FileSystemConformanceTest, AHeldFileCanBeReadButNotRemovedRenamedOrReplaced) {
    if (!disk_->CanHold()) {
        GTEST_SKIP() << "only Windows refuses to delete an open file";
    }
    const std::filesystem::path held = root_ / "folder" / "canvas" / "picture.qoi";
    Put(held, "pixels");
    Put(root_ / "other", "other");
    disk_->Hold(held);

    EXPECT_EQ(Fs().Read(held, 100), "pixels");
    EXPECT_FALSE(Fs().Remove(held));
    EXPECT_FALSE(Fs().Rename(held, root_ / "moved.qoi"));
    EXPECT_FALSE(Fs().Rename(root_ / "other", held)) << "replaced";
    EXPECT_FALSE(Fs().Rename(root_ / "folder" / "canvas", root_ / "folder" / "renamed")) << "its directory";
    EXPECT_FALSE(Fs().Rename(root_ / "folder", root_ / "renamed")) << "a directory further up";
    EXPECT_EQ(Fs().Read(held, 100), "pixels");

    disk_->ReleaseAll();
    EXPECT_TRUE(Fs().Rename(root_ / "folder", root_ / "renamed"));
    EXPECT_TRUE(Fs().Remove(root_ / "renamed" / "canvas" / "picture.qoi"));
}

TEST_P(FileSystemConformanceTest, ALinkIsSeenAsALinkAndFollowedByEverythingElse) {
    Put(root_ / "elsewhere" / "file", "behind the link");
    Fs().MakeDirectory(root_ / "tree");
    disk_->MakeLink(root_ / "tree" / "link", root_ / "elsewhere");

    EXPECT_EQ(Fs().LinkStatus(root_ / "tree" / "link"), Kind::Link);
    EXPECT_EQ(Fs().Status(root_ / "tree" / "link"), Kind::Directory);
    ASSERT_TRUE(Fs().List(root_ / "tree").has_value());
    EXPECT_EQ(Fs().List(root_ / "tree")->at(0).kind, Kind::Link);
    EXPECT_EQ(Fs().Read(root_ / "tree" / "link" / "file", 100), "behind the link");
    EXPECT_EQ(Fs().LinkStatus(root_ / "tree" / "link" / "file"), Kind::File) << "through it, in the middle";

    Put(root_ / "tree" / "link" / "written", "through");
    EXPECT_EQ(Fs().Read(root_ / "elsewhere" / "written", 100), "through");

    EXPECT_TRUE(Fs().RemoveAll(root_ / "tree"));
    EXPECT_EQ(Fs().Read(root_ / "elsewhere" / "file", 100), "behind the link") << "never followed by RemoveAll";
}

// A file does not replace a link to a directory, any more than it replaces
// the directory: Windows refuses the rename.
TEST_P(FileSystemConformanceTest, AFileDoesNotReplaceALinkToADirectory) {
    Fs().MakeDirectory(root_ / "elsewhere");
    disk_->MakeLink(root_ / "link", root_ / "elsewhere");
    Put(root_ / "file", "a file");
    EXPECT_FALSE(Fs().Rename(root_ / "file", root_ / "link"));
    EXPECT_EQ(Fs().LinkStatus(root_ / "link"), Kind::Link);
    EXPECT_EQ(Fs().Read(root_ / "file", 100), "a file");
}

INSTANTIATE_TEST_SUITE_P(Disks, FileSystemConformanceTest,
                         ::testing::Values(DiskKind{"Real", [] { return std::unique_ptr<Disk>(new RealDisk()); }},
                                           DiskKind{"Model", [] { return std::unique_ptr<Disk>(new ModelDisk()); }}),
                         [](const ::testing::TestParamInfo<DiskKind>& info) { return std::string(info.param.name); });

}  // namespace
}  // namespace sz::core
