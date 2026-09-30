#include "core/config/bar_layout.h"

#include <algorithm>

namespace sz::core {

namespace {

// One walk, for both groups: what `all` holds is kept in the order the list
// gives it, and whatever the list didn't mention comes after.
void Normalize(BarButtonList& list, const ChromeButton* all, size_t count) {
    BarButtonList kept;
    kept.reserve(count);
    const auto holds = [&kept](ChromeButton button) {
        return std::any_of(kept.begin(), kept.end(),
                            [button](const BarButtonSetting& entry) { return entry.button == button; });
    };
    for (const BarButtonSetting& entry : list) {
        const bool belongsHere = std::find(all, all + count, entry.button) != all + count;
        if (belongsHere && !holds(entry.button)) {
            kept.push_back(entry);
        }
    }
    for (size_t at = 0; at < count; ++at) {
        if (!holds(all[at])) {
            kept.push_back(BarButtonSetting{all[at], /*shown=*/true});
        }
    }
    list = std::move(kept);
}

BarButtonList AllShown(const ChromeButton* all, size_t count) {
    BarButtonList list;
    list.reserve(count);
    for (size_t at = 0; at < count; ++at) {
        list.push_back(BarButtonSetting{all[at], /*shown=*/true});
    }
    return list;
}

}  // namespace

std::string_view BarButtonKey(ChromeButton button) {
    switch (button) {
        case ChromeButton::Close:
            return "close";
        case ChromeButton::Maximize:
            return "maximize";
        case ChromeButton::Minimize:
            return "minimize";
        case ChromeButton::More:
            return "more";
        case ChromeButton::Pin:
            return "pin";
        case ChromeButton::Pen:
            return "pen";
        case ChromeButton::Eraser:
            return "eraser";
        case ChromeButton::Text:
            return "text";
        case ChromeButton::Color:
            return "color";
    }
    return "";
}

std::optional<ChromeButton> BarButtonFromKey(std::string_view key) {
    for (const ChromeButton button : kSnippetBarButtons) {
        if (BarButtonKey(button) == key) {
            return button;
        }
    }
    for (const ChromeButton button : kDrawingBarButtons) {
        if (BarButtonKey(button) == key) {
            return button;
        }
    }
    return std::nullopt;
}

BarButtonList DefaultSnippetBar() { return AllShown(kSnippetBarButtons.data(), kSnippetBarButtons.size()); }

BarButtonList DefaultDrawingBar() { return AllShown(kDrawingBarButtons.data(), kDrawingBarButtons.size()); }

void NormalizeSnippetBar(BarButtonList& list) {
    Normalize(list, kSnippetBarButtons.data(), kSnippetBarButtons.size());
}

void NormalizeDrawingBar(BarButtonList& list) {
    Normalize(list, kDrawingBarButtons.data(), kDrawingBarButtons.size());
}

}  // namespace sz::core
