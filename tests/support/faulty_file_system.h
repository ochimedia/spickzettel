#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/util/file_system.h"

namespace sz::core::fakes {

// Wraps another file system - a MemoryFileSystem, usually, or the real disk
// - and makes it misbehave on request, the three ways the disk has been
// seen to:
//
//   - a file held open by another program, as a picture viewer holds one on
//     Windows (Hold)
//   - an operation that just fails, once or every time (FailWhen)
//   - a crash: after some number of changes, the disk stops changing
//     (CrashAfter) - which is everything a crash leaves behind, since what
//     the process believed afterwards is gone with it
//
// Queries pass straight through. Every change - MakeDirectory,
// WriteNewFile, Rename, Remove - is counted as it is attempted, whether or
// not it succeeds, so that a test can crash between any two of them.
class FaultyFileSystem final : public FileSystem {
public:
    enum class Op { Read, List, MakeDirectory, WriteNewFile, Rename, Remove };

    explicit FaultyFileSystem(FileSystem& inner) : inner_(inner) {}

    // Another program has `path` open without delete sharing: it can still
    // be read, but it cannot be removed, renamed or replaced, and no
    // directory above it can be renamed. Matched by path, spelled any way
    // that normalizes to the same thing.
    void Hold(const std::filesystem::path& path);
    void Release(const std::filesystem::path& path);
    void ReleaseAll() { held_.clear(); }

    // `op` on a path `matches` accepts fails, with no effect, `times` times
    // - every time, while `times` is negative, until ClearFailures.
    void FailWhen(Op op, std::function<bool(const std::filesystem::path&)> matches, int times = -1);
    void ClearFailures() { failures_.clear(); }

    // Lets `changes` more changes through, counted from now; every one after
    // that does nothing and reports failure. The first one refused, if it
    // writes a file, leaves the first half of it: a crash in the middle of
    // writing. A disk that has crashed stays crashed until ClearCrash - the
    // process is gone, and asking for another crash does not bring it back.
    void CrashAfter(size_t changes) {
        if (!Crashed()) {
            crashAt_ = changesAttempted_ + changes;
        }
    }
    bool Crashed() const { return crashAt_ && changesAttempted_ > *crashAt_; }
    // The process that crashed is gone; a new one gets a disk that changes
    // again. What another program holds stays held.
    void ClearCrash() { crashAt_.reset(); }
    // The changes attempted so far - what a test counts a scenario's crash
    // points by.
    size_t ChangesAttempted() const { return changesAttempted_; }

    Kind Status(const std::filesystem::path& path) override { return inner_.Status(path); }
    Kind LinkStatus(const std::filesystem::path& path) override { return inner_.LinkStatus(path); }
    std::optional<uintmax_t> FileSize(const std::filesystem::path& path) override { return inner_.FileSize(path); }
    std::optional<uintmax_t> HardLinkCount(const std::filesystem::path& path) override {
        return inner_.HardLinkCount(path);
    }
    std::optional<std::vector<Entry>> List(const std::filesystem::path& dir) override;
    std::optional<std::string> Read(const std::filesystem::path& path, uintmax_t maxBytes) override;
    bool MakeDirectory(const std::filesystem::path& path) override;
    WriteResult WriteNewFile(const std::filesystem::path& path, const void* data, size_t size) override;
    bool Rename(const std::filesystem::path& from, const std::filesystem::path& to) override;
    bool Remove(const std::filesystem::path& path) override;

private:
    struct Failure {
        Op op;
        std::function<bool(const std::filesystem::path&)> matches;
        int times;
    };
    // Whether `op` on `path` is to fail now; counts it against its rule.
    bool Fails(Op op, const std::filesystem::path& path);
    // Counts a change, and whether it is past the crash.
    bool PastTheCrash();
    bool IsHeld(const std::filesystem::path& path) const;
    bool HoldsAnythingUnder(const std::filesystem::path& dir) const;

    FileSystem& inner_;
    std::set<std::filesystem::path> held_;  // normalized
    std::vector<Failure> failures_;
    size_t changesAttempted_ = 0;
    std::optional<size_t> crashAt_;
};

}  // namespace sz::core::fakes
