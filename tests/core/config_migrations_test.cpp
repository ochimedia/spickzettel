#include "core/config/config_migrations.h"

#include <array>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace sz::core {
namespace {

using json = nlohmann::ordered_json;

// The chain itself, with steps of its own: a file runs every step from its
// version on, in order, and none before. Each real step gets a test of its
// own below, from a small document of its source version (docs/SETTINGS.md,
// section 8).
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

// Version 1 to 2: the edit-mode border's opacity goes into its color's
// alpha, as the color's alpha times the opacity, which is what version 1
// drew. What version 1 read as its default counts as it.
TEST(ConfigMigrationsTest, V1ToV2FoldsTheEditModeBordersOpacityIntoItsColor) {
    const auto migrated = [](json border) {
        json doc = {{"appearance", {{"editModeBorder", std::move(border)}}}};
        EXPECT_TRUE(MigrateConfig(doc, 1));
        return doc["appearance"]["editModeBorder"];
    };
    EXPECT_EQ(migrated({{"color", "#5AA9FF"}, {"opacity", 0.4}, {"width", 16.0}}),
              (json{{"color", "#5AA9FF66"}, {"width", 16.0}}));
    EXPECT_EQ(migrated({{"color", "#5AA9FF"}, {"opacity", 1.0}}), (json{{"color", "#5AA9FF"}}))
        << "opaque keeps six digits";
    EXPECT_EQ(migrated({{"color", "#5AA9FF80"}, {"opacity", 0.5}}), (json{{"color", "#5AA9FF40"}}));
    EXPECT_EQ(migrated({{"opacity", 0.5}}), (json{{"color", "#FFFFFF80"}})) << "white, version 1's default";
    EXPECT_EQ(migrated({{"color", "#000000"}}), (json{{"color", "#00000038"}})) << "0.22, version 1's default";
    EXPECT_EQ(migrated({{"color", 5}, {"opacity", "half"}}), (json{{"color", "#FFFFFF38"}}));
    EXPECT_EQ(migrated({{"color", "#12345"}, {"opacity", 3}}), (json{{"color", "#FFFFFF"}}));
    EXPECT_EQ(migrated({{"width", 16.0}}), (json{{"width", 16.0}})) << "both defaults: nothing to write";

    for (json untouched : {json::object(), json{{"appearance", "hand-edited"}},
                           json{{"appearance", {{"editModeBorder", 7}}}}}) {
        const json before = untouched;
        EXPECT_TRUE(MigrateConfig(untouched, 1));
        EXPECT_EQ(untouched, before);
    }
}

}  // namespace
}  // namespace sz::core
