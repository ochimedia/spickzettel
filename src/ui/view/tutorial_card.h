#pragma once

// The tutorial card - docs/VIEW_LAYER.md, section 7, and docs/TUTORIAL.md,
// sections 3 and 7.4: the card, a small window with the step up, its hint
// and the buttons, above the panels and below the delete confirmation;
// and the spotlight, a ring around what the step points at, drawn over
// everything but the pointer. What it keeps of its own is the runner -
// which step is up, and what it has seen - and whether the card has been
// dragged somewhere. It reads the app through the world, and asks for
// everything else as an action.

#include <optional>
#include <string_view>

#include "core/session/session.h"
#include "ui/editor.h"
#include "ui/tutorial/topics.h"
#include "ui/tutorial/tutorial.h"
#include "ui/tutorial/world.h"
#include "ui/view/anchors.h"
#include "ui/view/view_host.h"
#include "ui/view_action.h"

namespace sz::ui {

class TutorialCard {
public:
    TutorialCard(const core::Session& session, const Editor& editor, const tutorial::World& world,
                 const AnchorBoard& anchors, ViewHost& host);

    // `topic` at its first step, in `folder`, just made for it.
    void Start(const tutorial::Topic& topic, core::FolderId folder);
    // `topic` at the step `id` names, in `folder` - after quitting partway.
    void Resume(const tutorial::Topic& topic, std::string_view id, core::FolderId folder);
    // The running topic let go of for another (section 13.3) - see
    // Tutorial::Leave.
    void Leave() { runner_.Leave(); }
    // Whether a topic has run since the app started: until one has, the
    // topic kept as running is a resume's to go on with.
    bool HasRun() const { return hasRun_; }
    // The tutorial's folder, made again.
    void MoveTo(core::FolderId folder) { runner_.MoveTo(folder); }
    // One of the card's buttons, as an action asked for it.
    void Press(TutorialButton button);

    const tutorial::Tutorial& Runner() const { return runner_; }
    // The topic running, or run last.
    const tutorial::Topic& CurrentTopic() const { return *topic_; }

    // Stage 1, in edit mode: the runner brought up to date with the app.
    // `now` in seconds.
    void Update(double now);
    // Stage 6: the card, while the tutorial is on.
    void Draw(float displayW, float displayH);
    // Stage 7: the spotlight, while the step up points at something on
    // screen and its goal is not met yet.
    void DrawSpotlight();

    // What the spotlight rings this frame, if anything - read from the
    // anchor board, so only once the canvas has been drawn.
    std::optional<AnchorRect> SpotRect() const;
    // What the hint's button asks for, while the hint up has one.
    std::optional<ViewAction> HintAction() const;

private:
    // The subject's rectangle, while it is on the canvas being looked at.
    std::optional<AnchorRect> SubjectRect() const;
    void DrawStep();
    void DrawSkipped();
    // Done, keep the folder - on the end card and the skip card.
    void DoneKeepButton();
    // The hint under a step's text, and its button.
    void DrawHint(const tutorial::Hint& hint);

    const core::Session& session_;
    const Editor& editor_;
    const tutorial::World& world_;
    const AnchorBoard& anchors_;
    ViewHost& host_;

    const tutorial::Topic* topic_;
    tutorial::Tutorial runner_;
    // Dragged by the user: left where it was put for the rest of the run.
    bool moved_ = false;
    bool hasRun_ = false;
};

}  // namespace sz::ui
