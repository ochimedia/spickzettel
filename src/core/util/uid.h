#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace sz::core {

// Identity for folders, canvases and items: a random number rendered as six
// base36 characters ("a7k2q9"), unique within a library.
//
// Random rather than a counter: every counter-based library starts at 1,
// so two libraries built independently collide on nearly every id, and
// moving a canvas directory from one into another - which the on-disk tree
// is meant to allow - would be a guaranteed conflict. Random ids make that
// a non-event.
//
// Six characters is 36^6 = 2,176,782,336. That is not the reason collisions
// don't happen, though - MakeUid checks. The size is what keeps the check
// from ever having to retry in practice, and what makes an unchecked merge
// of two hand-copied directories overwhelmingly likely to be clean.
constexpr size_t kUidLength = 6;
constexpr uint64_t kUidSpace = 2176782336ull;  // 36^6

// Six base36 characters, zero-padded ("000001", "a7k2q9"). Values at or
// above kUidSpace are wrapped rather than rejected: this is a rendering
// function, and the only source of ids is MakeUid, which cannot produce one.
std::string FormatUid(uint64_t id);

// The inverse, or nullopt if `text` isn't exactly kUidLength base36
// characters. Lowercase only - see the header comment on why the alphabet
// has no uppercase in it.
std::optional<uint64_t> ParseUid(std::string_view text);

// A fresh id that `isTaken` says nothing holds yet, never 0 (which means
// "no id" throughout this codebase).
//
// The check is real, not a probabilistic hand-wave: with a large enough
// space it is tempting to draw once and assume, and that assumption is
// exactly what fails once someone has copied a directory by hand.
//
// `randomBits` is injectable so a test can force the collision path by
// handing back a value that is already taken; the default draws from a
// thread-local Mersenne twister seeded from std::random_device.
uint64_t MakeUid(const std::function<bool(uint64_t)>& isTaken,
                  const std::function<uint64_t()>& randomBits = {});

}  // namespace sz::core
