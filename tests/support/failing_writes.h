#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <sqlite3.h>

namespace sz::test {

// Writes to a library made to fail on demand: a trigger on every table the
// library writes aborts the statement, and with it the write's transaction,
// while the table `failing` says so. Instant, where a lock held elsewhere
// (see HeldLibrary) would cost the store's busy timeout each time.
//
// FailAll fails every write. FailSnippet fails only a write of that
// snippet's own row - its record or its strokes, not the place a canvas's
// order gives it - or its picture: for a test that needs one command to
// fail and the next, about another snippet, to land.
class FailingWrites {
public:
    explicit FailingWrites(const std::filesystem::path& file) {
        const std::u8string name = file.u8string();
        sqlite3_open(std::string(name.begin(), name.end()).c_str(), &db_);
        const std::string all = "EXISTS (SELECT 1 FROM failing WHERE item IS NULL)";
        const auto about = [](const char* id) {
            return std::string("EXISTS (SELECT 1 FROM failing WHERE item IS NULL OR item = ") + id + ")";
        };
        std::string sql = "CREATE TABLE failing (item INTEGER);";
        const auto trigger = [&sql](const std::string& name, const std::string& when, const std::string& condition) {
            sql += "CREATE TRIGGER fail_" + name + " BEFORE " + when + " WHEN " + condition +
                   " BEGIN SELECT RAISE(ABORT, 'failing'); END;";
        };
        for (const char* table : {"folders", "canvases", "meta"}) {
            for (const char* change : {"INSERT", "UPDATE", "DELETE"}) {
                trigger(std::string(change) + "_" + table, std::string(change) + " ON " + table, all);
            }
        }
        trigger("insert_items", "INSERT ON items", about("NEW.id"));
        trigger("update_items", "UPDATE ON items", all);
        trigger("update_item_content", "UPDATE OF record, strokes ON items", about("NEW.id"));
        trigger("delete_items", "DELETE ON items", about("OLD.id"));
        trigger("insert_pictures", "INSERT ON pictures", about("NEW.item_id"));
        trigger("update_pictures", "UPDATE ON pictures", about("NEW.item_id"));
        trigger("delete_pictures", "DELETE ON pictures", about("OLD.item_id"));
        Exec(sql);
    }
    ~FailingWrites() { sqlite3_close(db_); }
    FailingWrites(const FailingWrites&) = delete;
    FailingWrites& operator=(const FailingWrites&) = delete;

    void FailAll() { Exec("DELETE FROM failing; INSERT INTO failing VALUES (NULL)"); }
    void FailSnippet(uint64_t id) {
        Exec("DELETE FROM failing; INSERT INTO failing VALUES (" + std::to_string(id) + ")");
    }
    void Stop() { Exec("DELETE FROM failing"); }
    void Set(bool failing) { failing ? FailAll() : Stop(); }

private:
    void Exec(const std::string& sql) {
        ASSERT_EQ(sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);
    }
    sqlite3* db_ = nullptr;
};

}  // namespace sz::test
