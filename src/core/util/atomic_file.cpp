#include "core/util/atomic_file.h"

#include <string>

namespace sz::core {

namespace {

// Whether what sits at `path` is a temporary of this app's own that a
// crash left behind: a plain file with no other name pointing at it. A
// hard link has two names and may be someone else's file under the second;
// a symlink, a junction or a directory is not a file of ours at all.
bool IsStaleTemporaryOfOurs(FileSystem& fs, const std::filesystem::path& path) {
    if (fs.LinkStatus(path) != FileSystem::Kind::File) {
        return false;
    }
    const std::optional<uintmax_t> links = fs.HardLinkCount(path);
    return links && *links == 1;
}

}  // namespace

bool WriteFileAtomically(FileSystem& fs, const std::filesystem::path& path, const void* data, size_t size) {
    fs.CreateDirectories(path.parent_path());

    // <file>.tmp, or the first of <file>.tmp1, .tmp2, ... that nothing is
    // at. A stale temporary of ours is removed and its name used; anything
    // else at a name is passed over. Bounded, so a directory full of things
    // at these names fails the write rather than searching forever.
    //
    // The temporary is on the disk before it is renamed over the old one
    // (see FileSystem::WriteNewFile). The rename is journaled and the data
    // is not, so without that a power cut could keep the rename and lose
    // the data: the file came back at its new length, full of zeros, with
    // the good version it replaced gone.
    std::filesystem::path tmp;
    bool written = false;
    for (int attempt = 0; attempt < 8 && !written; ++attempt) {
        tmp = path;
        tmp += attempt == 0 ? std::string(".tmp") : ".tmp" + std::to_string(attempt);
        FileSystem::WriteResult result = fs.WriteNewFile(tmp, data, size);
        if (result == FileSystem::WriteResult::NameTaken && IsStaleTemporaryOfOurs(fs, tmp)) {
            fs.Remove(tmp);
            result = fs.WriteNewFile(tmp, data, size);  // taken again meanwhile means passed over
        }
        if (result == FileSystem::WriteResult::Failed) {
            return false;  // no other name will do better
        }
        written = result == FileSystem::WriteResult::Written;
    }
    if (!written) {
        return false;
    }
    if (!fs.Rename(tmp, path)) {
        fs.Remove(tmp);  // nothing half-landed left beside the real file
        return false;
    }
    return true;
}

}  // namespace sz::core
