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
        case LibraryAtStart::FirstRun:
            welcomePending_ = Welcome::Start;
            return;
        case LibraryAtStart::Loaded:
            break;
    }
    const std::map<std::string, std::string>& progress = settings_.Get(setting::kTutorialProgress);
    const std::string& current = settings_.Get(setting::kTutorialCurrent);
    if (const auto running = progress.find(current);
        running != progress.end() && tutorial::FindTopic(current) != nullptr && running->second != "finished" &&
        running->second != "skipped") {
        welcomePending_ = Welcome::Resume;  // at a step's id
    } else if (progress.empty()) {
        welcomePending_ = Welcome::Start;  // never shown: an install from before it
    } else {
        welcomePending_ = Welcome::Nothing;
    }
}

void OverlayApp::DoTutorial(const action::TutorialPress& a) {
    const std::vector<FolderId> folders = tutorialCard_.Runner().Folders();
    const std::vector<std::string> profiles = tutorialCard_.Runner().MadeProfiles();
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
        settingsPage_.RemoveProfiles(profiles);
        DeleteTarget target{DeleteTarget::Kind::Folder};
        for (const FolderId each : folders) {
            const Folder* found = Manager().FindFolder(each);
            if (found == nullptr || Manager().IsDeleted(*found)) {
                continue;
            }
            if (target.id == 0) {
                target.id = each;
                target.name = found->name;
            } else {
                target.alsoFolders.push_back(each);
            }
        }
        if (target.id != 0) {
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
    // canvas up as it is (section 18.3).
    if (!topic->folder) {
        tutorialCard_.Start(*topic, 0);
    } else if (const FolderId folder = MakeTutorialFolder(*topic); folder != 0) {
        tutorialCard_.Start(*topic, folder);
    }
}

void OverlayApp::DoTutorial(const action::ResumeTutorial&) {
    const std::string& current = settings_.Get(setting::kTutorialCurrent);
    const tutorial::Topic* topic = tutorial::FindTopic(current);
    const auto& progress = settings_.Get(setting::kTutorialProgress);
    const auto at = progress.find(current);
    if (topic == nullptr || at == progress.end()) {
        return;
    }
    if (!topic->folder) {
        tutorialCard_.Resume(*topic, at->second, 0);
        return;
    }
    if (const FolderId folder = GoToTutorialFolder(*topic, settings_.Get(setting::kTutorialFolder)); folder != 0) {
        tutorialCard_.Resume(*topic, at->second, folder);
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
    // commit, which the tray writes to the file. Until a topic has run,
    // nothing is: the topic kept as running is a resume's, still to come.
    if (!tutorialCard_.HasRun()) {
        return;
    }
    const tutorial::Tutorial& runner = tutorialCard_.Runner();
    const std::string topic(tutorialCard_.CurrentTopic().id);
    // Nothing to say for a topic let go of partway: its step stays kept.
    if (std::string progress = runner.Progress(); !progress.empty()) {
        std::map<std::string, std::string> kept = settings_.Get(setting::kTutorialProgress);
        if (std::string& entry = kept[topic]; entry != progress) {
            entry = std::move(progress);
            settings_.Set(setting::kTutorialProgress, std::move(kept));
        }
    }
    if (std::string current = runner.On() ? topic : std::string();
        current != settings_.Get(setting::kTutorialCurrent)) {
        settings_.Set(setting::kTutorialCurrent, std::move(current));
    }
    if (runner.On() && runner.Folder() != settings_.Get(setting::kTutorialFolder)) {
        settings_.Set(setting::kTutorialFolder, runner.Folder());
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
