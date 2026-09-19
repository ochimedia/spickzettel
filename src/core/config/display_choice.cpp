#include "core/config/display_choice.h"

#include <algorithm>

namespace sz::core {

platform::DisplayInfo ChooseDisplay(const std::vector<platform::DisplayInfo>& displays, std::string_view id,
                                    std::string_view name) {
    if (displays.empty()) {
        return platform::DisplayInfo{"", "", 0, 0, 1920, 1080, true};
    }
    if (!id.empty()) {
        const auto byId =
            std::find_if(displays.begin(), displays.end(), [&](const auto& display) { return display.id == id; });
        if (byId != displays.end()) {
            return *byId;
        }
        // Two monitors of the same model are two displays with one name, and
        // guessing between them would be wrong half the time - the primary
        // is at least the answer the user already knows.
        if (!name.empty() && std::count_if(displays.begin(), displays.end(),
                                           [&](const auto& display) { return display.name == name; }) == 1) {
            return *std::find_if(displays.begin(), displays.end(),
                                 [&](const auto& display) { return display.name == name; });
        }
    }
    const auto primary =
        std::find_if(displays.begin(), displays.end(), [](const auto& display) { return display.primary; });
    return primary != displays.end() ? *primary : displays.front();
}

}  // namespace sz::core
