#pragma once

#include <string>

namespace sz::core {

// The local date and time, "2026-09-07 22:36:14" - what a folder or canvas
// is called until someone renames it. A counted "Folder N" says nothing
// about which is which a week later; when it was made at least narrows
// that down. Two made in the same second share a name, which is allowed -
// names are not identity here.
std::string TimestampName();

}  // namespace sz::core
