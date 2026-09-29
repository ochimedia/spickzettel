#pragma once

// The string editor (docs/STRING_EDITOR.md), in its own build only: F2
// over any text opens a small window on the catalog string it came from,
// shows the edit on screen as it is typed, and saves it to
// assets/ui_strings.json. Its own words are not in the catalog: it is a
// tool for the catalog, never handed out.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "platform/i_overlay_window.h"
#include "platform/platform_types.h"
#include "ui/string_editor/string_match.h"

namespace sz::ui::string_editor {

class StringEditor {
public:
    StringEditor();

    // An input event, before the app sees it: F2 asks for the text under
    // the pointer, and while the window is open every event is its own.
    // True for one taken.
    bool Offer(const platform::InputEvent& event);
    // Over everything else, once everything else is drawn and stacked.
    // `window` takes the keyboard while the window is open.
    void Draw(platform::IOverlayWindow* window, float displayW, float displayH);

private:
    // The catalog string at `index` to edit, what was typed into another
    // one undone.
    void Choose(std::size_t index);
    // The edit, shown where the string is drawn, when it can be.
    void Show();
    // Back to the string as it was chosen.
    void Undo();
    bool Save();
    void Close(platform::IOverlayWindow* window);
    void Pick(ImVec2 at);
    void DrawChoices();

    // Each catalog string by its current text.
    std::vector<std::string_view> Texts() const;

    bool pickAsked_ = false;
    bool open_ = false;
    bool placeAt_ = false;
    bool focusWindow_ = false;
    bool focusField_ = false;
    bool keyboard_ = false;
    ImVec2 pickedAt_;
    // What was under the pointer, and the strings it could be.
    std::optional<ImRect> pickedRect_;
    std::string pickedText_;
    Fit fit_ = Fit::Exact;
    std::vector<std::size_t> choices_;
    std::string search_;

    std::optional<std::size_t> chosen_;
    // The chosen string as it was chosen, and as it is being typed.
    std::string before_;
    std::string typed_;
    std::string problem_;
    std::string note_;
};

}  // namespace sz::ui::string_editor
