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
#include <vector>

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
    // The list of topics (section 13.3), up until one is chosen or it is
    // left.
    void OpenList() {
        listing_ = true;
        PlaceAnew();
    }
    bool Listing() const { return listing_; }

    // Where the user is with a topic, as the list says it.
    enum class Status { New, Running, Started, Done };
    Status StatusOf(const tutorial::Topic& topic) const;
    // The warnings the skip card repeats (section 13.6): Basics' own not
    // reached in this run, while Basics runs; from any other topic, all of
    // Basics' until Basics has been finished once.
    std::vector<const tutorial::Step*> SkipWarnings() const;
    // The tutorial's folder, made again.
    void MoveTo(core::FolderId folder) { runner_.MoveTo(folder); }
    // One of the card's buttons, as an action asked for it.
    void Press(TutorialButton button);

    const tutorial::Tutorial& Runner() const { return runner_; }
    // The topic running, or run last.
    const tutorial::Topic& CurrentTopic() const { return *topic_; }
    // Whether the end or skip card's checkbox says to keep what the topic
    // made, at its Done or More topics.
    bool Keep() const { return keep_; }

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
    // The tutorial's profile's name, for the card's words - empty for none.
    std::string_view ProfileName() const {
        const std::optional<std::string>& profile = runner_.Profile();
        return profile ? std::string_view(*profile) : std::string_view();
    }
    // Where the spotlight is drawn this frame: the spot, while the step
    // up waits for its goal and nothing covers it.
    std::optional<AnchorRect> SpotlightRect() const;
    // What the hint's button asks for, while the hint up has one.
    std::optional<ViewAction> HintAction() const;

private:
    // The subject's rectangle, while it is on the canvas being looked at.
    std::optional<AnchorRect> SubjectRect() const;
    // The Overview's trash button under a tile of the tutorial's, one with
    // a snippet on it first; and the Restore of what of the tutorial's is
    // in the trash - under its tile, or on its folder's row.
    std::optional<AnchorRect> DeleteCanvasRect() const;
    std::optional<AnchorRect> RestoreRect() const;
    void DrawStep();
    void DrawSkipped();
    void DrawList();
    // The end card's and the skip card's way out: the checkbox that keeps
    // what the topic made, over Back, Done and More topics.
    void EndButtons();
    // What the checkbox keeps, as it says it - none with nothing left to
    // keep.
    const char* KeepLabel() const;
    // The hint under a step's text, and its button.
    void DrawHint(const tutorial::Hint& hint);

    const core::Session& session_;
    const Editor& editor_;
    const tutorial::World& world_;
    const AnchorBoard& anchors_;
    ViewHost& host_;

    const tutorial::Topic* topic_;
    tutorial::Tutorial runner_;
    // Where the card sits unless the user dragged it (docs/TUTORIAL.md,
    // section 19): one of a few places, kept until it is in the way.
    enum class Place { Top, Bottom, TopLeft, TopRight, LowerRight };
    // Where the card is drawn this frame: its place, or on the way there.
    ImVec2 Placed(float displayW, float displayH);
    // How wide the card is: the list twice a step's card, where the
    // display has room for it, in two columns (docs/TUTORIAL.md, section
    // 19.5).
    float Width(float displayW) const;
    // Placed as at the start - where the user dragged it forgotten - as a
    // topic starts, and as the list opens or closes (docs/TUTORIAL.md,
    // section 19.5).
    void PlaceAnew() {
        moved_ = false;
        place_.reset();
        drawnAt_.reset();
    }

    // Dragged by the user: left where it was put until the list opens.
    bool moved_ = false;
    // Its place, none until the first frame of a run or of the list, and
    // whether it was chosen over the Overview.
    std::optional<Place> place_;
    bool placedOverOverview_ = false;
    // Where it was drawn last frame, and where the slide to its place
    // began and how long ago, in seconds.
    std::optional<ImVec2> drawnAt_;
    ImVec2 slideFrom_;
    float slideAge_ = 0.0f;
    bool hasRun_ = false;
    bool listing_ = false;
    bool keep_ = false;
};

}  // namespace sz::ui
