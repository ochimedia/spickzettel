#pragma once

// What a widget asks for - docs/VIEW_LAYER.md, section 6. Recorded as the
// widget is drawn and done in the frame's last stage, Apply, in the order
// recorded (see OverlayApp::Act): a draw reads the library through
// references into its lists, and a change made in the middle of one would
// move what the rest of it is reading, or draw half a frame from one
// library and half from another.
//
// Each is a value - ids, a place, a name - and never a reference into the
// model, which is the failure the deferral exists to prevent. What stays in
// the draw is a widget's own value, changed as it is changed: a setting,
// a snippet's style or a note's text as the session previews it, the pen's
// color while the chooser is dragged.

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/item.h"
#include "ui/interaction/command.h"
#include "ui/interaction/levels.h"
#include "ui/tutorial/topics.h"

namespace sz::ui {

// What a delete is of: a canvas or a folder, what is deleted in a folder,
// or everything deleted. `name` is taken when it is asked, for the
// confirmation's own "Delete <name>?", so it need not be looked up again.
struct DeleteTarget {
    // DeletedCanvasesIn is a folder that is not deleted itself, and what
    // goes for good is the canvases in it that are - see
    // Session::DeleteMarkedCanvasesPermanently. Trash is every folder and
    // canvas that is deleted, with no id or name - see Session::EmptyTrash.
    // Both always for good.
    enum class Kind { Canvas, Folder, DeletedCanvasesIn, Trash };
    Kind kind = Kind::Canvas;
    uint64_t id = 0;  // CanvasId or FolderId depending on kind
    std::string name;
    // Deleted already, so this is Delete permanently rather than a mark.
    bool forGood = false;
    // More folders that go with a folder's delete, under the one
    // confirmation - the tutorial's, at its Done (docs/TUTORIAL.md,
    // section 17.3). Never with forGood.
    std::vector<uint64_t> alsoFolders;
};

// One of the tutorial card's own buttons. Done ends the tutorial and puts
// its folder in the trash, unless Keep - the end and skip cards' checkbox,
// toggled - says to keep it; MoreTopics does the same and opens the list
// of topics, and CloseList leaves the list - back to the topic running,
// if one is (docs/TUTORIAL.md, section 20).
enum class TutorialButton { Next, Back, Skip, Done, Keep, MoreTopics, CloseList };

namespace action {

// A command - a context menu's row, the canvas bar's two buttons - run
// through Dispatch, as a key would run it.
struct RunCommand {
    Command command;
};
// A canvas's tile, in the Overview or on the canvas bar: through the
// editor, which ends the canvas scope first.
struct SwitchCanvas {
    core::CanvasId canvas = 0;
};
// A folder's row: the folder browsed, or a deleted one looked into.
struct SwitchFolder {
    core::FolderId folder = 0;
};
struct ShowDeletedFolder {
    core::FolderId folder = 0;
};
// A drop: a folder to `place` among the folders, a canvas to `place` among
// its folder's, or a canvas into another folder.
struct ReorderFolder {
    core::FolderId folder = 0;
    size_t place = 0;
};
struct ReorderCanvas {
    core::CanvasId canvas = 0;
    size_t place = 0;
};
struct MoveCanvasToFolder {
    core::CanvasId canvas = 0;
    core::FolderId folder = 0;
};
// A name field let go of, after an edit.
struct RenameFolder {
    core::FolderId folder = 0;
    std::string name;
};
struct RenameCanvas {
    core::CanvasId canvas = 0;
    std::string name;
};
// The Overview's footer. A new canvas is switched to - or, while a snippet
// is being sent somewhere, is where it goes.
struct NewFolder {};
struct NewCanvas {};
// A tile picked while a snippet is being sent somewhere.
struct SendPicked {
    core::CanvasId canvas = 0;
};
// A deleted folder's or canvas's Restore.
struct Restore {
    uint64_t id = 0;
};
// The confirmation's Delete, or a delete button where Settings says not to
// ask.
struct Delete {
    DeleteTarget target;
};
// A dock chip.
struct RestoreMinimized {
    core::ItemId item = 0;
};
// A panel's backdrop; the picker's Cancel and a tile, for the Overview.
struct ClosePanel {
    PanelKind panel = PanelKind::Overview;
};
// The note editor let go of, with the text it held as the frame began.
struct FinishNoteEdit {
    std::string text;
};

// The tutorial (docs/TUTORIAL.md, section 7.3). The card's own buttons:
// the runner's state, and the folder at the end.
struct TutorialPress {
    TutorialButton button = TutorialButton::Next;
};
// A topic's start, at its first step, in a folder made for it - the
// topic's id (see tutorial::Topics), Basics for one there is no longer.
struct StartTutorial {
    std::string topic{tutorial::kBasicsTopic};
};
// Open the tutorial: its list of topics (section 13.4), with the Overview,
// which Settings is in, closed for it.
struct OpenTutorialList {};
// Go back to the tutorial: to its folder, made again when it is gone.
struct BackToTutorial {};
// Put one here: a snippet to practice on, on the canvas being looked at.
struct PracticeSnippet {};

// The library size reminder's buttons: the Overview with Show deleted on,
// where Empty trash is; and Settings > Behavior, where retention and the
// reminder are.
struct ShowTrash {};
struct ShowTrashSettings {};

}  // namespace action

using ViewAction =
    std::variant<action::RunCommand, action::SwitchCanvas, action::SwitchFolder, action::ShowDeletedFolder,
                 action::ReorderFolder, action::ReorderCanvas, action::MoveCanvasToFolder, action::RenameFolder,
                 action::RenameCanvas, action::NewFolder, action::NewCanvas, action::SendPicked, action::Restore,
                 action::Delete, action::RestoreMinimized, action::ClosePanel, action::FinishNoteEdit,
                 action::TutorialPress, action::StartTutorial, action::OpenTutorialList,
                 action::BackToTutorial,
                 action::PracticeSnippet, action::ShowTrash, action::ShowTrashSettings>;

}  // namespace sz::ui
