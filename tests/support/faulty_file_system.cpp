#include "support/faulty_file_system.h"

#include <system_error>

namespace sz::core::fakes {

namespace {

std::filesystem::path Normalize(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    std::filesystem::path normal = absolute.lexically_normal();
    if (normal.filename().empty() && normal.has_relative_path()) {
        normal = normal.parent_path();
    }
    return normal;
}

bool IsStrictlyUnder(const std::filesystem::path& path, const std::filesystem::path& dir) {
    const std::filesystem::path relative = path.lexically_relative(dir);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}

}  // namespace

void FaultyFileSystem::Hold(const std::filesystem::path& path) { held_.insert(Normalize(path)); }

void FaultyFileSystem::Release(const std::filesystem::path& path) { held_.erase(Normalize(path)); }

void FaultyFileSystem::FailWhen(Op op, std::function<bool(const std::filesystem::path&)> matches, int times) {
    failures_.push_back({op, std::move(matches), times});
}

bool FaultyFileSystem::Fails(Op op, const std::filesystem::path& path) {
    for (Failure& failure : failures_) {
        if (failure.op != op || failure.times == 0 || !failure.matches(path)) {
            continue;
        }
        if (failure.times > 0) {
            --failure.times;
        }
        return true;
    }
    return false;
}

bool FaultyFileSystem::PastTheCrash() {
    ++changesAttempted_;
    return crashAt_ && changesAttempted_ > *crashAt_;
}

bool FaultyFileSystem::IsHeld(const std::filesystem::path& path) const { return held_.count(Normalize(path)) > 0; }

bool FaultyFileSystem::HoldsAnythingUnder(const std::filesystem::path& dir) const {
    const std::filesystem::path normal = Normalize(dir);
    for (const std::filesystem::path& held : held_) {
        if (IsStrictlyUnder(held, normal)) {
            return true;
        }
    }
    return false;
}

std::optional<std::vector<FileSystem::Entry>> FaultyFileSystem::List(const std::filesystem::path& dir) {
    if (Fails(Op::List, dir)) {
        return std::nullopt;
    }
    return inner_.List(dir);
}

std::optional<std::string> FaultyFileSystem::Read(const std::filesystem::path& path, uintmax_t maxBytes) {
    if (Fails(Op::Read, path)) {
        return std::nullopt;
    }
    return inner_.Read(path, maxBytes);
}

bool FaultyFileSystem::MakeDirectory(const std::filesystem::path& path) {
    if (PastTheCrash() || Fails(Op::MakeDirectory, path)) {
        return false;
    }
    return inner_.MakeDirectory(path);
}

FileSystem::WriteResult FaultyFileSystem::WriteNewFile(const std::filesystem::path& path, const void* data,
                                                       size_t size) {
    if (PastTheCrash()) {
        // The one change the crash interrupts leaves its first half behind.
        if (changesAttempted_ == *crashAt_ + 1) {
            inner_.WriteNewFile(path, data, size / 2);
        }
        return WriteResult::Failed;
    }
    if (Fails(Op::WriteNewFile, path)) {
        return WriteResult::Failed;
    }
    return inner_.WriteNewFile(path, data, size);
}

bool FaultyFileSystem::Rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (PastTheCrash() || Fails(Op::Rename, from)) {
        return false;
    }
    // Windows refuses to rename a file that is open without delete sharing,
    // to replace one, or to rename any directory it is in.
    if (IsHeld(from) || IsHeld(to) || HoldsAnythingUnder(from)) {
        return false;
    }
    return inner_.Rename(from, to);
}

bool FaultyFileSystem::Remove(const std::filesystem::path& path) {
    if (PastTheCrash() || Fails(Op::Remove, path)) {
        return false;
    }
    if (IsHeld(path)) {
        return false;
    }
    return inner_.Remove(path);
}

}  // namespace sz::core::fakes
