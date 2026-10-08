#pragma once

// The app's own popups - docs/VIEW_LAYER.md, sections 4 and 7: the snippet
// menu, the canvas tile menu, empty canvas's menu, Properties, the color
// chooser, the pen's or the eraser's shapes, the delete confirmation and
// the library size reminder. The machine's Popup level says
// which is up, one at a time, and opening one ends the one that was there;
// this keeps one record of it, set when it is asked for and let go of when
// its closing is done, and the queue of what only a frame can do to ImGui's
// popups.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/session/session.h"
#include "core/session/settings.h"
#include "ui/context_menu.h"
#include "ui/editor.h"
#include "ui/interaction/levels.h"
#include "ui/view/view_host.h"

namespace sz::ui {

// The context menus' ImGui popup ids - see ContextMenu.
inline constexpr const char* kItemContextMenuId = "##item_context_menu";
inline constexpr const char* kCanvasContextMenuId = "##canvas_context_menu";
inline constexpr const char* kEmptyCanvasMenuId = "##empty_canvas_menu";
inline constexpr const char* kShapeMenuId = "##shape_menu";
// The rest of the popups, by the ids their draws begin them with.
inline constexpr const char* kItemPropertiesPopupId = "##item_properties_popover";
inline constexpr const char* kColorChooserPopupId = "##color_chooser";
inline constexpr const char* kConfirmDeletePopupId = "##confirm_delete_popover";
inline constexpr const char* kConfirmReassignPopupId = "##confirm_reassign_popover";
inline constexpr const char* kLibraryReminderPopupId = "##library_reminder_popover";
// A popup's ImGui id, as its draw begins it.
const char* PopupId(PopupKind kind);

class Popups {
public:
    Popups(core::Session& session, core::Settings& settings, Editor& editor, ViewHost& host);

    // ===== Asked for =====
    //
    // Each is put on the machine's Popup level first (ending the one that
    // was there, whose closing is done then), recorded, and opened at the
    // next frame's Open stage (see ApplyEffects): what asks for one - a
    // right click, a bar button - arrives from the input stream, before the
    // frame, or from inside another window's draw.

    // The snippet menu, over `item`, at `at`.
    void OpenItemMenu(core::ItemId item, ImVec2 at);
    // A canvas bar tile's menu, for `canvas`. Rendered at the top level of
    // the frame rather than inside the bar's own window, like every other
    // popup here; the bar is held out for as long as the menu is up (see
    // CanvasBar), since a menu floating over the panel it belongs to having
    // slid away would be a puzzle.
    void OpenCanvasMenu(core::CanvasId canvas, ImVec2 at);
    // Empty canvas's menu, at `at`.
    void OpenEmptyCanvasMenu(ImVec2 at);
    // Properties, for `item`, by `at` - or, asked from nowhere in
    // particular, by the selection bar's More button.
    void OpenItemProperties(core::ItemId item, std::optional<platform::Vec2> at);
    // The color chooser, beside `from`, the point it was asked from.
    void OpenColorChooser(ImVec2 from);
    // The shapes of `tool` - the pen's or the eraser's - at `at`.
    void OpenShapeMenu(core::Tool tool, ImVec2 at);
    // The delete confirmation, for `target`.
    void OpenConfirmDelete(DeleteTarget target);
    // The key confirmation, for `request`.
    void OpenConfirmReassign(KeyReassign request);
    // The reminder that the library holds `bytes`, past the size Settings
    // reminds at (see AppConfig::librarySizeReminder).
    void OpenLibraryReminder(int64_t bytes);

    // ===== The machine's side (see Popup) =====

    // Whether the popup of `kind` is up - asked for, and not yet closed,
    // including the frames before one draws it.
    bool Up(PopupKind kind) const { return popup_.has_value() && popup_->kind == kind; }
    // What the snippet menu or Properties is about, and the canvas tile
    // menu's canvas - nothing while that one is not up.
    std::optional<core::ItemId> ItemOf(PopupKind kind) const {
        return Up(kind) ? std::optional<core::ItemId>(popup_->item) : std::nullopt;
    }
    std::optional<core::CanvasId> CanvasOf(PopupKind kind) const {
        return Up(kind) ? std::optional<core::CanvasId>(popup_->canvas) : std::nullopt;
    }
    // Ended from outside - by the machine, as a command's scope or view-only
    // mode ends it (see Popup::Interrupt): its closing is done at once,
    // since the frame that would notice may never come - the overlay put
    // away, the app exiting. ImGui's half waits for the next frame, where
    // there is one.
    void Close(PopupKind kind);
    // The innermost popup open closed, as Escape does to it - which may be
    // one of ImGui's own inside it.
    void CloseInnermost();
    // ImGui's active widget let go of, and a drag and drop dropped - see
    // Widget.
    void LetGoOfWidget();
    // The pen's color kept if the chooser is up: the overlay settling with
    // it still up - put away, which leaves a popup up for the next showing,
    // and perhaps never shown again - keeps what it was left on now.
    void KeepChooserColor();

    // ===== The frame =====

    // Stage 3, Open: the queue, in the order asked - a popup opened
    // replaces the one open before it, so the one asked for last is the one
    // that stays up. Not at the very start of the frame: opened there, the
    // canvas bar's menu was closed again before it was drawn - ImGui wants a
    // popup opened after the frame's other windows, shortly before it is
    // begun.
    void ApplyEffects();
    // What was asked of ImGui and not done yet let go of, for a frame that
    // will not come in edit mode - see OverlayApp::SetMode.
    void ForgetEffects() { effects_.clear(); }
    // Stage 4: the five popups over the canvas.
    void DrawOverCanvas(float displayW, float displayH);
    // Stage 6: the delete confirmation and the library size reminder, over
    // the panels.
    void DrawConfirmDelete();
    void DrawConfirmReassign();
    void DrawLibraryReminder();

private:
    const core::CanvasManager& Manager() const { return session_.Manager(); }
    const core::AppConfig& Cfg() const { return settings_.Stored(); }

    // The popup that is up: its kind, what it is about - the snippet
    // menu's and Properties' snippet, the canvas tile menu's canvas, the
    // delete confirmation's target - where it opens, and whether a frame
    // has drawn it, which tells "closed by itself" from "asked for and not
    // opened yet" when a frame finds ImGui without it.
    struct PopupRecord {
        PopupKind kind = PopupKind::ItemMenu;
        core::ItemId item = 0;
        core::CanvasId canvas = 0;
        std::optional<DeleteTarget> deleteTarget;
        std::optional<KeyReassign> reassign;
        core::Tool tool = core::Tool::Draw;  // the shape menu's
        int64_t bytes = 0;                   // the library reminder's
        ImVec2 at{0.0f, 0.0f};
        bool drawn = false;
    };
    std::optional<PopupRecord> popup_;
    void Open(PopupRecord popup);
    // What closing a popup of `kind` does, done once however it closes -
    // by itself, or ended from outside (see Close) - and the record let go
    // of. Nothing unless it is the popup up. The snippet menu, the canvas
    // tile menu and the delete confirmation forget what they were about,
    // which letting go of the record does; Properties ends the style edit;
    // the color chooser keeps the pen's color.
    void Closed(PopupKind kind);
    // What each popup's draw says of it, every frame: whether ImGui drew it.
    // A popup the record says was drawn and ImGui no longer has closed by
    // itself - a row chosen, a click outside, Escape - and its closing is
    // done here.
    void Drawn(PopupKind kind, bool drawn);
    // The pen's color, kept as AppConfig::strokeColorRGBA: what the chooser
    // was left on.
    void KeepPenColor();

    // Properties: the two opacities, the background color, and the text's
    // size and color, for the snippet it is open on. What is done *to* a
    // snippet - fullscreen, copy, restack, move - is the snippet menu's, not
    // Properties'. Each widget previews its change through the session (see
    // Session::PreviewStyle), and the edit ends as the hand lets go of it.
    void RenderItemPropertiesPopover();
    // What Properties shows of its snippet, taken once a frame and not read
    // from the snippet again: any widget may ask the session for a preview,
    // and a preview can end the edit open before it, whose write failing
    // puts the library back as it was - which moves every snippet in it.
    struct PopoverItem {
        core::ItemId id = 0;
        core::ItemStyle style;
        bool pictureStored = false;
        bool hasNote = false;
    };
    // Previews `style` on it, and keeps it as what the rest of the frame
    // shows.
    void PreviewPopoverStyle(PopoverItem& item, const core::ItemStyle& style);
    void RenderItemOpacity(PopoverItem& item);
    void RenderItemBackgroundColor(PopoverItem& item);
    void RenderItemTextStyle(PopoverItem& item);
    // The snippet menu: Properties' actions as a list of named rows with
    // their shortcuts beside them, plus the ones that only have a key. The
    // rows are rebuilt every frame it is up, so "nothing to clear" and
    // "nothing behind it" are answered from the canvas as it is now rather
    // than as it was when the menu opened.
    void RenderItemContextMenu();
    void BuildItemContextMenuRows(const core::Item& item, std::vector<ContextMenuEntry>& rows);
    void RenderCanvasContextMenu();
    void BuildCanvasContextMenuRows(const core::Canvas& canvas, std::vector<ContextMenuEntry>& rows) const;
    // Empty canvas's: the ways to make a snippet, Paste, and the way to the
    // Overview and Settings - what the canvas itself offers, with no snippet
    // to act on.
    void RenderEmptyCanvasMenu();
    void BuildEmptyCanvasMenuRows(std::vector<ContextMenuEntry>& rows) const;
    // "Ctrl+D" for a command a key reaches, empty for one none does - what
    // a menu row shows on its right. FormatKeyComboLabel's "(none)" is the
    // right answer for a key editor and the wrong one here, where a row
    // without a key should simply show nothing.
    std::string MenuShortcutLabel(CommandId id) const;
    // A context menu row for `command`: grayed out when it is not
    // available (see Editor::Available), with its key beside it.
    ContextMenuEntry MenuRow(const Command& command, const char* id, const Icon* icon, const char* label,
                             bool separatorAbove = false) const;
    // The color chooser: one picker, and what it is set to is the color
    // drawn with - changed as it is dragged.
    void RenderColorChooser(float displayW, float displayH);
    // The shapes of the pen or the eraser, each a row that puts that tool
    // in hand drawing it, the one in hand marked, and beside a shape the
    // modifier that draws it for one drag. A pick stays until another.
    void RenderShapeMenu();
    void BuildShapeMenuRows(core::Tool tool, std::vector<ContextMenuEntry>& rows) const;
    // Cancel or Delete, for a canvas or a folder, or what is deleted in a
    // folder. A snippet asks nothing: its delete is undoable instead (see
    // Session::DeleteItem), and a canvas takes every snippet on it along.
    void RenderConfirmDeletePopover();
    // Cancel or Reassign, for a key a Settings row took that something
    // else holds, naming each thing it unbinds.
    void RenderConfirmReassignPopover();
    void RenderLibraryReminderPopover();

    // Something only a frame can do to ImGui's popups, asked for from
    // wherever - between frames included - and done in the next frame's
    // Open stage. ImGui::OpenPopup scopes its id against the window being
    // drawn, and between frames there is none - a bar button fires from
    // the input stream, before the frame's NewFrame, and OpenPopup there
    // dereferenced an empty id stack (a verified crash, not a hypothetical
    // one). Asked for inside a frame, it waits too, which keeps every
    // popup's id at the top level of the frame whatever nesting it was
    // asked from. See docs/INTERACTIONS.md, section 3.
    struct Effect {
        enum class Kind {
            // The popup the record holds opened, if it is still that one.
            OpenPopup,
            // A popup of the machine's closed from outside (see
            // Popup::Interrupt), and the innermost popup open closed, as
            // Escape does to it - which may be one of ImGui's own inside it.
            ClosePopup,
            CloseInnermostPopup,
            // ImGui's active widget let go of, and a drag and drop dropped -
            // see Widget.
            LetGoOfWidget,
        };
        Kind kind = Kind::OpenPopup;
        PopupKind popup = PopupKind::ItemMenu;  // which, to open or close
    };
    // Asked twice before a frame, done once - as asked the second time, and
    // in its place: a popup closed and asked for again is up.
    void Queue(const Effect& effect);
    std::vector<Effect> effects_;

    core::Session& session_;
    core::Settings& settings_;
    Editor& editor_;
    ViewHost& host_;

    // The four context menus - drawn from the record, which says whether
    // one is up and over what.
    ContextMenu itemContextMenu_{kItemContextMenuId};
    ContextMenu canvasContextMenu_{kCanvasContextMenuId};
    ContextMenu emptyCanvasMenu_{kEmptyCanvasMenuId};
    ContextMenu shapeMenu_{kShapeMenuId};
};

}  // namespace sz::ui
