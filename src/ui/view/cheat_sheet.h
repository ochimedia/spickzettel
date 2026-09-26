#pragma once

// The cheat sheet - docs/VIEW_LAYER.md, section 7: every key and gesture,
// grouped, with the keys as they are bound. A panel over a dimmed canvas
// like the Overview, on the machine's Panel level, but with nothing in it
// to click: Escape, its own key again, or a click outside it closes it. It
// keeps nothing of its own.

#include <string>
#include <vector>

#include "core/config/app_config.h"
#include "core/config/shortcut_action.h"
#include "core/session/settings.h"
#include "ui/editor.h"
#include "ui/view/view_host.h"

namespace sz::ui {

// The cheat sheet's content, apart from drawing it so the tests can read
// it: groups of rows, each what to press and what it does. A row with no
// keys is a line of context for the rows under it ("On empty canvas").
// Built from the bindings as they are - the global hotkeys, the tool
// shortcuts `shortcuts` resolves to, the creation triggers - so a rebound
// key reads as bound, and an unbound one, or a trigger set to Off, drops
// its row rather than promising nothing.
struct CheatSheetRow {
    std::string keys;
    std::string what;
};
struct CheatSheetSection {
    const char* title;
    std::vector<CheatSheetRow> rows;
};
std::vector<CheatSheetSection> BuildCheatSheet(const core::AppConfig& config, const core::ShortcutBindings& shortcuts);

class CheatSheet {
public:
    CheatSheet(core::Settings& settings, Editor& editor, ViewHost& host);

    // Whether it is up - the machine's Panel level holds it.
    bool IsOpen() const;
    // Up if it was not, and put away if it was - its own key.
    void Toggle();
    // Its backdrop and its panel, while it is up.
    void Draw(float displayW, float displayH);

private:
    core::Settings& settings_;
    Editor& editor_;
    ViewHost& host_;
};

}  // namespace sz::ui
