#include "ui/view/tutorial_card.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

#include <imgui.h>
// For MovingWindow: whether the user is dragging the card this frame - see
// Draw.
#include <imgui_internal.h>

#include "generated/ui_strings.h"
#include "ui/selection_layout.h"
#include "ui/theme.h"
#include "ui/tutorial/welcome_chain.h"
#include "ui/ui_scale.h"
#include "ui/widgets.h"

namespace sz::ui {

namespace {

constexpr const char* kCardWindow = "##tutorial_card";
// The card's width at 100%, and how far below the top of the screen it
// sits - clear of the message line, which is at the top center too.
constexpr float kCardWidth = 360.0f;
constexpr float kCardTop = 72.0f;
// Where it goes when what it is about lies under it: this far above the
// bottom, clear of the canvas bar's edge and the dock.
constexpr float kCardBottomMargin = 96.0f;

bool Overlap(const AnchorRect& a, const AnchorRect& b) {
    return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y;
}

// A button in the accent, for the step's way on.
bool AccentButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::AccentHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::AccentHover());
    ImGui::PushStyleColor(ImGuiCol_Text, theme::AccentInk());
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return pressed;
}

// A button with no fill until hovered, for the way out.
bool QuietButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kHoverWash);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kHoverWash);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kGraphite300);
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return pressed;
}

// Text wrapped at the card's edge.
void Wrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// A check mark the height of a line, at the cursor, in the accent.
void CheckMark() {
    const float size = ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 points[] = {ImVec2(at.x + size * 0.15f, at.y + size * 0.55f),
                             ImVec2(at.x + size * 0.40f, at.y + size * 0.80f),
                             ImVec2(at.x + size * 0.85f, at.y + size * 0.20f)};
    drawList->AddPolyline(points, 3, ImGui::GetColorU32(theme::Accent()), ImDrawFlags_None, Px(2.0f));
}

}  // namespace

TutorialCard::TutorialCard(const core::Session& session, const Editor& editor, const tutorial::World& world,
                           const AnchorBoard& anchors, ViewHost& host)
    : session_(session),
      editor_(editor),
      world_(world),
      anchors_(anchors),
      host_(host),
      runner_(tutorial::WelcomeChain()) {}

void TutorialCard::Start(core::FolderId folder) {
    runner_.Start(folder);
    moved_ = false;
}

void TutorialCard::Press(TutorialButton button) {
    switch (button) {
        case TutorialButton::Next:
            runner_.Next();
            return;
        case TutorialButton::Back:
            runner_.Back();
            return;
        case TutorialButton::Skip:
            runner_.Skip();
            return;
        case TutorialButton::Done:
            runner_.Done();
            return;
    }
}

void TutorialCard::Update(double now) {
    if (runner_.On()) {
        runner_.Update(world_, now);
    }
}

// ================= Where things are =================

std::optional<AnchorRect> TutorialCard::SubjectRect() const {
    const std::optional<core::ItemId> subject = runner_.Subject();
    if (!subject.has_value()) {
        return std::nullopt;
    }
    const core::CanvasManager& manager = session_.Manager();
    const core::Canvas* canvas = manager.CurrentOrNull();
    if (canvas == nullptr) {
        return std::nullopt;
    }
    for (const core::Item& item : canvas->items) {
        if (item.id == *subject) {
            if (item.minimized || manager.IsDeleted(*canvas, item)) {
                return std::nullopt;
            }
            return AnchorRect{ImVec2(item.rect.x, item.rect.y),
                              ImVec2(item.rect.x + item.rect.w, item.rect.y + item.rect.h)};
        }
    }
    return std::nullopt;  // on another canvas
}

std::optional<AnchorRect> TutorialCard::SpotRect() const {
    if (runner_.GetState() != tutorial::Tutorial::State::OnStep) {
        return std::nullopt;
    }
    switch (runner_.CurrentSpot()) {
        case tutorial::Spot::None:
            return std::nullopt;
        case tutorial::Spot::Subject:
            return SubjectRect();
        case tutorial::Spot::SubjectHandle: {
            // The bottom-right corner's handle, while the handles are drawn;
            // the snippet itself until then.
            const std::optional<AnchorRect> subject = SubjectRect();
            if (!subject.has_value() || !editor_.SelectionLive() || !editor_.IsSelected(*runner_.Subject())) {
                return subject;
            }
            const core::Rect rect{subject->min.x, subject->min.y, subject->max.x - subject->min.x,
                                  subject->max.y - subject->min.y};
            for (const HandleSpec& spec : HandleSpecs(rect)) {
                if (spec.handle == ResizeHandle::SE) {
                    const HitRect handle = HandleDrawRect(spec.center);
                    return AnchorRect{ImVec2(handle.min.x, handle.min.y), ImVec2(handle.max.x, handle.max.y)};
                }
            }
            return subject;
        }
        case tutorial::Spot::SelectionBarClose:
            return anchors_.Find(Anchor{AnchorId::SelectionBarClose});
        case tutorial::Spot::DrawingBarPen:
            return anchors_.Find(Anchor{AnchorId::DrawingBarPen});
        case tutorial::Spot::DockChip: {
            const std::optional<core::ItemId> subject = runner_.Subject();
            return subject.has_value() ? anchors_.Find(Anchor{AnchorId::DockChip, *subject}) : std::nullopt;
        }
    }
    return std::nullopt;
}

// ================= The card =================

void TutorialCard::Draw(float displayW, float displayH) {
    if (!runner_.On()) {
        return;
    }
    const float width = Px(kCardWidth);
    // Where the user put it - ImGui moves a window dragged by its body -
    // or top center, unless what the step is about lies under that; then
    // bottom center.
    const ImGuiWindow* last = ImGui::FindWindowByName(kCardWindow);
    if (last != nullptr && ImGui::GetCurrentContext()->MovingWindow == last) {
        moved_ = true;
    }
    if (!moved_) {
        const float height = last != nullptr ? last->Size.y : Px(160.0f);
        const ImVec2 top((displayW - width) * 0.5f, Px(kCardTop));
        const AnchorRect atTop{top, ImVec2(top.x + width, top.y + height)};
        const std::optional<AnchorRect> spot = SpotRect();
        const std::optional<AnchorRect> subject = SubjectRect();
        const bool covers = (spot.has_value() && Overlap(atTop, *spot)) || (subject.has_value() && Overlap(atTop, *subject));
        ImGui::SetNextWindowPos(covers ? ImVec2(top.x, displayH - height - Px(kCardBottomMargin)) : top);
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
    ImGui::Begin(kCardWindow, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    if (runner_.GetState() == tutorial::Tutorial::State::Skipped) {
        DrawSkipped();
    } else {
        DrawStep();
    }
    ImGui::End();
}

void TutorialCard::DrawStep() {
    const tutorial::Step& step = runner_.CurrentStep();
    const size_t index = runner_.StepIndex();
    const size_t count = runner_.StepCount();
    const bool last = index + 1 == count;

    // Step 3 of 13, over a thin bar that fills as the steps go.
    ImGui::TextColored(theme::kGraphite300, strings::kTutorialCardStep, static_cast<int>(index + 1),
                       static_cast<int>(count));
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = Px(3.0f);
        ImGui::Dummy(ImVec2(w, h));
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(theme::kFieldBg), h);
        const float done = w * static_cast<float>(index + 1) / static_cast<float>(count);
        drawList->AddRectFilled(at, ImVec2(at.x + done, at.y + h), ImGui::GetColorU32(theme::Accent()), h);
    }
    ImGui::Spacing();

    ImGui::TextColored(theme::kWhite, "%s", step.title);
    if (step.kind == tutorial::StepKind::Do && runner_.GoalMet()) {
        ImGui::SameLine();
        CheckMark();
    }
    Wrapped(theme::kGraphite100, tutorial::Expand(step.text(world_), world_));
    if (const std::optional<tutorial::Hint>& hint = runner_.CurrentHint()) {
        ImGui::Spacing();
        DrawHint(*hint);
    }
    ImGui::Spacing();
    ImGui::Separator();

    if (index > 0) {
        if (ImGui::Button(Labeled(strings::kTutorialCardBack, "tutorial_back"))) {
            host_.Act(action::TutorialPress{TutorialButton::Back});
        }
        ImGui::SameLine();
    }
    const bool enabled = runner_.NextEnabled();
    ImGui::BeginDisabled(!enabled);
    const bool next = last ? AccentButton(Labeled(strings::kTutorialCardDone, "tutorial_done"))
                           : AccentButton(Labeled(strings::kTutorialCardNext, "tutorial_next"));
    ImGui::EndDisabled();
    if (!enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", strings::kTutorialCardNextWaits);
    }
    if (next) {
        host_.Act(action::TutorialPress{last ? TutorialButton::Done : TutorialButton::Next});
    }
    if (!last) {
        // The way out, right-aligned and quieter.
        const char* skip = Labeled(strings::kTutorialCardSkip, "tutorial_skip");
        const float skipW = ImGui::CalcTextSize(strings::kTutorialCardSkip).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - skipW));
        if (QuietButton(skip)) {
            host_.Act(action::TutorialPress{TutorialButton::Skip});
        }
    }
}

std::optional<ViewAction> TutorialCard::HintAction() const {
    const std::optional<tutorial::Hint>& hint = runner_.CurrentHint();
    if (runner_.GetState() != tutorial::Tutorial::State::OnStep || !hint.has_value()) {
        return std::nullopt;
    }
    switch (hint->button) {
        case tutorial::HintButton::None:
            return std::nullopt;
        case tutorial::HintButton::BackToTutorial:
            return action::BackToTutorial{};
        case tutorial::HintButton::BackThere:
            return action::SwitchCanvas{hint->canvas};
        case tutorial::HintButton::PutOneHere:
            return action::PracticeSnippet{};
    }
    return std::nullopt;
}

void TutorialCard::DrawHint(const tutorial::Hint& hint) {
    Wrapped(theme::Accent(), tutorial::Expand(hint.text, world_, hint.canvas));
    const char* label = nullptr;
    switch (hint.button) {
        case tutorial::HintButton::None:
            return;
        case tutorial::HintButton::BackToTutorial:
            label = strings::kTutorialCardBackToTutorial;
            break;
        case tutorial::HintButton::BackThere:
            label = strings::kTutorialCardBackThere;
            break;
        case tutorial::HintButton::PutOneHere:
            label = strings::kTutorialCardPutOneHere;
            break;
    }
    if (ImGui::SmallButton(Labeled(label, "tutorial_hint"))) {
        if (std::optional<ViewAction> action = HintAction()) {
            host_.Act(std::move(*action));
        }
    }
}

void TutorialCard::DrawSkipped() {
    ImGui::TextColored(theme::kWhite, "%s", strings::kTutorialSkippedTitle);
    // The warnings not reached before the skip, which nobody should miss.
    const std::vector<const tutorial::Step*> warnings = runner_.WarningsNotReached();
    if (!warnings.empty()) {
        Wrapped(theme::kGraphite100, strings::kTutorialSkippedText);
        for (const tutorial::Step* warning : warnings) {
            ImGui::Spacing();
            Wrapped(theme::Accent(), warning->title);
            Wrapped(theme::kGraphite100, tutorial::Expand(warning->text(world_), world_));
        }
    }
    ImGui::Spacing();
    Wrapped(theme::kGraphite300, strings::kTutorialSkippedAgain);
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(Labeled(strings::kTutorialCardBack, "tutorial_back"))) {
        host_.Act(action::TutorialPress{TutorialButton::Back});
    }
    ImGui::SameLine();
    if (AccentButton(Labeled(strings::kTutorialCardDone, "tutorial_done"))) {
        host_.Act(action::TutorialPress{TutorialButton::Done});
    }
}

// ================= The spotlight =================

void TutorialCard::DrawSpotlight() {
    if (runner_.GetState() != tutorial::Tutorial::State::OnStep || runner_.GoalMet()) {
        return;
    }
    // Not through a panel: the step's line says to close it first.
    const tutorial::Cover cover = world_.CanvasCover();
    if (cover == tutorial::Cover::Overview || cover == tutorial::Cover::CheatSheet) {
        return;
    }
    const std::optional<AnchorRect> spot = SpotRect();
    if (!spot.has_value()) {
        return;
    }
    // A slow pulse, about two seconds a beat, that never fades out.
    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(ImGui::GetTime() * 3.0));
    ImVec4 color = theme::Accent();
    color.w = 0.55f + 0.45f * pulse;
    const float pad = Px(6.0f);
    ImGui::GetForegroundDrawList()->AddRect(ImVec2(spot->min.x - pad, spot->min.y - pad),
                                            ImVec2(spot->max.x + pad, spot->max.y + pad),
                                            ImGui::GetColorU32(color), Px(8.0f), ImDrawFlags_None, Px(3.0f));
}

}  // namespace sz::ui
