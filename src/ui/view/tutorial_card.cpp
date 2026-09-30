#include "ui/view/tutorial_card.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include <imgui.h>
// For MovingWindow: whether the user is dragging the card this frame - see
// Draw.
#include <imgui_internal.h>

#include "generated/ui_strings.h"
#include "ui/interaction/levels.h"
#include "ui/selection_layout.h"
#include "ui/theme.h"
#include "ui/ui_scale.h"
#include "ui/view/settings_page.h"
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
// In a top corner, this far from the side.
constexpr float kCardSideMargin = 24.0f;
// The list's two columns, and the gap between them.
constexpr float kListWidth = 2.0f * kCardWidth;
constexpr float kListColumnGap = 12.0f;
// How long the card slides to a new place, in seconds.
constexpr float kCardSlide = 0.15f;

bool Overlap(const AnchorRect& a, const AnchorRect& b) {
    return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y;
}

// Whether a spot is one of the Overview's own, which only it draws - its
// Settings tab's among them.
bool InOverview(tutorial::Spot spot) {
    switch (spot) {
        case tutorial::Spot::SectionProfiles:
        case tutorial::Spot::SectionBehavior:
        case tutorial::Spot::MakeProfile:
        case tutorial::Spot::Showing:
        case tutorial::Spot::DontStealFocus:
        case tutorial::Spot::Revert:
        case tutorial::Spot::NewFolder:
        case tutorial::Spot::ShowDeleted:
        case tutorial::Spot::MadeFolder:
        case tutorial::Spot::TutorialFolder:
        case tutorial::Spot::DeleteCanvas:
        case tutorial::Spot::Restore:
            return true;
        default:
            return false;
    }
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
    drawList->AddPolyline(points, 3, ImGui::GetColorU32(theme::kTutorialHighlight), ImDrawFlags_None, Px(2.0f));
}

}  // namespace

TutorialCard::TutorialCard(const core::Session& session, const Editor& editor, const tutorial::World& world,
                           const AnchorBoard& anchors, ViewHost& host)
    : session_(session),
      editor_(editor),
      world_(world),
      anchors_(anchors),
      host_(host),
      topic_(tutorial::FindTopic(tutorial::kBasicsTopic)),
      runner_(topic_->chain()) {}

void TutorialCard::Start(const tutorial::Topic& topic, core::FolderId folder) {
    Resume(topic, {}, folder);
}

void TutorialCard::Resume(const tutorial::Topic& topic, std::string_view id, core::FolderId folder) {
    topic_ = &topic;
    runner_ = tutorial::Tutorial(topic.chain());
    runner_.Resume(id, folder);
    PlaceAnew();
    keep_ = false;
    hasRun_ = true;
    listing_ = false;
}

void TutorialCard::Press(TutorialButton button) {
    switch (button) {
        case TutorialButton::Next:
            runner_.Next();
            return;
        case TutorialButton::Back:
            runner_.Back();
            keep_ = false;
            return;
        case TutorialButton::Skip:
            runner_.Skip();
            keep_ = false;
            return;
        case TutorialButton::Done:
            runner_.Done();
            return;
        case TutorialButton::Keep:
            keep_ = !keep_;
            return;
        case TutorialButton::MoreTopics:
            // The topic ends here, as at Done: the list has no way back
            // to it (docs/TUTORIAL.md, section 20).
            runner_.Done();
            OpenList();
            return;
        case TutorialButton::CloseList:
            listing_ = false;
            PlaceAnew();
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
        case tutorial::Spot::SelectionBarPin: {
            // The snippet itself until it is selected and its bar is drawn.
            const std::optional<AnchorRect> pin = anchors_.Find(Anchor{AnchorId::SelectionBarPin});
            return pin.has_value() ? pin : SubjectRect();
        }
        case tutorial::Spot::SelectionBarPen:
        case tutorial::Spot::SelectionBarEraser:
        case tutorial::Spot::SelectionBarText:
        case tutorial::Spot::SelectionBarColor: {
            // The snippet itself until it is selected and its bar is drawn.
            const tutorial::Spot spot = runner_.CurrentSpot();
            const AnchorId id = spot == tutorial::Spot::SelectionBarPen      ? AnchorId::SelectionBarPen
                                : spot == tutorial::Spot::SelectionBarEraser ? AnchorId::SelectionBarEraser
                                : spot == tutorial::Spot::SelectionBarText   ? AnchorId::SelectionBarText
                                                                             : AnchorId::SelectionBarColor;
            const std::optional<AnchorRect> button = anchors_.Find(Anchor{id});
            return button.has_value() ? button : SubjectRect();
        }
        case tutorial::Spot::DockChip: {
            const std::optional<core::ItemId> subject = runner_.Subject();
            return subject.has_value() ? anchors_.Find(Anchor{AnchorId::DockChip, *subject}) : std::nullopt;
        }
        case tutorial::Spot::CanvasBarNew:
            return anchors_.Find(Anchor{AnchorId::CanvasBarNew});
        case tutorial::Spot::CanvasBarOverview:
            return anchors_.Find(Anchor{AnchorId::CanvasBarOverview});
        case tutorial::Spot::NewFolder:
            return anchors_.Find(Anchor{AnchorId::OverviewNewFolder});
        case tutorial::Spot::ShowDeleted:
            return anchors_.Find(Anchor{AnchorId::OverviewShowDeleted});
        case tutorial::Spot::MadeFolder:
            for (const core::FolderId folder : runner_.MadeFolders()) {
                if (const std::optional<AnchorRect> row = anchors_.Find(Anchor{AnchorId::OverviewFolderRow, folder})) {
                    return row;
                }
            }
            return std::nullopt;
        case tutorial::Spot::TutorialFolder:
            return anchors_.Find(Anchor{AnchorId::OverviewFolderRow, runner_.Folder()});
        case tutorial::Spot::DeleteCanvas:
            return DeleteCanvasRect();
        case tutorial::Spot::Restore:
            return RestoreRect();
        case tutorial::Spot::SectionProfiles:
            return WayTo(tutorial::SettingsSection::Profiles, std::nullopt);
        case tutorial::Spot::SectionBehavior:
            return WayTo(tutorial::SettingsSection::Behavior, std::nullopt);
        case tutorial::Spot::MakeProfile:
            return WayTo(tutorial::SettingsSection::Profiles, anchors_.Find(Anchor{AnchorId::SettingsMakeProfile}));
        case tutorial::Spot::Showing:
            return WayTo(tutorial::SettingsSection::Behavior, anchors_.Find(Anchor{AnchorId::SettingsShowing}));
        case tutorial::Spot::DontStealFocus:
            return WayTo(tutorial::SettingsSection::Behavior,
                         anchors_.Find(Anchor{AnchorId::SettingsDontStealFocus}));
        case tutorial::Spot::Revert:
            // The first arrow drawn, of the section's eight rows.
            for (uint64_t row = 0; row < 8; ++row) {
                if (const std::optional<AnchorRect> arrow = anchors_.Find(Anchor{AnchorId::SettingsRevert, row})) {
                    return arrow;
                }
            }
            return WayTo(tutorial::SettingsSection::Behavior, std::nullopt);
    }
    return std::nullopt;
}

std::optional<AnchorRect> TutorialCard::WayTo(tutorial::SettingsSection section,
                                              std::optional<AnchorRect> spot) const {
    if (spot.has_value() || world_.CanvasCover() != tutorial::Cover::Overview) {
        return spot;
    }
    // The Overview on its Canvases or About tab: its Settings tab first.
    if (!world_.OverviewShowsSettings()) {
        return anchors_.Find(Anchor{AnchorId::OverviewSettingsTab});
    }
    // Another section picked: the row of the one the spot is in. On it
    // already, the spot is only not drawn yet - a revert arrow before
    // anything is set - and nothing is ringed.
    if (world_.SettingsSectionShown() == section) {
        return std::nullopt;
    }
    const SettingsPage::SettingsSection row = section == tutorial::SettingsSection::Profiles
                                                  ? SettingsPage::SettingsSection::Profiles
                                                  : SettingsPage::SettingsSection::Behavior;
    return anchors_.Find(Anchor{AnchorId::SettingsSection, static_cast<uint64_t>(row)});
}

std::optional<AnchorRect> TutorialCard::DeleteCanvasRect() const {
    std::vector<tutorial::CanvasFacts> canvases;
    std::vector<core::CanvasId> withSnippets;
    for (const core::FolderId folder : runner_.Folders()) {
        for (const tutorial::CanvasFacts& canvas : world_.CanvasesIn(folder)) {
            if (!canvas.deleted) {
                canvases.push_back(canvas);
            }
        }
        for (const tutorial::SnippetFacts& snippet : world_.SnippetsIn(folder)) {
            if (!snippet.deleted) {
                withSnippets.push_back(snippet.canvas);
            }
        }
    }
    // A pass for those with a snippet on them, then one for any.
    for (const bool needsSnippet : {true, false}) {
        for (const tutorial::CanvasFacts& canvas : canvases) {
            if (needsSnippet && std::find(withSnippets.begin(), withSnippets.end(), canvas.id) == withSnippets.end()) {
                continue;
            }
            if (const std::optional<AnchorRect> button =
                    anchors_.Find(Anchor{AnchorId::OverviewCanvasDelete, canvas.id})) {
                return button;
            }
        }
    }
    return std::nullopt;
}

std::optional<AnchorRect> TutorialCard::RestoreRect() const {
    const std::vector<core::FolderId> folders = runner_.Folders();
    for (const core::FolderId folder : folders) {
        for (const tutorial::CanvasFacts& canvas : world_.CanvasesIn(folder)) {
            if (!canvas.deleted) {
                continue;
            }
            if (const std::optional<AnchorRect> button = anchors_.Find(Anchor{AnchorId::OverviewRestore, canvas.id})) {
                return button;
            }
        }
    }
    // The grid shows another folder: the row of the folder that holds it.
    for (const core::FolderId folder : folders) {
        if (const std::optional<AnchorRect> button = anchors_.Find(Anchor{AnchorId::OverviewRestore, folder})) {
            return button;
        }
    }
    return std::nullopt;
}

// ================= The card =================

float TutorialCard::Width(float displayW) const {
    if (listing_ && Px(kListWidth) + 2.0f * Px(kCardSideMargin) <= displayW) {
        return Px(kListWidth);
    }
    return Px(kCardWidth);
}

ImVec2 TutorialCard::Placed(float displayW, float displayH) {
    // The list at the top center, always: it points at nothing.
    //
    // A step's card at the top center to begin with, unless what the step
    // is about lies under it; then bottom center, then the top corners,
    // where a large snippet on a small display leaves room at neither.
    // Over the Overview, the lower right first. What the step is about is what the ring is on
    // and the subject, and after them the bars over the selection, whose
    // buttons a line may name when nothing rings them: where every place
    // covers something, the one that covers least.
    //
    // Once placed, it stays - across steps too - until it covers what the
    // user is asked to click, the ring's anchor or a bar; the subject
    // alone does not move it. Then it goes to the place least in the way,
    // then covering least, then nearest, and slides there. The Overview
    // opened or closed has changed all that is under it: placed anew, as
    // at the start (docs/TUTORIAL.md, section 19).
    const float width = Width(displayW);
    const ImGuiWindow* last = ImGui::FindWindowByName(kCardWindow);
    const float height = last != nullptr ? last->Size.y : Px(160.0f);
    const float side = Px(kCardSideMargin);
    const auto at = [&](Place place) {
        const float top = Px(kCardTop);
        const float bottom = displayH - height - Px(kCardBottomMargin);
        ImVec2 pos;
        switch (place) {
            case Place::Top:
                pos = ImVec2((displayW - width) * 0.5f, top);
                break;
            case Place::Bottom:
                pos = ImVec2((displayW - width) * 0.5f, bottom);
                break;
            case Place::TopLeft:
                pos = ImVec2(side, top);
                break;
            case Place::TopRight:
                pos = ImVec2(displayW - width - side, top);
                break;
            case Place::LowerRight:
                pos = ImVec2(displayW - width - side, bottom);
                break;
        }
        // All of it on screen: the list is tall, and a small display or a
        // large scale leaves less below the top place than it needs.
        pos.y = std::max(0.0f, std::min(pos.y, displayH - height - side));
        return pos;
    };
    if (listing_) {
        drawnAt_ = at(Place::Top);
        return *drawnAt_;
    }

    const std::optional<AnchorRect> spot = SpotRect();
    const std::optional<AnchorRect> subject = SubjectRect();
    const std::optional<AnchorRect> bars[] = {
        anchors_.Find(Anchor{AnchorId::SelectionBarPin}),  anchors_.Find(Anchor{AnchorId::SelectionBarClose}),
        anchors_.Find(Anchor{AnchorId::SelectionBarPen}),    anchors_.Find(Anchor{AnchorId::SelectionBarEraser}),
        anchors_.Find(Anchor{AnchorId::SelectionBarText}),   anchors_.Find(Anchor{AnchorId::SelectionBarColor})};
    const auto under = [&](Place place, const std::optional<AnchorRect>& rect) {
        const ImVec2 pos = at(place);
        return rect.has_value() && Overlap(AnchorRect{pos, ImVec2(pos.x + width, pos.y + height)}, *rect) ? 1 : 0;
    };
    // What the user is asked to click, under the card there.
    const auto inTheWay = [&](Place place) {
        int weight = under(place, spot);
        for (const std::optional<AnchorRect>& bar : bars) {
            weight += under(place, bar);
        }
        return weight;
    };
    // And all it covers, the anchor and the subject counting double.
    const auto covered = [&](Place place) {
        return inTheWay(place) + under(place, spot) + 2 * under(place, subject);
    };

    // Over the Overview, whose grid fills from the top left and runs under
    // top center, the lower right comes first: the grid is usually empty
    // there (docs/TUTORIAL.md, question 38).
    std::vector<Place> places = {Place::Top, Place::Bottom, Place::TopLeft, Place::TopRight};
    const bool overOverview = world_.CanvasCover() == tutorial::Cover::Overview;
    if (overOverview) {
        places.insert(places.begin(), Place::LowerRight);
    }
    if (overOverview != placedOverOverview_) {
        place_.reset();
    }
    if (!place_) {
        Place best = places.front();
        int least = covered(best);
        for (const Place place : places) {
            if (least == 0) {
                break;
            }
            if (const int weight = covered(place); weight < least) {
                best = place;
                least = weight;
            }
        }
        place_ = best;
        placedOverOverview_ = overOverview;
        slideFrom_ = drawnAt_.value_or(at(best));
        slideAge_ = 0.0f;
    } else if (inTheWay(*place_) > 0) {
        const ImVec2 here = at(*place_);
        const auto distance = [&](Place place) {
            const ImVec2 there = at(place);
            return std::hypot(there.x - here.x, there.y - here.y);
        };
        // Where it is stays a candidate: if every place is in the way as
        // much, it need not move.
        if (std::find(places.begin(), places.end(), *place_) == places.end()) {
            places.push_back(*place_);
        }
        const auto rank = [&](Place place) { return std::tuple(inTheWay(place), covered(place), distance(place)); };
        Place best = *place_;
        for (const Place place : places) {
            if (rank(place) < rank(best)) {
                best = place;
            }
        }
        if (best != *place_) {
            place_ = best;
            slideFrom_ = drawnAt_.value_or(here);
            slideAge_ = 0.0f;
        }
    }

    // The slide: from where it was drawn when the place changed, easing
    // out. Its place is followed as it is, so a card that grows while it
    // slides still lands where it should.
    const ImVec2 target = at(*place_);
    ImVec2 pos = target;
    if (slideAge_ < kCardSlide) {
        const float t = slideAge_ / kCardSlide;
        const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        pos = ImVec2(slideFrom_.x + (target.x - slideFrom_.x) * eased, slideFrom_.y + (target.y - slideFrom_.y) * eased);
        slideAge_ += ImGui::GetIO().DeltaTime;
    }
    drawnAt_ = pos;
    return pos;
}

void TutorialCard::Draw(float displayW, float displayH) {
    if (!runner_.On() && !listing_) {
        return;
    }
    const float width = Width(displayW);
    // Where the user put it - ImGui moves a window dragged by its body -
    // or its place.
    const ImGuiWindow* last = ImGui::FindWindowByName(kCardWindow);
    if (last != nullptr && ImGui::GetCurrentContext()->MovingWindow == last) {
        moved_ = true;
    }
    if (!moved_) {
        ImGui::SetNextWindowPos(Placed(displayW, displayH));
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
    ImGui::Begin(kCardWindow, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    if (listing_) {
        DrawList();
    } else if (runner_.GetState() == tutorial::Tutorial::State::Skipped) {
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
        drawList->AddRectFilled(at, ImVec2(at.x + done, at.y + h), ImGui::GetColorU32(theme::kTutorialHighlight), h);
    }
    ImGui::Spacing();

    ImGui::TextColored(theme::kWhite, "%s", step.title);
    if (step.kind == tutorial::StepKind::Do && runner_.GoalMet()) {
        ImGui::SameLine();
        CheckMark();
    }
    Wrapped(theme::kGraphite100, tutorial::Expand(step.text(world_), world_, 0, ProfileName()));
    if (const std::optional<tutorial::Hint>& hint = runner_.CurrentHint()) {
        ImGui::Spacing();
        DrawHint(*hint);
    }
    if (last) {
        EndButtons();
        return;
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
    const bool next = AccentButton(Labeled(strings::kTutorialCardNext, "tutorial_next"));
    ImGui::EndDisabled();
    if (!enabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", strings::kTutorialCardNextWaits);
    }
    if (next) {
        host_.Act(action::TutorialPress{TutorialButton::Next});
    }
    // The way out, right-aligned and quieter.
    const char* skip = Labeled(strings::kTutorialCardSkip, "tutorial_skip");
    const float skipW = ImGui::CalcTextSize(strings::kTutorialCardSkip).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - skipW));
    if (QuietButton(skip)) {
        host_.Act(action::TutorialPress{TutorialButton::Skip});
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
    Wrapped(theme::kTutorialHighlight, tutorial::Expand(hint.text, world_, hint.canvas, ProfileName()));
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
    const std::vector<const tutorial::Step*> warnings = SkipWarnings();
    if (!warnings.empty()) {
        Wrapped(theme::kGraphite100, strings::kTutorialSkippedText);
        for (const tutorial::Step* warning : warnings) {
            ImGui::Spacing();
            Wrapped(theme::kTutorialHighlight, warning->title);
            Wrapped(theme::kGraphite100, tutorial::Expand(warning->text(world_), world_));
        }
    }
    ImGui::Spacing();
    Wrapped(theme::kGraphite300, strings::kTutorialSkippedAgain);
    EndButtons();
}

void TutorialCard::EndButtons() {
    // Above the buttons, since it changes what two of them do: read
    // before either is pressed (docs/TUTORIAL.md, section 20).
    if (const char* label = KeepLabel()) {
        ImGui::Spacing();
        bool keep = keep_;
        if (ImGui::Checkbox(Labeled(label, "tutorial_keep"), &keep)) {
            host_.Act(action::TutorialPress{TutorialButton::Keep});
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(Labeled(strings::kTutorialCardBack, "tutorial_back"))) {
        host_.Act(action::TutorialPress{TutorialButton::Back});
    }
    ImGui::SameLine();
    if (AccentButton(Labeled(strings::kTutorialCardDone, "tutorial_done"))) {
        host_.Act(action::TutorialPress{TutorialButton::Done});
    }
    // As plain to see as Done: the other way on from here.
    ImGui::SameLine();
    if (AccentButton(Labeled(strings::kTutorialCardMoreTopics, "tutorial_moretopics"))) {
        host_.Act(action::TutorialPress{TutorialButton::MoreTopics});
    }
}

TutorialCard::Made TutorialCard::LeftToKeep() const {
    Made left;
    const std::vector<tutorial::FolderFacts> folders = world_.Folders();
    for (const core::FolderId each : runner_.Folders()) {
        const auto found =
            std::find_if(folders.begin(), folders.end(), [&](const tutorial::FolderFacts& f) { return f.id == each; });
        if (found != folders.end() && !found->deleted) {
            left.folders.push_back(*found);
        }
    }
    const std::vector<tutorial::ProfileFacts> profiles = world_.Profiles();
    for (const core::ProfileId made : runner_.MadeProfiles()) {
        if (std::any_of(profiles.begin(), profiles.end(),
                        [&](const tutorial::ProfileFacts& p) { return p.id == made; })) {
            left.profiles.push_back(made);
        }
    }
    return left;
}

std::string TutorialCard::ProfileName() const {
    const std::optional<core::ProfileId>& profile = runner_.Profile();
    if (!profile) {
        return {};
    }
    for (const tutorial::ProfileFacts& facts : world_.Profiles()) {
        if (facts.id == *profile) {
            return facts.name;
        }
    }
    return {};
}

const char* TutorialCard::KeepLabel() const {
    const Made left = LeftToKeep();
    // A topic with nothing on a canvas keeps its profiles (section 18.3).
    if (!topic_->folder) {
        return left.profiles.empty()       ? nullptr
               : left.profiles.size() == 1 ? strings::kTutorialCardKeepProfile
                                           : strings::kTutorialCardKeepProfiles;
    }
    return left.folders.empty()       ? nullptr
           : left.folders.size() == 1 ? strings::kTutorialCardKeepFolder
                                      : strings::kTutorialCardKeepFolders;
}

TutorialCard::Status TutorialCard::StatusOf(const tutorial::Topic& topic) const {
    if (runner_.On() && topic_ == &topic) {
        return Status::Running;
    }
    const std::string progress = world_.TopicProgress(topic.id);
    if (progress.empty()) {
        return Status::New;
    }
    return progress == "finished" ? Status::Done : Status::Started;
}

std::vector<const tutorial::Step*> TutorialCard::SkipWarnings() const {
    if (topic_->id == tutorial::kBasicsTopic) {
        return runner_.WarningsNotReached();
    }
    std::vector<const tutorial::Step*> warnings;
    if (world_.TopicProgress(tutorial::kBasicsTopic) == "finished") {
        return warnings;
    }
    for (const tutorial::Step& step : tutorial::FindTopic(tutorial::kBasicsTopic)->chain()) {
        if (step.warning) {
            warnings.push_back(&step);
        }
    }
    return warnings;
}

void TutorialCard::DrawList() {
    ImGui::TextColored(theme::kWhite, "%s", strings::kTutorialListTitle);
    Wrapped(theme::kGraphite100, strings::kTutorialListText);
    ImGui::Spacing();
    // The topics in two columns where the list is wide enough, read row by
    // row; the two in a row as tall as the taller (docs/TUTORIAL.md,
    // section 19.5).
    const std::vector<tutorial::Topic>& topics = tutorial::Topics();
    const int columns = ImGui::GetContentRegionAvail().x > Px(kListWidth) * 0.75f ? 2 : 1;
    const float gap = Px(kListColumnGap);
    const float cellW = (ImGui::GetContentRegionAvail().x - gap * static_cast<float>(columns - 1)) /
                        static_cast<float>(columns);
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 pad(style.FramePadding.x, style.FramePadding.y);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (size_t first = 0; first < topics.size(); first += static_cast<size_t>(columns)) {
        const size_t last = std::min(topics.size(), first + static_cast<size_t>(columns));
        const ImVec2 rowStart = ImGui::GetCursorScreenPos();
        // A cell per topic, pressed as a whole: its text drawn first, on
        // the upper channel, and its button and hover behind it once the
        // row's height is known.
        drawList->ChannelsSplit(2);
        drawList->ChannelsSetCurrent(1);
        float rowH = 0.0f;
        for (size_t i = first; i < last; ++i) {
            const tutorial::Topic& topic = topics[i];
            const Status status = StatusOf(topic);
            char statusText[64];
            switch (status) {
                case Status::New:
                    std::snprintf(statusText, sizeof(statusText), "%s", strings::kTutorialListNew);
                    break;
                case Status::Running:
                    std::snprintf(statusText, sizeof(statusText), strings::kTutorialListAtStep,
                                  static_cast<int>(runner_.StepIndex() + 1), static_cast<int>(runner_.StepCount()));
                    break;
                case Status::Started:
                    std::snprintf(statusText, sizeof(statusText), "%s", strings::kTutorialListStarted);
                    break;
                case Status::Done:
                    std::snprintf(statusText, sizeof(statusText), "%s", strings::kTutorialListDone);
                    break;
            }
            // How many steps it has, on the status's line - a line of its
            // own made six topics taller than a small display
            // (docs/TUTORIAL.md, section 18.8). Not beside "At step 3 of
            // 8", which says it.
            std::string statusLine = statusText;
            if (status != Status::Running) {
                char steps[32];
                std::snprintf(steps, sizeof(steps), strings::kTutorialListSteps,
                              static_cast<int>(topic.chain().size()));
                statusLine.append(", ").append(steps);
            }
            const float cellX = rowStart.x + static_cast<float>(i - first) * (cellW + gap);
            ImGui::SetCursorScreenPos(ImVec2(cellX + pad.x, rowStart.y + pad.y));
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cellW - 2.0f * pad.x);
            ImGui::TextColored(theme::kWhite, "%s", topic.title);
            ImGui::SameLine();
            ImGui::TextColored(status == Status::Running ? theme::kTutorialHighlight : theme::kGraphite300, "%s",
                               statusLine.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, theme::kGraphite100);
            ImGui::TextUnformatted(topic.gist);
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            rowH = std::max(rowH, ImGui::GetItemRectMax().y + pad.y - rowStart.y);
        }

        drawList->ChannelsSetCurrent(0);
        for (size_t i = first; i < last; ++i) {
            const tutorial::Topic& topic = topics[i];
            const Status status = StatusOf(topic);
            const ImVec2 start(rowStart.x + static_cast<float>(i - first) * (cellW + gap), rowStart.y);
            const ImVec2 end(start.x + cellW, start.y + rowH);
            ImGui::SetCursorScreenPos(start);
            const std::string id = "tutorial_topic_" + std::string(topic.id);
            const bool pressed = ImGui::InvisibleButton(id.c_str(), ImVec2(cellW, rowH));
            if (status == Status::Running) {
                drawList->AddRect(start, end, ImGui::GetColorU32(theme::kTutorialHighlight), Px(6.0f),
                                  ImDrawFlags_None, Px(1.5f));
            }
            if (ImGui::IsItemHovered()) {
                drawList->AddRectFilled(start, end, ImGui::GetColorU32(theme::kHoverWash), Px(6.0f));
            }
            if (pressed) {
                // The topic running goes on where it is; any other starts
                // at its first step (question 12).
                if (status == Status::Running) {
                    host_.Act(action::TutorialPress{TutorialButton::CloseList});
                } else {
                    host_.Act(action::StartTutorial{std::string(topic.id)});
                }
            }
        }
        drawList->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(rowStart.x, rowStart.y + rowH));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::Spacing();
    }
    ImGui::Separator();
    // Close: back to the topic running, if the list was opened from
    // Settings during one; the end and skip cards' More topics ended theirs
    // (section 20).
    if (ImGui::Button(Labeled(strings::kTutorialListClose, "tutorial_closelist"))) {
        host_.Act(action::TutorialPress{TutorialButton::CloseList});
    }
}

// ================= The spotlight =================

std::optional<AnchorRect> TutorialCard::SpotlightRect() const {
    if (listing_ || runner_.GetState() != tutorial::Tutorial::State::OnStep || runner_.GoalMet()) {
        return std::nullopt;
    }
    // Not through a panel: the step's line says to close it first. The
    // Overview's own spots are the exception, drawn only while it is up -
    // and not through what is over it, the delete confirmation.
    const tutorial::Cover cover = world_.CanvasCover();
    if (InOverview(runner_.CurrentSpot())) {
        if (cover != tutorial::Cover::Overview || editor_.Input().At(Level::Popup) != nullptr) {
            return std::nullopt;
        }
    } else if (cover == tutorial::Cover::Overview || cover == tutorial::Cover::CheatSheet) {
        return std::nullopt;
    }
    return SpotRect();
}

void TutorialCard::DrawSpotlight() {
    const std::optional<AnchorRect> spot = SpotlightRect();
    if (!spot.has_value()) {
        return;
    }
    // A slow pulse, about two seconds a beat, that never fades out: a solid
    // ring, clear of the selection frame, in a soft glow of the same color.
    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(ImGui::GetTime() * 3.0));
    ImVec4 ring = theme::kTutorialHighlight;
    ring.w = 0.65f + 0.35f * pulse;
    ImVec4 glow = theme::kTutorialHighlight;
    glow.w = 0.10f + 0.12f * pulse;
    const float pad = Px(8.0f);
    const ImVec2 min(spot->min.x - pad, spot->min.y - pad);
    const ImVec2 max(spot->max.x + pad, spot->max.y + pad);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddRect(min, max, ImGui::GetColorU32(glow), Px(10.0f), ImDrawFlags_None, Px(12.0f));
    drawList->AddRect(min, max, ImGui::GetColorU32(ring), Px(10.0f), ImDrawFlags_None, Px(5.0f));
}

}  // namespace sz::ui
