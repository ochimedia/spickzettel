#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

#include "core/util/file_system.h"

namespace sz::core {

// Writes `size` bytes at `data` to `path` through a fresh temporary file
// beside it, renamed over `path` once every byte is down - so a reader
// (this same process after a crash included) never sees a half-written
// file; rename() is atomic on the same filesystem on Windows and Linux.
// The one way this app writes a file: every record, every picture, the
// settings file.
//
// Fresh means created exclusively, under a name nothing was at: a file
// already at `<path>.tmp` is never opened, let alone truncated. What is
// there is either a temporary of ours that a crash left behind - a plain
// file with no other name, which is removed and the name used - or
// something else: a hard link to a file elsewhere, a symlink, a directory,
// all of which are passed over for the next name (`<path>.tmp1`, ...). The
// first version opened `<path>.tmp` with truncation and trusted whatever
// was there; a hard link at that name to a file outside the library had
// the library's bytes written into that file.
//
// Returns false, and leaves nothing new behind, when the temporary cannot
// be created, written whole, or renamed into place. Creates `path`'s
// parent directories as needed.
//
// Through `fs`, or the disk itself when none is given.
bool WriteFileAtomically(FileSystem& fs, const std::filesystem::path& path, const void* data, size_t size);
inline bool WriteFileAtomically(FileSystem& fs, const std::filesystem::path& path, std::string_view content) {
    return WriteFileAtomically(fs, path, content.data(), content.size());
}
inline bool WriteFileAtomically(const std::filesystem::path& path, const void* data, size_t size) {
    return WriteFileAtomically(DefaultFileSystem(), path, data, size);
}
inline bool WriteFileAtomically(const std::filesystem::path& path, std::string_view content) {
    return WriteFileAtomically(DefaultFileSystem(), path, content.data(), content.size());
}

}  // namespace sz::core
