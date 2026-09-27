#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace sz::test {

namespace temp_dir_detail {

inline constexpr const char* kVariable = "SPICKZETTEL_TESTS_TEMP";

// The folder a parent process handed down, or empty.
inline std::string Handed() {
#ifdef _WIN32
    char* value = nullptr;
    size_t length = 0;
    std::string handed;
    if (_dupenv_s(&value, &length, kVariable) == 0 && value != nullptr) {
        handed = value;
    }
    std::free(value);
    return handed;
#else
    const char* value = std::getenv(kVariable);
    return value != nullptr ? value : "";
#endif
}

struct Dir {
    std::filesystem::path path;
    bool owned = false;
    Dir() {
        if (const std::string handed = Handed(); !handed.empty()) {
            path = handed;
            return;
        }
#ifdef _WIN32
        const int pid = _getpid();
#else
        const int pid = static_cast<int>(getpid());
#endif
        path = std::filesystem::temp_directory_path() / ("spickzettel-tests-" + std::to_string(pid));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);  // left by a crashed run whose process id this one reuses
        std::filesystem::create_directories(path, ec);
        owned = true;
#ifdef _WIN32
        _putenv_s(kVariable, path.string().c_str());
#else
        setenv(kVariable, path.string().c_str(), 1);
#endif
    }
    ~Dir() {
        if (owned) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
};

}  // namespace temp_dir_detail

// This test program's own folder under the system's temporary one, named
// for its process, where every test puts its files - made on first use and
// removed when the program ends. Under the temporary folder itself, one
// fixed name per test, two runs at once - two worktrees - wrote each
// other's files.
//
// A process this one starts - a death test's child, which runs the test
// again - is handed the same folder through the environment, and leaves
// it to this one to remove.
inline std::filesystem::path TempDir() {
    static const temp_dir_detail::Dir dir;
    return dir.path;
}

}  // namespace sz::test
