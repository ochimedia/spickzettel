#pragma once

// The welcome chain - docs/TUTORIAL.md, section 4: what a first run is
// led through, one row per step. A step is added here, with its strings
// in assets/ui_strings.json under tutorial.<id>.

#include <vector>

#include "ui/tutorial/step.h"

namespace sz::ui::tutorial {

// Built once, and lives as long as the program.
const std::vector<Step>& WelcomeChain();

}  // namespace sz::ui::tutorial
