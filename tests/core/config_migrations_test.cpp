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

// A step that throws on a shape it did not expect - a hand-edited file
// with a string where it goes into an object - fails the migration, which
// sets the file aside as not settings. It went through TryParseConfig, and
// nothing caught it: the app ended at every start.
TEST(ConfigMigrationsTest, AStepThatThrowsFailsTheMigration) {
    const std::array<ConfigMigration, 2> steps = {
        [](json& doc) { doc["hotkeys"]["editMode"] = "Ctrl+Alt+E"; },
        [](json& doc) { doc["ran"] = true; },
    };
    json doc = {{"hotkeys", "hand-edited"}};
    EXPECT_FALSE(MigrateConfig(doc, 1, steps));
    EXPECT_FALSE(doc.contains("ran")) << "no step after it";

    json fine = {{"hotkeys", json::object()}};
    EXPECT_TRUE(MigrateConfig(fine, 1, steps));
}

}  // namespace
}  // namespace sz::core
