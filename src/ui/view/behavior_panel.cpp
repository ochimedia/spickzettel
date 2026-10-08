#include "ui/view/behavior_panel.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

#include <imgui.h>

#include "core/config/settings_catalog.h"
#include "generated/ui_strings.h"
#include "ui/icons_generated.h"
#include "ui/settings_widgets.h"
#include "ui/text_spans.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/widgets.h"

namespace sz::ui {

using namespace ::sz::core;

namespace {

// The rows, in the order of Settings > Behavior and its tree: what each
// needs is above it, and `depth` is how far in Settings indents it. The
// two rows that need nothing are at the bottom, as there.
struct Row {
    const char* label;
    const ProfileSetting<BoolRule>* setting;
    int depth;
    // Read only on the way up into edit mode - how the window is shown, or
    // the screen frozen as it comes up - so switching it restarts the
    // overlay, or the row would read on while nothing had changed.
    bool restarts;
};

constexpr Row kRows[] = {
    {strings::kHudDontStealFocus, &setting::kDontStealFocus, 0, true},
    {strings::kInputTakeFocusOverElevatedLabel, &setting::kTakeFocusOverElevated, 1, true},
    {strings::kHudDontForwardKeystrokes, &setting::kDontForwardKeystrokes, 1, false},
    {strings::kHudUseRawMouseInput, &setting::kRawMouseInput, 1, false},
    {strings::kInputCounterRawMouseLabel, &setting::kCounterRawMouseInput, 2, false},
    {strings::kHudUseSoftwarePointer, &setting::kSoftwarePointer, 0, false},
    {strings::kHudFreezeScreenWhileEditing, &setting::kFreezeScreen, 0, true},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
static_assert(kRowCount <= 9, "a number key each");

// Whether a key or button a switch can be made with is down, which a
// restart waits for - any button, not only the one a row is clicked with:
// a digit pressed with the right button held, mid-drag, ends the drag but
// not the press, whose up a restart would lose too.
bool AnySwitchHeld() {
    for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) {
        if (ImGui::IsMouseDown(button)) {
            return true;
        }
    }
    for (int i = 0; i < kRowCount; ++i) {
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(ImGuiKey_1 + i))) {
            return true;
        }
    }
    return false;
}

}  // namespace

BehaviorPanel::BehaviorPanel(Settings& settings, ViewHost& host) : settings_(settings), host_(host) {}

bool BehaviorPanel::Value(int row) const { return settings_.Live().*kRows[row].setting->value; }

// The same preconditions Settings grays its checkboxes on, from the one
// place that states them.
bool BehaviorPanel::Available(int row) const {
    const ProfileableSettings& live = settings_.Live();
    const ProfileSetting<BoolRule>* setting = kRows[row].setting;
    if (setting == &setting::kTakeFocusOverElevated) {
        return live.dontStealFocus;
    }
    if (setting == &setting::kDontForwardKeystrokes) {
        return platform::EditModeInputOptions::KeystrokesCanBeHeld(live.dontStealFocus);
    }
    if (setting == &setting::kRawMouseInput) {
        return live.InputOptions().RawMouseInputCanBeUsed(live.dontStealFocus);
    }
    if (setting == &setting::kCounterRawMouseInput) {
        return live.InputOptions().CounterRawMouseInputCanBeUsed(live.dontStealFocus);
    }
    return true;
}

void BehaviorPanel::Switch(int row) {
    if (row < 0 || row >= kRowCount || !Available(row)) {
        return;  // grayed, and a key on it does nothing to match
    }
    // Into the profile that matched, if one did - not the Settings panel's
    // edit target, which may be another profile entirely.
    settings_.Set(*kRows[row].setting, !Value(row), settings_.ActiveProfile());
    pendingRestart_ = pendingRestart_ || kRows[row].restarts;
}

std::optional<int> BehaviorPanel::RowFor(const Event& event, bool editMode) const {
    if (!open_ || !editMode || event.modifiers.ctrl || event.modifiers.alt || event.modifiers.shift) {
        return std::nullopt;
    }
    for (int i = 0; i < kRowCount; ++i) {
        if (event.key == '1' + i) {
            return i;
        }
    }
    return std::nullopt;
}

void BehaviorPanel::Draw() {
    if (!open_) {
        return;
    }
    const float wrapWidth = Px(440.0f);
    // Top left at first, where the input options HUD was; it can be dragged
    // by its background anywhere else for the rest of the session.
    ImGui::SetNextWindowPos(Px(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("##behavior_panel", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Dummy(ImVec2(wrapWidth, 0.0f));  // the window's width, whatever its lines are

    // The title, and on its line the way to close it: its key, and a button.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::kWhite, "%s", strings::kBehaviorPanelTitle);
    const platform::KeyCombo& ownKey = settings_.Stored().hotkeyBehaviorPanel;
    const float closeW = Px(kPillButtonSize);
    if (ownKey.IsValid()) {
        char closeHint[96];
        std::snprintf(closeHint, sizeof(closeHint), strings::kBehaviorPanelClose, FormatKeyComboLabel(ownKey).c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(closeHint).x -
                                                closeW - ImGui::GetStyle().ItemSpacing.x));
        ImGui::TextColored(theme::kGraphite300, "%s", closeHint);
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - closeW));
    if (PillIconButton("##behavior_close", icons::kX, false)) {
        open_ = false;
    }
    ImGui::Separator();

    // Where a switch goes: the profile that runs, or the defaults when
    // none does.
    std::string target = strings::kBehaviorPanelDefaults;
    if (const std::optional<size_t> profile = settings_.ActiveProfile();
        profile.has_value() && *profile < settings_.Profiles().size()) {
        target = WithValue(strings::kBehaviorPanelProfile, settings_.Profiles()[*profile].name);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
    WrappedSpans(theme::kGraphite200, theme::Accent(), MarkedSpans(target));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    DrawRows();

    // How to use it, and where the rows are explained: each row's help is
    // in Settings, beside its checkbox. Shown here for the row under the
    // pointer, it was read only by someone already reaching for the row.
    ImGui::Separator();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
    WrappedSpans(theme::kGraphite300, theme::Accent(), MarkedSpans(strings::kBehaviorPanelFooter));
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void BehaviorPanel::DrawRows() {
    const float indent = Px(24.0f);
    const float numberW = ImGui::CalcTextSize("9").x + Px(12.0f);
    for (int i = 0; i < kRowCount; ++i) {
        const Row& row = kRows[i];
        ImGui::PushID(i);
        const bool available = Available(i);
        char number[4];
        std::snprintf(number, sizeof(number), "%d", i + 1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(available ? theme::kGraphite300 : theme::kGraphite500, "%s", number);
        ImGui::SameLine(numberW + indent * static_cast<float>(row.depth) + ImGui::GetStyle().WindowPadding.x);
        // Settings' own checkbox, for the profile that runs: a row the
        // profile states for itself is marked, with the arrow that hands
        // it back to the defaults, and a grayed row that is on shows a
        // dash, as there. A click or the arrow can change what runs, so a
        // restart is asked for by what changed, not by which was used.
        const bool before = Value(i);
        SettingCheckbox(settings_, settings_.ActiveProfile(), *row.setting, "row", row.label, nullptr, !available);
        if (row.restarts && Value(i) != before) {
            pendingRestart_ = true;
        }
        ImGui::PopID();
    }
}

void BehaviorPanel::Update(bool editMode) {
    const int digits = editMode && open_ ? kRowCount : 0;
    if (host_.Window() != nullptr && digits != appliedDigits_) {
        host_.Window()->SetPanelDigits(digits);
        appliedDigits_ = digits;
    }
    if (digits == 0) {
        // Closed before the key came up: nobody left to have asked, and a
        // restart later would hide and show the overlay for no reason
        // anyone could see.
        pendingRestart_ = false;
        return;
    }
    if (pendingRestart_ && !AnySwitchHeld()) {
        pendingRestart_ = false;
        host_.RestartOverlay();
    }
}

}  // namespace sz::ui
