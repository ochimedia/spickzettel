#pragma once

// The words the app speaks in: which tool is in the hand, what is being
// placed, what a shortcut can be bound to, which global hotkey is meant.
// Kept out of any UI header so that the session and the settings can name
// them without depending on how they are drawn - a different UI speaks
// the same words.

#include <optional>

namespace sz::core {

// What is in the hand - six tools, exactly one at a time, so what a press
// will do is always the one tool marked on the drawing bar. What a
// marking tool does is varied with a modifier held as the press starts,
// rather than by a tool of its own:
//
// - Draw: freehand; Shift for a straight line, Ctrl for a rectangle (see
//   DrawShapeFor). Draw's color and width are the pen's, whichever shape.
// - Erase: the circular eraser, its own width; Ctrl for a rectangle dragged
//   out and erased at once. Ctrl means rectangle for both on purpose.
// - Text: a press on the snippet being drawn on opens its note for typing.
//   A note is a drawing with text in it - there is no separate note kind.
// - Select: the hand at rest, what Escape and the key of the tool already
//   in hand put it back to. Snippets are objects to it, the way a drawing
//   program's selector sees them: a click selects one, a drag moves the
//   selection, and a selected snippet wears handles to resize it by. A
//   press on empty canvas clears the selection. Holding Alt is this tool
//   for as long as it is held, whatever is in hand.
// - NewDrawing, NewScreenshot: the next left press, on a snippet or not,
//   places a drawing or a screenshot - a click for the whole screen, a drag
//   for an area. Each places once: a drawing hands over to Draw, to be drawn
//   in, and a screenshot hands back whatever tool was in hand before it.
//   Tools rather than a creation "armed" on top of the tool in hand, so the
//   pen and "new drawing" can never both be lit at once.
//
// With any tool a drag on empty canvas frames a snippet, and a double-click
// or a hold makes one fullscreen; a plain click makes nothing (see
// OverlayApp::HandleCreationGesture). Nothing persists these values: a
// shortcut is stored by name (see ShortcutActionKey), and nothing else
// stores a tool.
enum class Tool { Draw, Erase, Text, Select, NewDrawing, NewScreenshot };

// Which shape a Draw or Erase press makes, from the modifiers held as it
// starts. Ctrl wins when both are held: it is the one both tools share.
enum class DrawShape { Freehand, Line, Rectangle };
constexpr DrawShape DrawShapeFor(bool ctrl, bool shift) {
    return ctrl ? DrawShape::Rectangle : shift ? DrawShape::Line : DrawShape::Freehand;
}
constexpr bool ErasesRectangle(bool ctrl) { return ctrl; }

// What a creation gesture is placing: a screen capture or a blank drawing.
// A choice of what to make next, not something the Item remembers (see
// Item::hasBackground). A note is a drawing with text typed into it (see
// Item::noteText) rather than a kind of its own.
enum class ItemCreationKind { Screenshot, Drawing };
// What a tool places, for the two that place something - nothing for the
// rest.
constexpr std::optional<ItemCreationKind> CreationKindFor(Tool tool) {
    switch (tool) {
        case Tool::NewDrawing:
            return ItemCreationKind::Drawing;
        case Tool::NewScreenshot:
            return ItemCreationKind::Screenshot;
        case Tool::Draw:
        case Tool::Erase:
        case Tool::Text:
        case Tool::Select:
            break;
    }
    return std::nullopt;
}
// Things that happen at once rather than being a tool - nothing to place,
// each with a shortcut of its own. NewCanvasWithSelection makes a canvas
// and takes the selected snippets to it, which with nothing selected is
// exactly NewCanvas; the two are separate actions rather than one that
// reads the selection, so that the canvas bar's own "+" keeps meaning
// only what its icon says.
enum class CreateAction { NewCanvas, NewCanvasWithSelection };

// One of the buttons on the selection bar - the small pill that floats
// over the selected snippets (see OverlayApp::PaintSelectionBar). Five of
// them act on the selection: Close deletes it, Pin pins or unpins it,
// Minimize sends it to the dock; Maximize and More take the snippet
// selected last. The other four are the *drawing* bar, which the pill
// shows instead while a snippet is in drawing mode (see
// Editor::DrawingItem): the marking tool to draw with, and the
// color.
//
// Which of them each bar carries, in which order, and which are shown at
// all is a setting - see core/config/bar_layout.h, which is why this is
// down here with the other words the settings have to be able to name
// rather than in the UI header that draws them.
enum class ChromeButton { Close, Maximize, Minimize, More, Pin, Pen, Eraser, Text, Color };

// What the clipboard does with the selected snippets. Copy and Cut put
// the selection on it; Paste puts what is on it onto the canvas being
// looked at. The clipboard holds ids rather than snippets, so a Cut takes
// nothing away until the Paste that moves it - see
// OverlayApp::PasteFromClipboard.
//
// Duplicate is the copy and the paste in one step, and is here because
// that is where a hand goes looking for it. It deliberately leaves the
// clipboard itself untouched: duplicating something is not a reason to
// lose what was copied ten minutes ago.
enum class ClipboardAction { Copy, Cut, Paste, Duplicate };

// Identifies one of the global hotkeys - kept distinct from any index into
// AppConfig itself, so that nothing naming one depends on how the config
// stores them.
enum class HotkeySlot { EditMode, ViewMode, QuickCapture, SilentCapture };

}  // namespace sz::core
