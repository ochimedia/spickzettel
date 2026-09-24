#include "core/util/file_system.h"

#include <cstdio>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sz::core {

namespace {

FileSystem::Kind KindOf(const std::filesystem::file_status& status) {
    switch (status.type()) {
        case std::filesystem::file_type::regular:
            return FileSystem::Kind::File;
        case std::filesystem::file_type::directory:
            return FileSystem::Kind::Directory;
        case std::filesystem::file_type::symlink:
            return FileSystem::Kind::Link;
#if defined(_MSC_VER)
        // A junction - what "mklink /J" makes, the reparse point Windows
        // lets a user create without privileges. file_type::junction is
        // MSVC's own extension; other standard libraries have no junctions
        // to report.
        case std::filesystem::file_type::junction:
            return FileSystem::Kind::Link;
#endif
        case std::filesystem::file_type::none:
        case std::filesystem::file_type::not_found:
            return FileSystem::Kind::None;
        default:
            return FileSystem::Kind::Other;
    }
}

// Opens `path` for writing only if nothing is there - C11's "x", which
// both CRTs honor - so that what is there is never opened, whatever it
// is. Null when the name is taken.
//
// _wfopen_s rather than _wfopen, which MSVC deprecates (C4996). It also
// opens the file unshared, which suits a temporary that nothing else has
// any business reading before it is renamed into place.
std::FILE* CreateFresh(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.c_str(), L"wbx") == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), "wbx");
#endif
}

// Asks the OS to put what was written on the disk itself, not only in its
// cache - see WriteFileAtomically for why before the rename. fflush first:
// this only reaches what the CRT has already handed down.
bool CommitToDisk(std::FILE* file) {
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;  // FlushFileBuffers on the handle
#else
    return fsync(fileno(file)) == 0;
#endif
}

}  // namespace

bool FileSystem::CreateDirectories(const std::filesystem::path& path) {
    if (path.empty()) {
        return true;  // the parent of a bare name: the current directory
    }
    const Kind kind = Status(path);
    if (kind != Kind::None) {
        return kind == Kind::Directory;
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty() && parent != path && !CreateDirectories(parent)) {
        return false;
    }
    return MakeDirectory(path);
}

bool FileSystem::RemoveAll(const std::filesystem::path& path) {
    const Kind kind = LinkStatus(path);
    if (kind == Kind::None) {
        return true;
    }
    if (kind == Kind::Directory) {
        const std::optional<std::vector<Entry>> entries = List(path);
        if (!entries) {
            return false;
        }
        bool emptied = true;
        for (const Entry& entry : *entries) {
            emptied = RemoveAll(path / entry.name) && emptied;
        }
        if (!emptied) {
            return false;
        }
    }
    return Remove(path);
}

FileSystem::Kind RealFileSystem::Status(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    return ec ? Kind::None : KindOf(status);
}

FileSystem::Kind RealFileSystem::LinkStatus(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    return ec ? Kind::None : KindOf(status);
}

std::optional<uintmax_t> RealFileSystem::FileSize(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? std::nullopt : std::optional<uintmax_t>(size);
}

std::optional<uintmax_t> RealFileSystem::HardLinkCount(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t links = std::filesystem::hard_link_count(path, ec);
    return ec ? std::nullopt : std::optional<uintmax_t>(links);
}

std::optional<std::vector<FileSystem::Entry>> RealFileSystem::List(const std::filesystem::path& dir) {
    // increment(ec) rather than a range-for: the iterator's operator++
    // throws on an error partway through, and one did take the app down.
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return std::nullopt;
    }
    std::vector<Entry> entries;
    while (it != std::filesystem::directory_iterator()) {
        std::error_code kindEc;
        const std::filesystem::file_status status = it->symlink_status(kindEc);
        entries.push_back({it->path().filename(), kindEc ? Kind::None : KindOf(status)});
        it.increment(ec);
        if (ec) {
            return std::nullopt;
        }
    }
    return entries;
}

std::optional<std::string> RealFileSystem::Read(const std::filesystem::path& path, uintmax_t maxBytes) {
    const std::optional<uintmax_t> size = FileSize(path);
    if (!size || *size > maxBytes) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes(static_cast<size_t>(*size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<uintmax_t>(in.gcount()) != *size) {
        return std::nullopt;
    }
    return bytes;
}

bool RealFileSystem::MakeDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directory(path, ec);
    return Status(path) == Kind::Directory;
}

FileSystem::WriteResult RealFileSystem::WriteNewFile(const std::filesystem::path& path, const void* data,
                                                     size_t size) {
    std::FILE* out = CreateFresh(path);
    if (!out) {
        // Refused with nothing at the name is not a collision: the directory
        // itself is not writable. (Not errno: a directory at the name is
        // refused as EACCES, not EEXIST.)
        return LinkStatus(path) != Kind::None ? WriteResult::NameTaken : WriteResult::Failed;
    }
    bool ok = size == 0 || std::fwrite(data, 1, size, out) == size;
    ok = (std::fflush(out) == 0) && ok;
    ok = ok && CommitToDisk(out);
    ok = (std::fclose(out) == 0) && ok;
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(path, ec);  // nothing half-written left behind
        return WriteResult::Failed;
    }
    return WriteResult::Written;
}

bool RealFileSystem::Rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
}

bool RealFileSystem::Remove(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

FileSystem& DefaultFileSystem() {
    static RealFileSystem disk;
    return disk;
}

}  // namespace sz::core
