#pragma once

// One step of a tutorial chain, as a row of its table - docs/TUTORIAL.md,
// sections 4 and 7.5. What a step says, what it points at, what it
// needs, when it is done and which mistakes it has a line for are all
// data here; the runner (tutorial.h) is the same for every step. No
// ImGui.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "ui/tutorial/world.h"

namespace sz::ui::tutorial {

// A read step is done when Next is pressed; a do step when its goal is
// met.
enum class StepKind { Read, Do };

// What a step needs before its goal can be met - section 6.4. A closed
// list: each has its line on the card, shared by every step that needs
// it, and some a button that gets it back. Checked in the order a step
// lists them; the first not met is what the card says.
enum class Need {
    // The current canvas is in the tutorial's folder. Button: back to it.
    InTutorialFolder,
    // No panel or popup over the canvas.
    CanvasUncovered,
    // No snippet in drawing mode.
    NoDrawingMode,
    // No creation tool in hand but the screenshot tool, which the
    // screenshot step can use.
    NoOtherTool,
    // One of the tutorial's snippets will do, anywhere in its folder.
    // Button: a practice snippet.
    ASubject,
    // The subject is on the current canvas. Button: to its canvas.
    SubjectHere,
    // The subject is not in the dock.
    SubjectOnScreen,
    // The subject is not fullscreen, which has no size of its own to move
    // or resize.
    SubjectCanMove,
    // The subject is selected, so that it has its bar and its resize band.
    SubjectSelected,
    // The subject is in drawing mode.
    DrawingOnSubject,
    // The subject is deleted, or was during this step - for the step that
    // brings it back.
    DeletedSubject,
    // The subject is pinned.
    SubjectPinned,
    // The pen in hand, in any of its shapes: with the eraser or Text, a
    // drag erases or opens the note instead.
    PenInHand,
    // Something drawn on the subject, for a step that erases.
    SubjectDrawnOn,
    // The Overview up - the opposite of CanvasUncovered - and showing the
    // canvases, not Settings or About.
    OverviewUp,
    CanvasesTab,
    // Show deleted on, in the Overview.
    DeletedShown,
    // A canvas or folder of the tutorial's in the trash, for the step that
    // restores it.
    SomethingInTrash,
    // The Overview up on its Settings tab - opened by the menu's Settings -
    // and the section picked there.
    SettingsUp,
    SettingsTab,
    ProfilesSection,
    BehaviorSection,
    // The overlay up over a program it can name, to make a profile for.
    AProgramUnderneath,
    // The tutorial's profile there (section 18.3); Showing on it; and a
    // Behavior setting stated in it.
    TutorialsProfile,
    ShowingIt,
    SomethingSetInIt,
};

// Which snippet a step is about - section 6.5.
enum class SubjectRule {
    // None: the step is about no snippet.
    None,
    // Any of the tutorial's snippets.
    Any,
    // One that can be moved and resized: not fullscreen.
    CanMove,
    // The one deleted last, for the step that brings it back.
    LastDeleted,
    // A pinned one, before one that is not - as Any when none is.
    Pinned,
};

// What the spotlight rings - section 3. The view turns it into a place
// on screen; one that is not on screen draws no ring.
enum class Spot {
    None,
    Subject,
    // The subject's resize band at its lower right corner.
    SubjectCorner,
    SelectionBarClose,
    // The selection bar's Pin, or the subject while no bar is drawn.
    SelectionBarPin,
    // The selection bar's drawing tools, or the subject while no bar is
    // drawn.
    SelectionBarPen,
    SelectionBarEraser,
    SelectionBarText,
    SelectionBarColor,
    // The subject's chip in the dock, while it is minimized.
    DockChip,
    // The canvas bar's + and Overview buttons.
    CanvasBarNew,
    CanvasBarOverview,
    // The Overview's New folder and Show deleted.
    NewFolder,
    ShowDeleted,
    // A folder's row in the Overview: the one made in the run, and the
    // tutorial's own.
    MadeFolder,
    TutorialFolder,
    // The trash button under the tile of a canvas of the tutorial's, one
    // with a snippet on it first.
    DeleteCanvas,
    // The Restore of what of the tutorial's is in the trash.
    Restore,
    // The Settings panel's Profiles and Behavior in its section list; Make
    // a profile for this; Showing; the Don't steal focus row; and the
    // first revert arrow in Behavior.
    SectionProfiles,
    SectionBehavior,
    MakeProfile,
    Showing,
    DontStealFocus,
    Revert,
};

// A button a hint line carries - section 6.4.
enum class HintButton { None, BackToTutorial, BackThere, PutOneHere };

// What a step notes as it begins, for its goal to compare with - section
// 5. Snippets that turn up during the step are noted as first seen.
struct StartRecord {
    // The canvas being looked at, every folder, and the canvases of the
    // tutorial's folders (section 17.3).
    core::CanvasId canvas = 0;
    std::vector<FolderFacts> folders;
    std::vector<CanvasFacts> canvases;
    // Every profile (section 18.3).
    std::vector<ProfileFacts> profiles;
    uint64_t showings = 0;
    uint64_t pinnedViews = 0;
    uint64_t viewModes = 0;
    // The capture hotkeys' screenshots, the quick one's and the silent's.
    uint64_t quickCaptures = 0;
    uint64_t silentCaptures = 0;
    // What the pen drew with.
    uint32_t penColor = 0;
    float penWidth = 0.0f;
    // The tutorial's snippets there were when the step began.
    std::unordered_set<core::ItemId> present;
    // Each snippet of the tutorial's as the step first saw it.
    std::unordered_map<core::ItemId, SnippetFacts> firstSeen;
};

// Ink gone from a snippet during a step, by what was in hand as it went -
// section 15.2: the eraser drags it off, the rectangle eraser cuts it out
// on the release, and with another tool in hand it went by the right
// button, or by undo.
struct InkGone {
    float rectangleEraser = 0.0f;
    float eraser = 0.0f;
    float otherTool = 0.0f;
};

// What a goal or a near miss is asked with: the world, the step's start
// record, the tutorial's snippets, folders and canvases as they are now,
// and what the runner has seen during the step.
struct Look {
    const World& world;
    const StartRecord& start;
    const std::vector<SnippetFacts>& snippets;
    // The tutorial's own folder, and its folders: that one and those made
    // in the run (section 17.3). Every folder, and the canvases of the
    // tutorial's.
    core::FolderId folder;
    const std::vector<core::FolderId>& folders;
    const std::vector<FolderFacts>& allFolders;
    const std::vector<CanvasFacts>& canvases;
    std::optional<core::ItemId> subject;
    // The tutorial's snippets seen deleted during this step - those
    // deleted when it began among them.
    const std::unordered_set<core::ItemId>& deletedThisStep;
    // Those seen pinned during this step - pinned when it began among
    // them.
    const std::unordered_set<core::ItemId>& pinnedThisStep;
    // The ink gone from each of the tutorial's snippets during this step.
    const std::unordered_map<core::ItemId, InkGone>& inkGoneThisStep;
    // The tutorial's canvases and folders seen in the trash during this
    // step - those in it when it began among them.
    const std::unordered_set<uint64_t>& trashedThisStep;
    // Every profile, and the tutorial's, if it has one (section 18.3).
    const std::vector<ProfileFacts>& profiles;
    const std::optional<core::ProfileId>& profile;

    // A snippet of the tutorial's as it is now, or null.
    const SnippetFacts* Now(core::ItemId id) const;
    // As the step first saw it, or null.
    const SnippetFacts* AtStart(core::ItemId id) const;
    const SnippetFacts* Subject() const { return subject ? Now(*subject) : nullptr; }
    const SnippetFacts* SubjectAtStart() const { return subject ? AtStart(*subject) : nullptr; }
    // The ink gone from the subject during this step - none without one.
    InkGone SubjectInkGone() const;
    // The live ones made since the step began, on the current canvas.
    std::vector<const SnippetFacts*> MadeHere() const;
    // Whether `folder` is one of the tutorial's.
    bool Tutorials(core::FolderId folder) const;
    // A folder or a canvas of the tutorial's as it is now, or null.
    const FolderFacts* FolderNow(core::FolderId id) const;
    const CanvasFacts* CanvasNow(core::CanvasId id) const;
    // The tutorial's profile as it is now, and as the step began - null
    // for none, or one not there then.
    const ProfileFacts* Profile() const;
    const ProfileFacts* ProfileAtStart() const;
};

using Check = bool (*)(const Look& look);

// A mistake one can see coming, and its line - section 6.6.
struct NearMiss {
    Check check;
    const char* text;
};

struct Step {
    // Stable: what the settings file keeps (section 7.6).
    std::string_view id;
    StepKind kind = StepKind::Read;
    // Next waits for the goal: a later step needs what this one makes.
    bool gated = false;
    // One of the warnings the skip card repeats, when it was not reached.
    bool warning = false;
    // The folders made while it is up are the tutorial's (section 17.3):
    // for the step that asks for one.
    bool keepsFolders = false;
    // The profiles made while it is up are the tutorial's (section 18.3).
    bool keepsProfiles = false;
    const char* title = "";
    // The text, chosen for the world as it is - a trigger set, a key
    // unbound - with the {placeholders} of Expand in it.
    const char* (*text)(const World& world) = nullptr;
    Spot spot = Spot::None;
    std::vector<Need> needs;
    SubjectRule subject = SubjectRule::None;
    // Null for a read step.
    Check goal = nullptr;
    std::vector<NearMiss> nearMisses;
};

}  // namespace sz::ui::tutorial
