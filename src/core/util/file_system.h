#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sz::core {

// Every disk operation the library store and the atomic writer make, behind
// one interface - so that a test can put a file system under them that
// fails, holds a file open the way a picture viewer does, or stops partway
// through a save the way a crash does. The real one (RealFileSystem) is
// std::filesystem with an error_code on every call. Nothing here throws.
//
// Primitive on purpose: what is built from several steps - creating a
// directory with its parents, removing a tree, writing a file atomically
// (see atomic_file.h) - is built from these, so that a test can stop it
// between any two of them.
//
// No member is named after a Win32 function that windows.h turns into a
// macro (CreateFile, CreateDirectory, RemoveDirectory, MoveFile): a
// translation unit that sees windows.h first would call a different name.
class FileSystem {
public:
    enum class Kind {
        None,       // nothing there, or nothing that could be asked about
        File,
        Directory,
        Link,       // a symlink or a junction - only from LinkStatus and List
        Other,      // anything else: a socket, a device
    };
    struct Entry {
        std::filesystem::path name;  // the name alone, not the whole path
        Kind kind;                   // as LinkStatus would report it
    };
    enum class WriteResult {
        Written,
        NameTaken,  // something is already at the name; it was not opened
        Failed,     // nothing could be written; nothing new is left behind
    };

    virtual ~FileSystem() = default;

    // What `path` names, following a link to what it points at.
    virtual Kind Status(const std::filesystem::path& path) = 0;
    // What `path` itself is: Link for a symlink or junction, whatever it
    // points at.
    virtual Kind LinkStatus(const std::filesystem::path& path) = 0;
    virtual std::optional<uintmax_t> FileSize(const std::filesystem::path& path) = 0;
    // How many names the file has - more than one is a hard link, which
    // may be somebody else's file under its other name.
    virtual std::optional<uintmax_t> HardLinkCount(const std::filesystem::path& path) = 0;
    // Everything in `dir`, in no particular order, or nullopt if it could
    // not be listed whole: an error partway through is no listing, never
    // half of one.
    virtual std::optional<std::vector<Entry>> List(const std::filesystem::path& dir) = 0;
    // The whole file, or nullopt when it is missing, cannot be read, or is
    // larger than `maxBytes` - which is checked before anything is read.
    virtual std::optional<std::string> Read(const std::filesystem::path& path, uintmax_t maxBytes) = 0;

    // Creates one directory, its parent already there. True once a
    // directory is at `path`, made now or before.
    virtual bool MakeDirectory(const std::filesystem::path& path) = 0;
    // Creates `path` only if nothing is at it - whatever is there is never
    // opened - writes `size` bytes, and has the OS put them on the disk
    // itself before returning, not only in its cache.
    virtual WriteResult WriteNewFile(const std::filesystem::path& path, const void* data, size_t size) = 0;
    // Renames a file or directory; a file at `to` is replaced.
    virtual bool Rename(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
    // Removes a file, a link (never what it points at) or an empty
    // directory. True once nothing is at `path`, removed now or never there.
    virtual bool Remove(const std::filesystem::path& path) = 0;

    // Built from the above.
    bool Exists(const std::filesystem::path& path) { return Status(path) != Kind::None; }
    // `path` and every missing parent. True once a directory is at `path`.
    bool CreateDirectories(const std::filesystem::path& path);
    // `path` and everything under it. A link is removed as a link and never
    // followed, wherever in the tree it is. True once nothing is at `path`.
    bool RemoveAll(const std::filesystem::path& path);
};

class RealFileSystem final : public FileSystem {
public:
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
};

// The disk itself, for everything that is not handed another file system.
FileSystem& DefaultFileSystem();

}  // namespace sz::core
