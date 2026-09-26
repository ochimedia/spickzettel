#include "core/config/config_migrations.h"

#include <algorithm>
#include <array>

#include <nlohmann/json.hpp>

namespace sz::core {
namespace {

// What keeps a chain sound (docs/SETTINGS.md, section 8):
//
// - A step works on the JSON, with the keys spelled out as its two
//   versions have them. It calls nothing of the current build's - not the
//   catalog, not the parser - since those change, and a step that used
//   them would quietly change meaning with them.
// - A step is never edited once a build that writes its target version
//   has been released. A fix is a new step.
// - There are no steps down. A newer file is read as it is.
//
// kSteps[n - 1] takes version n to n + 1. None yet: version 1 is the
// first, and every build so far has written it.
constexpr std::array<ConfigMigration, kConfigVersion - 1> kSteps{};

}  // namespace

std::span<const ConfigMigration> ConfigMigrations() { return kSteps; }

void MigrateConfig(nlohmann::ordered_json& doc, int version, std::span<const ConfigMigration> steps) {
    for (size_t step = static_cast<size_t>(std::max(version, 1)) - 1; step < steps.size(); ++step) {
        steps[step](doc);
    }
}

}  // namespace sz::core
