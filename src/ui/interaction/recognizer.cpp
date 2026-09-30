#include "ui/interaction/recognizer.h"

#include <memory>
#include <optional>

#include "ui/editor.h"
#include "ui/interaction/gestures.h"
#include "ui/interaction/levels.h"

namespace sz::ui {

namespace {

Command About(CommandId id, core::ItemId item) {
    Command command{id};
    command.item = item;
    return command;
}

// A fullscreen snippet of `kind`, made by `madeBy`.
Command Fullscreen(core::ItemCreationKind kind, MadeBy madeBy) {
    Command command{kind == core::ItemCreationKind::Screenshot ? CommandId::FullscreenScreenshot
                                                               : CommandId::FullscreenDrawing};
    command.kind = kind;
    command.madeBy = madeBy;
    return command;
}

// Framing a snippet of `kind`, from the press, once it is a drag.
Pending::Meaning Frames(core::ItemCreationKind kind, MadeBy madeBy) {
    Pending::Meaning meaning;
    meaning.drag = [kind, madeBy](const Event& press, Editor& /*editor*/) {
        return std::make_unique<Framing>(press, kind, madeBy);
    };
    meaning.dragPx = kCreationDragThreshold;
    meaning.frames = true;
    meaning.framing = kind;
    return meaning;
}

// The press on a snippet takes hold of the selection - selecting the
// snippet first if it is not in it - and brings it forward while
// AppConfig::raiseSelectedSnippet is on, as a block in its own order: it
// is the selection that is taken hold of, the way a window manager raises
// a window you take hold of. Off, the stacking order is the context
// menu's alone to change, as in a drawing program.
void TakeHoldOf(core::ItemId item, Editor& editor) {
    if (!editor.IsSelected(item)) {
        editor.SelectOnly(item);
    }
    if (editor.Cfg().raiseSelectedSnippet) {
        editor.GetSession().BringItemsToFront(editor.Selection());
    }
}

// Whether a press here is one on empty canvas that should make a snippet:
// nothing under it, and nothing open that it is really a click outside of
// - a panel, a popup, a note being typed into - which that click is for
// closing. Making a snippet as well would turn every dismissal into a new
// drawing.
bool MakesASnippet(const PointerTarget& target, bool noteOpen, Editor& editor) {
    return target.kind == PointerTarget::Kind::None && !noteOpen && !editor.PointerOverView() &&
           !editor.PanelOpen() && !editor.PopupOpen();
}

Answer RecognizeLeft(const Event& press, const PointerTarget& target, bool isDouble, bool noteOpen, Editor& editor) {
    const platform::Modifiers& held = editor.Held();
    const bool drawing = editor.InDrawingMode();
    // 2: a bar button, held until its release.
    if (target.kind == PointerTarget::Kind::Button) {
        return Answer::Start(std::nullopt, std::make_unique<BarPress>(press, target.button));
    }
    // 3: a handle, only ever pressed to drag it.
    if (target.kind == PointerTarget::Kind::Handle) {
        return Answer::Start(std::nullopt, Placement::ResizeByHandle(press, target.item, target.handle, editor));
    }
    if (drawing && !held.alt) {
        // 4: on a snippet being drawn on, the tool in hand, on that one - a
        // note opened for typing with Text, which is no stroke at all.
        if (target.kind == PointerTarget::Kind::Body && editor.IsDrawingOn(target.item)) {
            if (editor.ActiveTool() == core::Tool::Text) {
                return Answer::Start(std::nullopt, std::make_unique<TypingNote>(target.item));
            }
            return Answer::Start(std::nullopt, Marking::ForTool(press, target.item, editor));
        }
        // 5: anywhere else, the press is for leaving - and held still, it is
        // what a double-click here would have been: drawing mode on the
        // other snippet, or a fullscreen snippet of empty canvas, of the
        // kind the modifier held picks. A double-click gets there by
        // leaving on its first press and arriving on its second; a hold has
        // only the one.
        Pending::Meaning meaning;
        if (target.kind == PointerTarget::Kind::Body) {
            meaning.hold = About(CommandId::DrawingMode, target.item);
        } else if (const std::optional<core::ItemCreationKind> kind = editor.EmptyCanvasCreationKind();
                   kind.has_value() && MakesASnippet(target, noteOpen, editor)) {
            meaning.hold = Fullscreen(*kind, MadeBy::Press);
        }
        return Answer::Start(Command{CommandId::LeaveDrawingMode}, std::make_unique<Pending>(press, meaning));
    }
    // 6: a creation tool in hand places wherever the press lands, on a
    // snippet or not, whatever modifier is held - a click for the whole
    // screen, a drag for an area.
    if (const std::optional<core::ItemCreationKind> kind = core::CreationKindFor(editor.ActiveTool())) {
        Pending::Meaning meaning = Frames(*kind, MadeBy::Tool);
        meaning.click = Fullscreen(*kind, MadeBy::Tool);
        return Answer::Start(std::nullopt, std::make_unique<Pending>(press, meaning));
    }
    if (target.kind == PointerTarget::Kind::Body && editor.SelectionLive() && editor.PressPicksUp()) {
        // 8, doubled: drawing mode on it. The first press selected it.
        if (isDouble && !held.shift) {
            return Answer::Start(About(CommandId::DrawingMode, target.item));
        }
        // 7: added to or taken out of the selection, and that is all the
        // press does - never a drag, and never a restack: gathering
        // snippets into a selection is not taking hold of any one of them.
        if (held.shift) {
            editor.ToggleSelected(target.item);
            return Answer::Claim();
        }
        // 8: selected, and moved once it is a drag - held still instead, it
        // enters drawing mode as a double-click would.
        TakeHoldOf(target.item, editor);
        Pending::Meaning meaning;
        const core::ItemId item = target.item;
        meaning.drag = [item](const Event& from, Editor& at) { return Placement::Move(from, item, at); };
        meaning.hold = About(CommandId::DrawingMode, item);
        return Answer::Start(std::nullopt, std::make_unique<Pending>(press, meaning));
    }
    if (target.kind != PointerTarget::Kind::None) {
        return Answer::Claim();
    }
    // Empty canvas.
    if (editor.SelectionLive() && !drawing) {
        // 9: with Shift, the start of a box to select by - and the
        // selection is left alone until the box says what it caught.
        if (held.shift) {
            Pending::Meaning meaning;
            meaning.drag = [](const Event& from, Editor& /*at*/) { return std::make_unique<BoxSelect>(from); };
            return Answer::Start(std::nullopt, std::make_unique<Pending>(press, meaning));
        }
        editor.ClearSelection();  // 10 and 11
    }
    // 10: framing a snippet of the kind the modifier held picks; a
    // double-click or a hold makes it fullscreen, and a plain click makes
    // nothing - a fullscreen snippet is too much to make by accident.
    const std::optional<core::ItemCreationKind> kind = editor.EmptyCanvasCreationKind();
    if (!kind.has_value() || !MakesASnippet(target, noteOpen, editor)) {
        return Answer::Claim();  // 11
    }
    // The hand moving on from a snippet it was drawing on (Alt held).
    const std::optional<Command> leave =
        drawing ? std::optional<Command>(Command{CommandId::LeaveDrawingMode}) : std::nullopt;
    if (isDouble) {
        // On the second press, as on a snippet (a change from making it on
        // the release): a second press that then drags is not framing.
        // Making it leaves drawing mode itself (see Editor::Run).
        return Answer::Start(Fullscreen(*kind, MadeBy::Press));
    }
    Pending::Meaning meaning = Frames(*kind, MadeBy::Press);
    meaning.hold = Fullscreen(*kind, MadeBy::Press);
    return Answer::Start(leave, std::make_unique<Pending>(press, meaning));
}

Answer RecognizeRight(const Event& press, const PointerTarget& target, bool noteOpen, Editor& editor) {
    const bool drawing = editor.InDrawingMode();
    // 2: the pen's or the eraser's bar button, whose menu opens on the
    // release over it. The other buttons have none, and take the press
    // for nothing, as they always have.
    if (target.kind == PointerTarget::Kind::Button) {
        if (!MenuForBarButton(target.button).has_value()) {
            return Answer::Claim();
        }
        return Answer::Start(std::nullopt, std::make_unique<BarPress>(press, target.button));
    }
    if (target.kind == PointerTarget::Kind::Body) {
        const core::ItemId item = target.item;
        // 12: on a snippet being drawn on, the right button is the eraser,
        // for quick corrections without changing the tool: a drag erases
        // along its path, and a press that never drags is a right click,
        // which leaves the mode. The erase starts only once it is a drag,
        // so a click takes nothing away.
        if (editor.IsDrawingOn(item) && !editor.Held().alt) {
            Pending::Meaning meaning;
            meaning.click = Command{CommandId::LeaveDrawingMode};
            meaning.drag = [item](const Event& from, Editor& /*at*/) { return Marking::RightErase(from, item); };
            return Answer::Start(std::nullopt, std::make_unique<Pending>(press, meaning));
        }
        // 13: selected, and resized from its nearest edge once it is a drag
        // - the one way to resize without aiming for a handle, with any
        // tool in hand. A right press that never drags is a right click:
        // its context menu, where it landed - or, on a snippet being drawn
        // on (reached with Alt), leaving drawing mode.
        const bool drawnOn = editor.IsDrawingOn(item);
        TakeHoldOf(item, editor);
        Pending::Meaning meaning;
        meaning.click = drawnOn ? Command{CommandId::LeaveDrawingMode} : About(CommandId::ItemMenu, item);
        meaning.drag = [item](const Event& from, Editor& at) {
            return Placement::ResizeFromNearestEdge(from, item, at);
        };
        return Answer::Start(std::nullopt, std::make_unique<Pending>(press, meaning));
    }
    // 14: a right click on empty canvas opens its menu where it landed, and
    // a drag does nothing. Held to the same test as a press that makes a
    // snippet: a right click that closes a note or a popover is for closing
    // it. And the hand moves on from a snippet it was drawing on, as a left
    // press here would.
    if (target.kind != PointerTarget::Kind::None || !MakesASnippet(target, noteOpen, editor)) {
        return Answer::Claim();
    }
    Pending::Meaning meaning;
    meaning.click = Command{CommandId::EmptyCanvasMenu};
    const std::optional<Command> leave =
        drawing ? std::optional<Command>(Command{CommandId::LeaveDrawingMode}) : std::nullopt;
    return Answer::Start(leave, std::make_unique<Pending>(press, meaning));
}

}  // namespace

Answer RecognizePress(const Event& press, Editor& editor) {
    // Asked before anything below can close the note: a press while a note
    // is open is for closing it, whatever happens to the note on the way.
    const bool noteOpen = editor.EditingNote().has_value();
    const bool isDouble = editor.TakeDoubleClick(press);
    // 1: a press on a panel of ImGui's own - a popover, the canvas bar, the
    // Overview, all above every item - is theirs alone.
    if (editor.PointerOverView()) {
        return Answer::Start(std::nullopt, std::make_unique<Widget>(press));
    }
    const PointerTarget target = editor.ResolvePointerTarget(press.position.x, press.position.y);
    switch (press.button) {
        case platform::MouseButton::Left:
            return RecognizeLeft(press, target, isDouble, noteOpen, editor);
        case platform::MouseButton::Right:
            return RecognizeRight(press, target, noteOpen, editor);
        case platform::MouseButton::Middle:
        case platform::MouseButton::X1:
        case platform::MouseButton::X2:
            break;  // 15: the bindings', which the Canvas level asks
    }
    return Answer::Pass();
}

}  // namespace sz::ui
