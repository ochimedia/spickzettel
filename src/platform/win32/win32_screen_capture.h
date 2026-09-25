#pragma once

#include <cstdint>
#include <vector>

#include <windows.h>

namespace sz::platform::win32 {

// The desktop as composited - what is on screen, other applications' own
// translucent windows included - `width` by `height` from `origin` in
// screen coordinates, as top-down BGRA rows. Without `exclude` in it: the
// overlay, which is on top of everything it would capture. Null when
// nothing is to be left out. False, and `pixelsBGRA` unspecified, when the
// screen could not be read whole.
//
// The window is left out by excluding it from capture for the moment it
// takes (WDA_EXCLUDEFROMCAPTURE), not by hiding it. Hiding an active window
// hands activation to the one underneath - the game - and showing it again
// takes it back: a focus gained and lost that the game had no part in, and
// that some games pause on. Where the system cannot exclude a window from
// capture (before Windows 10 2004) it is hidden for the moment instead.
bool CaptureScreen(HWND exclude, POINT origin, int width, int height, std::vector<uint8_t>& pixelsBGRA);

}  // namespace sz::platform::win32
