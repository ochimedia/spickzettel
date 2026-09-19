#pragma once

#include <vector>

#include "platform/platform_types.h"

namespace sz::platform::win32 {

// Every active display, left to right (top to bottom where two share a
// column), as Win32PlatformHost::ListDisplays reports them.
//
// Two sources joined by GDI device name (\\.\DISPLAYn). EnumDisplayMonitors
// gives what the overlay needs to cover a display - where it is, how big,
// whether it is the primary - and, with GetDpiForMonitor, its scale.
// QueryDisplayConfig gives what a person and a saved setting need - the
// monitor's own name from its EDID and the device path it is remembered by
// - plus the refresh rate. Should the second fail, a display is still
// listed, named and identified by its GDI name, which is enough to put the
// overlay on it now even though it may not find the same monitor later.
std::vector<DisplayInfo> EnumerateDisplays();

}  // namespace sz::platform::win32
