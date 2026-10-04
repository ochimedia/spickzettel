#include "ui/string_editor/string_editor.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "core/util/atomic_file.h"
#include "generated/ui_strings.h"
#include "ui/string_editor/text_ledger.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/widgets.h"

namespace sz::ui::string_editor {

namespace {

constexpr const char* kWindowName = "Edit text###sz_string_editor";
constexpr int kF2 = platform::KeyCombo::kFunctionKeyBase + 2;
constexpr int kF3 = platform::KeyCombo::kFunctionKeyBase + 3;
// The field's width, and how many rows a search lists.
constexpr float kWidth = 560.0f;
constexpr std::size_t kMostFound = 20;

std::size_t CatalogSize() { return std::size(strings::kEditable); }

// Each catalog string by where it lives: text drawn straight from one is
// that one, whatever other strings read the same.
const std::unordered_map<const char*, std::size_t>& Sources() {
    static const std::unordered_map<const char*, std::size_t> sources = [] {
        std::unordered_map<const char*, std::size_t> map;
        for (std::size_t i = 0; i < CatalogSize(); ++i) {
            map.emplace(strings::kEditable[i].text, i);
        }
        return map;
    }();
    return sources;
}

// `text` into the catalog string at `index`, where it fits.
bool Put(std::size_t index, const std::string& text) {
    const strings::EditableString& entry = strings::kEditable[index];
    if (text.size() + 1 > entry.room) {
        return false;
    }
    std::memcpy(entry.text, text.c_str(), text.size() + 1);
    return true;
}

// The first line of `text`, cut short, for a row of a list.
std::string Preview(std::string_view text) {
    constexpr std::size_t kLongest = 60;
    std::string line(text.substr(0, text.find('\n')));
    if (line.size() > kLongest || line.size() < text.size()) {
        line = line.substr(0, std::min(line.size(), kLongest)) + "...";
    }
    return line;
}

std::string Lower(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

}  // namespace

StringEditor::StringEditor() = default;

std::vector<std::string_view> StringEditor::Texts() const {
    std::vector<std::string_view> texts;
    texts.reserve(CatalogSize());
    for (std::size_t i = 0; i < CatalogSize(); ++i) {
        texts.emplace_back(strings::kEditable[i].text);
    }
    return texts;
}

bool StringEditor::Offer(const platform::InputEvent& event) {
    const bool key = (event.kind == platform::InputEventKind::KeyDown || event.kind == platform::InputEventKind::KeyUp) &&
                     (event.key == kF2 || event.key == kF3) && event.modifiers == platform::Modifiers{};
    if (key && event.kind == platform::InputEventKind::KeyDown && !event.repeat) {
        pickAsked_ = event.key == kF2 ? Asked::Under : Asked::Tooltip;
    }
    return key || open_;
}

void StringEditor::Pick(ImVec2 at, Asked asked) {
    // F2 or F3 over the window itself asks for nothing.
    if (open_) {
        const ImGuiWindow* window = ImGui::FindWindowByName(kWindowName);
        if (window != nullptr && window->Rect().Contains(at)) {
            return;
        }
    }
    Undo();
    chosen_.reset();
    choices_.clear();
    search_.clear();
    pickedRect_.reset();
    pickedText_.clear();
    fit_ = Fit::Exact;
    tooltip_ = asked == Asked::Tooltip;

    std::vector<DrawnText> under;
    if (!tooltip_) {
        under = TextLedger::Get().Under(at);
    } else if (std::optional<DrawnText> tooltip = TextLedger::Get().AskedTooltip()) {
        under.push_back(std::move(*tooltip));
    }
    const std::vector<std::string_view> texts = Texts();
    for (const DrawnText& drawn : under) {
        if (const auto source = Sources().find(drawn.source); source != Sources().end()) {
            choices_ = {source->second};
        } else {
            Matches matches = Match(drawn.text, texts);
            fit_ = matches.fit;
            choices_ = std::move(matches.indices);
        }
        if (!choices_.empty()) {
            pickedRect_ = drawn.rect;
            pickedText_ = drawn.text;
            break;
        }
    }
    if (choices_.empty() && !under.empty()) {
        pickedRect_ = under.front().rect;
        pickedText_ = under.front().text;
    }
    // A tooltip whose item is not known has nothing to ring.
    if (pickedRect_.has_value() && pickedRect_->GetArea() <= 0.0f) {
        pickedRect_.reset();
    }
    open_ = true;
    focusWindow_ = true;
    placeAt_ = true;
    pickedAt_ = at;
    if (!choices_.empty()) {
        Choose(choices_.front());
    }
}

void StringEditor::Choose(std::size_t index) {
    Undo();
    chosen_ = index;
    before_ = strings::kEditable[index].text;
    typed_ = before_;
    problem_.clear();
    note_.clear();
    focusField_ = true;
}

void StringEditor::Show() {
    if (!chosen_.has_value()) {
        return;
    }
    // The tutorial's texts go through tutorial::Expand, which fills in the
    // key of any command.
    const bool tutorial = std::string_view(strings::kEditable[*chosen_].key).starts_with("tutorial.");
    problem_ = FieldsProblem(before_, typed_, tutorial);
    note_.clear();
    if (!problem_.empty()) {
        Put(*chosen_, before_);
    } else if (!Put(*chosen_, typed_)) {
        note_ = "Too long to show before the next build - Save still writes it.";
        Put(*chosen_, before_);
    }
}

void StringEditor::Undo() {
    if (chosen_.has_value()) {
        Put(*chosen_, before_);
        typed_ = before_;
        problem_.clear();
    }
}

bool StringEditor::Save() {
    if (!chosen_.has_value() || !problem_.empty()) {
        return false;
    }
    const std::filesystem::path path(strings::kCatalogFile);
    std::ostringstream file;
    {
        // Closed before the file is replaced: Windows renames nothing over
        // a file still open.
        std::ifstream in(path, std::ios::binary);
        file << in.rdbuf();
        if (!in) {
            note_ = "Could not read " + path.string() + ".";
            return false;
        }
    }
    const char* key = strings::kEditable[*chosen_].key;
    const std::optional<std::string> changed = WithValue(file.str(), key, typed_);
    if (!changed.has_value()) {
        note_ = std::string("\"") + key + "\" is not on a line of its own in " + path.string() + ".";
        return false;
    }
    if (!core::WriteFileAtomically(path, *changed)) {
        note_ = "Could not write " + path.string() + ".";
        return false;
    }
    before_ = typed_;
    return true;
}

void StringEditor::Close(platform::IOverlayWindow* window) {
    open_ = false;
    chosen_.reset();
    choices_.clear();
    pickedRect_.reset();
    if (keyboard_ && window != nullptr) {
        window->ReleaseTextInput();
    }
    keyboard_ = false;
}

void StringEditor::DrawChoices() {
    if (choices_.size() > 1 || (choices_.size() == 1 && fit_ != Fit::Exact)) {
        ImGui::TextDisabled("%s", fit_ == Fit::Exact    ? "Several strings read like this:"
                                  : fit_ == Fit::Filled ? "With what the app fills in, this reads like:"
                                                        : "Could be part of:");
        for (const std::size_t index : choices_) {
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(strings::kEditable[index].key, chosen_ == index)) {
                Choose(index);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Preview(strings::kEditable[index].text).c_str());
            ImGui::PopID();
        }
    } else if (choices_.empty()) {
        if (pickedText_.empty()) {
            ImGui::TextDisabled("%s", tooltip_ ? "No tooltip here." : "No text under the pointer.");
        } else {
            ImGui::TextDisabled("Not from ui_strings.json - search for it, or for what it is made of:");
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(kWidth));
            ImGui::TextUnformatted(pickedText_.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    ImGui::SetNextItemWidth(Px(kWidth));
    ImGui::InputTextWithHint("##search", "Search keys and texts", search_.data(), search_.capacity() + 1,
                             ImGuiInputTextFlags_CallbackResize, &ResizeStringForInputText, &search_);
    if (!search_.empty()) {
        const std::string wanted = Lower(search_);
        std::size_t found = 0;
        for (std::size_t i = 0; i < CatalogSize() && found < kMostFound; ++i) {
            const strings::EditableString& entry = strings::kEditable[i];
            if (Lower(entry.key).find(wanted) == std::string::npos &&
                Lower(entry.text).find(wanted) == std::string::npos) {
                continue;
            }
            ++found;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(entry.key, chosen_ == i)) {
                Choose(i);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Preview(entry.text).c_str());
            ImGui::PopID();
        }
        if (found == 0) {
            ImGui::TextDisabled("Nothing found.");
        }
    }
}

void StringEditor::Draw(platform::IOverlayWindow* window, float displayW, float displayH) {
    TextLedger& ledger = TextLedger::Get();
    ledger.Watch(ImGui::GetCurrentContext());
    if (pickAsked_ != Asked::Nothing) {
        Pick(ImGui::GetIO().MousePos, pickAsked_);
        pickAsked_ = Asked::Nothing;
    }
    if (!open_) {
        return;
    }
    if (!keyboard_ && window != nullptr) {
        window->RequestTextInput();
        keyboard_ = true;
    }

    ledger.SetPaused(true);
    if (pickedRect_.has_value()) {
        ImGui::GetForegroundDrawList()->AddRect(
            ImVec2(pickedRect_->Min.x - Px(3.0f), pickedRect_->Min.y - Px(3.0f)),
            ImVec2(pickedRect_->Max.x + Px(3.0f), pickedRect_->Max.y + Px(3.0f)),
            ImGui::GetColorU32(theme::kTutorialHighlight), Px(4.0f), Px(2.0f));
    }
    if (placeAt_) {
        // Under what was picked, or over it in the lower half of the display.
        const bool below = pickedAt_.y < displayH * 0.5f;
        const float edge = pickedRect_.has_value() ? (below ? pickedRect_->Max.y : pickedRect_->Min.y) : pickedAt_.y;
        const float x = std::clamp(pickedAt_.x, Px(kWidth) * 0.5f + Px(24.0f), displayW - Px(kWidth) * 0.5f - Px(24.0f));
        ImGui::SetNextWindowPos(ImVec2(x, below ? edge + Px(12.0f) : edge - Px(12.0f)), ImGuiCond_Always,
                                ImVec2(0.5f, below ? 0.0f : 1.0f));
        placeAt_ = false;
    }
    bool keepOpen = true;
    bool save = false;
    bool cancel = false;
    // Solid, where the app's panels let what is under them show through: it
    // sits over text, and is read with it.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(theme::kPanelBg.x, theme::kPanelBg.y, theme::kPanelBg.z, 1.0f));
    // The topmost popup open, a menu or the delete confirmation, that the
    // window sits within - see below.
    ImGuiWindow* popup = nullptr;
    for (const ImGuiPopupData& open : ImGui::GetCurrentContext()->OpenPopupStack) {
        if (open.Window != nullptr && open.Window->Active) {
            popup = open.Window;
        }
    }
    // Not focused as it appears, which would close that popup: focused once
    // it is within it, below.
    ImGui::Begin(kWindowName, &keepOpen,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleColor();
    // Within the popup, as far as ImGui can tell: a popup stays open while
    // the window focused is within it, and a modal leaves a window within
    // it the input. So the menu or the confirmation picked from stays up,
    // showing the edit, and the window can be typed in over a modal. Set
    // straight after Begin, which sets it: the text field is a window of
    // its own, focused while this one is being drawn.
    ImGuiWindow* self = ImGui::GetCurrentWindow();
    self->ParentWindowInBeginStack = popup;
    DrawChoices();
    if (chosen_.has_value()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", strings::kEditable[*chosen_].key);
        if (focusField_) {
            ImGui::SetKeyboardFocusHere();
            focusField_ = false;
        }
        // Tall enough for the text as it wraps, and a line to add.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float line = ImGui::GetTextLineHeight();
        const float wrapped =
            ImGui::CalcTextSize(typed_.c_str(), nullptr, false,
                                Px(kWidth) - style.FramePadding.x * 2.0f - style.ScrollbarSize)
                .y;
        const float height = std::clamp(wrapped + line, line * 3.0f, line * 16.0f);
        if (ImGui::InputTextMultiline("##text", typed_.data(), typed_.capacity() + 1,
                                      ImVec2(Px(kWidth), height + style.FramePadding.y * 2.0f),
                                      ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_WordWrap,
                                      &ResizeStringForInputText, &typed_)) {
            Show();
        }
        if (!problem_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kDanger);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(kWidth));
            ImGui::TextUnformatted(problem_.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::BeginDisabled(!problem_.empty() || typed_ == before_);
        save = ImGui::Button("Save");
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    cancel = ImGui::Button("Cancel");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", chosen_.has_value() ? "Ctrl+Enter saves, Esc cancels" : "Esc closes");
    if (!note_.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(kWidth));
        ImGui::TextDisabled("%s", note_.c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        save = save || (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter, false));
        cancel = cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    }
    ImGui::End();
    if (focusWindow_) {
        ImGui::FocusWindow(self);
        focusWindow_ = false;
    }
    ImGui::BringWindowToDisplayFront(self);
    ledger.SetPaused(false);

    if (save) {
        if (typed_ == before_ || Save()) {
            Close(window);
        }
    } else if (cancel || !keepOpen) {
        Undo();
        Close(window);
    }
}

}  // namespace sz::ui::string_editor
