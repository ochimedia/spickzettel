#pragma once

#include <string>
#include <string_view>

// kVersion and the build flags come through here so callers need one include.
#include "generated/build_config.h"

namespace sz::core::build {

// `git describe --tags --always --dirty` for the tree this was built from,
// or "unknown" without git or without a .git directory. A function rather
// than a constant: it changes with every commit, and the header holding it
// is included by exactly one translation unit so a commit rebuilds one
// file, not everything that wants the version number.
std::string_view GitDescribe();

// ABOUT.md, compiled in - shown on the Overview's About tab. Same
// one-translation-unit reasoning: it is the largest thing here and changes
// on its own schedule.
std::string_view AboutText();

// THIRD-PARTY-NOTICES.md, compiled in: the licences of everything inside
// the binary, in the binary because that is what those licences ask for.
std::string_view NoticesText();

// What to show a person: the version, plus the git description when it
// says something the version does not. An exact release build reads
// "0.1.0"; a test build reads "0.1.0 (v0.1.0-12-gabc1234-dirty)". A demo
// build is suffixed " - demo", a prerelease " - prerelease", and one that
// is both " - demo, prerelease".
std::string VersionLine();

}  // namespace sz::core::build
