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

// Whether what sits at `path` is a temporary of this app's own that a
// crash left behind: a plain file with no other name pointing at it. A
// hard link has two names and may be someone else's file under the second;
// a symlink, a junction or a directory is not a file of ours at all.
bool IsStaleTemporaryOfOurs(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)) || ec) {
        return false;
    }
    return std::filesystem::hard_link_count(path, ec) == 1 && !ec;
}

enum class WriteResult {
    Written,
    NameTaken,  // something is already at the name; it was not opened
    Failed,     // nothing could be written; nothing new is left behind
};

// Creates `path` only if nothing is at it - C11's "x", which both CRTs
// honor, so that whatever is there is never opened - writes `size` bytes,
// and has the OS put them on the disk itself before returning, not only in
// its cache.
//
// _wfopen_s rather than _wfopen, which MSVC deprecates (C4996). It also
// opens the file unshared, which suits a temporary that nothing else has
// any business reading before it is renamed into place.
WriteResult WriteNewFile(const std::filesystem::path& path, const void* data, size_t size) {
#if defined(_WIN32)
    std::FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), L"wbx") != 0) {
        out = nullptr;
    }
#else
    std::FILE* out = std::fopen(path.c_str(), "wbx");
#endif
    if (!out) {
        // Refused with nothing at the name is not a collision: the directory
        // itself is not writable. (Not errno: a directory at the name is
        // refused as EACCES, not EEXIST.)
        std::error_code ec;
        return std::filesystem::symlink_status(path, ec).type() != std::filesystem::file_type::not_found && !ec
                   ? WriteResult::NameTaken
                   : WriteResult::Failed;
    }
    bool ok = size == 0 || std::fwrite(data, 1, size, out) == size;
    ok = (std::fflush(out) == 0) && ok;
#if defined(_WIN32)
    ok = ok && _commit(_fileno(out)) == 0;  // FlushFileBuffers on the handle
#else
    ok = ok && fsync(fileno(out)) == 0;
#endif
    ok = (std::fclose(out) == 0) && ok;
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(path, ec);  // nothing half-written left behind
        return WriteResult::Failed;
    }
    return WriteResult::Written;
}

}  // namespace

bool WriteFileAtomically(const std::filesystem::path& path, const void* data, size_t size) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    // <file>.tmp, or the first of <file>.tmp1, .tmp2, ... that nothing is
    // at. A stale temporary of ours is removed and its name used; anything
    // else at a name is passed over. Bounded, so a directory full of things
    // at these names fails the write rather than searching forever.
    //
    // The temporary is on the disk before it is renamed over the old one
    // (see WriteNewFile). The rename is journaled and the data
    // is not, so without that a power cut could keep the rename and lose
    // the data: the file came back at its new length, full of zeros, with
    // the good version it replaced gone.
    std::filesystem::path tmp;
    bool written = false;
    for (int attempt = 0; attempt < 8 && !written; ++attempt) {
        tmp = path;
        tmp += attempt == 0 ? std::string(".tmp") : ".tmp" + std::to_string(attempt);
        WriteResult result = WriteNewFile(tmp, data, size);
        if (result == WriteResult::NameTaken && IsStaleTemporaryOfOurs(tmp)) {
            std::filesystem::remove(tmp, ec);
            result = WriteNewFile(tmp, data, size);  // taken again meanwhile means passed over
        }
        if (result == WriteResult::Failed) {
            return false;  // no other name will do better
        }
        written = result == WriteResult::Written;
    }
    if (!written) {
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);  // nothing half-landed left beside the real file
        return false;
    }
    return true;
}

}  // namespace sz::core
