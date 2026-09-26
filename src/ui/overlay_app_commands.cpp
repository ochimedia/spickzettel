#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Menu rows =================

std::string OverlayApp::MenuShortcutLabel(CommandId id) const {
    // Live, not Stored: what is bound right now, profile and all, is what
    // the row has to promise.
    const std::vector<platform::KeyCombo> keys = KeysFor(id, Cfg(), settings_.Live().shortcuts);
    return keys.empty() ? std::string() : FormatKeyComboLabel(keys.front());
}

ContextMenuEntry OverlayApp::MenuRow(const Command& command, const char* id, const Icon* icon, const char* label,
                                     bool separatorAbove) const {
    return ContextMenuEntry{static_cast<int>(command.id), id, icon, label, MenuShortcutLabel(command.id),
                            Available(command), separatorAbove};
}

}  // namespace sz::ui
