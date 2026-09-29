#include "core/config/config_migrations.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

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
// Version 2 (0.2.0): the edit-mode border's opacity went into its
// color's alpha, as the snippet colors have it - one swatch where there
// were a swatch and a slider. Version 1 drew the color's alpha times the
// opacity, so that product is the alpha written; a key missing or of the
// wrong kind counts as version 1's default, as version 1 read it. With
// neither key there, both are defaults, and version 2's default color is
// the same product.
void MigrateV1ToV2(nlohmann::ordered_json& doc) {
    const auto appearance = doc.find("appearance");
    if (appearance == doc.end() || !appearance->is_object()) {
        return;
    }
    const auto border = appearance->find("editModeBorder");
    if (border == appearance->end() || !border->is_object()) {
        return;
    }
    const auto opacityAt = border->find("opacity");
    const auto colorAt = border->find("color");
    if (opacityAt == border->end() && colorAt == border->end()) {
        return;
    }
    double opacity = 0.22;
    if (opacityAt != border->end() && opacityAt->is_number()) {
        opacity = std::clamp(opacityAt->get<double>(), 0.0, 1.0);
    }
    // "#RRGGBB", or "#RRGGBBAA"; white otherwise.
    uint32_t color = 0xFFFFFFFFu;
    if (colorAt != border->end() && colorAt->is_string()) {
        const std::string text = colorAt->get<std::string>();
        uint32_t packed = 0;
        const char* end = text.data() + text.size();
        if ((text.size() == 7 || text.size() == 9) && text[0] == '#' &&
            std::from_chars(text.data() + 1, end, packed, 16).ptr == end) {
            color = text.size() == 9 ? packed : (packed << 8) | 0xFFu;
        }
    }
    const auto alpha = static_cast<uint32_t>(std::lround(static_cast<double>(color & 0xFFu) * opacity));
    color = (color & 0xFFFFFF00u) | alpha;
    char text[10];
    if (alpha == 0xFFu) {
        std::snprintf(text, sizeof(text), "#%06X", static_cast<unsigned>(color >> 8));
    } else {
        std::snprintf(text, sizeof(text), "#%08X", static_cast<unsigned>(color));
    }
    (*border)["color"] = std::string(text);
    border->erase("opacity");
}

// kSteps[n - 1] takes version n to n + 1.
constexpr std::array<ConfigMigration, kConfigVersion - 1> kSteps{MigrateV1ToV2};

// Sized by kConfigVersion, so a version raised without its step still
// compiles - with a null step, called at the first older file read.
static_assert(std::ranges::none_of(kSteps, [](ConfigMigration step) { return step == nullptr; }),
              "every version below kConfigVersion needs its step in kSteps");

}  // namespace

std::span<const ConfigMigration> ConfigMigrations() { return kSteps; }

bool MigrateConfig(nlohmann::ordered_json& doc, int version, std::span<const ConfigMigration> steps) {
    try {
        for (size_t step = static_cast<size_t>(std::max(version, 1)) - 1; step < steps.size(); ++step) {
            steps[step](doc);
        }
    } catch (const nlohmann::ordered_json::exception&) {
        return false;
    }
    return true;
}

}  // namespace sz::core
