#include "core/util/atomic_file.h"

#include <cstdio>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sz::core {

namespace {

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

// Whether what sits at `path` is a temporary of this app's own that a
// crash left behind: a plain file with no other name pointing at it. A
// hard link has two names and may be someone else's file under the second;
// a symlink, a junction or a directory is not a file of ours at all.
bool IsStaleTemporaryOfOurs(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
        return false;
    }
    const auto links = std::filesystem::hard_link_count(path, ec);
    return !ec && links == 1;
}

}  // namespace

bool WriteFileAtomically(const std::filesystem::path& path, const void* data, size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // <file>.tmp, or the first of <file>.tmp1, .tmp2, ... that nothing is
    // at. A stale temporary of ours is removed and its name used; anything
    // else at a name is passed over. Bounded, so a directory full of things
    // at these names fails the write rather than searching forever.
    std::filesystem::path tmp;
    std::FILE* out = nullptr;
    for (int attempt = 0; attempt < 8 && !out; ++attempt) {
        tmp = path;
        tmp += attempt == 0 ? std::string(".tmp") : ".tmp" + std::to_string(attempt);
        out = CreateFresh(tmp);
        if (out) {
            break;
        }
        // Refused with nothing at the name is not a collision: the directory
        // itself is not writable, and no other name will do better. (Not
        // errno: a directory at the name is refused as EACCES, not EEXIST.)
        if (!std::filesystem::exists(std::filesystem::symlink_status(tmp, ec))) {
            return false;
        }
        if (IsStaleTemporaryOfOurs(tmp)) {
            std::filesystem::remove(tmp, ec);
            out = CreateFresh(tmp);  // taken again meanwhile means passed over
        }
    }
    if (!out) {
        return false;
    }

    bool ok = size == 0 || std::fwrite(data, 1, size, out) == size;
    ok = (std::fflush(out) == 0) && ok;
    // On the disk before it is renamed over the old one. The rename is
    // journaled and the data is not, so without this a power cut could
    // keep the rename and lose the data: the file came back at its new
    // length, full of zeros, with the good version it replaced gone.
    ok = ok && CommitToDisk(out);
    ok = (std::fclose(out) == 0) && ok;
    if (!ok) {
        std::filesystem::remove(tmp, ec);  // nothing half-written left beside the real file
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace sz::core
