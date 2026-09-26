#include "core/config/config_migrations.h"

#include <array>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace sz::core {
namespace {

using json = nlohmann::ordered_json;

// The chain itself, with steps of its own, since there is no real one yet:
// a file runs every step from its version on, in order, and none before.
// Each real step gets a test of its own beside this one, from a small
// document of its source version (docs/SETTINGS.md, section 8).
TEST(ConfigMigrationsTest, AFileRunsEveryStepFromItsVersionOnInOrder) {
    const std::array<ConfigMigration, 3> steps = {
        [](json& doc) { doc["ran"].push_back("1 to 2"); },
        [](json& doc) { doc["ran"].push_back("2 to 3"); },
        [](json& doc) { doc["ran"].push_back("3 to 4"); },
    };
    const auto migrated = [&steps](int version) {
        json doc = json::object();
        MigrateConfig(doc, version, steps);
        return doc.value("ran", json::array());
    };
    EXPECT_EQ(migrated(1), (json{"1 to 2", "2 to 3", "3 to 4"}));
    EXPECT_EQ(migrated(3), (json{"3 to 4"}));
    EXPECT_EQ(migrated(4), json::array()) << "already the last version";
    EXPECT_EQ(migrated(9), json::array()) << "a newer file is read as it is";
}

}  // namespace
}  // namespace sz::core
