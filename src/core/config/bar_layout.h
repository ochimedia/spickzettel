#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "core/session/actions.h"

namespace sz::core {

// The two bars that float over the selection - the snippet one, and the
// drawing one a snippet open for drawing shows instead (see ChromeButton)
// - as the settings hold them: which buttons each carries, in what order,
// and which of them are shown at all.
//
// A list rather than a set of flags, because the order is half of what is
// being set: the bar is drawn left to right in exactly this order, and
// Settings > Interaction is a row of the same buttons to drag about and
// switch on or off.
//
// A hidden button stays in the list rather than being dropped from it, so
// that switching it back on puts it back where it was rather than at the
// end.
struct BarButtonSetting {
    ChromeButton button = ChromeButton::Pin;
    bool shown = true;

    bool operator==(const BarButtonSetting&) const = default;
};
using BarButtonList = std::vector<BarButtonSetting>;

// Every button each bar can carry, in the order it ships in. The two sets
// are disjoint: a button belongs to one bar, and a list naming a button
// from the other one is not a layout of this bar (see NormalizeSnippetBar).
inline constexpr std::array<ChromeButton, 5> kSnippetBarButtons = {
    ChromeButton::Pin, ChromeButton::More, ChromeButton::Minimize, ChromeButton::Maximize, ChromeButton::Close,
};
inline constexpr std::array<ChromeButton, 4> kDrawingBarButtons = {
    ChromeButton::Pen, ChromeButton::Eraser, ChromeButton::Text, ChromeButton::Colour,
};

// The name a button is written as in config.json - the compatibility
// surface, like ShortcutActionKey's own names, so the enum may be
// reordered freely and the file still means what it said.
std::string_view BarButtonKey(ChromeButton button);
std::optional<ChromeButton> BarButtonFromKey(std::string_view key);

// Both bars as they ship: every button, in the order above, all shown.
BarButtonList DefaultSnippetBar();
BarButtonList DefaultDrawingBar();

// A list read from the file, made usable: anything that is not this bar's
// button is dropped, a button named twice is kept once, and a button the
// file never mentioned - one this version has and the version that wrote
// the file did not - is appended, shown. Appended and shown rather than
// hidden, because a button nobody has said anything about is new, and a
// new one arriving invisible is a feature that silently isn't there.
void NormalizeSnippetBar(BarButtonList& list);
void NormalizeDrawingBar(BarButtonList& list);

}  // namespace sz::core
