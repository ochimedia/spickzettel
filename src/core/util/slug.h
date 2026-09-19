#pragma once

#include <cstdint>
#include <string>

namespace sz::core {

// Turns `name` + `id` into a filesystem-safe, human-readable directory name
// for Windows, Linux and macOS alike - e.g. MakeSlug("Boss Room!", id) ->
// "boss-room-a7k2q9". The readable half is for whoever browses the
// library; the trailing uid (see util/uid.h) is the identity, and is what
// keeps two things with the same name apart without a collision-retry
// loop. It also rules out ever colliding with a bare Windows-reserved
// device name (CON, NUL, COM1, ...), since those are exact-match checks
// and every slug carries the trailing "-<uid>".
//
// Recomputed from the current name on every save, not stored: the
// directory is renamed to follow the name, and the uid is what says it is
// still the same directory - see LibraryStore, which is the only caller.
std::string MakeSlug(const std::string& name, uint64_t id);

}  // namespace sz::core
