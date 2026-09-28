#pragma once

// The stack of what the overlay has on screen, as a test reads it: every
// window drawn in the last frame, back to front, each named for the surface
// it is - docs/VIEW_LAYER.md, section 3. What a stack test compares with
// that section's table.

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace sz::test {

// Section 3's table, bottom to top: the surfaces that are windows. The five
// popups over the canvas are one row - the machine keeps one up at a time -
// and a popup ImGui opens inside another surface, a dropdown or a color
// picker, sits just above it. The overlays (the drag previews, the size
// preview, the badge, the messages, the software pointer) are drawn on
// ImGui's foreground list, above every window, and are not here.
inline const std::vector<std::string>& StackTable() {
    static const std::vector<std::string> table = {
        "canvas",
        "items",
        "note editor",
        "dock",
        "canvas bar",
        "snippet menu",
        "canvas tile menu",
        "empty canvas menu",
        "properties",
        "popup in properties",
        "color chooser",
        "hud",
        "chrome",
        "overview backdrop",
        "overview",
        "popup in overview",
        "cheat sheet backdrop",
        "cheat sheet",
        "tutorial card",
        "delete confirmation",
    };
    return table;
}

// Whether `surfaces` is in the table's order - each one in it, and none
// before one that sits below it. What makes a test's expected list a
// reading of the table rather than of the code.
inline bool InStackOrder(const std::vector<std::string>& surfaces) {
    const std::vector<std::string>& table = StackTable();
    long last = -1;
    for (const std::string& surface : surfaces) {
        const auto at = std::find(table.begin(), table.end(), surface);
        if (at == table.end() || std::distance(table.begin(), at) <= last) {
            return false;
        }
        last = static_cast<long>(std::distance(table.begin(), at));
    }
    return true;
}

namespace view_stack_detail {

// The surface a window drawn at the top level of the frame is, by its
// name; a popup is named by the caller, which knows which one it opened.
inline std::string SurfaceOf(const ImGuiWindow* window) {
    const char* name = window->Name;
    const auto starts = [name](const char* prefix) { return std::strncmp(name, prefix, std::strlen(prefix)) == 0; };
    if (starts("##spickzettel_canvas")) return "canvas";
    if (starts("##spickzettel_view_only")) return "view only";
    if (starts("##sz_items_layer")) return "items";
    if (starts("##noteedit")) return "note editor";
    if (starts("##dock")) return "dock";
    if (starts("##canvas_bar")) return "canvas bar";
    if (starts("##sz_input_hud_layer")) return "hud";
    if (starts("##sz_chrome_layer")) return "chrome";
    if (starts("##overview_backdrop")) return "overview backdrop";
    if (starts("##overview_panel")) return "overview";
    if (starts("##cheat_sheet_backdrop")) return "cheat sheet backdrop";
    if (starts("##cheat_sheet_panel")) return "cheat sheet";
    if (starts("##tutorial_card")) return "tutorial card";
    return name;  // a window with no row: the comparison shows it
}

}  // namespace view_stack_detail

// Every window the last frame drew, back to front, by surface. A popup
// opened at the top level of the frame - one of the app's own - is named
// `appPopup`; one ImGui opened inside another surface is "popup in" that
// surface. Child windows are drawn with their parent and tooltips follow
// the pointer, so neither is a place in the stack of its own.
inline std::vector<std::string> SurfacesBackToFront(const std::string& appPopup = "app popup") {
    using view_stack_detail::SurfaceOf;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const auto isFallback = [](const ImGuiWindow* window) {
        return window != nullptr && std::strncmp(window->Name, "Debug##", 7) == 0;
    };
    std::vector<std::string> surfaces;
    for (const ImGuiWindow* window : g.Windows) {
        if (!window->Active || window->Hidden || (window->Flags & ImGuiWindowFlags_ChildWindow) != 0 ||
            (window->Flags & ImGuiWindowFlags_Tooltip) != 0 || isFallback(window)) {
            continue;
        }
        if ((window->Flags & ImGuiWindowFlags_Popup) == 0) {
            surfaces.push_back(SurfaceOf(window));
            continue;
        }
        const ImGuiWindow* parent = window->ParentWindow != nullptr ? window->ParentWindow->RootWindow : nullptr;
        if (parent == nullptr || isFallback(parent)) {
            surfaces.push_back(appPopup);
        } else if ((parent->Flags & ImGuiWindowFlags_Popup) != 0) {
            surfaces.push_back("popup in " + appPopup);
        } else {
            surfaces.push_back("popup in " + SurfaceOf(parent));
        }
    }
    return surfaces;
}

// For a failure message.
inline std::string Describe(const std::vector<std::string>& surfaces) {
    std::string text;
    for (const std::string& surface : surfaces) {
        text += (text.empty() ? "" : " < ") + surface;
    }
    return text;
}

}  // namespace sz::test
