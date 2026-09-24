#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/util/file_system.h"

namespace sz::core::fakes {

// A file system held in memory, for tests that need the disk to misbehave
// (see FaultyFileSystem) or need thousands of runs to be fast. It behaves
// like Windows where the store depends on it:
//
//   - a directory is removed only when empty
//   - a rename replaces a file at the destination, never a directory, and
//     never moves a directory under itself
//   - a file is created only in a directory that exists
//   - a link (MakeLink - a junction, as far as the store can tell) is
//     reported as a link by LinkStatus and List, and followed by
//     everything else, in the middle of a path as at its end
//
// It does not model: case-insensitive names, hard links (every file has
// one name), permissions, or a file held open - that is FaultyFileSystem's.
// Every path is made absolute against the real current directory and then
// looked up here; nothing touches the real disk.
class MemoryFileSystem final : public FileSystem {
public:
    MemoryFileSystem();
    ~MemoryFileSystem() override;

    Kind Status(const std::filesystem::path& path) override;
    Kind LinkStatus(const std::filesystem::path& path) override;
    std::optional<uintmax_t> FileSize(const std::filesystem::path& path) override;
    std::optional<uintmax_t> HardLinkCount(const std::filesystem::path& path) override;
    std::optional<std::vector<Entry>> List(const std::filesystem::path& dir) override;
    std::optional<std::string> Read(const std::filesystem::path& path, uintmax_t maxBytes) override;
    bool MakeDirectory(const std::filesystem::path& path) override;
    WriteResult WriteNewFile(const std::filesystem::path& path, const void* data, size_t size) override;
    bool Rename(const std::filesystem::path& from, const std::filesystem::path& to) override;
    bool Remove(const std::filesystem::path& path) override;

    // For a test to set the scene: a link at `path` to `target`, which need
    // not exist; and a file with `text` in it, parents created, replacing
    // whatever file was there.
    bool MakeLink(const std::filesystem::path& path, const std::filesystem::path& target);
    bool Put(const std::filesystem::path& path, const std::string& text);
    // Every file under `root`, by path, with its contents - what a test
    // compares two disks by.
    std::map<std::filesystem::path, std::string> FilesUnder(const std::filesystem::path& root);

private:
    struct Node;
    // The node at `path`. Links on the way are followed; the last one only
    // when `followLast`.
    Node* Find(const std::filesystem::path& path, bool followLast, int depth = 0);
    // The directory `path` would be created in, links followed, and the
    // name it would have there - or null if there is no such directory.
    Node* ParentOf(const std::filesystem::path& path, std::filesystem::path& name);

    std::unique_ptr<Node> universe_;  // holds the roots ("C:\", "/") as its children
};

}  // namespace sz::core::fakes
