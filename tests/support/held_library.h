#pragma once

#include <filesystem>
#include <string>

#include <sqlite3.h>

namespace sz::test {

// Another program holding the library file - a backup tool, say - from the
// moment this is made until it lets go. With `readers`, one that is writing
// to it: others may still read, and nobody else may write. Without, one
// that shares it with nobody.
//
// A store that has not opened the file yet cannot open it past a hold
// without readers, and says so (see LibraryStore::Open) - open it first
// when the point is what a save does.
class HeldLibrary {
public:
    HeldLibrary(const std::filesystem::path& file, bool readers) {
        const std::u8string name = file.u8string();
        sqlite3_open(std::string(name.begin(), name.end()).c_str(), &db_);
        sqlite3_exec(db_, readers ? "BEGIN IMMEDIATE" : "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr);
    }
    ~HeldLibrary() { Release(); }
    HeldLibrary(const HeldLibrary&) = delete;
    HeldLibrary& operator=(const HeldLibrary&) = delete;

    void Release() {
        if (db_ != nullptr) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

private:
    sqlite3* db_ = nullptr;
};

}  // namespace sz::test
