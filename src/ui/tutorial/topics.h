#pragma once

// The tutorial's topics - docs/TUTORIAL.md, section 13: the chains a user
// picks from the tutorial's list, in the order the list shows them. A
// topic is a row here, a chain in chains.cpp and its strings under
// tutorial.topics.<id> in assets/ui_strings.json. No ImGui.

#include <string_view>
#include <vector>

#include "ui/tutorial/step.h"

namespace sz::ui::tutorial {

struct Topic {
    // Stable: the key of its progress in the settings file (section 13.7).
    std::string_view id;
    const char* title = "";
    // One line on what it covers, for the list.
    const char* gist = "";
    const std::vector<Step>& (*chain)() = nullptr;
    // Whether a run has a folder of its own (section 13.5). Profiles has
    // none: nothing in it is on a canvas (section 18.3).
    bool folder = true;
};

// The id of the topic a first run starts, and whose warnings a skip card
// shows until it is finished (section 13.6).
inline constexpr std::string_view kBasicsTopic = "basics";

const std::vector<Topic>& Topics();
// The topic `id` names, or null for one there is no longer.
const Topic* FindTopic(std::string_view id);

}  // namespace sz::ui::tutorial
