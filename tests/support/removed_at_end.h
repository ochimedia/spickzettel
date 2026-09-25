#pragma once

#include <filesystem>
#include <system_error>

namespace sz::test {

// Removes a directory when it goes out of scope. Declared before whatever
// holds a file in it open - a library store, say - so that it goes after
// them: Windows does not remove a file that is open.
class RemovedAtEnd {
public:
    explicit RemovedAtEnd(std::filesystem::path dir) : dir_(std::move(dir)) {}
    ~RemovedAtEnd() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    RemovedAtEnd(const RemovedAtEnd&) = delete;
    RemovedAtEnd& operator=(const RemovedAtEnd&) = delete;

private:
    std::filesystem::path dir_;
};

}  // namespace sz::test
