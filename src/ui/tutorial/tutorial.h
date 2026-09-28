#pragma once

// The tutorial's runner - docs/TUTORIAL.md, section 5: which step of a
// chain is up, whether it is done, what its card says under the text,
// and which snippet it is about. The same for every step; what a step
// is lives in its row (step.h). It reads the app through a World, once
// a frame of edit mode, and changes nothing but its own state: what the
// card's buttons ask for is the view's to do. No ImGui.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "ui/tutorial/step.h"
#include "ui/tutorial/world.h"

namespace sz::ui::tutorial {

// How long a do step shows its check before the card moves on by itself
// (question 2).
inline constexpr double kMoveOnSeconds = 1.0;

// A line under the step's text: a need not met, or a near miss.
struct Hint {
    // A ui_strings text, with the {placeholders} of Expand.
    const char* text = "";
    HintButton button = HintButton::None;
    // For BackThere, and what {canvas} names: the subject's canvas.
    core::CanvasId canvas = 0;
    // The need it is for, if it is one.
    std::optional<Need> need;
};

class Tutorial {
public:
    enum class State {
        // No tutorial: never started, finished, or skipped and let go of.
        Off,
        // A step is up.
        OnStep,
        // The skip card is up: Back goes to the step skipped from, Done
        // lets go.
        Skipped,
    };
    // How the last run ended - what the settings file keeps once it is
    // Off (see Progress).
    enum class Outcome { None, Finished, Skipped };

    // `chain` outlives the tutorial - see chains.h. A tutorial is replaced
    // by another for another chain.
    explicit Tutorial(const std::vector<Step>& chain) : chain_(&chain) {}

    // At the first step, in `folder` - a folder the view has just made
    // for it (section 6.5).
    void Start(core::FolderId folder);
    // At the step `id` names - the first, when none does - as a start
    // after quitting partway does (section 7.6).
    void Resume(std::string_view id, core::FolderId folder);
    // The tutorial's folder was made again (Go back to the tutorial, with
    // the old one gone).
    void MoveTo(core::FolderId folder) { folder_ = folder; }

    // Once a frame of edit mode: the step's start record, the subject,
    // the goal, the needs, the near misses, and moving on a second after
    // a do step is done - section 5, in that order. `now` in seconds, on
    // any clock that only goes forward.
    void Update(const World& world, double now);

    // The card's buttons. Next does nothing on a gated step whose goal is
    // not met; on the last step it finishes. Back from the skip card goes
    // to the step skipped from. Done lets go of the skip card - or, on
    // the last step, is Next.
    void Next();
    void Back();
    void Skip();
    void Done();
    // Let go of for another topic (section 13.3): finished from the last
    // step, skipped from the skip card, and otherwise left where it was,
    // Off with nothing to say, so that the progress kept stays at its
    // step.
    void Leave();

    State GetState() const { return state_; }
    Outcome GetOutcome() const { return outcome_; }
    bool On() const { return state_ != State::Off; }
    core::FolderId Folder() const { return folder_; }
    size_t StepIndex() const { return index_; }
    size_t StepCount() const { return chain_->size(); }
    const std::vector<Step>& Chain() const { return *chain_; }
    // The step up - or skipped from, on the skip card. Only while On.
    const Step& CurrentStep() const { return (*chain_)[index_]; }
    // Whether the step up has had its goal met, on this visit or an
    // earlier one (goals are latched).
    bool GoalMet() const { return done_.size() > index_ && done_[index_]; }
    bool NextEnabled() const;
    const std::optional<Hint>& CurrentHint() const { return hint_; }
    std::optional<core::ItemId> Subject() const { return lookSubject_; }
    // What the spotlight rings now: the step's spot, or the subject's
    // chip in the dock while that is what the step needs.
    Spot CurrentSpot() const;
    // The warnings the skip card repeats: those not reached before the
    // skip.
    std::vector<const Step*> WarningsNotReached() const;

    // What the settings file keeps (section 7.6): the step's id while one
    // is up, "skipped" from the skip card on, "finished" once done - and
    // empty for a tutorial never started.
    std::string Progress() const;

private:
    void Begin();
    // Notes what the tutorial's snippets are now: first seen, the order
    // they turned up in, and which went from live to deleted.
    void Observe(const std::vector<SnippetFacts>& snippets);
    void ChooseSubject(const World& world, const std::vector<SnippetFacts>& snippets);
    // The first need not met, as the hint, or nothing.
    std::optional<Hint> UnmetNeed(const World& world, const std::vector<SnippetFacts>& snippets) const;

    const std::vector<Step>* chain_;
    State state_ = State::Off;
    Outcome outcome_ = Outcome::None;
    core::FolderId folder_ = 0;
    size_t index_ = 0;
    // The furthest step reached this run, for the skip card's warnings.
    size_t reached_ = 0;
    // Per step: its goal met at some point (latched).
    std::vector<bool> done_;

    // The visit to the step up.
    bool begun_ = false;
    bool metThisVisit_ = false;
    std::optional<double> doneAt_;
    StartRecord start_;
    std::unordered_set<core::ItemId> deletedThisStep_;
    std::optional<Hint> hint_;

    // Across steps: the tutorial's snippets in the order they turned up,
    // whether each was deleted when last seen, the last one deleted, and
    // the subject, which stays from step to step while it will do.
    std::vector<core::ItemId> order_;
    std::unordered_map<core::ItemId, bool> wasDeleted_;
    std::optional<core::ItemId> lastDeleted_;
    std::optional<core::ItemId> subject_;
    // The subject as the step up sees it - none for a step about none.
    std::optional<core::ItemId> lookSubject_;
};

// `text` with its placeholders filled in from `world`:
//  - {key:<command>}, the key bound to the command of that name in
//    kCommands - "undo", "toggleEditMode" - or nothing when unbound;
//  - {trigger:screenshot} and {trigger:drawing}, the trigger's key - Ctrl,
//    Alt - or nothing when it is plain or off;
//  - {canvas}, the name of `canvas`.
// Anything else in braces is left as it is.
std::string Expand(std::string_view text, const World& world, core::CanvasId canvas = 0);

}  // namespace sz::ui::tutorial
