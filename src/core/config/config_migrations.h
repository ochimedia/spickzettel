#pragma once

#include <span>

#include <nlohmann/json_fwd.hpp>

namespace sz::core {

// The settings file's version: the one this build writes, and the newest it
// reads as its own. A file of an older version is migrated before it is
// read (MigrateConfig); one of a newer version was written by a newer build
// and is read as well as this one can, but not written over (ConfigSource::
// Newer). A version is for what an older file would otherwise be read wrong
// in - a key renamed or moved, a value whose meaning changed. Adding a key
// or dropping one is not: an absent key reads as its default, and an
// unknown one is ignored. See docs/SETTINGS.md, section 8.
inline constexpr int kConfigVersion = 1;

// One step: a settings file of version n, as JSON, made into version n + 1.
using ConfigMigration = void (*)(nlohmann::ordered_json& doc);

// Every step there is, in order; the first takes version 1 to 2.
std::span<const ConfigMigration> ConfigMigrations();

// A file of `version` brought up to the end of `steps`, through every step
// from the one that takes `version` on: n reaches n + k by all k steps in
// between. A chain rather than a conversion from each old version to the
// current one, which would have every conversion rewritten at every new
// version, against a shape nobody had looked at in a while; a step is
// written once, while the two shapes it sits between are fresh.
void MigrateConfig(nlohmann::ordered_json& doc, int version,
                   std::span<const ConfigMigration> steps = ConfigMigrations());

}  // namespace sz::core
