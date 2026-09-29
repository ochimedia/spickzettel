#pragma once

// What text the last frame drew, and where - for the string editor to find
// the text under the pointer (docs/STRING_EDITOR.md). Fed by the call the
// string editor's build adds to ImFont::RenderText (see cmake/
// FetchImGui.cmake), so it sees every piece of text ImGui draws: a
// widget's, a window's, and every AddText of the app's own.

#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

namespace sz::ui::string_editor {

struct DrawnText {
    std::string text;
    // Where it was drawn from - one of the catalog's own strings when it
    // was drawn straight from one.
    const char* source = nullptr;
    // Its bounds, clipped to what was visible of it.
    ImRect rect;
    // Its draw list's place in the frame's draw data: the higher, the
    // later drawn, over the lower.
    int layer = 0;
};

class TextLedger {
public:
    // The one ImGui calls into.
    static TextLedger& Get();

    // Kept up to date with `context`'s frames from now on - the frame is
    // over when it has been rendered, and only then are the layers known.
    void Watch(ImGuiContext* context);
    // What is drawn while paused is left out: the string editor's own
    // window.
    void SetPaused(bool paused) { paused_ = paused; }

    // The call from ImFont::RenderText.
    void Record(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const ImVec4& clip,
                const char* begin, const char* end, float wrap);

    // The last frame's text under `point`, the one on top first; of two in
    // one layer, the smaller.
    std::vector<DrawnText> Under(ImVec2 point) const;

private:
    struct Pending {
        DrawnText drawn;
        ImDrawList* list = nullptr;
    };
    static void Rendered(ImGuiContext* context, ImGuiContextHook* hook);

    ImGuiContext* context_ = nullptr;
    bool paused_ = false;
    std::vector<Pending> drawing_;
    std::vector<DrawnText> drawn_;
};

}  // namespace sz::ui::string_editor
