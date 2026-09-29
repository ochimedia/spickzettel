#include "ui/string_editor/text_ledger.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <unordered_map>

// Called from the start of ImFont::RenderText in the string editor's build
// - see cmake/FetchImGui.cmake. Declared there, at global scope, as here.
void SzTextDrawn(ImDrawList* draw_list, ImFont* font, float size, const ImVec2& pos, const ImVec4& clip_rect,
                 const char* text_begin, const char* text_end, float wrap_width) {
    sz::ui::string_editor::TextLedger::Get().Record(draw_list, font, size, pos, clip_rect, text_begin, text_end,
                                                    wrap_width);
}

namespace sz::ui::string_editor {

TextLedger& TextLedger::Get() {
    static TextLedger ledger;
    return ledger;
}

void TextLedger::Watch(ImGuiContext* context) {
    if (context_ == context) {
        return;
    }
    context_ = context;
    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_RenderPost;
    hook.Callback = &TextLedger::Rendered;
    hook.UserData = this;
    ImGui::AddContextHook(context, &hook);
}

void TextLedger::Record(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const ImVec4& clip,
                        const char* begin, const char* end, float wrap) {
    if (paused_ || context_ == nullptr || ImGui::GetCurrentContext() != context_ || begin == nullptr) {
        return;
    }
    if (end == nullptr) {
        end = begin + std::strlen(begin);
    }
    if (begin == end) {
        return;
    }
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, wrap, begin, end);
    ImRect rect(pos, ImVec2(pos.x + extent.x, pos.y + extent.y));
    rect.ClipWithFull(ImRect(clip.x, clip.y, clip.z, clip.w));
    if (rect.GetWidth() <= 0.0f || rect.GetHeight() <= 0.0f) {
        return;
    }
    Pending pending;
    pending.drawn.text.assign(begin, end);
    pending.drawn.source = begin;
    pending.drawn.rect = rect;
    pending.list = list;
    drawing_.push_back(std::move(pending));
}

void TextLedger::Rendered(ImGuiContext* context, ImGuiContextHook* hook) {
    TextLedger& ledger = *static_cast<TextLedger*>(hook->UserData);
    // The frame's draw lists in the order they are drawn: what a list drew
    // is under everything a later one did. A list not among them - a window
    // hidden this frame - drew nothing that is seen.
    std::unordered_map<const ImDrawList*, int> layers;
    const ImGuiContext& g = *context;
    const ImDrawData* data = g.Viewports.Size > 0 ? &g.Viewports[0]->DrawDataP : nullptr;
    if (data != nullptr && data->Valid) {
        for (int i = 0; i < data->CmdLists.Size; ++i) {
            layers.emplace(data->CmdLists[i], i);
        }
    }
    ledger.drawn_.clear();
    for (Pending& pending : ledger.drawing_) {
        const auto layer = layers.find(pending.list);
        if (layer != layers.end()) {
            pending.drawn.layer = layer->second;
            ledger.drawn_.push_back(std::move(pending.drawn));
        }
    }
    ledger.drawing_.clear();
}

std::vector<DrawnText> TextLedger::Under(ImVec2 point) const {
    std::vector<DrawnText> under;
    for (const DrawnText& drawn : drawn_) {
        if (drawn.rect.Contains(point)) {
            under.push_back(drawn);
        }
    }
    std::stable_sort(under.begin(), under.end(), [](const DrawnText& a, const DrawnText& b) {
        if (a.layer != b.layer) {
            return a.layer > b.layer;
        }
        return a.rect.GetArea() < b.rect.GetArea();
    });
    return under;
}

}  // namespace sz::ui::string_editor
