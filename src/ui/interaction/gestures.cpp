#include "ui/interaction/gestures.h"

#include <algorithm>
#include <cmath>

#include "core/canvas/item_geometry.h"
#include "ui/editor.h"

namespace sz::ui {

namespace {

float Distance(platform::Vec2 a, platform::Vec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

// The box around a gesture's snapshotted rects - the selection as it
// stood at the press. Retaken when a fullscreen snippet among them is
// restored, which changes the very rect the box was drawn around.
template <class StartRects>
core::Rect BoundsOf(const StartRects& rects) {
    if (rects.empty()) {
        return core::Rect{};
    }
    float x0 = rects.front().rect.x;
    float y0 = rects.front().rect.y;
    float x1 = x0 + rects.front().rect.w;
    float y1 = y0 + rects.front().rect.h;
    for (const auto& start : rects) {
        x0 = std::min(x0, start.rect.x);
        y0 = std::min(y0, start.rect.y);
        x1 = std::max(x1, start.rect.x + start.rect.w);
        y1 = std::max(y1, start.rect.y + start.rect.h);
    }
    return core::Rect{x0, y0, x1 - x0, y1 - y0};
}

// Which edge(s) of `rect` a point at (x, y) is nearest to, for the
// right-drag "resize from nearest edge" way in - divides the rect into a
// 3x3 grid: the outer thirds on either axis pick that edge (both axes at
// once for a corner); dead center (neither axis in an outer third) falls
// back to whichever single edge is nearest by plain distance, so a press
// anywhere in the rect always resolves to *something* rather than
// resizing nothing.
void NearestResizeEdges(const core::Rect& rect, float x, float y, bool& left, bool& right, bool& top,
                        bool& bottom) {
    const float px = x - rect.x;
    const float py = y - rect.y;
    left = px < rect.w / 3.0f;
    right = px > rect.w * 2.0f / 3.0f;
    top = py < rect.h / 3.0f;
    bottom = py > rect.h * 2.0f / 3.0f;
    if (!left && !right && !top && !bottom) {
        const float distLeft = px, distRight = rect.w - px, distTop = py, distBottom = rect.h - py;
        const float minDist = std::min({distLeft, distRight, distTop, distBottom});
        left = minDist == distLeft;
        right = !left && minDist == distRight;
        top = !left && !right && minDist == distTop;
        bottom = !left && !right && !top;
    }
}

platform::MouseEvent PenEvent(platform::Vec2 at, platform::MouseEventKind kind) {
    return platform::MouseEvent{at, platform::MouseButton::Left, kind};
}

core::Session::Shape SessionShape(core::DrawShape shape) {
    return shape == core::DrawShape::Rectangle ? core::Session::Shape::Rectangle : core::Session::Shape::Line;
}

}  // namespace

// ================= What every gesture answers alike =================

Answer Gesture::Offer(const Event& event, Editor& editor) {
    switch (event.kind) {
        case EventKind::PointerDown:
            if (event.button != button_) {
                return Answer::Claim();  // another button: ignored
            }
            // Its own button pressed again: the release went missing - a
            // capture taken mid-drag, a focus stolen. What it began ends
            // where it got to, as that release would have ended it, and the
            // press is taken afresh. Left in flight, it took this press as
            // its own and carried on from where it was.
            Interrupt(editor);
            return Answer::Finish(/*usedUp=*/false);
        case EventKind::PointerMove:
            return Moved(event, editor);
        case EventKind::PointerUp:
            if (event.button != button_) {
                return Answer::Claim();
            }
            return Released(event, editor);
        case EventKind::Wheel:
            // On the mouse holding the gesture: a notch nudged mid-stroke
            // would switch the canvas under it, and one mid-drag would be
            // filed inside the drag.
            return Answer::Claim();
        case EventKind::KeyDown:
            if (event.key == platform::KeyCombo::kEscape) {
                return Answer::Cancel();
            }
            return Answer::Pass();
        case EventKind::KeyUp:
            return Answer::Pass();
        case EventKind::Modifiers:
            return ModifiersChanged(event, editor);
        case EventKind::Tick:
            return Ticked(event, editor);
        case EventKind::Hotkey:
            return Answer::Pass();
    }
    return Answer::Pass();
}

// ================= Spent =================

Answer Spent::Offer(const Event& event, Editor& /*editor*/) {
    switch (event.kind) {
        case EventKind::PointerDown:
            // Its own button again: the release came after all, unseen -
            // and this press is a new one.
            return event.button == button_ ? Answer::Finish(/*usedUp=*/false) : Answer::Claim();
        case EventKind::PointerMove:
            return Answer::Claim();
        case EventKind::PointerUp:
            return event.button == button_ ? Answer::Finish() : Answer::Claim();
        case EventKind::Wheel:
            return Answer::Claim();
        case EventKind::KeyDown:
        case EventKind::KeyUp:
        case EventKind::Modifiers:
        case EventKind::Tick:
        case EventKind::Hotkey:
            return Answer::Pass();  // Escape included: nothing here to cancel
    }
    return Answer::Pass();
}

// ================= Pending =================

Answer Pending::Moved(const Event& event, Editor& editor) {
    const float travel = Distance(press_.position, event.position);
    if (travel > kDoubleClickPx) {
        strayed_ = true;  // a hold is a finger on one spot
    }
    const bool dragged = meaning_.frames ? travel > meaning_.dragPx : travel >= meaning_.dragPx;
    if (!dragged) {
        return Answer::Claim();
    }
    if (!meaning_.drag) {
        // A drag means nothing here. Still for a hold within reach, the
        // press waits for it; otherwise the rest of it is spent - a click
        // it could have been is not one now.
        return meaning_.click.has_value() || strayed_ ? Answer::Finish() : Answer::Claim();
    }
    return Answer::Start(std::nullopt, meaning_.drag(press_, editor));
}

Answer Pending::Released(const Event& event, Editor& editor) {
    editor.RememberClick(press_);
    std::optional<Command> click = meaning_.click;
    if (click.has_value() && !click->at.has_value()) {
        click->at = event.position;  // what it opens, it opens where it landed
    }
    return Answer::Finish(/*usedUp=*/true, click);
}

Answer Pending::Ticked(const Event& event, Editor& /*editor*/) {
    if (!meaning_.hold.has_value() || strayed_ || event.seconds - press_.seconds < kHoldSeconds) {
        return Answer::Pass();
    }
    // What a double-click would have been, for a hand that cannot
    // double-click; the rest of the press is spent.
    return Answer::Start(meaning_.hold);
}

// ================= Placement =================

std::unique_ptr<Placement> Placement::Move(const Event& press, core::ItemId item, Editor& editor) {
    auto placement = std::unique_ptr<Placement>(new Placement(press, item));
    for (const core::ItemId id : editor.Selection()) {
        if (const core::Item* selected = editor.Manager().FindItemAnywhere(id)) {
            placement->startRects_.push_back({id, selected->rect});
        }
    }
    return placement;
}

std::unique_ptr<Placement> Placement::ResizeByHandle(const Event& press, core::ItemId item, ResizeHandle handle,
                                                     Editor& editor) {
    auto placement = std::unique_ptr<Placement>(new Placement(press, item));
    placement->resize_ = true;
    placement->handle_ = handle;
    placement->fromDrag_ = false;
    ResizeHandleEdges(handle, placement->left_, placement->right_, placement->top_, placement->bottom_);
    placement->SnapshotResizeTargets(editor);
    return placement;
}

std::unique_ptr<Placement> Placement::ResizeFromNearestEdge(const Event& press, core::ItemId item, Editor& editor) {
    auto placement = std::unique_ptr<Placement>(new Placement(press, item));
    placement->resize_ = true;
    if (const core::Item* found = editor.Manager().FindItemAnywhere(item)) {
        // From the rect as it is - for a fullscreen snippet, the screen; the
        // snapshot is retaken from the restored rect as the drag begins.
        NearestResizeEdges(found->rect, press.position.x, press.position.y, placement->left_, placement->right_,
                           placement->top_, placement->bottom_);
    }
    placement->SnapshotResizeTargets(editor);
    return placement;
}

void Placement::SnapshotResizeTargets(Editor& editor) {
    const core::Item* grabbed = editor.Manager().FindItemAnywhere(item_);
    if (grabbed == nullptr) {
        return;
    }
    if (!editor.IsSelected(item_) || editor.Selection().size() < 2) {
        startRects_.push_back({item_, grabbed->rect});
        return;
    }
    // One of several selected: the drag takes all of them. The snippet
    // grabbed is in the selection by construction, so it is in here too.
    for (const core::ItemId id : editor.Selection()) {
        if (const core::Item* selected = editor.Manager().FindItemAnywhere(id)) {
            startRects_.push_back({id, selected->rect});
        }
    }
    group_ = startRects_.size() > 1;
    if (group_) {
        startBounds_ = BoundsOf(startRects_);
    }
}

void Placement::Begin(const Event& event, Editor& editor) {
    std::vector<core::ItemId> ids;
    for (const StartRect& start : startRects_) {
        ids.push_back(start.item);
    }
    // Where the snippets it may touch are now is the session's, which files
    // the gesture as one undo entry when it ends, if anything moved.
    editor.GetSession().BeginPlacement(ids);
    last_ = event.position;
    if (!fromDrag_) {
        return;  // the band, pressed: nothing has moved yet
    }
    // A fullscreen snippet is taken out of fullscreen the moment it is
    // dragged, as a window manager un-maximizes a window you take hold of
    // - but only now that it *is* a drag, so a click that merely selects it
    // leaves it be. The snapshot is retaken from the restored rect; the few
    // pixels the pointer has traveled by then are not worth a jump.
    for (StartRect& start : startRects_) {
        const core::Item* item = editor.Manager().FindItemAnywhere(start.item);
        if (item != nullptr && item->isFullscreen) {
            editor.GetSession().PreviewLeaveFullscreen(start.item);
            item = editor.Manager().FindItemAnywhere(start.item);
            if (item != nullptr) {
                start.rect = item->rect;
            }
        }
    }
    if (group_) {
        startBounds_ = BoundsOf(startRects_);
    }
    Apply(event.position, editor);
}

Answer Placement::Moved(const Event& event, Editor& editor) {
    last_ = event.position;
    Apply(event.position, editor);
    return Answer::Claim();
}

Answer Placement::ModifiersChanged(const Event& /*event*/, Editor& editor) {
    // Shift frees or keeps a resize's shape as it is pressed or let go of,
    // not only at the next move.
    if (resize_) {
        Apply(last_, editor);
    }
    return Answer::Pass();
}

Answer Placement::Released(const Event& event, Editor& editor) {
    // Where the button came up, first: movement comes once a frame, and a
    // release carries its own position, which the last move may not have
    // reached - a quick drag, or the band let go of with no move at all.
    // An interruption has no position of its own, and ends where it got.
    if (event.position.x != last_.x || event.position.y != last_.y) {
        last_ = event.position;
        Apply(event.position, editor);
    }
    // The whole gesture, one entry - or none, if it came back to where it
    // began.
    editor.GetSession().EndPlacement();
    return Answer::Finish();
}

void Placement::Interrupt(Editor& editor) { editor.GetSession().EndPlacement(); }

void Placement::Cancel(Editor& editor) { editor.GetSession().CancelPlacement(); }

void Placement::Apply(platform::Vec2 pointer, Editor& editor) {
    const float dx = pointer.x - press_.position.x;
    const float dy = pointer.y - press_.position.y;
    if (resize_ && group_) {
        ResizeAsAGroup(dx, dy, editor);
        return;
    }
    for (const StartRect& start : startRects_) {
        const core::Item* item = editor.Manager().FindItemAnywhere(start.item);
        if (item == nullptr) {
            continue;  // deleted mid-drag - nothing left to move/resize
        }
        core::Rect newRect = start.rect;
        if (resize_) {
            // The snippet's own setting (see Item::keepAspect), which Shift
            // flips - read at every move, so it takes effect at once.
            const bool lockAspect = item->keepAspect != editor.Held().shift;
            core::ApplyResizeHandleDelta(newRect, left_, right_, top_, bottom_, dx, dy, lockAspect);
        } else {
            newRect.x += dx;
            newRect.y += dy;
        }
        // Re-anchored to this deliberate move/resize as it goes - see
        // Session::PreviewRect.
        editor.GetSession().PreviewRect(
            start.item, core::ClampRectToViewport(newRect, editor.DisplayWidth(), editor.DisplayHeight()));
    }
}

// Every selected snippet scaled by one factor about the point the drag
// leaves fixed - the opposite corner, or the middle of the axis an edge
// does not drive, which is how one snippet's own edge grows too.
//
// Scaled, never stretched, whatever Shift says: a group is snippets of
// several shapes, and stretching the box around them would reshape every
// one of them at once. Shift frees a single snippet's resize because there
// the one shape being changed is the one under the hand.
//
// The smallest snippet decides how far down the group can go: one of them
// reaching the floor stops all of them, rather than that one flattening
// against it while the rest carry on shrinking.
void Placement::ResizeAsAGroup(float dx, float dy, Editor& editor) {
    const core::Rect box = startBounds_;
    if (box.w <= 0.0f || box.h <= 0.0f) {
        return;
    }
    core::Rect dragged = box;
    core::ApplyResizeHandleDelta(dragged, left_, right_, top_, bottom_, dx, dy, /*lockAspect=*/true);
    float scale = dragged.w / box.w;
    float floorScale = 0.0f;
    for (const StartRect& start : startRects_) {
        if (start.rect.w <= 0.0f || start.rect.h <= 0.0f) {
            continue;
        }
        const core::MinItemSize smallest = core::MinimumSizeForAspectRatio(start.rect.w / start.rect.h);
        floorScale = std::max({floorScale, smallest.w / start.rect.w, smallest.h / start.rect.h});
    }
    scale = std::max(scale, floorScale);

    const float anchorX = left_ ? box.x + box.w : (right_ ? box.x : box.x + box.w * 0.5f);
    const float anchorY = top_ ? box.y + box.h : (bottom_ ? box.y : box.y + box.h * 0.5f);
    for (const StartRect& start : startRects_) {
        const core::Rect scaled{anchorX + (start.rect.x - anchorX) * scale,
                                anchorY + (start.rect.y - anchorY) * scale, start.rect.w * scale,
                                start.rect.h * scale};
        // Clamped one snippet at a time, exactly as a move of the whole
        // selection is: at the very edges of the screen that can put one
        // of them out of step with the rest, and the alternative is a
        // snippet scaled off the screen with nothing left to grab. One
        // deleted mid-drag is passed over by the session.
        editor.GetSession().PreviewRect(
            start.item, core::ClampRectToViewport(scaled, editor.DisplayWidth(), editor.DisplayHeight()));
    }
}

// ================= BoxSelect =================

core::Rect BoxSelect::Bounds() const {
    const float x0 = std::min(from_.x, to_.x);
    const float y0 = std::min(from_.y, to_.y);
    return core::Rect{x0, y0, std::max(from_.x, to_.x) - x0, std::max(from_.y, to_.y) - y0};
}

Answer BoxSelect::Moved(const Event& event, Editor& /*editor*/) {
    to_ = event.position;
    return Answer::Claim();
}

// Joins rather than replaces, because Shift is already the key that adds
// one snippet to the selection and takes one out - a box drawn with it
// held is the same thing said about several at once.
Answer BoxSelect::Released(const Event& event, Editor& editor) {
    to_ = event.position;
    editor.AddTouchedToSelection(Bounds());
    return Answer::Finish();
}

// ================= Framing =================

core::Rect Framing::Frame() const {
    return core::Rect{std::min(from_.x, to_.x), std::min(from_.y, to_.y), std::abs(from_.x - to_.x),
                      std::abs(from_.y - to_.y)};
}

Answer Framing::Moved(const Event& event, Editor& /*editor*/) {
    to_ = event.position;
    return Answer::Claim();
}

Answer Framing::Released(const Event& event, Editor& /*editor*/) {
    to_ = event.position;
    Command made{CommandId::FrameSnippet};
    made.rect = Frame();
    made.kind = kind_;
    made.madeBy = madeBy_;
    return Answer::Finish(/*usedUp=*/true, made);
}

// ================= Widget =================

void Widget::Interrupt(Editor& editor) {
    editor.GetSession().EndStyleEdit();
    editor.Views().LetGoOfWidget();
}

void Widget::Begin(const Event& /*event*/, Editor& editor) { penColorAtPress_ = editor.DrawColorRGBA(); }

// A slider's or a swatch's value, and the pen's color in its chooser, are
// written as they are dragged, and put back here - see docs/INTERACTIONS.md,
// section 5.
void Widget::Cancel(Editor& editor) {
    editor.GetSession().CancelStyleEdit();
    editor.CancelSettingsPreviews();
    editor.SetDrawColor(penColorAtPress_);
    editor.Views().LetGoOfWidget();
}

// ================= BarPress =================

Answer BarPress::Moved(const Event& event, Editor& /*editor*/) {
    at_ = event.position;
    if (Distance(pressedAt_, event.position) > kDoubleClickPx) {
        strayed_ = true;  // a hold is a finger on one spot
    }
    return Answer::Claim();
}

Answer BarPress::Released(const Event& event, Editor& editor) {
    const PointerTarget target = editor.ResolvePointerTarget(event.position.x, event.position.y);
    if (target.kind != PointerTarget::Kind::Button || target.button != chrome_) {
        return Answer::Finish();  // let go elsewhere: nothing
    }
    if (button_ == platform::MouseButton::Right) {
        return Answer::Finish(/*usedUp=*/true, editor.BarButtonMenuCommand(chrome_, event.position));
    }
    return Answer::Finish(/*usedUp=*/true, editor.BarButtonCommand(chrome_));
}

Answer BarPress::Ticked(const Event& event, Editor& editor) {
    if (strayed_ || event.seconds - pressSeconds_ < kHoldSeconds) {
        return Answer::Pass();
    }
    // The menu, where the finger is; the command's scope ends this, and
    // the rest of the press is spent - the release fires nothing.
    const std::optional<Command> menu = editor.BarButtonMenuCommand(chrome_, at_);
    return menu.has_value() ? Answer::Start(menu) : Answer::Pass();
}

// ================= Stroke =================

std::unique_ptr<Marking> Marking::ForTool(const Event& press, core::ItemId item, Editor& editor) {
    // What a Draw or Erase press makes is decided as it starts, from the
    // modifiers held then - Ctrl for a rectangle with either tool, Shift for
    // a line with Draw (see DrawShapeFor) - and holds for the whole gesture,
    // so letting go of a key halfway through a drag changes nothing it has
    // done. The one thing that can change mid-drag is which of the two
    // shapes a shape is. With no modifier held, the shape is whatever was
    // picked from the tool's menu on the bar.
    const core::DrawShape shape = editor.ShapeForPress();
    Kind kind = Kind::Freehand;
    if (editor.ActiveTool() == core::Tool::Erase) {
        kind = shape == core::DrawShape::Rectangle ? Kind::EraseRect : Kind::Erase;
    } else if (shape != core::DrawShape::Freehand) {
        kind = Kind::Shape;
    }
    auto stroke = std::unique_ptr<Marking>(new Marking(press, item, kind));
    stroke->shape_ = shape;
    return stroke;
}

std::unique_ptr<Marking> Marking::RightErase(const Event& press, core::ItemId item) {
    return std::unique_ptr<Marking>(new Marking(press, item, Kind::Erase));
}

core::Rect Marking::EraseBox() const {
    return core::Rect{std::min(press_.position.x, last_.x), std::min(press_.position.y, last_.y),
                      std::abs(press_.position.x - last_.x), std::abs(press_.position.y - last_.y)};
}

void Marking::Begin(const Event& event, Editor& editor) {
    core::Session& session = editor.GetSession();
    const platform::Vec2 at = press_.position;
    items_ = {item_};
    for (const core::ItemId id : editor.DrawingItems()) {
        if (id != item_) {
            items_.push_back(id);
        }
    }
    switch (kind_) {
        case Kind::Freehand:
            // Accumulated into the live layer (screen space) while it is
            // drawn, then moved into the snippet in its own native space as
            // it ends (see Session::CommitLiveStroke).
            session.LiveLayer().Clear();  // nothing of an earlier stroke is part of this one
            editor.Pen().OnMouseEvent(PenEvent(at, platform::MouseEventKind::Down), session.LiveLayer());
            break;
        case Kind::Shape:
            // The whole of a shape is the session's: its preview, and how
            // short a drag is a stray click - see Session::BeginShape.
            session.BeginShape(items_, SessionShape(shape_), at.x, at.y, editor.DrawColorRGBA(), editor.DrawWidth());
            break;
        case Kind::Erase:
            // One gesture in the session: the strokes it clips, snapshotted
            // as it starts and filed as a single undo entry as it ends.
            // From the press, for the right button's, which begins once it
            // is a drag.
            session.BeginErase(items_, at.x, at.y, editor.EraserWidth());
            if (event.kind == EventKind::PointerMove) {
                session.ExtendErase(event.position.x, event.position.y, editor.EraserWidth());
            }
            editor.NoteErasing(core::DrawShape::Freehand);
            break;
        case Kind::EraseRect:
            // Dragged out from a fixed corner with nothing erased while it
            // is - the rect is only a preview - then erased once, on
            // release, as one undoable step.
            editor.NoteErasing(core::DrawShape::Rectangle);
            break;
    }
    last_ = event.position;
}

Answer Marking::Moved(const Event& event, Editor& editor) {
    core::Session& session = editor.GetSession();
    last_ = event.position;
    switch (kind_) {
        case Kind::Freehand:
            editor.Pen().OnMouseEvent(PenEvent(event.position, platform::MouseEventKind::Move), session.LiveLayer());
            break;
        case Kind::Shape:
            // Pressing the other modifier mid-drag turns a line into a
            // rectangle or back; letting go of both leaves it as it was.
            if (editor.Held().ctrl || editor.Held().shift) {
                shape_ = core::DrawShapeFor(editor.Held().ctrl, editor.Held().shift);
                session.SetShape(SessionShape(shape_));
            }
            session.UpdateShape(event.position.x, event.position.y);
            break;
        case Kind::Erase:
            session.ExtendErase(event.position.x, event.position.y, editor.EraserWidth());
            break;
        case Kind::EraseRect:
            break;
    }
    return Answer::Claim();
}

Answer Marking::Released(const Event& event, Editor& editor) {
    core::Session& session = editor.GetSession();
    switch (kind_) {
        case Kind::Freehand:
            editor.Pen().OnMouseEvent(PenEvent(event.position, platform::MouseEventKind::Up), session.LiveLayer());
            session.CommitLiveStroke(items_);
            break;
        case Kind::Shape:
            session.EndShape(event.position.x, event.position.y);
            break;
        case Kind::Erase:
            // On to where it came up, as a placement's release does.
            session.ExtendErase(event.position.x, event.position.y, editor.EraserWidth());
            session.EndErase();
            break;
        case Kind::EraseRect:
            // A drag too short to be meant erases nothing.
            last_ = event.position;
            if (Distance(press_.position, event.position) >= kRegionMinSize) {
                const core::Rect box = EraseBox();
                session.EraseRect(items_, box.x, box.y, box.x + box.w, box.y + box.h);
            }
            break;
    }
    return Answer::Finish();
}

// Kept, as its release would end it, where it last was: the stroke, the
// shape or the erase filed as one undo step. Ended where the pointer is
// instead, a stroke whose release went missing got a straight tail to the
// next press. The rectangle eraser alone makes nothing: its release is
// what erases, and nothing is erased that the release did not ask for.
void Marking::Interrupt(Editor& editor) {
    core::Session& session = editor.GetSession();
    switch (kind_) {
        case Kind::Freehand:
            editor.Pen().OnMouseEvent(PenEvent(last_, platform::MouseEventKind::Up), session.LiveLayer());
            session.CommitLiveStroke(items_);
            break;
        case Kind::Shape:
            session.EndShape(last_.x, last_.y);
            break;
        case Kind::Erase:
            session.EndErase();
            break;
        case Kind::EraseRect:
            break;
    }
}

// Escape: nothing of it is left.
void Marking::Cancel(Editor& editor) {
    core::Session& session = editor.GetSession();
    switch (kind_) {
        case Kind::Freehand:
            session.LiveLayer().Clear();
            break;
        case Kind::Shape:
            session.CancelShape();
            break;
        case Kind::Erase:
            session.CancelErase();
            break;
        case Kind::EraseRect:
            break;
    }
}

}  // namespace sz::ui
