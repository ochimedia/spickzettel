#pragma once

// The Gesture level of docs/INTERACTIONS.md: what a held button is doing -
// a press not yet understood (Pending), the rest of a press that has had
// its say (Spent), and each gesture of section 9. What they answer is the
// table of section 5; what they do to the library goes through the
// session, and is filed when they finish, filed as it stands when they
// are interrupted, and rolled back when they are canceled (section 8).

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/canvas/item.h"
#include "core/session/actions.h"
#include "platform/platform_types.h"
#include "ui/interaction/command.h"
#include "ui/interaction/machine.h"
#include "ui/selection_layout.h"

namespace sz::ui {

// px the pointer has to travel from a press on a snippet, a right press or
// a Shift press before the press is a drag rather than a click: a click
// selects and moves nothing, so nothing is written until then.
inline constexpr float kSelectionDragThreshold = 4.0f;
// px, past which a press on empty canvas, or with a creation tool, frames a
// snippet rather than being a click.
inline constexpr float kCreationDragThreshold = 6.0f;
// What makes two clicks a double-click: the second press within this long
// and this far of the first. ImGui's own defaults are 0.30 s and 6 px; a
// little more time, since a double-click is how a fullscreen snippet is
// made and how drawing mode is entered, and a near miss there is a click
// that did nothing.
inline constexpr double kDoubleClickSeconds = 0.35;
inline constexpr float kDoubleClickPx = 6.0f;
// How long a press has to be held still to stand in for a double-click -
// for a finger or a pen, which cannot double-click reliably. Judged still
// by kDoubleClickPx, as a double-click is: past that it is a drag, and a
// drag is never a hold.
inline constexpr double kHoldSeconds = 0.5;

// Every gesture made with a held button, answering alike what section 5
// has in common for them: another button's press, moves and release are
// ignored (Windows' touch press-and-hold injects a right press into a
// finger held still); its own button pressed again means its release went
// missing, and it ends as interrupted with the press routed afresh; the
// wheel is ignored; Escape cancels it; other keys, the modifiers, the
// clock and a hotkey are passed on - a command they start interrupts it.
// What is left is each gesture's own: its moves, its
// release, and what it does with time and the modifiers.
class Gesture : public Interaction {
public:
    explicit Gesture(platform::MouseButton button) : button_(button) {}
    Level level() const override { return Level::Gesture; }
    platform::MouseButton Button() const { return button_; }
    Answer Offer(const Event& event, Editor& editor) final;

protected:
    // A move, with the button held or not - the pointer is the gesture's.
    virtual Answer Moved(const Event& event, Editor& editor) = 0;
    // Its own button let go of.
    virtual Answer Released(const Event& event, Editor& editor) = 0;
    virtual Answer Ticked(const Event& /*event*/, Editor& /*editor*/) { return Answer::Pass(); }
    virtual Answer ModifiersChanged(const Event& /*event*/, Editor& /*editor*/) { return Answer::Pass(); }

    platform::MouseButton button_;
};

// The rest of a press that has had its say (section 6.3): it swallows its
// button's moves and release, and any other button's press and release,
// until its own release comes. "The rest of the drag does nothing" is this
// state. Put on the Gesture level by the machine whenever it is left empty
// with a button still held - see Machine::Offer.
class Spent final : public Interaction {
public:
    explicit Spent(platform::MouseButton button) : button_(button) {}
    Level level() const override { return Level::Gesture; }
    const char* Name() const override { return "Spent"; }
    platform::MouseButton Button() const { return button_; }
    Answer Offer(const Event& event, Editor& editor) override;
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}

private:
    platform::MouseButton button_;
};

// A press whose meaning is still open (section 6.2): it holds the press
// and what a click, a drag and a hold would each do, and does nothing
// until one of them happens.
class Pending final : public Gesture {
public:
    // What becomes of the press. A drag is made from the press when the
    // pointer has traveled far enough; none, and a drag ends the press
    // with nothing done.
    using DragMaker = std::function<std::unique_ptr<Interaction>(const Event& press, Editor& editor)>;
    struct Meaning {
        std::optional<Command> click;  // on the release, at the release's point
        std::optional<Command> hold;   // held still for kHoldSeconds
        DragMaker drag;
        // How far is a drag: kSelectionDragThreshold, reached - or
        // kCreationDragThreshold, passed, for a snippet being framed.
        float dragPx = kSelectionDragThreshold;
        bool frames = false;
        // The kind of snippet a drag would frame, if it would - see
        // Editor::ArmedCreation.
        std::optional<core::ItemCreationKind> framing;
    };
    Pending(const Event& press, Meaning meaning) : Gesture(press.button), press_(press), meaning_(std::move(meaning)) {}
    const char* Name() const override { return "Pending"; }
    const Meaning& GetMeaning() const { return meaning_; }
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}

protected:
    Answer Moved(const Event& event, Editor& editor) override;
    Answer Released(const Event& event, Editor& editor) override;
    Answer Ticked(const Event& event, Editor& editor) override;

private:
    Event press_;
    Meaning meaning_;
    // Moved too far for a hold: past kDoubleClickPx.
    bool strayed_ = false;
};

// A snippet moved, or resized by a handle or from its nearest edge. A move
// carries the whole selection; a resize is one snippet's, or the whole
// selection's scaled as a group when the snippet is one of several
// selected. The gesture is a snapshot taken as it begins (every moved
// snippet's rect and the pointer's position at the press) plus the *full*
// delta from there, recomputed on every move - not an incremental delta
// per event, which drifts under event coalescing.
class Placement final : public Gesture {
public:
    // A drag from a press on a snippet: the selection, from the press.
    static std::unique_ptr<Placement> Move(const Event& press, core::ItemId item, Editor& editor);
    // A handle pressed: at once, since a handle is only ever pressed to
    // drag it.
    static std::unique_ptr<Placement> ResizeByHandle(const Event& press, core::ItemId item, ResizeHandle handle,
                                                     Editor& editor);
    // A right-drag on a snippet: from whichever edge or corner is nearest
    // the press, decided from the rect as it was then.
    static std::unique_ptr<Placement> ResizeFromNearestEdge(const Event& press, core::ItemId item, Editor& editor);

    const char* Name() const override { return resize_ ? "Resize" : "Move"; }
    void Begin(const Event& event, Editor& editor) override;
    void Interrupt(Editor& editor) override;
    void Cancel(Editor& editor) override;

    // The snippet taken hold of, what the pointer is doing to it, and by
    // which handle - for what the view lights up.
    core::ItemId Item() const { return item_; }
    bool Resizing() const { return resize_; }
    std::optional<ResizeHandle> Handle() const { return handle_; }

protected:
    Answer Moved(const Event& event, Editor& editor) override;
    Answer Released(const Event& event, Editor& editor) override;
    Answer ModifiersChanged(const Event& event, Editor& editor) override;

private:
    Placement(const Event& press, core::ItemId item) : Gesture(press.button), press_(press), item_(item) {}
    // Every snippet the gesture may change, with its rect as it began.
    struct StartRect {
        core::ItemId item = 0;
        core::Rect rect;
    };
    // The snapshot of a resize: `item` alone, or the whole selection and
    // the box around it when it is one of several.
    void SnapshotResizeTargets(Editor& editor);
    // Every snippet at the place the pointer's travel from the press puts it.
    void Apply(platform::Vec2 pointer, Editor& editor);
    // A resize carrying the whole selection: every snippet in it scaled by
    // one factor about the corner or edge the drag leaves fixed.
    void ResizeAsAGroup(float dx, float dy, Editor& editor);

    Event press_;
    core::ItemId item_;
    bool resize_ = false;
    std::optional<ResizeHandle> handle_;
    bool left_ = false;
    bool right_ = false;
    bool top_ = false;
    bool bottom_ = false;
    std::vector<StartRect> startRects_;
    bool group_ = false;
    core::Rect startBounds_;
    // Taken from a press that became a drag, rather than begun at the
    // press: a fullscreen snippet comes out of fullscreen as it begins.
    bool fromDrag_ = true;
    platform::Vec2 last_;
};

// The box dragged with Shift from open canvas to select by: every snippet
// it touches joins the selection on the release.
class BoxSelect final : public Gesture {
public:
    explicit BoxSelect(const Event& press) : Gesture(press.button), from_(press.position), to_(press.position) {}
    const char* Name() const override { return "BoxSelect"; }
    void Begin(const Event& event, Editor& /*editor*/) override { to_ = event.position; }
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}
    core::Rect Bounds() const;

protected:
    Answer Moved(const Event& event, Editor& editor) override;
    Answer Released(const Event& event, Editor& editor) override;

private:
    platform::Vec2 from_;
    platform::Vec2 to_;
};

// A snippet being framed: dragged out from the press, made on the release
// if it is big enough to be meant.
class Framing final : public Gesture {
public:
    Framing(const Event& press, core::ItemCreationKind kind, MadeBy madeBy)
        : Gesture(press.button), kind_(kind), madeBy_(madeBy), from_(press.position), to_(press.position) {}
    const char* Name() const override { return "Framing"; }
    void Begin(const Event& event, Editor& /*editor*/) override { to_ = event.position; }
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}
    core::ItemCreationKind Kind() const { return kind_; }
    core::Rect Frame() const;

protected:
    Answer Moved(const Event& event, Editor& editor) override;
    Answer Released(const Event& event, Editor& editor) override;

private:
    core::ItemCreationKind kind_;
    MadeBy madeBy_;
    platform::Vec2 from_;
    platform::Vec2 to_;
};

// A press on one of ImGui's windows, held - a slider dragged in the
// Properties popover, a tile dragged in the Overview or the canvas bar, a
// button held: the widget's, and the canvas's nothing (section 6.5, rule
// 1). Escape calls it off: the view lets go of the widget, and a style it
// was previewing goes back to what it was. Ended from outside, the view
// lets go of it too, keeping what it had done.
class Widget final : public Gesture {
public:
    explicit Widget(const Event& press) : Gesture(press.button), pressedAt_(press.position) {}
    const char* Name() const override { return "Widget"; }
    void Begin(const Event& event, Editor& editor) override;
    void Interrupt(Editor& editor) override;
    // Calls off what the drag changed: a snippet's style, a setting's
    // preview, the pen's color in its chooser.
    void Cancel(Editor& editor) override;
    // Where the press went down - which window it is, for a view that
    // cares whether it was its own.
    platform::Vec2 PressedAt() const { return pressedAt_; }

protected:
    Answer Moved(const Event& /*event*/, Editor& /*editor*/) override { return Answer::Claim(); }
    Answer Released(const Event& /*event*/, Editor& /*editor*/) override { return Answer::Finish(); }

private:
    platform::Vec2 pressedAt_;
    // The pen's color as the press went down - see Cancel.
    uint32_t penColorAtPress_ = 0;
};

// A selection bar button held down, fired by a release over it - the rule
// ImGui's own Button follows. Nothing else on the bar reacts meanwhile.
class BarPress final : public Gesture {
public:
    BarPress(const Event& press, core::ChromeButton button) : Gesture(press.button), chrome_(button) {}
    const char* Name() const override { return "BarPress"; }
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}
    core::ChromeButton Pressed() const { return chrome_; }

protected:
    Answer Moved(const Event& /*event*/, Editor& /*editor*/) override { return Answer::Claim(); }
    Answer Released(const Event& event, Editor& editor) override;

private:
    core::ChromeButton chrome_;
};

// A mark on the snippet in drawing mode: a freehand stroke, a line or a
// rectangle, the eraser's path or its rectangle - decided at the press
// from the tool and the modifiers held then (see Editor::ShapeForPress).
// The right button's erase is one too, begun from the press once it drags.
class Marking final : public Gesture {
public:
    enum class Kind { Freehand, Shape, Erase, EraseRect };
    // The tool in hand's, on the left button, from the press.
    static std::unique_ptr<Marking> ForTool(const Event& press, core::ItemId item, Editor& editor);
    // The right button's erase, from the press it was a drag of.
    static std::unique_ptr<Marking> RightErase(const Event& press, core::ItemId item);

    const char* Name() const override { return "Marking"; }
    void Begin(const Event& event, Editor& editor) override;
    void Interrupt(Editor& editor) override;
    void Cancel(Editor& editor) override;

    Kind GetKind() const { return kind_; }
    core::DrawShape Shape() const { return shape_; }
    // The rectangle eraser's rectangle, from its fixed corner to the pointer.
    core::Rect EraseBox() const;

protected:
    Answer Moved(const Event& event, Editor& editor) override;
    Answer Released(const Event& event, Editor& editor) override;

private:
    Marking(const Event& press, core::ItemId item, Kind kind)
        : Gesture(press.button), press_(press), item_(item), kind_(kind), last_(press.position) {}

    Event press_;
    core::ItemId item_;
    Kind kind_;
    core::DrawShape shape_ = core::DrawShape::Freehand;  // for a Shape
    // Where its last press or move was - where it ends when it has to be
    // ended without its release. The pointer is somewhere else by then: at
    // the next press, for a release that went missing.
    platform::Vec2 last_;
};

}  // namespace sz::ui
