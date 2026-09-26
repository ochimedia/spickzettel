#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/canvas/item_geometry.h"

#include <imgui.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Tool / creation-arming state =================

namespace {

// px, click-vs-drag gesture threshold for the region-capture drag.
constexpr float kCreationDragThreshold = 6.0f;

// The shortcut name of a mouse button a shortcut may be, or 0 for the two
// gestures are made with.
int ComboKeyForMouseButton(platform::MouseButton button) {
    switch (button) {
        case platform::MouseButton::Middle:
            return platform::KeyCombo::kMiddleButton;
        case platform::MouseButton::X1:
            return platform::KeyCombo::kX1Button;
        case platform::MouseButton::X2:
            return platform::KeyCombo::kX2Button;
        case platform::MouseButton::Left:
        case platform::MouseButton::Right:
            return 0;
    }
    return 0;
}

// px the pointer has to travel from a press on a snippet before the press
// is a drag rather than a click: a click selects and moves nothing, so
// nothing about the snippet is written until then.
constexpr float kSelectionDragThreshold = 4.0f;

// The box around a gesture's snapshotted rects - the selection as it
// stood at the press. Retaken when a fullscreen snippet among them is
// restored, which changes the very rect the box was drawn around.
Rect BoundsOfStartRects(const std::vector<ItemGesture::StartRect>& rects) {
    if (rects.empty()) {
        return Rect{};
    }
    float x0 = rects.front().rect.x;
    float y0 = rects.front().rect.y;
    float x1 = x0 + rects.front().rect.w;
    float y1 = y0 + rects.front().rect.h;
    for (const ItemGesture::StartRect& start : rects) {
        x0 = std::min(x0, start.rect.x);
        y0 = std::min(y0, start.rect.y);
        x1 = std::max(x1, start.rect.x + start.rect.w);
        y1 = std::max(y1, start.rect.y + start.rect.h);
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

// Whether a resize holds the snippet's shape unless Shift says otherwise -
// the snippet's own setting (see Item::keepAspect). Shift flips whichever
// it is.
bool KeepsAspectRatio(const Item& item) { return item.keepAspect; }

// Which edge(s) of `rect` a point at (x, y) is nearest to, for the
// Alt+right-drag "resize from nearest edge" way in (see
// OverlayApp::HandleItemGesture) - divides the rect into a 3x3 grid: the
// outer thirds on
// either axis pick that edge (both axes at once for a corner); dead
// center (neither axis in an outer third) falls back to whichever single
// edge is nearest by plain distance, so a press anywhere in the rect
// always resolves to *something* rather than resizing nothing.
void NearestResizeEdges(const Rect& rect, float x, float y, bool& left, bool& right, bool& top, bool& bottom) {
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

}  // namespace

void OverlayApp::SetToolShortcut(ShortcutAction action, platform::KeyCombo combo) {
    if (combo.key != 0) {
        // Whatever else held this key loses it. The alternative - refusing
        // the change - leaves the user to go and find the other holder
        // themselves, and two rows claiming one key is a state where only
        // the first of them could ever fire (see HandleCommandKey).
        //
        // Judged against what the *edited* target resolves to, not against
        // what is running: a collision inside a profile is a collision when
        // that profile is active, and a key the defaults use elsewhere is
        // not this profile's problem to solve.
        const ProfileableSettings edited = EditedSettings();
        for (const ShortcutAction other : kAllShortcutActions) {
            if (other != action && edited.shortcuts[ShortcutActionIndex(other)] == combo) {
                SetEditedShortcut(other, platform::KeyCombo{});
            }
        }
    }
    SetEditedShortcut(action, combo);
}

bool OverlayApp::NoteDoubleClick(const platform::MouseEvent& event) {
    const double now = ImGui::GetTime();
    // A modified press is never half of a double-click: Shift+click twice
    // on one snippet adds it and takes it out again, and nothing else. But
    // for the modifier that picks what a press on empty canvas makes: with
    // it held, a double-click there makes that kind fullscreen, as a plain
    // one does the plain kind - and both presses have to have it.
    const std::optional<CreationTrigger> held = CreationTriggerFor(editor_.Held());
    if (!held.has_value() || (*held != CreationTrigger::Plain && *held != Cfg().screenshotTrigger &&
                              *held != Cfg().drawingTrigger)) {
        hand_.lastPress.reset();
        return false;
    }
    bool isDouble = false;
    if (hand_.lastPress.has_value() && hand_.lastPress->button == event.button && hand_.lastPress->modifiers == *held &&
        now - hand_.lastPress->atSeconds <= kDoubleClickSeconds) {
        const float dx = event.position.x - hand_.lastPress->x;
        const float dy = event.position.y - hand_.lastPress->y;
        isDouble = std::sqrt(dx * dx + dy * dy) <= kDoubleClickPx;
    }
    if (isDouble) {
        hand_.lastPress.reset();  // the pair is spent: a third press starts over
    } else {
        hand_.lastPress = LastPress{event.button, *held, now, event.position.x, event.position.y};
    }
    return isDouble;
}
void OverlayApp::HoldPress(const platform::MouseEvent& event, std::optional<ItemId> item,
                           std::optional<ItemCreationKind> creates) {
    if (hand_.pressIsDouble) {
        // Spent already: the second press of a double-click does on release
        // what the hold would do, and must not do it twice.
        hand_.heldPress.reset();
        return;
    }
    hand_.heldPress = HeldPress{event.button, event.position.x, event.position.y, ImGui::GetTime(), item, creates};
}
void OverlayApp::MatureHeldPress() {
    if (!hand_.heldPress.has_value() || ImGui::GetTime() - hand_.heldPress->atSeconds < kHoldSeconds) {
        return;
    }
    const HeldPress held = *hand_.heldPress;
    hand_.heldPress.reset();
    if (held.item.has_value()) {
        if (const ItemGesture* move = GestureIf<ItemGesture>(); move != nullptr && move->button == held.button) {
            if (move->moved) {
                return;  // became a drag after all - a move, not a hold
            }
            // The move the press started never moved, so nothing of it was
            // written; without it the release finds nothing to end.
            hand_.gesture = std::monostate{};
        }
        editor_.EnterDrawingMode(*held.item);
        return;
    }
    if (held.creates.has_value()) {
        if (const CreationGesture* creation = GestureIf<CreationGesture>();
            creation != nullptr && held.button == platform::MouseButton::Left) {
            if (creation->dragTo.has_value()) {
                return;  // became a drag after all - a frame, not a hold
            }
            // Dropped from under the release, which then finds nothing to
            // place and places nothing.
            hand_.gesture = std::monostate{};
        }
        const ItemId made = editor_.CreateFullscreenItem(*held.creates);
        // As a double-click's fullscreen drawing is: watched until something
        // goes into it - see Editor::UntouchedDrawing.
        if (*held.creates != ItemCreationKind::Screenshot && made != 0) {
            editor_.WatchAsUntouched(made);
        }
    }
}

void OverlayApp::PlaceWelcomeNotes(float displayW, float displayH) {
    // Each sized to its text rather than to the screen - fitted to the
    // hand-wrapped lines at their size, a hair wider than the longest -
    // so none sits there mostly empty. The welcome is 10 lines at 18px;
    // the two warnings 8 lines at 22px, larger because they are the two
    // things a new user must not skip.
    //
    // All of it at the interface scale, although notes are content and
    // content is not scaled: these are the app talking, made while it
    // starts, and someone who reads at 150% should not have to find the
    // text-size slider to read the note that tells them where it is.
    const ImVec2 welcomeSize = Px(400.0f, 214.0f);
    const ImVec2 warningSize = Px(300.0f, 214.0f);
    const float gap = Px(24.0f);
    // A light red: a warning, readable on the note's dark backing, and not
    // the danger red of a delete button.
    constexpr uint32_t kWarningTextRGBA = 0xFF8C80FFu;

    // In a row, the welcome first, centered - or, on a screen too narrow
    // for that, in a column. On one too small for either they overlap
    // rather than going off screen, which ClampRectToViewport sees to.
    const float rowW = welcomeSize.x + 2.0f * (warningSize.x + gap);
    const bool row = rowW <= displayW * 0.95f;
    const ImVec2 group = row ? ImVec2(rowW, welcomeSize.y)
                             : ImVec2(welcomeSize.x, welcomeSize.y + 2.0f * (warningSize.y + gap));
    ImVec2 at((displayW - group.x) * 0.5f, (displayH - group.y) * 0.5f);

    editor_.EnsureCanvasForNewItem();
    // Made as they are, text and all, and not on the history: nobody made
    // them, so there is nothing for an undo to take back.
    const auto place = [&](ImVec2 size, const char* name, std::string text, float textSizePx,
                           uint32_t textColorRGBA) -> ItemId {
        Item note;
        note.name = name;
        note.rect = ClampRectToViewport(Rect{at.x, at.y, size.x, size.y}, displayW, displayH);
        (row ? at.x : at.y) += (row ? size.x : size.y) + gap;
        // Boxes of text, whose shape is the point of resizing them: the text
        // wraps to the new width rather than scaling with it.
        note.keepAspect = false;
        // A Text Note's backing (see kNoteBackgroundColorRGBA), but darker:
        // these land on whatever the desktop happens to show, and half
        // transparent over a white window left the red text washed out.
        note.picture.tintColorRGBA = kNoteBackgroundColorRGBA;
        note.picture.opacity = 0.8f;
        note.noteText = std::move(text);
        note.noteTextSizePx = std::min(textSizePx, kNoteTextSizeMax);
        note.noteTextColorRGBA = textColorRGBA;
        return session_.CreateItem(std::move(note), /*undoable=*/false);
    };
    // Deliberately short. This is the first thing anyone sees, and its job
    // is only to get them to the point where the app can explain itself:
    // one gesture, the right-click menus, the key that brings the overlay
    // back, and the cheat sheet for everything else - with the keys as
    // they are bound, which a config carried over from elsewhere may have
    // changed. Wrapped by hand at a width the note's own rect fits, since
    // Item::noteText is drawn as-is (DrawItemContent wraps too, but on its
    // own boundaries - keeping the key lines intact reads better than
    // letting them break wherever the item's width happens to fall).
    const std::string showKey = FormatKeyComboLabel(Cfg().hotkeyEditMode);
    const platform::KeyCombo& sheetKey =
        settings_.Live().shortcuts[ShortcutActionIndex(ShortcutAction::CheatSheet)];
    char text[512];
    if (sheetKey.key != 0) {
        std::snprintf(text, sizeof(text), strings::kWelcomeBody, showKey.c_str(),
                      FormatKeyComboLabel(sheetKey).c_str());
    } else {
        std::snprintf(text, sizeof(text), strings::kWelcomeBodyNoCheatSheetKey, showKey.c_str());
    }
    if (place(welcomeSize, strings::kWelcomeName, text, Px(18.0f), Item{}.noteTextColorRGBA) == 0) {
        return;
    }

    // Behavior and profiles, because the right input settings differ by
    // game and the defaults will be wrong for some; anti-cheat, because
    // hooking input and drawing over a game is what such a system looks
    // for, and a ban is not something to find out about afterwards.
    for (const auto& [name, body] : {std::pair{strings::kWelcomeBehaviorName, strings::kWelcomeBehaviorBody},
                                     std::pair{strings::kWelcomeAntiCheatName, strings::kWelcomeAntiCheatBody}}) {
        place(warningSize, name, body, Px(22.0f), kWarningTextRGBA);
    }
}

// ================= The gesture in flight =================

// Not through OnMouse: this can run inside the handling of a press - a
// press elsewhere leaves drawing mode - which a synthesized release would
// end. Each kind is ended by hand instead, keeping what it has done.
//
// Nothing a release would newly do, because what ends a gesture this way
// is not its release: a menu opened, a snippet made or a bar button fired
// by a canvas switch or a capture would be one nobody asked for, over a
// canvas they did not click on.
void OverlayApp::EndGesture() {
    // A press held still is the gesture's too: left armed, it went on to
    // enter drawing mode on its snippet half a second after an undo or a
    // delete had ended the press it came with.
    hand_.heldPress.reset();
    if (const StrokeInFlight* stroke = GestureIf<StrokeInFlight>(); stroke != nullptr) {
        // As its release would end it, where it last was: the stroke, the
        // shape or the erase kept and filed as one undo step - see
        // HandleStrokeEvent. Ended where the pointer is instead, a stroke
        // whose release went missing got a straight tail to the next press.
        if (ImGui::GetCurrentContext() != nullptr) {
            HandleStrokeEvent(
                platform::MouseEvent{stroke->last, platform::MouseButton::Left, platform::MouseEventKind::Up});
        }
        hand_.gesture = std::monostate{};
        return;
    }
    Gesture ended = std::exchange(hand_.gesture, std::monostate{});
    if (std::holds_alternative<ItemGesture>(ended)) {
        // Where it has got to, as one undo step - or none, if it never moved.
        session_.EndPlacement();
    } else if (const RightErase* erase = std::get_if<RightErase>(&ended); erase != nullptr && erase->erasing) {
        session_.EndErase();
    }
    // A snippet being framed, a box, a held bar button, a right click on
    // empty canvas: nothing done yet, and nothing done now.
}

// ================= The selection's gestures: select, move, resize, and the bar's buttons =================

// Every way a snippet is selected, moved or resized comes through here, as
// one gesture with three ways in - what ResolvePointerTarget says is under
// the press:
//  - a snippet's body, on the left button, while the selection is live
//    (Select in hand, or Alt held with any tool): the press selects it -
//    alone, or added to or taken out of the selection with Shift - and a
//    drag from there moves the whole selection. A click that never becomes
//    a drag moves nothing;
//  - one of a selected snippet's handles: a drag resizes that one snippet
//    by that handle, keeping its shape or not (see KeepsAspectRatio; Shift
//    flips it);
//  - a snippet's body on the right button: a resize of that snippet from
//    whichever edge or corner is nearest the press (NearestResizeEdges),
//    decided once at the press - the one way to resize without aiming for
//    a handle, with any tool in hand. A right press that never becomes a
//    drag is a right click, which has selected the snippet and no more.
// Whichever started it, the gesture is a start snapshot (every moved
// snippet's rect and the pointer's position at the press) plus the *full*
// delta from there, recomputed fresh on every Move - not an incremental
// delta accumulated per event, which drifts under event coalescing, and
// not ImGui's own GetMouseDragDelta, since this is the raw platform
// pipeline (see OnMouse) and no widget backs any of it.
//
// A press that selects a snippet brings it to the front while
// AppConfig::raiseSelectedSnippet is on (the default), the way a window
// manager raises a window you take hold of; off, the stacking order is the
// context menu's alone to change, as in a drawing program. A press
// on a snippet of a multi-selection brings the whole selection forward,
// keeping its own order: it is the selection that is taken hold of. A
// Shift-click that adds to or takes from the selection restacks nothing.
//
// The selection bar's buttons ride the same pipeline: a press on one is
// held until release, and fires only if the release lands on the same
// button - the rule ImGui's own Button follows.
//
// Returns true if this event was consumed (the caller returns without its
// own normal handling); false lets it fall through to whatever it would
// otherwise do - a stroke or a creation gesture for the left button, a
// drawing for the right.
bool OverlayApp::HandleItemGesture(const platform::MouseEvent& event) {
    if (event.button != platform::MouseButton::Left && event.button != platform::MouseButton::Right) {
        return false;
    }
    if (const ItemGesture* item = GestureIf<ItemGesture>(); item != nullptr && item->button != event.button) {
        // The other button's business - a drag it started is left alone
        // rather than hijacked, and it gets nothing of ours.
        return false;
    }
    if (GestureIf<BoxSelection>() != nullptr) {
        return HandleBoxSelection(event);
    }
    if (const BarPress* press = GestureIf<BarPress>()) {
        if (event.button != platform::MouseButton::Left) {
            return false;
        }
        if (event.kind == platform::MouseEventKind::Up) {
            const ChromeButton pressed = press->button;
            hand_.gesture = std::monostate{};
            const PointerTarget target = editor_.ResolvePointerTarget(event.position.x, event.position.y);
            if (target.kind == PointerTarget::Kind::Button && target.button == pressed) {
                editor_.ActivateBarButton(pressed);
            }
        }
        return true;  // held: nothing else starts under a pressed button
    }
    if (ItemGesture* gesture = GestureIf<ItemGesture>()) {
        // Continuing (or ending) the gesture - always consumed regardless
        // of Alt's current state (releasing Alt mid-drag shouldn't abandon
        // it half-finished).
        if (event.kind == platform::MouseEventKind::Up) {
            ItemGesture ended = std::move(*gesture);
            hand_.gesture = std::monostate{};
            // The whole gesture, one entry - or none, for a press that
            // never moved anything.
            session_.EndPlacement();
            if (ended.button == platform::MouseButton::Right && !ended.moved) {
                // A right press on a snippet that never dragged is a right
                // click - a drag resizes instead. On the snippet being
                // drawn on it leaves drawing mode, and opens nothing:
                // there, the right button belongs to the eraser (see
                // OnMouse), and this click only got here because Alt took
                // the snippet out of its hands. On any other snippet the
                // press has already selected it, and the click opens its
                // context menu where it landed.
                if (editor_.DrawingItem() == ended.item) {
                    editor_.ExitDrawingMode();
                } else {
                    OpenItemContextMenu(ended.item, ImVec2(event.position.x, event.position.y));
                }
            }
            return true;
        }
        if (event.kind != platform::MouseEventKind::Move) {
            return true;
        }
        const float dx = event.position.x - gesture->startMouseX;
        const float dy = event.position.y - gesture->startMouseY;
        if (!gesture->moved) {
            if (std::sqrt(dx * dx + dy * dy) < kSelectionDragThreshold) {
                return true;  // still a click, as far as anyone can tell
            }
            gesture->moved = true;
            KeepPlacedDrawings();
            // A fullscreen snippet is taken out of fullscreen the moment
            // it is dragged, as a window manager un-maximizes a window you
            // take hold of - but only now that it *is* a drag, so a click
            // that merely selects it leaves it be. The snapshot is retaken
            // from the restored rect; the few pixels the pointer has
            // traveled by then are not worth a jump.
            for (ItemGesture::StartRect& start : gesture->startRects) {
                const Item* item = Manager().FindItemAnywhere(start.item);
                if (item != nullptr && item->isFullscreen) {
                    session_.PreviewLeaveFullscreen(start.item);
                    item = Manager().FindItemAnywhere(start.item);
                    if (item != nullptr) {
                        start.rect = item->rect;
                    }
                }
            }
            if (gesture->group) {
                // The box was drawn around the rects the press found, and
                // one of them has just been restored out of fullscreen.
                gesture->startBounds = BoundsOfStartRects(gesture->startRects);
            }
        }
        if (gesture->resize && gesture->group) {
            ResizeSelectionAsAGroup(dx, dy);
            return true;
        }
        const ImVec2 display(editor_.DisplayWidth(), editor_.DisplayHeight());
        for (const ItemGesture::StartRect& start : gesture->startRects) {
            const Item* item = Manager().FindItemAnywhere(start.item);
            if (item == nullptr) {
                continue;  // deleted mid-drag - nothing left to move/resize
            }
            Rect newRect = start.rect;
            if (gesture->resize) {
                // Recomputed from the modifier on every event, so Shift
                // pressed or let go mid-drag takes effect at once.
                const bool lockAspect = KeepsAspectRatio(*item) != editor_.Held().shift;
                ApplyResizeHandleDelta(newRect, gesture->left, gesture->right, gesture->top, gesture->bottom, dx,
                                       dy, lockAspect);
            } else {
                newRect.x += dx;
                newRect.y += dy;
            }
            // Re-anchored to this deliberate move/resize as it goes - see
            // Session::PreviewRect.
            session_.PreviewRect(start.item, ClampRectToViewport(newRect, display.x, display.y));
        }
        return true;
    }
    if (event.kind != platform::MouseEventKind::Down || ImGui::GetIO().WantCaptureMouse ||
        GestureInFlight()) {
        // Only a press starts one, and not a press that a panel of ImGui's
        // own is under: a popover, the canvas bar, the Overview all
        // sit above every item, and a click landing on one of them is
        // theirs alone. Nor while another gesture is in flight - one at a
        // time, see Hand::gesture.
        return false;
    }
    const PointerTarget target = editor_.ResolvePointerTarget(event.position.x, event.position.y);
    ItemGesture gesture;
    gesture.button = event.button;
    gesture.startMouseX = event.position.x;
    gesture.startMouseY = event.position.y;
    if (event.button == platform::MouseButton::Right) {
        if (target.kind != PointerTarget::Kind::Body) {
            return false;
        }
        if (editor_.DrawingItem() == target.item && !editor_.Held().alt) {
            // On the snippet being drawn on the right button is the eraser
            // (see OnMouse), unless Alt picks the snippet up.
            return false;
        }
        const Item* item = Manager().FindItemAnywhere(target.item);
        if (item == nullptr) {
            return false;
        }
        if (!editor_.IsSelected(target.item)) {
            editor_.SelectOnly(target.item);
        }
        if (Cfg().raiseSelectedSnippet) {
            // The whole selection, as a block - see BringItemsToFront.
            // Reorders canvas.items, so `item` is found again afterwards
            // rather than read through the pointer from before.
            session_.BringItemsToFront(editor_.Selection());
            item = Manager().FindItemAnywhere(target.item);
            if (item == nullptr) {
                return false;
            }
        }
        // A resize from the nearest edge or corner - decided from the rect
        // as it is now, which for a fullscreen snippet is the screen; the
        // snapshot is retaken from the restored rect once it is a drag
        // (see the in-flight handling above), and a click that never
        // drags leaves a fullscreen snippet fullscreen.
        gesture.item = target.item;
        gesture.resize = true;
        NearestResizeEdges(item->rect, event.position.x, event.position.y, gesture.left, gesture.right, gesture.top,
                           gesture.bottom);
        SnapshotResizeTargets(gesture, target.item);
        BeginPlacementRecord(gesture);
        hand_.gesture = std::move(gesture);
        return true;
    }
    switch (target.kind) {
        case PointerTarget::Kind::Button:
            hand_.gesture = BarPress{target.button};
            return true;
        case PointerTarget::Kind::Handle: {
            const Item* item = Manager().FindItemAnywhere(target.item);
            if (item == nullptr) {
                return false;
            }
            gesture.item = target.item;
            gesture.resize = true;
            gesture.handle = target.handle;
            sz::ui::ResizeHandleEdges(target.handle, gesture.left, gesture.right, gesture.top, gesture.bottom);
            SnapshotResizeTargets(gesture, target.item);
            gesture.moved = true;  // a handle is only ever pressed to drag it
            BeginPlacementRecord(gesture);
            hand_.gesture = std::move(gesture);
            KeepPlacedDrawings();
            return true;
        }
        case PointerTarget::Kind::Body: {
            if (!editor_.SelectionLive() || !editor_.PressPicksUp()) {
                return false;  // a creation tool's press, or a stroke in drawing mode
            }
            if (hand_.pressIsDouble && !editor_.Held().shift) {
                // The second press of a double-click on a snippet: drawing
                // mode on it, rather than a move. The first press already
                // selected it.
                editor_.EnterDrawingMode(target.item);
                return true;
            }
            if (editor_.Held().shift) {
                // Added to or taken out of the selection, and that is all
                // the press does - a Shift-press is never a drag, and
                // never restacks either: gathering snippets into a
                // selection is not taking hold of any one of them, and a
                // stack built up with care should not reshuffle as it is
                // picked from. Taking hold of the selection afterwards
                // raises it, as a block in the order it already has.
                editor_.ToggleSelected(target.item);
                return true;
            }
            if (!editor_.IsSelected(target.item)) {
                editor_.SelectOnly(target.item);
            }
            if (Cfg().raiseSelectedSnippet) {
                // What is taken hold of is the selection, so the selection
                // comes forward - as a block, in its own order, rather than
                // the one snippet under the pointer out of it.
                session_.BringItemsToFront(editor_.Selection());
            }
            gesture.item = target.item;
            for (const ItemId id : editor_.Selection()) {
                if (const Item* item = Manager().FindItemAnywhere(id)) {
                    gesture.startRects.push_back({id, item->rect});
                }
            }
            BeginPlacementRecord(gesture);
            hand_.gesture = std::move(gesture);
            // Held still instead of dragged, the press enters drawing mode
            // as a double-click would - see MatureHeldPress.
            HoldPress(event, target.item, std::nullopt);
            return true;
        }
        case PointerTarget::Kind::None:
            // Empty canvas: the press clears the selection - and is then
            // the creation gesture's, which is why this falls through
            // rather than consuming it (see HandleCreationGesture: a drag
            // frames a snippet, a double-click makes one fullscreen, a
            // click makes nothing). Not in drawing mode, where the press
            // is for leaving it (see OnMouse).
            if (editor_.SelectionLive() && !editor_.DrawingItem().has_value()) {
                if (editor_.Held().shift) {
                    // With Shift, the press is the start of a box to
                    // select by instead - consumed, so no snippet is
                    // framed under it, and the selection is left alone
                    // until the box says what it caught.
                    hand_.gesture = BoxSelection{event.position.x, event.position.y, event.position.x, event.position.y,
                                            /*moved=*/false};
                    return true;
                }
                editor_.ClearSelection();
            }
            return false;
    }
    return false;
}

// The box drawn over the canvas with Shift held, from the press on open
// canvas that started it (see HandleItemGesture) to the release that
// applies it: every snippet the box touches, by any overlap at all, joins
// the selection.
//
// Joins rather than replaces, because Shift is already the key that adds
// one snippet to the selection and takes one out - a box drawn with it
// held is the same thing said about several at once. A press that never
// travels is an ordinary Shift-click on open canvas: it caught nothing,
// and the selection stands.
bool OverlayApp::HandleBoxSelection(const platform::MouseEvent& event) {
    if (event.button != platform::MouseButton::Left) {
        return false;  // the other button's business, as every gesture here
    }
    BoxSelection& box = *GestureIf<BoxSelection>();
    if (event.kind == platform::MouseEventKind::Move) {
        box.toX = event.position.x;
        box.toY = event.position.y;
        if (!box.moved) {
            const float dx = box.toX - box.fromX;
            const float dy = box.toY - box.fromY;
            box.moved = std::sqrt(dx * dx + dy * dy) >= kSelectionDragThreshold;
        }
        return true;
    }
    if (event.kind == platform::MouseEventKind::Up) {
        const BoxSelection ended = box;
        hand_.gesture = std::monostate{};
        if (ended.moved) {
            editor_.AddTouchedToSelection(ended.Bounds());
        }
    }
    return true;
}

void OverlayApp::BeginPlacementRecord(const ItemGesture& gesture) {
    std::vector<ItemId> ids;
    for (const ItemGesture::StartRect& start : gesture.startRects) {
        ids.push_back(start.item);
    }
    session_.BeginPlacement(ids);
}

void OverlayApp::SnapshotResizeTargets(ItemGesture& gesture, ItemId itemId) {
    const Item* grabbed = Manager().FindItemAnywhere(itemId);
    if (grabbed == nullptr) {
        return;
    }
    if (!editor_.IsSelected(itemId) || editor_.Selection().size() < 2) {
        gesture.startRects.push_back({itemId, grabbed->rect});
        return;
    }
    // One of several selected: the drag takes all of them. The snippet
    // the handle belongs to is in the selection by construction, so it is
    // in here too, and nothing decides separately which snippet was
    // grabbed.
    for (const ItemId id : editor_.Selection()) {
        if (const Item* item = Manager().FindItemAnywhere(id)) {
            gesture.startRects.push_back({id, item->rect});
        }
    }
    gesture.group = gesture.startRects.size() > 1;
    if (gesture.group) {
        gesture.startBounds = BoundsOfStartRects(gesture.startRects);
    }
}

// Every selected snippet scaled by one factor about the point the drag
// leaves fixed - the opposite corner, or the middle of the axis an edge
// handle does not drive, which is how one snippet's own edge handle grows
// too.
//
// Scaled, never stretched, whatever Shift says: a group is snippets of
// several shapes, and stretching the box around them would reshape every
// one of them at once - a screenshot squashed, a drawing's strokes pulled
// out of round. Shift frees a single snippet's resize because there the
// one shape being changed is the one under the hand.
//
// The smallest snippet decides how far down the group can go: one of them
// reaching the floor stops all of them, rather than that one flattening
// against it while the rest carry on shrinking and the group quietly
// reshapes itself.
void OverlayApp::ResizeSelectionAsAGroup(float dx, float dy) {
    const ItemGesture* held = GestureIf<ItemGesture>();
    if (held == nullptr) {
        return;
    }
    const ItemGesture& gesture = *held;
    const Rect box = gesture.startBounds;
    if (box.w <= 0.0f || box.h <= 0.0f) {
        return;
    }
    Rect dragged = box;
    ApplyResizeHandleDelta(dragged, gesture.left, gesture.right, gesture.top, gesture.bottom, dx, dy,
                            /*lockAspect=*/true);
    float scale = dragged.w / box.w;
    float floorScale = 0.0f;
    for (const ItemGesture::StartRect& start : gesture.startRects) {
        if (start.rect.w <= 0.0f || start.rect.h <= 0.0f) {
            continue;
        }
        const MinItemSize smallest = MinimumSizeForAspectRatio(start.rect.w / start.rect.h);
        floorScale = std::max({floorScale, smallest.w / start.rect.w, smallest.h / start.rect.h});
    }
    scale = std::max(scale, floorScale);

    const float anchorX = gesture.left ? box.x + box.w : (gesture.right ? box.x : box.x + box.w * 0.5f);
    const float anchorY = gesture.top ? box.y + box.h : (gesture.bottom ? box.y : box.y + box.h * 0.5f);
    const ImVec2 display(editor_.DisplayWidth(), editor_.DisplayHeight());
    for (const ItemGesture::StartRect& start : gesture.startRects) {
        const Rect scaled{anchorX + (start.rect.x - anchorX) * scale, anchorY + (start.rect.y - anchorY) * scale,
                           start.rect.w * scale, start.rect.h * scale};
        // Clamped one snippet at a time, exactly as a move of the whole
        // selection is: at the very edges of the screen that can put one
        // of them out of step with the rest, and the alternative is a
        // snippet scaled off the screen with nothing left to grab. One
        // deleted mid-drag is passed over by the session.
        session_.PreviewRect(start.item, ClampRectToViewport(scaled, display.x, display.y));
    }
}

// ================= Making a snippet: a press on empty canvas =================

bool OverlayApp::PressMakesASnippet(const platform::MouseEvent& event) const {
    if (event.button != platform::MouseButton::Left && event.button != platform::MouseButton::Right) {
        return false;
    }
    // Not a press on a panel, and not one that is really a click outside
    // something open - the Overview, a popover, a note being
    // typed into - which that click is for closing. Making a snippet as
    // well would turn every dismissal into a new drawing.
    if (ImGui::GetIO().WantCaptureMouse || PanelOpen() || editor_.EditingNote().has_value() || hand_.noteOpenAtPress ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        return false;
    }
    const PointerTarget target = editor_.ResolvePointerTarget(event.position.x, event.position.y);
    return target.kind == PointerTarget::Kind::None && !target.body.has_value();
}

void OverlayApp::HandleEmptyCanvasRightPress(const platform::MouseEvent& event) {
    if (const EmptyCanvasRightPress* press = GestureIf<EmptyCanvasRightPress>()) {
        if (event.kind == platform::MouseEventKind::Move) {
            // A drag is no click, and there is nothing for one to do here:
            // ended, so the rest of it finds nothing and does nothing.
            const float dx = event.position.x - press->x;
            const float dy = event.position.y - press->y;
            if (std::sqrt(dx * dx + dy * dy) >= kSelectionDragThreshold) {
                hand_.gesture = std::monostate{};
            }
        } else if (event.kind == platform::MouseEventKind::Up) {
            hand_.gesture = std::monostate{};
            OpenEmptyCanvasMenu(ImVec2(event.position.x, event.position.y));
        }
        return;
    }
    // Held to the same test as a press that makes a snippet: a right click
    // that closes a note or a popover is for closing it.
    if (event.kind != platform::MouseEventKind::Down || GestureInFlight() ||
        !PressMakesASnippet(event)) {
        return;
    }
    // The hand moving on from a snippet it was drawing on, as a left press
    // here would be.
    editor_.ExitDrawingMode();
    hand_.gesture = EmptyCanvasRightPress{event.position.x, event.position.y};
}

bool OverlayApp::HandleCreationGesture(const platform::MouseEvent& event) {
    if (CreationGesture* creation = GestureIf<CreationGesture>()) {
        if (event.button != platform::MouseButton::Left) {
            return false;  // the other button's business
        }
        switch (event.kind) {
            case platform::MouseEventKind::Down:
                break;
            case platform::MouseEventKind::Move:
                if (!creation->dragTo.has_value()) {
                    const float dx = event.position.x - creation->downX;
                    const float dy = event.position.y - creation->downY;
                    if (std::sqrt(dx * dx + dy * dy) <= kCreationDragThreshold) {
                        break;  // still a click, as far as anyone can tell
                    }
                }
                creation->dragTo = ImVec2(event.position.x, event.position.y);
                break;
            case platform::MouseEventKind::Up: {
                const CreationGesture gesture = *creation;
                hand_.gesture = std::monostate{};
                ItemId made = 0;
                if (gesture.dragTo.has_value()) {
                    const ImVec2 to = gesture.dragTo.value_or(ImVec2(gesture.downX, gesture.downY));
                    made = editor_.CreateRegionItem(
                        gesture.kind, Rect{std::min(gesture.downX, to.x), std::min(gesture.downY, to.y),
                                           std::abs(gesture.downX - to.x), std::abs(gesture.downY - to.y)});
                } else if (gesture.isDouble || !gesture.fromEmptyCanvas) {
                    // Fullscreen: the second press of a double-click on
                    // empty canvas, or any click with a creation tool in
                    // hand, which was picked on purpose.
                    made = editor_.CreateFullscreenItem(gesture.kind);
                } else {
                    // A plain click on empty canvas makes nothing - a
                    // fullscreen snippet is too much to make by accident -
                    // whichever kind it would have been.
                    return true;
                }
                // A drawing a press on empty canvas made is watched until
                // something goes into it - see Editor::UntouchedDrawing. One made
                // with a creation tool was asked for, and is not.
                if (gesture.fromEmptyCanvas && gesture.kind != ItemCreationKind::Screenshot && made != 0) {
                    editor_.WatchAsUntouched(made);
                }
                // A creation tool places once. A drawing has already handed
                // over to Draw (see HandOverNewItem); a screenshot hands
                // back the tool that was in hand before it. Nothing placed -
                // a drag too small to keep - leaves the tool in hand to try
                // again.
                if (!gesture.fromEmptyCanvas && made != 0 && editor_.ActiveTool() == Tool::NewScreenshot) {
                    editor_.PutDownCreationTool();
                }
                break;
            }
        }
        return true;
    }
    if (event.kind != platform::MouseEventKind::Down || event.button != platform::MouseButton::Left ||
        GestureInFlight()) {
        return false;
    }
    CreationGesture gesture;
    gesture.downX = event.position.x;
    gesture.downY = event.position.y;
    if (const std::optional<ItemCreationKind> toolKind = CreationKindFor(editor_.ActiveTool())) {
        // A creation tool in hand: the press places it wherever it lands,
        // on a snippet or not, whatever modifier is held.
        gesture.kind = *toolKind;
    } else {
        // On empty canvas the modifier held picks the kind - and a press
        // with one that picks nothing makes nothing.
        const std::optional<ItemCreationKind> kind = editor_.EmptyCanvasCreationKind();
        if (!kind.has_value() || !PressMakesASnippet(event)) {
            return false;
        }
        // The hand moving on from a snippet it was drawing on.
        editor_.ExitDrawingMode();
        gesture.kind = *kind;
        gesture.fromEmptyCanvas = true;
        gesture.isDouble = hand_.pressIsDouble;
        // Held still instead of dragged, the press makes the snippet
        // fullscreen as a double-click would - see MatureHeldPress.
        HoldPress(event, std::nullopt, gesture.kind);
    }
    hand_.gesture = gesture;
    return true;
}

void OverlayApp::KeepPlacedDrawings() {
    const ItemGesture* gesture = GestureIf<ItemGesture>();
    if (gesture == nullptr) {
        return;
    }
    std::vector<ItemId> ids;
    for (const ItemGesture::StartRect& start : gesture->startRects) {
        ids.push_back(start.item);
    }
    editor_.KeepDrawingsPlaced(ids);
}

void OverlayApp::EndDrawingGesture() {
    if (GestureIf<StrokeInFlight>() != nullptr || GestureIf<RightErase>() != nullptr) {
        EndGesture();
    }
}

std::optional<ItemCreationKind> OverlayApp::CreationInFlight() const {
    const CreationGesture* creation = GestureIf<CreationGesture>();
    return creation != nullptr ? std::optional<ItemCreationKind>(creation->kind) : std::nullopt;
}

// The Canvas level, until the levels above it take over what it hands on
// here: each event goes to the handler that took it before the machine
// was there - see docs/INTERACTIONS.md, "Phase 3, in steps".
class OverlayApp::CanvasRoot : public Interaction {
public:
    explicit CanvasRoot(OverlayApp& app) : app_(app) {}
    Level level() const override { return Level::Canvas; }
    const char* Name() const override { return "Canvas"; }
    Answer Offer(const Event& event, Editor& /*editor*/) override {
        switch (event.kind) {
            case EventKind::PointerDown:
            case EventKind::PointerMove:
            case EventKind::PointerUp:
            case EventKind::Wheel:
            case EventKind::KeyDown:
                app_.OnCanvasEvent(event);
                return Answer::Claim();
            case EventKind::KeyUp:
            case EventKind::Modifiers:
            case EventKind::Tick:
                return Answer::Claim();  // nothing waits on these here yet
            case EventKind::Hotkey:
                return Answer::Start(Command{event.command});
            case EventKind::Lifecycle:
                return Answer::Claim();  // the tray still tells the app directly
        }
        return Answer::Claim();
    }
    // The root is never ended.
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}

private:
    OverlayApp& app_;
};

void OverlayApp::InstallCanvasRoot() { editor_.Input().SetRoot(std::make_unique<CanvasRoot>(*this)); }

void OverlayApp::OnInput(const platform::InputEvent& event) {
    editor_.SetHeld(event.modifiers);
    editor_.SetNow(event.seconds);
    // The display as the last frame saw it, which is what the event's
    // position is in.
    if (ImGui::GetCurrentContext() != nullptr) {
        editor_.SetDisplaySize(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    }
    editor_.Input().Offer(Event::FromInput(event));
}

void OverlayApp::OnCanvasEvent(const Event& event) {
    using platform::MouseButton;
    using platform::MouseEventKind;
    const auto gestureButton = [](MouseButton button) {
        return button == MouseButton::Left || button == MouseButton::Right;
    };
    switch (event.kind) {
        case EventKind::PointerDown:
        case EventKind::PointerUp:
            if (gestureButton(event.button)) {
                OnMouse(platform::MouseEvent{event.position, event.button,
                                             event.kind == EventKind::PointerDown ? MouseEventKind::Down
                                                                                  : MouseEventKind::Up});
            } else if (event.kind == EventKind::PointerDown && !viewOnly_ && !GestureInFlight()) {
                // A shortcut the button may be. Not while a gesture is in
                // flight: it is on the mouse holding the gesture, and waits
                // for it as the other button does (see Hand::ignoredButton).
                HandleCommandKey(ComboKeyForMouseButton(event.button), /*repeat=*/false);
            }
            return;
        case EventKind::PointerMove:
            // A move is each held gesture button's - both, when both are
            // down - and nobody's with neither.
            for (const MouseButton button : {MouseButton::Left, MouseButton::Right}) {
                if (event.buttons & platform::ButtonBit(button)) {
                    OnMouse(platform::MouseEvent{event.position, button, MouseEventKind::Move});
                }
            }
            return;
        case EventKind::Wheel:
            if (!viewOnly_) {
                HandleMouseWheel(event.wheel);
            }
            return;
        case EventKind::KeyDown:
            if (!viewOnly_) {
                HandleCommandKey(event.key, event.repeat);
            }
            return;
        case EventKind::KeyUp:
        case EventKind::Modifiers:
        case EventKind::Tick:
        case EventKind::Hotkey:
        case EventKind::Lifecycle:
            return;  // not handed on - see CanvasRoot
    }
}

void OverlayApp::OnMouse(const platform::MouseEvent& event) {
    if (viewOnly_) {
        // Real OS-level click-through (see IOverlayWindow::SetInputPassthrough)
        // means this shouldn't even fire on Windows, but the Linux dev
        // harness has no such mechanism - guard here too so view-only mode
        // is genuinely read-only on every backend, not just the real one.
        return;
    }
    // One button at a time: the first to press owns the pointer until it
    // lets go, and the other is ignored until it lets go too - see
    // Hand::pressedButton. Middle-button events, which nothing below reads,
    // are not part of this, so a wheel click cannot lock a button out.
    if (event.button == platform::MouseButton::Left || event.button == platform::MouseButton::Right) {
        if (hand_.ignoredButton == event.button) {
            if (event.kind != platform::MouseEventKind::Down) {
                if (event.kind == platform::MouseEventKind::Up) {
                    hand_.ignoredButton.reset();
                }
                return;
            }
            // Pressed again, still ignored as far as this knows: its
            // release went missing, as the owning button's can (below).
            // Taken as that release, rather than as one more event of the
            // press it ended - which swallowed this press, and its release
            // with it.
            hand_.ignoredButton.reset();
        }
        if (event.kind == platform::MouseEventKind::Down) {
            if (hand_.pressedButton.has_value() && *hand_.pressedButton != event.button) {
                hand_.ignoredButton = event.button;
                return;
            }
            // Pressed again while it is still down, as far as this knows:
            // its release went missing - capture taken mid-drag, a focus
            // stolen - and never came. Whatever it began ends where it got
            // to, as that release would have ended it. Left in flight, it
            // took this press as its own and carried on from where it was:
            // an item jumped, a stroke drew a line from its last point.
            if (hand_.pressedButton == event.button) {
                EndGesture();
            }
            hand_.pressedButton = event.button;
        } else if (event.kind == platform::MouseEventKind::Up && hand_.pressedButton == event.button) {
            hand_.pressedButton.reset();
        }
    }
    // A press while a note is open is for closing it, whatever happens to
    // the note on the way (see PressMakesASnippet) - asked before settling
    // the untouched snippet below, which can close that note itself. And
    // whether this press is the second of a double-click, for the handlers
    // below that care.
    if (event.kind == platform::MouseEventKind::Down) {
        hand_.noteOpenAtPress = editor_.EditingNote().has_value();
        hand_.pressIsDouble = NoteDoubleClick(event);
    }
    // A press held still is remembered until it moves or lets go - see
    // Hand::heldPress. Another button's press ends it too: a hold is one
    // finger on one spot.
    if (hand_.heldPress.has_value()) {
        if (event.kind != platform::MouseEventKind::Move) {
            hand_.heldPress.reset();
        } else {
            const float dx = event.position.x - hand_.heldPress->x;
            const float dy = event.position.y - hand_.heldPress->y;
            if (std::sqrt(dx * dx + dy * dy) > kDoubleClickPx) {
                hand_.heldPress.reset();
            }
        }
    }
    // A press anywhere but on the drawing a stray click made is the hand
    // moving on from it - see Editor::UntouchedDrawing. Not a press on a panel,
    // which is as likely to be picking a color to draw in it with.
    if (const std::optional<ItemId> untouched = editor_.UntouchedDrawing();
        event.kind == platform::MouseEventKind::Down && untouched.has_value() &&
        !ImGui::GetIO().WantCaptureMouse && !PanelOpen()) {
        // A press on the selection bar is not moving on either: the bar is
        // the selection's, and the drawing may be in it - its Pin, its
        // drawing buttons, and its Close, which settles it itself.
        const PointerTarget target = editor_.ResolvePointerTarget(event.position.x, event.position.y);
        const bool onIt = (target.kind != PointerTarget::Kind::None && target.item == *untouched) ||
                          target.body == untouched || target.kind == PointerTarget::Kind::Button;
        if (!onIt) {
            editor_.SettleUntouchedDrawing();
        }
    }
    if (event.button == platform::MouseButton::Right) {
        // A right press on a snippet resizes it from its nearest edge, or
        // is a right click if it never drags - checked first. Starting
        // one is gated on !WantCaptureMouse like every selection gesture
        // (over a panel the press is the panel's); one in flight is
        // consumed regardless.
        if (HandleItemGesture(event)) {
            return;
        }
        // In drawing mode the right button on the snippet being drawn on
        // is the eraser, for quick corrections without changing the tool:
        // a drag erases along its path, and a press that never drags is a
        // right click, which leaves the mode. The erase starts only once
        // it is a drag, so a click takes nothing away.
        if (RightErase* erase = GestureIf<RightErase>()) {
            if (event.kind == platform::MouseEventKind::Move) {
                if (!erase->erasing) {
                    const float dx = event.position.x - erase->x;
                    const float dy = event.position.y - erase->y;
                    if (std::sqrt(dx * dx + dy * dy) < kSelectionDragThreshold || !editor_.DrawingItem().has_value()) {
                        return;
                    }
                    erase->erasing = true;
                    session_.BeginErase(*editor_.DrawingItem(), erase->x, erase->y, editor_.EraserWidth());
                }
                session_.ExtendErase(event.position.x, event.position.y, editor_.EraserWidth());
            } else if (event.kind == platform::MouseEventKind::Up) {
                const bool erased = erase->erasing;
                hand_.gesture = std::monostate{};
                if (erased) {
                    session_.EndErase();
                } else {
                    editor_.ExitDrawingMode();
                }
            }
            return;
        }
        if (event.kind == platform::MouseEventKind::Down && editor_.DrawingItem().has_value() && !editor_.Held().alt &&
            !ImGui::GetIO().WantCaptureMouse && !GestureInFlight()) {
            const PointerTarget target = editor_.ResolvePointerTarget(event.position.x, event.position.y);
            if (target.kind == PointerTarget::Kind::Body && target.item == *editor_.DrawingItem()) {
                hand_.gesture = RightErase{event.position.x, event.position.y, false};
                return;
            }
        }
        // A click on empty canvas opens its menu - see
        // HandleEmptyCanvasRightPress. Nothing else is the right button's.
        HandleEmptyCanvasRightPress(event);
        return;
    }
    if (event.button != platform::MouseButton::Left) {
        return;
    }
    // The selection's gestures - a press on a snippet, on a selected
    // snippet's handle, or on the selection bar - all one gesture engine:
    // see HandleItemGesture. Before the WantCaptureMouse gate below only in
    // the sense that a gesture already in flight is consumed regardless;
    // starting one is gated on it too.
    if (HandleItemGesture(event)) {
        return;
    }
    // A creation already in flight takes its moves and its release wherever
    // they land, over a panel included - only the press that starts one is
    // held to the gates below. So does a stroke.
    if (GestureIf<CreationGesture>() != nullptr && HandleCreationGesture(event)) {
        return;
    }
    if (event.kind != platform::MouseEventKind::Down) {
        // A stroke in flight takes its moves and its release wherever they
        // land - see HandleStrokeEvent, which started it on the press.
        if (GestureIf<StrokeInFlight>() != nullptr) {
            HandleStrokeEvent(event);
        }
        return;
    }
    if (ImGui::GetIO().WantCaptureMouse) {
        return;  // a real ImGui widget (a popover, the canvas bar) owns this click
    }

    if (editor_.DrawingItem().has_value() && !editor_.Held().alt) {
        // Drawing mode: a press on the snippet being drawn on draws with
        // the tool in hand; a press anywhere else - empty canvas, another
        // snippet - leaves the mode, and does nothing more, since that is
        // what the press was for. (A press on the snippet's handles or its
        // bar was HandleItemGesture's, above; with Alt held the press picks
        // the snippet up instead, there too.)
        const PointerTarget target = editor_.ResolvePointerTarget(event.position.x, event.position.y);
        if (target.kind == PointerTarget::Kind::Body && target.item == *editor_.DrawingItem()) {
            HandleStrokeEvent(event);
        } else {
            editor_.ExitDrawingMode();
            // Held still, though, the press is what a double-click here
            // would have been: drawing mode on the other snippet, or a
            // fullscreen snippet of empty canvas, of the kind the modifier
            // held picks - see MatureHeldPress. A double-click gets there
            // by leaving on its first press and arriving on its second; a
            // hold has only the one.
            if (target.kind == PointerTarget::Kind::Body) {
                HoldPress(event, target.item, std::nullopt);
            } else if (const std::optional<ItemCreationKind> kind = editor_.EmptyCanvasCreationKind();
                       kind.has_value() && PressMakesASnippet(event)) {
                HoldPress(event, std::nullopt, *kind);
            }
        }
        return;
    }

    // A press on empty canvas frames a snippet, of the kind the modifier
    // held picks, or makes one fullscreen on a double-click, and a press
    // anywhere places with a creation tool -
    // see HandleCreationGesture. A press on a snippet was
    // HandleItemGesture's already; nothing else is left for it to be.
    HandleCreationGesture(event);
}

void OverlayApp::HandleStrokeEvent(const platform::MouseEvent& event) {
    // The stroke goes into the snippet in drawing mode, which is on the
    // current canvas (see PruneSelection) - but the live layer below is
    // reached through a check rather than assumed, so "there's no canvas"
    // stays a checked condition everywhere.
    if (!editor_.DrawingItem().has_value()) {
        return;
    }
    const Canvas* canvas = Manager().CurrentOrNull();
    if (!canvas) {
        return;
    }

    // Only with nothing in flight: a key for Text pressed halfway through a
    // stroke would otherwise take that stroke's moves and its release, and
    // it stayed in flight for good - no gesture could start after it, and a
    // paint or erase session was never closed.
    if (editor_.ActiveTool() == Tool::Text && !GestureInFlight()) {
        // Not stroke-based at all (see Tool::Text's own doc comment) - a
        // press opens the snippet's noteText for editing instead of
        // starting a drag, the same way clicking a real text field just
        // focuses it.
        if (event.kind == platform::MouseEventKind::Down) {
            editor_.BeginEditingNote(*editor_.DrawingItem());
        }
        return;
    }

    const ItemId armed = *editor_.DrawingItem();

    // What a Draw or Erase press makes is decided as it starts, from the
    // modifiers held then - Ctrl for a rectangle with either tool, Shift for
    // a line with Draw (see DrawShapeFor) - and holds for the whole gesture,
    // so letting go of a key halfway through a drag changes nothing it has
    // done. The one thing that can change mid-drag is which of the two
    // shapes a shape is.
    // With no modifier held, the shape is whatever the drawing bar has cycled
    // the tool to (see ShapeForPress), so a plain drag can be a line or a
    // rectangle for a hand with no keyboard.
    using Kind = StrokeInFlight::Kind;
    if (event.kind == platform::MouseEventKind::Down) {
        if (GestureInFlight()) {
            return;  // one gesture at a time - see Hand::gesture
        }
        StrokeInFlight stroke;
        if (editor_.ActiveTool() == Tool::Erase) {
            stroke.kind = editor_.ShapeForPress() == DrawShape::Rectangle ? Kind::EraseRect : Kind::Erase;
        } else {
            stroke.shape = editor_.ShapeForPress();
            stroke.kind = stroke.shape != DrawShape::Freehand ? Kind::Shape : Kind::Freehand;
        }
        hand_.gesture = stroke;
    }
    StrokeInFlight* stroke = GestureIf<StrokeInFlight>();
    if (stroke == nullptr) {
        return;
    }
    if (event.kind != platform::MouseEventKind::Up) {
        stroke->last = event.position;
    }
    const auto sessionShape = [](DrawShape shape) {
        return shape == DrawShape::Rectangle ? Session::Shape::Rectangle : Session::Shape::Line;
    };

    switch (stroke->kind) {
        case Kind::Erase:
            // One gesture in the session: the strokes it clips,
            // snapshotted as it starts and filed as a single undo entry as
            // it ends - see Session::BeginErase.
            if (event.kind == platform::MouseEventKind::Down) {
                session_.BeginErase(armed, event.position.x, event.position.y, editor_.EraserWidth());
            } else if (event.kind == platform::MouseEventKind::Move) {
                session_.ExtendErase(event.position.x, event.position.y, editor_.EraserWidth());
            } else {
                session_.EndErase();
            }
            break;
        case Kind::EraseRect:
            // Dragged out from a fixed corner with nothing erased while it
            // is - the rect is only a preview (RenderRectEraserOverlay) -
            // then erased once, on release, as one undoable step (see
            // Session::EraseRect). A drag too short to be meant erases
            // nothing.
            if (event.kind == platform::MouseEventKind::Down) {
                stroke->rect = RectErase{event.position.x, event.position.y, event.position.x, event.position.y};
            } else if (event.kind == platform::MouseEventKind::Move) {
                stroke->rect.x1 = event.position.x;
                stroke->rect.y1 = event.position.y;
            } else {
                const RectErase rect = stroke->rect;
                const float dx = event.position.x - rect.x0;
                const float dy = event.position.y - rect.y0;
                if (std::sqrt(dx * dx + dy * dy) >= kRegionMinSize) {
                    session_.EraseRect(armed, std::min(rect.x0, event.position.x), std::min(rect.y0, event.position.y),
                                       std::max(rect.x0, event.position.x), std::max(rect.y0, event.position.y));
                }
            }
            break;
        case Kind::Shape:
            // The whole of a shape is the session's: its preview, and how
            // short a drag is a stray click - see Session::BeginShape. Pressing the other
            // modifier mid-drag turns a line into a rectangle or back;
            // letting go of both leaves it as it was.
            if (event.kind == platform::MouseEventKind::Down) {
                session_.BeginShape(armed, sessionShape(stroke->shape), event.position.x, event.position.y,
                                    editor_.DrawColorRGBA(), editor_.DrawWidth());
            } else if (event.kind == platform::MouseEventKind::Move) {
                if (editor_.Held().ctrl || editor_.Held().shift) {
                    stroke->shape = DrawShapeFor(editor_.Held().ctrl, editor_.Held().shift);
                    session_.SetShape(sessionShape(stroke->shape));
                }
                session_.UpdateShape(event.position.x, event.position.y);
            } else {
                session_.EndShape(event.position.x, event.position.y);
            }
            break;
        case Kind::Freehand:
            // Freehand: accumulated into the live layer (screen
            // space) while the stroke is in progress, then transformed into
            // native space and moved into the armed item (see
            // Session::CommitLiveStroke).
            if (event.kind == platform::MouseEventKind::Down) {
                session_.LiveLayer().Clear();  // nothing of an earlier stroke is part of this one
            }
            editor_.Pen().OnMouseEvent(event, session_.LiveLayer());
            if (event.kind == platform::MouseEventKind::Up) {
                session_.CommitLiveStroke(armed);
            }
            break;
    }
    if (event.kind == platform::MouseEventKind::Up) {
        hand_.gesture = std::monostate{};
    }
}

}  // namespace sz::ui
