#pragma once

#include <cstdint>
#include <functional>

namespace sz::core {

// Identity for folders, canvases and items: a random number, unique within
// a library.
//
// Random rather than a counter: every counter-based library starts at 1,
// so two libraries built independently collide on nearly every id, and
// anything that ever moves things from one into the other would be a
// guaranteed conflict. Random ids make that a non-event.
//
// The space is 36^6 = 2,176,782,336, from when an id was also a
// directory's six-character base36 name; every library already written
// holds ids inside it. That is not the reason collisions don't happen,
// though - MakeUid checks. The size is what keeps the check from ever
// having to retry in practice.
constexpr uint64_t kUidSpace = 2176782336ull;  // 36^6

// A fresh id that `isTaken` says nothing holds yet, never 0 (which means
// "no id" throughout this codebase).
//
// The check is real, not a probabilistic hand-wave: with a large enough
// space it is tempting to draw once and assume.
//
// `randomBits` is injectable so a test can force the collision path by
// handing back a value that is already taken; the default draws from a
// thread-local Mersenne twister seeded from std::random_device.
uint64_t MakeUid(const std::function<bool(uint64_t)>& isTaken,
                  const std::function<uint64_t()>& randomBits = {});

}  // namespace sz::core
