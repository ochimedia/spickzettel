#pragma once

#include <string_view>
#include <vector>

#include "platform/platform_types.h"

namespace sz::core {

// Which of the attached displays the overlay goes on, for a display
// remembered by its `id` and `name` (see platform::DisplayInfo for what
// each is). Always answers something:
//
// - the display with that id;
// - failing that, the one display with that name, if exactly one has it -
//   the same monitor plugged into another port, which gives it a new id;
// - failing that, the primary display (the first one, if none says it is).
//
// An empty id means "the primary display" and is the default.
//
// With nothing attached at all - which no real backend reports, but an
// empty list costs nothing to survive - a 1920x1080 display at the origin,
// so there is always a size to make a window.
platform::DisplayInfo ChooseDisplay(const std::vector<platform::DisplayInfo>& displays, std::string_view id,
                                    std::string_view name);

}  // namespace sz::core
