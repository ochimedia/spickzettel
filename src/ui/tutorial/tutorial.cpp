#include "ui/tutorial/tutorial.h"

#include <algorithm>

#include "generated/ui_strings.h"

namespace sz::ui::tutorial {

namespace strings = ::sz::strings;

const SnippetFacts* Look::Now(core::ItemId id) const {
    for (const SnippetFacts& snippet : snippets) {
        if (snippet.id == id) {
            return &snippet;
        }
    }
    return nullptr;
}

const SnippetFacts* Look::AtStart(core::ItemId id) const {
    const auto it = start.firstSeen.find(id);
    return it == start.firstSeen.end() ? nullptr : &it->second;
}

InkGone Look::SubjectInkGone() const {
    if (!subject) {
        return {};
    }
    const auto it = inkGoneThisStep.find(*subject);
    return it == inkGoneThisStep.end() ? InkGone{} : it->second;
}

std::vector<const SnippetFacts*> Look::MadeHere() const {
    std::vector<const SnippetFacts*> made;
    const core::CanvasId here = world.CurrentCanvas();
    for (const SnippetFacts& snippet : snippets) {
        if (!snippet.deleted && snippet.canvas == here && !start.present.contains(snippet.id)) {
            made.push_back(&snippet);
        }
    }
    return made;
}

bool Look::Tutorials(core::FolderId id) const {
    return id != 0 && std::find(folders.begin(), folders.end(), id) != folders.end();
}

const FolderFacts* Look::FolderNow(core::FolderId id) const {
    if (!Tutorials(id)) {
        return nullptr;
    }
    for (const FolderFacts& facts : allFolders) {
        if (facts.id == id) {
            return &facts;
        }
    }
    return nullptr;
}

const CanvasFacts* Look::CanvasNow(core::CanvasId id) const {
    for (const CanvasFacts& facts : canvases) {
        if (facts.id == id) {
            return &facts;
        }
    }
    return nullptr;
}

const ProfileFacts* Look::Profile() const {
    if (!profile) {
        return nullptr;
    }
    for (const ProfileFacts& facts : profiles) {
        if (facts.name == *profile) {
            return &facts;
        }
    }
    return nullptr;
}

const ProfileFacts* Look::ProfileAtStart() const {
    if (!profile) {
        return nullptr;
    }
    for (const ProfileFacts& facts : start.profiles) {
        if (facts.name == *profile) {
            return &facts;
        }
    }
    return nullptr;
}

void Tutorial::Start(core::FolderId folder) { Resume({}, folder); }

void Tutorial::Resume(std::string_view id, core::FolderId folder) {
    size_t at = 0;
    for (size_t i = 0; i < chain_->size(); ++i) {
        if ((*chain_)[i].id == id) {
            at = i;
        }
    }
    state_ = State::OnStep;
    outcome_ = Outcome::None;
    folder_ = folder;
    index_ = at;
    reached_ = at;
    // The steps before one resumed at were passed, and a gated one only
    // when it was done: going back to it must not lock Next.
    done_.assign(chain_->size(), false);
    std::fill(done_.begin(), done_.begin() + static_cast<std::ptrdiff_t>(at), true);
    order_.clear();
    wasDeleted_.clear();
    lastDeleted_.reset();
    subject_.reset();
    madeFolders_.clear();
    madeProfiles_.clear();
    profile_.reset();
    Begin();
}

std::vector<core::FolderId> Tutorial::Folders() const {
    std::vector<core::FolderId> folders{folder_};
    folders.insert(folders.end(), madeFolders_.begin(), madeFolders_.end());
    return folders;
}

void Tutorial::Begin() {
    begun_ = false;
    metThisVisit_ = false;
    doneAt_.reset();
    hint_.reset();
    lookSubject_.reset();
    reached_ = std::max(reached_, index_);
}

bool Tutorial::NextEnabled() const {
    return state_ == State::OnStep && (!CurrentStep().gated || GoalMet());
}

void Tutorial::Next() {
    if (!NextEnabled()) {
        return;
    }
    if (index_ + 1 == chain_->size()) {
        state_ = State::Off;
        outcome_ = Outcome::Finished;
        return;
    }
    ++index_;
    Begin();
}

void Tutorial::Back() {
    if (state_ == State::Skipped) {
        state_ = State::OnStep;
        Begin();
        return;
    }
    if (state_ == State::OnStep && index_ > 0) {
        --index_;
        Begin();
    }
}

void Tutorial::Skip() {
    if (state_ == State::OnStep) {
        state_ = State::Skipped;
    }
}

void Tutorial::Done() {
    if (state_ == State::Skipped) {
        state_ = State::Off;
        outcome_ = Outcome::Skipped;
    } else if (state_ == State::OnStep && index_ + 1 == chain_->size()) {
        Next();
    }
}

void Tutorial::Leave() {
    if (state_ == State::Skipped) {
        Done();
        return;
    }
    if (state_ == State::OnStep && index_ + 1 == chain_->size()) {
        state_ = State::Off;
        outcome_ = Outcome::Finished;
        return;
    }
    state_ = State::Off;
    outcome_ = Outcome::None;
}

Spot Tutorial::CurrentSpot() const {
    if (state_ != State::OnStep) {
        return Spot::None;
    }
    if (hint_ && hint_->need == Need::SubjectOnScreen) {
        return Spot::DockChip;
    }
    if (hint_ && hint_->need == Need::ShowingIt) {
        return Spot::Showing;
    }
    return CurrentStep().spot;
}

std::vector<const Step*> Tutorial::WarningsNotReached() const {
    std::vector<const Step*> warnings;
    for (size_t i = reached_ + 1; i < chain_->size(); ++i) {
        if ((*chain_)[i].warning) {
            warnings.push_back(&(*chain_)[i]);
        }
    }
    return warnings;
}

std::string Tutorial::Progress() const {
    switch (state_) {
        case State::OnStep:
            return std::string(CurrentStep().id);
        case State::Skipped:
            return "skipped";
        case State::Off:
            break;
    }
    switch (outcome_) {
        case Outcome::Finished:
            return "finished";
        case Outcome::Skipped:
            return "skipped";
        case Outcome::None:
            break;
    }
    return {};
}

void Tutorial::Observe(const std::vector<SnippetFacts>& snippets) {
    for (const SnippetFacts& snippet : snippets) {
        start_.firstSeen.try_emplace(snippet.id, snippet);
        const auto [seen, isNew] = wasDeleted_.try_emplace(snippet.id, snippet.deleted);
        if (isNew) {
            order_.push_back(snippet.id);
        } else if (snippet.deleted && !seen->second) {
            lastDeleted_ = snippet.id;
        }
        seen->second = snippet.deleted;
        if (snippet.deleted) {
            deletedThisStep_.insert(snippet.id);
        }
        if (snippet.pinned && !snippet.deleted) {
            pinnedThisStep_.insert(snippet.id);
        }
    }
}

void Tutorial::TallyInk(const World& world, const std::vector<SnippetFacts>& snippets) {
    const core::Tool tool = world.ToolInHand();
    for (const SnippetFacts& snippet : snippets) {
        if (snippet.deleted) {
            // Gone as a whole, which is no erasing; back, it starts again
            // from what it has then.
            inkLastFrame_.erase(snippet.id);
            continue;
        }
        const float ink = snippet.InkPx();
        const auto [last, isNew] = inkLastFrame_.try_emplace(snippet.id, ink);
        if (!isNew && ink < last->second) {
            InkGone& gone = inkGoneThisStep_[snippet.id];
            const float less = last->second - ink;
            if (tool != core::Tool::Erase) {
                gone.otherTool += less;
            } else if (world.EraserShape() == core::DrawShape::Rectangle) {
                gone.rectangleEraser += less;
            } else {
                gone.eraser += less;
            }
        }
        last->second = ink;
    }
}

void Tutorial::ChooseSubject(const World& world, const std::vector<SnippetFacts>& snippets) {
    const SubjectRule rule = CurrentStep().subject;
    lookSubject_.reset();
    auto find = [&](core::ItemId id) -> const SnippetFacts* {
        for (const SnippetFacts& snippet : snippets) {
            if (snippet.id == id) {
                return &snippet;
            }
        }
        return nullptr;
    };
    switch (rule) {
        case SubjectRule::None:
            return;
        case SubjectRule::LastDeleted:
            if (lastDeleted_ && find(*lastDeleted_) != nullptr) {
                lookSubject_ = lastDeleted_;
            }
            return;
        case SubjectRule::Any:
        case SubjectRule::CanMove:
        case SubjectRule::Pinned:
            break;
    }
    const core::CanvasId here = world.CurrentCanvas();
    // What the rule asks of a snippet besides: one that can move, or one
    // pinned.
    auto wanted = [&](const SnippetFacts& snippet) {
        switch (rule) {
            case SubjectRule::CanMove:
                return !snippet.fullscreen;
            case SubjectRule::Pinned:
                return snippet.pinned;
            case SubjectRule::None:
            case SubjectRule::Any:
            case SubjectRule::LastDeleted:
                break;
        }
        return true;
    };
    // How well a snippet does as the subject: on this canvas, on screen,
    // and what the rule asks besides. 7 is all three.
    auto quality = [&](const SnippetFacts& snippet) {
        return (snippet.canvas == here ? 4 : 0) + (!snippet.minimized ? 2 : 0) + (wanted(snippet) ? 1 : 0);
    };
    auto fits = [&](std::optional<core::ItemId> id) {
        const SnippetFacts* snippet = id ? find(*id) : nullptr;
        return snippet != nullptr && !snippet->deleted && quality(*snippet) == 7;
    };
    // The hand first: a snippet being drawn on, then the one selected
    // last - working on "the wrong one" is working on the right one.
    std::optional<core::ItemId> pick;
    if (fits(world.DrawingItem())) {
        pick = world.DrawingItem();
    } else {
        const std::vector<core::ItemId> selection = world.Selection();
        for (auto it = selection.rbegin(); it != selection.rend() && !pick; ++it) {
            if (fits(*it)) {
                pick = *it;
            }
        }
    }
    // Then the subject there was, and then the newest that will do.
    if (!pick && fits(subject_)) {
        pick = subject_;
    }
    for (auto it = order_.rbegin(); it != order_.rend() && !pick; ++it) {
        if (fits(*it)) {
            pick = *it;
        }
    }
    // None will do: the subject there was, while it is there at all, so
    // that the needs can say what is wrong with it - or else the best
    // there is, the newest of those.
    if (!pick && subject_) {
        const SnippetFacts* was = find(*subject_);
        if (was != nullptr && !was->deleted) {
            pick = subject_;
        }
    }
    if (!pick) {
        int best = -1;
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            const SnippetFacts* snippet = find(*it);
            if (snippet != nullptr && !snippet->deleted && quality(*snippet) > best) {
                best = quality(*snippet);
                pick = *it;
            }
        }
    }
    if (pick) {
        subject_ = pick;
    }
    lookSubject_ = pick;
}

std::optional<Hint> Tutorial::UnmetNeed(const World& world, const Look& look) const {
    const SnippetFacts* subject = look.Subject();
    auto unmet = [](Need need, const char* text, HintButton button = HintButton::None, core::CanvasId canvas = 0) {
        return Hint{text, button, canvas, need};
    };
    // A need about the subject, with no subject, is the need for one.
    const Hint noSubject = unmet(Need::ASubject, strings::kTutorialNeedASubject, HintButton::PutOneHere);
    const Hint noProfile = unmet(Need::TutorialsProfile, strings::kTutorialNeedTutorialsProfile);
    for (const Need need : CurrentStep().needs) {
        switch (need) {
            case Need::InTutorialFolder:
                if (!look.Tutorials(world.FolderOf(world.CurrentCanvas()))) {
                    return unmet(need, strings::kTutorialNeedInTutorialFolder, HintButton::BackToTutorial);
                }
                break;
            case Need::CanvasUncovered:
                switch (world.CanvasCover()) {
                    case Cover::None:
                        break;
                    case Cover::Overview:
                        return unmet(need, strings::kTutorialNeedCloseOverview);
                    case Cover::CheatSheet:
                        return unmet(need, strings::kTutorialNeedCloseCheatSheet);
                    case Cover::Popup:
                        return unmet(need, strings::kTutorialNeedClosePopup);
                }
                break;
            case Need::NoDrawingMode:
                if (world.DrawingItem()) {
                    return unmet(need, strings::kTutorialNeedNoDrawingMode);
                }
                break;
            case Need::NoOtherTool: {
                const auto tool = world.CreationToolInHand();
                if (tool && *tool != core::ItemCreationKind::Screenshot) {
                    return unmet(need, strings::kTutorialNeedNoOtherTool);
                }
                break;
            }
            case Need::ASubject:
                if (subject == nullptr) {
                    return noSubject;
                }
                break;
            case Need::SubjectHere:
                if (subject == nullptr) {
                    return noSubject;
                }
                if (subject->canvas != world.CurrentCanvas()) {
                    return unmet(need, strings::kTutorialNeedSubjectHere, HintButton::BackThere, subject->canvas);
                }
                break;
            case Need::SubjectOnScreen:
                if (subject == nullptr) {
                    return noSubject;
                }
                if (subject->minimized) {
                    return unmet(need, strings::kTutorialNeedSubjectOnScreen);
                }
                break;
            case Need::SubjectCanMove:
                if (subject == nullptr) {
                    return noSubject;
                }
                if (subject->fullscreen) {
                    return unmet(need, strings::kTutorialNeedSubjectCanMove);
                }
                break;
            case Need::SubjectSelected: {
                if (subject == nullptr) {
                    return noSubject;
                }
                const std::vector<core::ItemId> selection = world.Selection();
                if (std::find(selection.begin(), selection.end(), subject->id) == selection.end()) {
                    return unmet(need, strings::kTutorialNeedSubjectSelected);
                }
                break;
            }
            case Need::DrawingOnSubject:
                if (subject == nullptr) {
                    return noSubject;
                }
                if (world.DrawingItem() != subject->id) {
                    return unmet(need, strings::kTutorialNeedDrawingOnSubject);
                }
                break;
            case Need::DeletedSubject:
                // Deleted now, or at some point in this step: brought back
                // since is what the step is waiting for.
                if (subject == nullptr || !deletedThisStep_.contains(subject->id)) {
                    return unmet(need, strings::kTutorialNeedDeletedSubject);
                }
                break;
            case Need::SubjectPinned:
                if (subject == nullptr) {
                    return noSubject;
                }
                if (!subject->pinned) {
                    return unmet(need, strings::kTutorialNeedSubjectPinned);
                }
                break;
            case Need::PenInHand:
                if (world.ToolInHand() != core::Tool::Draw) {
                    return unmet(need, strings::kTutorialNeedPenInHand);
                }
                break;
            case Need::SubjectDrawnOn:
                if (subject == nullptr) {
                    return noSubject;
                }
                if (subject->strokes.empty()) {
                    return unmet(need, strings::kTutorialNeedSubjectDrawnOn);
                }
                break;
            case Need::OverviewUp:
                if (world.CanvasCover() != Cover::Overview) {
                    return unmet(need, strings::kTutorialNeedOverviewUp);
                }
                break;
            case Need::CanvasesTab:
                if (!world.OverviewShowsCanvases()) {
                    return unmet(need, strings::kTutorialNeedCanvasesTab);
                }
                break;
            case Need::DeletedShown:
                if (!world.OverviewShowsDeleted()) {
                    return unmet(need, strings::kTutorialNeedDeletedShown);
                }
                break;
            case Need::SomethingInTrash: {
                const bool canvas = std::any_of(look.canvases.begin(), look.canvases.end(),
                                                [](const CanvasFacts& facts) { return facts.deleted; });
                const bool folder = std::any_of(look.folders.begin(), look.folders.end(), [&](core::FolderId id) {
                    const FolderFacts* facts = look.FolderNow(id);
                    return facts != nullptr && facts->deleted;
                });
                if (!canvas && !folder) {
                    return unmet(need, strings::kTutorialNeedSomethingInTrash);
                }
                break;
            }
            case Need::SettingsUp:
                if (world.CanvasCover() != Cover::Overview) {
                    return unmet(need, strings::kTutorialNeedSettingsUp);
                }
                break;
            case Need::SettingsTab:
                if (!world.OverviewShowsSettings()) {
                    return unmet(need, strings::kTutorialNeedSettingsTab);
                }
                break;
            case Need::ProfilesSection:
                if (world.SettingsSectionShown() != SettingsSection::Profiles) {
                    return unmet(need, strings::kTutorialNeedProfilesSection);
                }
                break;
            case Need::BehaviorSection:
                if (world.SettingsSectionShown() != SettingsSection::Behavior) {
                    return unmet(need, strings::kTutorialNeedBehaviorSection);
                }
                break;
            case Need::AProgramUnderneath:
                if (world.Underneath().empty()) {
                    return unmet(need, world.KeyLabel(CommandId::ToggleEditMode)
                                           ? strings::kTutorialNeedAProgramUnderneath
                                           : strings::kTutorialNeedAProgramUnderneathTray);
                }
                break;
            // The three about the tutorial's profile are, with none, the
            // need for one.
            case Need::TutorialsProfile:
                if (look.Profile() == nullptr) {
                    return noProfile;
                }
                break;
            case Need::ShowingIt:
                if (look.Profile() == nullptr) {
                    return noProfile;
                }
                if (world.SettingsShowing() != look.Profile()->name) {
                    return unmet(need, strings::kTutorialNeedShowingIt);
                }
                break;
            case Need::SomethingSetInIt:
                if (look.Profile() == nullptr) {
                    return noProfile;
                }
                if (look.Profile()->stated == 0) {
                    return unmet(need, strings::kTutorialNeedSomethingSetInIt);
                }
                break;
            case Need::OverAnotherProgram:
                if (look.Profile() == nullptr) {
                    return noProfile;
                }
                if (look.Profile()->matchesUnderneath) {
                    return unmet(need, world.KeyLabel(CommandId::ToggleEditMode)
                                           ? strings::kTutorialNeedOverAnotherProgram
                                           : strings::kTutorialNeedOverAnotherProgramTray);
                }
                break;
        }
    }
    return std::nullopt;
}

void Tutorial::Update(const World& world, double now) {
    if (state_ != State::OnStep) {
        return;
    }
    const std::vector<FolderFacts> allFolders = world.Folders();
    // A folder made while a step that keeps them is up is the tutorial's
    // from then on, this frame included.
    if (begun_ && CurrentStep().keepsFolders) {
        for (const FolderFacts& facts : allFolders) {
            const bool wasThere = std::any_of(start_.folders.begin(), start_.folders.end(),
                                              [&](const FolderFacts& was) { return was.id == facts.id; });
            if (!facts.deleted && !wasThere && facts.id != folder_ &&
                std::find(madeFolders_.begin(), madeFolders_.end(), facts.id) == madeFolders_.end()) {
                madeFolders_.push_back(facts.id);
            }
        }
    }
    const std::vector<ProfileFacts> profiles = world.Profiles();
    // So are the profiles made while a step that keeps them is up.
    if (begun_ && CurrentStep().keepsProfiles) {
        for (const ProfileFacts& facts : profiles) {
            const bool wasThere = std::any_of(start_.profiles.begin(), start_.profiles.end(),
                                              [&](const ProfileFacts& was) { return was.name == facts.name; });
            if (!wasThere &&
                std::find(madeProfiles_.begin(), madeProfiles_.end(), facts.name) == madeProfiles_.end()) {
                madeProfiles_.push_back(facts.name);
            }
        }
    }
    // The tutorial's among them: the newest still there that matches a
    // program - a blank one made first by mistake is not it - else the
    // newest still there.
    profile_.reset();
    for (auto it = madeProfiles_.rbegin(); it != madeProfiles_.rend(); ++it) {
        const auto facts = std::find_if(profiles.begin(), profiles.end(),
                                        [&](const ProfileFacts& each) { return each.name == *it; });
        if (facts == profiles.end()) {
            continue;
        }
        if (!facts->program.empty()) {
            profile_ = *it;
            break;
        }
        if (!profile_) {
            profile_ = *it;
        }
    }
    const std::vector<core::FolderId> folders = Folders();
    std::vector<SnippetFacts> snippets;
    std::vector<CanvasFacts> canvases;
    for (const core::FolderId folder : folders) {
        const std::vector<SnippetFacts> in = world.SnippetsIn(folder);
        snippets.insert(snippets.end(), in.begin(), in.end());
        const std::vector<CanvasFacts> of = world.CanvasesIn(folder);
        canvases.insert(canvases.end(), of.begin(), of.end());
    }
    if (!begun_) {
        start_ = StartRecord{};
        start_.canvas = world.CurrentCanvas();
        start_.folders = allFolders;
        start_.canvases = canvases;
        start_.profiles = profiles;
        start_.showings = world.Showings();
        start_.pinnedViews = world.PinnedViews();
        start_.viewModes = world.ViewModes();
        start_.quickCaptures = world.Captures(core::HotkeySlot::QuickCapture);
        start_.silentCaptures = world.Captures(core::HotkeySlot::SilentCapture);
        start_.penColor = world.PenColor();
        start_.penWidth = world.PenWidth();
        for (const SnippetFacts& snippet : snippets) {
            start_.present.insert(snippet.id);
        }
        deletedThisStep_.clear();
        pinnedThisStep_.clear();
        inkGoneThisStep_.clear();
        inkLastFrame_.clear();
        trashedThisStep_.clear();
        begun_ = true;
    }
    for (const CanvasFacts& facts : canvases) {
        if (facts.deleted) {
            trashedThisStep_.insert(facts.id);
        }
    }
    for (const FolderFacts& facts : allFolders) {
        if (facts.deleted && std::find(folders.begin(), folders.end(), facts.id) != folders.end()) {
            trashedThisStep_.insert(facts.id);
        }
    }
    Observe(snippets);
    TallyInk(world, snippets);
    ChooseSubject(world, snippets);

    const Step& step = CurrentStep();
    // Met on this visit, the step only waits out its second: what meeting
    // it did may well leave a need unmet - the delete step's subject is
    // gone - and that is no reason to say so now.
    if (metThisVisit_) {
        hint_.reset();
        if (now - *doneAt_ >= kMoveOnSeconds && index_ + 1 < chain_->size()) {
            ++index_;
            Begin();
        }
        return;
    }
    // The goal first: a result is a result, however it came about, and
    // what makes it may itself leave a need unmet - deleting the only
    // snippet leaves no subject. The needs guide only while it is not met.
    const Look look{world,    start_,           snippets,         folder_,          folders,
                    allFolders, canvases,       lookSubject_,     deletedThisStep_, pinnedThisStep_,
                    inkGoneThisStep_, trashedThisStep_, profiles, profile_};
    if (step.goal != nullptr && step.goal(look)) {
        metThisVisit_ = true;
        done_[index_] = true;
        doneAt_ = now;
        hint_.reset();
        return;
    }
    hint_ = UnmetNeed(world, look);
    if (hint_) {
        return;
    }
    for (const NearMiss& miss : step.nearMisses) {
        if (miss.check(look)) {
            hint_ = Hint{miss.text};
            break;
        }
    }
}

std::string Expand(std::string_view text, const World& world, core::CanvasId canvas, std::string_view profile) {
    auto trigger = [](core::CreationTrigger held) -> std::string {
        switch (held) {
            case core::CreationTrigger::Ctrl:
                return "Ctrl";
            case core::CreationTrigger::Alt:
                return "Alt";
            case core::CreationTrigger::Plain:
            case core::CreationTrigger::Off:
                break;
        }
        return {};
    };
    std::string out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t open = text.find('{', at);
        const size_t close = open == std::string_view::npos ? open : text.find('}', open);
        if (close == std::string_view::npos) {
            out.append(text.substr(at));
            break;
        }
        out.append(text.substr(at, open - at));
        const std::string_view name = text.substr(open + 1, close - open - 1);
        std::optional<std::string> value;
        if (name.starts_with("key:")) {
            const std::string_view command = name.substr(4);
            for (const CommandInfo& info : kCommands) {
                if (info.name == command) {
                    value = world.KeyLabel(info.id).value_or(std::string{});
                }
            }
        } else if (name == "trigger:screenshot") {
            value = trigger(world.ScreenshotTrigger());
        } else if (name == "trigger:drawing") {
            value = trigger(world.DrawingTrigger());
        } else if (name == "canvas") {
            value = world.CanvasName(canvas);
        } else if (name == "profile") {
            value = std::string(profile);
        } else if (name == "program" || name == "running") {
            value = std::string();
            for (const ProfileFacts& facts : world.Profiles()) {
                if (name == "program" && facts.name == profile) {
                    value = facts.program;
                } else if (name == "running" && facts.running) {
                    value = facts.name;
                }
            }
        } else if (name == "underneath") {
            value = world.Underneath();
        } else if (name == "showing") {
            value = world.SettingsShowing().value_or(strings::kProfilesDefaults);
        }
        out.append(value ? *value : std::string(text.substr(open, close - open + 1)));
        at = close + 1;
    }
    return out;
}

}  // namespace sz::ui::tutorial
