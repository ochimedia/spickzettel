#pragma once

// The tutorial's chains - docs/TUTORIAL.md, sections 4 and 13: one per
// topic, one row per step. A step is added to its chain here, with its
// strings in assets/ui_strings.json under tutorial.<id>; a chain, as a
// topic, in topics.cpp.

#include <vector>

#include "ui/tutorial/step.h"

namespace sz::ui::tutorial {

// Each built once, and lives as long as the program.
//
// Basics: a screenshot, moving and resizing it, deleting it and undoing
// that, the two warnings, and the overlay put away and back.
const std::vector<Step>& BasicsChain();
// Drawing: drawing mode, a stroke, and out of it again.
const std::vector<Step>& DrawingChain();
// Pinning and view mode: a pin, the pinned view, see-through, view mode
// and the pin taken off.
const std::vector<Step>& PinningChain();
// Capturing: a drawing, a screenshot of the whole screen, and the quick
// and silent capture hotkeys.
const std::vector<Step>& CapturingChain();
// Folders and canvases: a new canvas, a snippet moved to it, the
// Overview, a folder made, renamed and gone back from, a canvas sorted
// into it, and a canvas deleted and restored from the trash.
const std::vector<Step>& FoldersChain();

}  // namespace sz::ui::tutorial
