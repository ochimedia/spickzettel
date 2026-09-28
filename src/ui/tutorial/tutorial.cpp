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

void Tutorial::Start(core::FolderId folder) { Resume({}, folder); }

void Tutorial::Resume(std::string_view id, core::FolderId folder) {
    size_t at = 0;
    for (size_t i = 0; i < chain_.size(); ++i) {
        if (chain_[i].id == id) {
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
    done_.assign(chain_.size(), false);
    std::fill(done_.begin(), done_.begin() + static_cast<std::ptrdiff_t>(at), true);
    order_.clear();
    wasDeleted_.clear();
    lastDeleted_.reset();
    subject_.reset();
    Begin();
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
    if (index_ + 1 == chain_.size()) {
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
    } else if (state_ == State::OnStep && index_ + 1 == chain_.size()) {
        Next();
    }
}

Spot Tutorial::CurrentSpot() const {
    if (state_ != State::OnStep) {
        return Spot::None;
    }
    if (hint_ && hint_->need == Need::SubjectOnScreen) {
        return Spot::DockChip;
    }
    return CurrentStep().spot;
}

std::vector<const Step*> Tutorial::WarningsNotReached() const {
    std::vector<const Step*> warnings;
    for (size_t i = reached_ + 1; i < chain_.size(); ++i) {
        if (chain_[i].warning) {
            warnings.push_back(&chain_[i]);
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
            break;
    }
    const core::CanvasId here = world.CurrentCanvas();
    // How well a snippet does as the subject: on this canvas, on screen,
    // and one that can move where the step needs that. 7 is all three.
    auto quality = [&](const SnippetFacts& snippet) {
        return (snippet.canvas == here ? 4 : 0) + (!snippet.minimized ? 2 : 0) +
               (rule != SubjectRule::CanMove || !snippet.fullscreen ? 1 : 0);
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

std::optional<Hint> Tutorial::UnmetNeed(const World& world, const std::vector<SnippetFacts>& snippets) const {
    const SnippetFacts* subject = nullptr;
    if (lookSubject_) {
        for (const SnippetFacts& snippet : snippets) {
            if (snippet.id == *lookSubject_) {
                subject = &snippet;
            }
        }
    }
    auto unmet = [](Need need, const char* text, HintButton button = HintButton::None, core::CanvasId canvas = 0) {
        return Hint{text, button, canvas, need};
    };
    // A need about the subject, with no subject, is the need for one.
    const Hint noSubject = unmet(Need::ASubject, strings::kTutorialNeedASubject, HintButton::PutOneHere);
    for (const Need need : CurrentStep().needs) {
        switch (need) {
            case Need::InTutorialFolder:
                if (world.FolderOf(world.CurrentCanvas()) != folder_) {
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
        }
    }
    return std::nullopt;
}

void Tutorial::Update(const World& world, double now) {
    if (state_ != State::OnStep) {
        return;
    }
    const std::vector<SnippetFacts> snippets = world.SnippetsIn(folder_);
    if (!begun_) {
        start_ = StartRecord{};
        start_.showings = world.Showings();
        for (const SnippetFacts& snippet : snippets) {
            start_.present.insert(snippet.id);
        }
        deletedThisStep_.clear();
        begun_ = true;
    }
    Observe(snippets);
    ChooseSubject(world, snippets);

    const Step& step = CurrentStep();
    // Met on this visit, the step only waits out its second: what meeting
    // it did may well leave a need unmet - the delete step's subject is
    // gone - and that is no reason to say so now.
    if (metThisVisit_) {
        hint_.reset();
        if (now - *doneAt_ >= kMoveOnSeconds && index_ + 1 < chain_.size()) {
            ++index_;
            Begin();
        }
        return;
    }
    // The goal first: a result is a result, however it came about, and
    // what makes it may itself leave a need unmet - deleting the only
    // snippet leaves no subject. The needs guide only while it is not met.
    const Look look{world, start_, snippets, lookSubject_, deletedThisStep_};
    if (step.goal != nullptr && step.goal(look)) {
        metThisVisit_ = true;
        done_[index_] = true;
        doneAt_ = now;
        hint_.reset();
        return;
    }
    hint_ = UnmetNeed(world, snippets);
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

std::string Expand(std::string_view text, const World& world, core::CanvasId canvas) {
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
        }
        out.append(value ? *value : std::string(text.substr(open, close - open + 1)));
        at = close + 1;
    }
    return out;
}

}  // namespace sz::ui::tutorial
