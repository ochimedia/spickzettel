#include "ui/overlay_app.h"

// OverlayApp's part in the tutorial (docs/TUTORIAL.md, section 7): what the
// start decided, the tutorial's actions as Apply does them, and what it
// counts for the world.

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/canvas/item_geometry.h"
#include "generated/ui_strings.h"
#include "ui/theme.h"

namespace sz::ui {

namespace {
// The backing a practice snippet gets. Black: it is there to be seen on
// whatever the desktop happens to show, pale or not.
constexpr uint32_t kNoteBackgroundColorRGBA = 0x000000FFu;
constexpr float kNoteBackgroundOpacity = 0.5f;
}  // namespace

void OverlayApp::WelcomeAtStart(LibraryAtStart library) {
    switch (library) {
        case LibraryAtStart::None:
            welcomePending_ = Welcome::Nothing;
            return;
        // A new library is not a new person: deleted, or set aside, with
        // the settings kept, it is someone who has met the app, and the
        // settings say what they have seen (docs/TUTORIAL.md, 13.7).
        case LibraryAtStart::FirstRun:
        case LibraryAtStart::Loaded:
            break;
    }
    // Basics from its welcome until it has been finished or skipped: a
    // first run, an install from before the tutorial, and a run quit
    // partway, which starts over - nothing goes on after a restart
    // (docs/TUTORIAL.md, 13.7).
    const std::map<std::string, std::string>& progress = settings_.Get(setting::kTutorialProgress);
    const auto basics = progress.find(std::string(tutorial::kBasicsTopic));
    const bool seen = basics != progress.end() && (basics->second == "finished" || basics->second == "skipped");
    welcomePending_ = seen ? Welcome::Nothing : Welcome::Start;
}

void OverlayApp::DoTutorial(const action::TutorialPress& a) {
    const TutorialCard::Made made = tutorialCard_.LeftToKeep();
    const bool on = tutorialCard_.Runner().On();
    const bool keep = tutorialCard_.Keep();
    tutorialCard_.Press(a.button);
    // Done, and More topics, end the tutorial with its folders in the
    // trash, asked first where Settings says to, as any folder's Delete is
    // (question 9): its own, and those made in the run (section 17.3),
    // under one confirmation. The card's Keep checkbox keeps them (section
    // 20).
    const bool ends = a.button == TutorialButton::Done || a.button == TutorialButton::MoreTopics;
    if (ends && on && !tutorialCard_.Runner().On() && !keep) {
        // And the profiles made in the run, without asking, as a profile's
        // own trash button has it (section 18.3).
        settingsPage_.RemoveProfiles(made.profiles);
        if (!made.folders.empty()) {
            DeleteTarget target{DeleteTarget::Kind::Folder, made.folders.front().id, made.folders.front().name};
            for (size_t i = 1; i < made.folders.size(); ++i) {
                target.alsoFolders.push_back(made.folders[i].id);
            }
            AskToDelete(std::move(target));
        }
    }
}

void OverlayApp::DoTutorial(const action::StartTutorial& a) {
    // From Settings, which is in the Overview: the tutorial is about the
    // canvas.
    overview_.Close();
    const tutorial::Topic* topic = tutorial::FindTopic(a.topic);
    if (topic == nullptr) {
        topic = tutorial::FindTopic(tutorial::kBasicsTopic);
    }
    // The topic running let go of first, keeping its folder, and what it
    // ended as kept for it (section 13.3).
    if (tutorialCard_.Runner().On()) {
        tutorialCard_.Leave();
        KeepTutorialProgress();
    }
    // A topic with nothing on a canvas has no folder, and leaves the
    // canvas up as it is (section 18.3); the others make theirs when
    // their first do step comes up, which for all but Basics is now.
    tutorialCard_.Start(*topic, 0);
    GiveTutorialItsFolder();
}

// The running topic's folder, once its first do step has come up and
// while it has none: a new one (section 13.5). Called as a topic starts,
// and each frame before the runner looks at the app, so that the folder is
// there for the do step that needs it from its first frame. A run that
// goes Back to a read step keeps the folder it has.
void OverlayApp::GiveTutorialItsFolder() {
    const tutorial::Tutorial& runner = tutorialCard_.Runner();
    if (!tutorialCard_.CurrentTopic().folder || runner.Folder() != 0 || !runner.On() || !runner.DoStepReached()) {
        return;
    }
    if (const FolderId folder = MakeTutorialFolder(tutorialCard_.CurrentTopic()); folder != 0) {
        tutorialCard_.MoveTo(folder);
    }
}

void OverlayApp::DoTutorial(const action::OpenTutorialList&) {
    overview_.Close();
    tutorialCard_.OpenList();
}

void OverlayApp::DoTutorial(const action::BackToTutorial&) {
    const FolderId folder = tutorialCard_.Runner().Folder();
    if (const FolderId now = GoToTutorialFolder(tutorialCard_.CurrentTopic(), folder); now != 0 && now != folder) {
        tutorialCard_.MoveTo(now);
    }
}

FolderId OverlayApp::MakeTutorialFolder(const tutorial::Topic& topic) {
    // As the Overview's New folder makes one: current once it is made, and
    // with a canvas in it, switched to.
    // Named for its topic, so that runs of several leave folders that can
    // be told apart (section 13.5).
    char name[128];
    std::snprintf(name, sizeof(name), strings::kTutorialFolderName, topic.title);
    const FolderId folder = session_.AddFolder(name);
    if (folder == 0) {
        return 0;
    }
    overview_.ScrollToFolder(folder);
    overview_.ForgetDeletedFolderShown();
    if (const CanvasId canvas = editor_.CreateCanvasInCurrentFolder(); canvas != 0) {
        editor_.SwitchCanvas(canvas);
    }
    return folder;
}

FolderId OverlayApp::GoToTutorialFolder(const tutorial::Topic& topic, FolderId folder) {
    for (const Canvas& canvas : Manager().Canvases()) {
        if (folder != 0 && canvas.folderId == folder && !Manager().IsDeleted(canvas)) {
            editor_.SwitchCanvas(canvas.id);
            return folder;
        }
    }
    // Gone - deleted, or never in this library - and a new one to go on in.
    return MakeTutorialFolder(topic);
}

void OverlayApp::KeepTutorialProgress() {
    // Where the runner is once it has moved on for the frame - by itself in
    // Prepare, or at a button just done. Only a change is set: a Set is a
    // commit, which the tray writes to the file. Nothing to say before a
    // topic has run, nor for one let go of partway: it stays "started".
    const tutorial::Tutorial& runner = tutorialCard_.Runner();
    if (std::string progress = runner.Progress(); !progress.empty()) {
        std::map<std::string, std::string> kept = settings_.Get(setting::kTutorialProgress);
        if (std::string& entry = kept[std::string(tutorialCard_.CurrentTopic().id)]; entry != progress) {
            entry = std::move(progress);
            settings_.Set(setting::kTutorialProgress, std::move(kept));
        }
    }
}

void OverlayApp::PlacePracticeSnippet() {
    const float displayW = editor_.DisplayWidth();
    const float displayH = editor_.DisplayHeight();
    if (displayW <= 0.0f || displayH <= 0.0f || Manager().CurrentOrNull() == nullptr) {
        return;
    }
    // A drawing with a backing, so it is seen: centered, and low enough to
    // stay clear of the card at the top.
    const ImVec2 size = Px(360.0f, 220.0f);
    Item practice;
    practice.name = strings::kTutorialPracticeName;
    practice.rect = ClampRectToViewport(
        Rect{(displayW - size.x) * 0.5f, displayH * 0.6f - size.y * 0.5f, size.x, size.y}, displayW, displayH);
    practice.picture.tintColorRGBA = kNoteBackgroundColorRGBA;
    practice.picture.opacity = kNoteBackgroundOpacity;
    session_.CreateItem(std::move(practice), /*undoable=*/false);
}

void OverlayApp::OnModeEntered(OverlayMode mode) {
    if (mode == OverlayMode::Pinned) {
        tutorialWorld_.CountPinnedView();
    } else if (mode == OverlayMode::View) {
        tutorialWorld_.CountViewMode();
    }
}

}  // namespace sz::ui
