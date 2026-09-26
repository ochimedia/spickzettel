#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <ctime>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ui/icon_draw.h"
#include "ui/icons_generated.h"
#include "core/canvas/item_geometry.h"
#include "core/drawing/stroke_mesh.h"
#include "platform/pen_glyph.h"

#include <imgui.h>
// For BringWindowToDisplayFront, FindWindowByName and the open-popup stack
// - see StackSurfaces, which sets the order of every window the overlay
// draws, since neither the order they are drawn in nor ImGui's own focus
// history says it. Pinned to a specific ImGui commit (see
// cmake/FetchImGui.cmake), so relying on this internal header is a
// deliberate, contained choice rather than an accident.
#include <imgui_internal.h>

namespace sz::ui {

using namespace overlay_detail;

namespace {

// How long the mouse wheel's own size preview stays up after the last
// step, and how much of that tail it spends fading. Long enough to read
// the number and judge the dot without turning into something that sits
// on screen after you've moved on - see RenderBrushSizePreview.
constexpr double kSizePreviewHoldSeconds = 1.1;
constexpr double kSizePreviewFadeSeconds = 0.35;

// A border (drawn with real content, so it can't be color-keyed away like
// the background) plus a status line of live state. Useful for confirming
// the overlay is actually rendering and receiving input, e.g. after moving
// to a new machine or display setup.
void DrawDebugOverlay(ImDrawList* drawList, const ImGuiIO& io, const CanvasManager& canvases,
                       const std::string& hoveredResizeHandle) {
    drawList->AddRect(ImVec2(4, 4), ImVec2(io.DisplaySize.x - 4, io.DisplaySize.y - 4), IM_COL32(0, 255, 255, 255),
                       0.0f, 3.0f, ImDrawFlags_None);
    char debugLine[200];
    std::snprintf(debugLine, sizeof(debugLine),
                   "Spickzettel overlay active | canvas=%s | items=%zu | mouse=(%.0f,%.0f)",
                   canvases.CurrentOrNull() ? canvases.CurrentOrNull()->name.c_str() : strings::kHotkeyNone,
                   canvases.CurrentOrNull() ? canvases.CurrentOrNull()->items.size() : size_t{0},
                   io.MousePos.x, io.MousePos.y);
    drawList->AddText(Px(16.0f, 16.0f), IM_COL32(0, 255, 255, 255), debugLine);
    // A live readout of exactly which resize handle (if any) the mouse is
    // over right now - see debugHoveredResizeHandle_'s own doc comment for
    // why this exists: a screenshot alone can't tell "covered by a handle
    // that's visually identical to its neighbor" apart from "not covered
    // by anything," this can.
    char handleLine[96];
    std::snprintf(handleLine, sizeof(handleLine), "resize handle: %s",
                   hoveredResizeHandle.empty() ? "none" : hoveredResizeHandle.c_str());
    drawList->AddText(ImVec2(Px(16.0f), Px(16.0f) + ImGui::GetTextLineHeight()), IM_COL32(0, 255, 255, 255),
                       handleLine);
}

}  // namespace

namespace overlay_detail {

const GalleryTool kGalleryTools[6] = {
    {Tool::Draw, &icons::kPen, strings::kToolDraw, strings::kToolDrawTip},
    {Tool::Erase, &icons::kEraser, strings::kToolErase, strings::kToolEraseTip},
    {Tool::Text, &icons::kType, strings::kToolText, strings::kToolTextTip},
    {Tool::Select, &icons::kSelect, strings::kToolSelect, strings::kToolSelectTip},
    {Tool::NewScreenshot, &icons::kCamera, strings::kToolNewScreenshot, strings::kToolNewScreenshotTip},
    {Tool::NewDrawing, &icons::kNote, strings::kToolNewDrawing, strings::kToolNewDrawingTip},
};

const CreateActionInfo kCreateActions[2] = {
    {CreateAction::NewCanvas, &icons::kPlus, strings::kCreateNewCanvas, strings::kCreateNewCanvas},
    {CreateAction::NewCanvasWithSelection, &icons::kPlus, strings::kCreateNewCanvasWithSelection,
     strings::kCreateNewCanvasWithSelection},
};

const ClipboardActionInfo kClipboardActions[4] = {
    {ClipboardAction::Copy, &icons::kCopy, strings::kClipboardCopy},
    {ClipboardAction::Cut, &icons::kScissors, strings::kClipboardCut},
    {ClipboardAction::Paste, &icons::kClipboard, strings::kClipboardPaste},
    {ClipboardAction::Duplicate, &icons::kCopy, strings::kClipboardDuplicate},
};

ShortcutAction ShortcutForTool(Tool tool) {
    switch (tool) {
        case Tool::Draw:
            return ShortcutAction::Draw;
        case Tool::Erase:
            return ShortcutAction::Erase;
        case Tool::Text:
            return ShortcutAction::Text;
        case Tool::Select:
            return ShortcutAction::Select;
        case Tool::NewScreenshot:
            return ShortcutAction::NewScreenshot;
        case Tool::NewDrawing:
            return ShortcutAction::NewDrawing;
    }
    return ShortcutAction::Draw;  // unreachable: the switch names every tool
}

ShortcutAction ShortcutForCreateAction(CreateAction action) {
    switch (action) {
        case CreateAction::NewCanvas:
            return ShortcutAction::NewCanvas;
        case CreateAction::NewCanvasWithSelection:
            return ShortcutAction::NewCanvasWithSelection;
    }
    return ShortcutAction::NewCanvas;  // unreachable: the switch names every action
}

ShortcutAction ShortcutForClipboardAction(ClipboardAction action) {
    switch (action) {
        case ClipboardAction::Copy:
            return ShortcutAction::Copy;
        case ClipboardAction::Cut:
            return ShortcutAction::Cut;
        case ClipboardAction::Paste:
            return ShortcutAction::Paste;
        case ClipboardAction::Duplicate:
            return ShortcutAction::Duplicate;
    }
    return ShortcutAction::Copy;  // unreachable: the switch names every action
}


}  // namespace overlay_detail

OverlayApp::OverlayApp(Settings& settings, Session& session)
    : editor_(settings, session), session_(session), settings_(settings) {
    editor_.SetViews(this);
}

void OverlayApp::AttachTo(platform::IOverlayWindow& window) {
    window_ = &window;
    editor_.AttachWindow(&window);
    window.SetFrameCallback([this](float dt) { OnFrame(dt); });
    window.SetInputCallback([this](const platform::InputEvent& ev) { OnInput(ev); });
}

void OverlayApp::SetMode(OverlayMode mode) {
    const bool wasViewOnly = IsViewOnly();
    mode_ = mode;
    noticeFinishedReported_ = false;  // a notice entered now is reported when its own message fades
    if (IsViewOnly() == wasViewOnly) {
        return;
    }
    OfferLifecycle(IsViewOnly() ? Lifecycle::ViewOnly : Lifecycle::EditMode);
    // Every mode but Edit, the pinned view and a notice included. Those two
    // are the overlay put away, as hidden is, which keeps what edit mode
    // left up - but unlike hidden they draw frames, and ImGui closes a
    // popup that a frame does not draw: a focused window that was not
    // active in the last frame loses focus at the next NewFrame, and losing
    // it closes the popups over it. Ended here, everything ends together,
    // rather than the popup alone behind the machine's back. See
    // docs/OVERLAY_STATES.md, section 10.
    if (IsViewOnly()) {
        // Nothing should stay "in progress" while merely viewing, so the
        // All scope ends everything above the canvas, top down: the
        // gesture, while drawing mode still says which snippet a stroke in
        // flight belongs to; a note being typed, whose editor is not drawn
        // in view-only mode and so would never hear that it closed; a
        // popup, a panel; drawing mode, or the creation tool in hand. Edit
        // mode starts clean later rather than resuming whatever happened
        // to be up. The effects first: a popup ended asks for itself to be
        // closed once edit mode draws again (see Popup::Interrupt).
        effects_.clear();
        editor_.Settle(Scope::All);
        editor_.SettleUntouchedDrawing();
        // A slider or swatch in the middle of a drag is not drawn again to
        // say it was let go of, which is where its preview is committed:
        // what it was dragged to is committed here instead. The pen's width
        // too, whose preview view-only mode does not draw; its color was
        // kept as the chooser, if it was up, was ended.
        settings_.CommitPreviews();
        KeepPen();
        // Normally cleared at the top of every RenderItems call - which
        // view-only mode never runs, so without this the debug overlay's
        // "resize handle:" line would keep showing whatever handle
        // happened to be hovered on the last edit-mode frame for the
        // whole view-only session.
        debugHoveredResizeHandle_.clear();
    }
}

std::vector<platform::DisplayInfo> OverlayApp::ListDisplays() {
    return displayListCallback_ ? displayListCallback_() : std::vector<platform::DisplayInfo>{};
}

bool OverlayApp::ChangeHotkey(HotkeySlot slot, platform::KeyCombo combo) {
    // Offered even when unchanged: the combo it already has may be one
    // that never registered, and picking it again is how to try again. The
    // callback stores it once it is registered; with nothing to register
    // with, it is stored here.
    if (hotkeyChangeCallback_) {
        return hotkeyChangeCallback_(slot, combo);
    }
    return settings_.Set(HotkeySetting(slot), combo);
}

void OverlayApp::OfferLifecycle(Lifecycle which) {
    Event event;
    event.kind = EventKind::Lifecycle;
    event.lifecycle = which;
    event.modifiers = editor_.Held();
    editor_.Input().Offer(event);
}

void OverlayApp::SettleForPersistence(Lifecycle why) {
    // Put away, what a command's Hand scope ends: the gesture, kept, and a
    // note being typed, committed - drawing mode, a panel and a popup are
    // still up at the next showing, as they were left. Ending for good,
    // everything above the canvas.
    OfferLifecycle(why);
    editor_.Settle(why == Lifecycle::SessionEnding ? Scope::All : Scope::Hand);
    // Going away is moving on too, as the next showing would say (see
    // OnOverlayShown) - but exit has no next showing, and a restart loads
    // the drawing as an ordinary snippet: a fullscreen empty one, over the
    // canvas.
    editor_.SettleUntouchedDrawing();
    // A drag put away with the overlay is not drawn again before the next
    // showing, and not at all before an exit: committed now, as its end
    // would have (see SetMode). And the pen as the hand left it: its width
    // before its preview has faded, its color with the chooser up.
    settings_.CommitPreviews();
    KeepPen();
}

bool OverlayApp::PointerOverView() const {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
}

bool OverlayApp::PopupOpen() const {
    return ImGui::GetCurrentContext() != nullptr && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
}

// ================= Frame =================

// Brackets one frame's use of the two stroke-mesh caches. A struct rather
// than a pair of calls because OnFrame has an early return in it (view-only
// mode), and a cache left thinking its frame is still running never drops
// what that frame didn't draw.
struct OverlayApp::MeshCacheFrame {
    StrokeMeshCache& canvas;
    StrokeMeshCache& preview;
    MeshCacheFrame(StrokeMeshCache& canvasCache, StrokeMeshCache& previewCache)
        : canvas(canvasCache), preview(previewCache) {
        canvas.BeginFrame();
        preview.BeginFrame();
    }
    ~MeshCacheFrame() {
        canvas.EndFrame();
        preview.EndFrame();
    }
    MeshCacheFrame(const MeshCacheFrame&) = delete;
    MeshCacheFrame& operator=(const MeshCacheFrame&) = delete;
};

void OverlayApp::ToolSized(bool pen) {
    // Kept as AppConfig::strokeWidth once the preview has faded, so a burst
    // of notches is one write (see RenderBrushSizePreview).
    if (pen) {
        drawWidthDirty_ = true;
    }
    sizePreviewExpireAtSeconds_ = ImGui::GetTime() + kSizePreviewHoldSeconds;
}

void OverlayApp::OnFrame(float /*deltaSeconds*/) {
    // Function scope, so every path out of here - including view-only mode's
    // own early return - closes the frame out.
    const MeshCacheFrame meshCacheFrame(strokeMeshCache_, previewMeshCache_);
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    Prepare(display.x, display.y);
    if (IsViewOnly()) {
        // A notice is this same click-through mode with the canvas left
        // out: nothing of the library on screen, only the message. See
        // OverlayMode::Notice.
        if (!IsNoticeOnly()) {
            RenderViewOnly(display.x, display.y);
        }
        DrawMessages();
        Apply();
        return;
    }
    DrawCanvas(display.x, display.y);
    // 3. Open: what was asked for that only a frame can do - see Effect -
    // just before the popups it opens are drawn.
    ApplyEffects();
    DrawPopups(display.x, display.y);
    DrawOverCanvas(display.x, display.y);
    DrawPanels(display.x, display.y);
    DrawMessages();
    StackSurfaces();
    DrawPointer();
    Apply();
}

namespace {
// Brings `window` to the front, if this frame drew it, and then every popup
// ImGui has open inside it - a dropdown, a help popover, a color picker -
// each followed by any open inside it in turn.
void Front(ImGuiWindow* window) {
    if (window == nullptr || !window->Active) {
        return;
    }
    ImGui::BringWindowToDisplayFront(window);
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    for (const ImGuiPopupData& popup : g.OpenPopupStack) {
        const ImGuiWindow* parent = popup.Window != nullptr ? popup.Window->ParentWindow : nullptr;
        if (parent != nullptr && popup.Window != window && parent->RootWindow == window) {
            Front(popup.Window);
        }
    }
}

void Front(const char* name) { Front(ImGui::FindWindowByName(name)); }

// The window of one of the app's popups, opened at the top level of a
// frame as all of them are, while ImGui has it open. The stack is set at
// the top level too, so the id hashes the same way here.
ImGuiWindow* AppPopupWindow(PopupKind kind) {
    const ImGuiID id = ImGui::GetID(PopupId(kind));
    for (const ImGuiPopupData& popup : ImGui::GetCurrentContext()->OpenPopupStack) {
        if (popup.PopupId == id) {
            return popup.Window;
        }
    }
    return nullptr;
}
}  // namespace

// Section 3 of docs/VIEW_LAYER.md, bottom to top: each surface brought to
// the front in turn, once everything is drawn. A window's place is read
// only when the frame is rendered and when the next frame hit-tests, so
// one pass here decides both. Before, each surface brought itself to the
// front as it was drawn, and whichever call ran last in the frame won: a
// popup inside the Overview had to come after the panel's own call, or it
// opened behind it - a dropdown that could not be clicked, a color picker
// that flashed up and vanished. The order is today's, including the
// border and the demo mark over the popups on the canvas (finding 3).
void OverlayApp::StackSurfaces() {
    Front("##spickzettel_canvas");
    Front("##sz_items_layer");
    if (const std::optional<ItemId> note = editor_.EditingNote()) {
        char noteWinName[40];
        std::snprintf(noteWinName, sizeof(noteWinName), "##noteedit%llu", static_cast<unsigned long long>(*note));
        Front(noteWinName);
    }
    Front("##dock");
    Front("##canvas_bar");
    // The popups over the canvas, one up at a time.
    for (const PopupKind kind : {PopupKind::ItemProperties, PopupKind::ItemMenu, PopupKind::CanvasMenu,
                                 PopupKind::EmptyCanvasMenu, PopupKind::ColorChooser}) {
        Front(AppPopupWindow(kind));
    }
    Front("##sz_input_hud_layer");
    Front("##sz_chrome_layer");
    Front("##overview_backdrop");
    Front("##overview_panel");
    Front("##cheat_sheet_backdrop");
    Front("##cheat_sheet_panel");
    Front(AppPopupWindow(PopupKind::ConfirmDelete));
}

void OverlayApp::Prepare(float displayW, float displayH) {
    // The display the canvas is on, for the editor - which draws nothing
    // and so has no other way to know it.
    editor_.SetDisplaySize(displayW, displayH);

    // The interface scale, before anything is drawn, so a whole frame is
    // drawn at one scale. The setting, or Windows' own for the display the
    // overlay is on - asked every frame, so the overlay follows a change
    // made in Windows while it is up.
    {
        const int percent = Cfg().uiScalePercent != 0 ? Cfg().uiScalePercent
                                                        : (window_ != nullptr ? window_->ScalePercent() : 100);
        if (!styleApplied_ || percent != appliedUiScalePercent_) {
            SetUiScale(static_cast<float>(percent) / 100.0f);
            theme::ApplyStyle(UiScale());
            appliedUiScalePercent_ = percent;
        }
    }
    // A new note's text size, decided once: the default at Windows' scale
    // for the display the overlay first came up on - see
    // AppConfig::noteTextSizePx. Windows' rather than the interface's,
    // which is the same thing unless someone has already set the other.
    if (Cfg().noteTextSizePx <= 0.0f && window_ != nullptr) {
        const float scale = static_cast<float>(window_->ScalePercent()) / 100.0f;
        settings_.Set(setting::kNoteTextSize, std::round(kDefaultNoteTextSizePx * scale));
    }
    if (!styleApplied_) {
        // No imgui.ini. ImGui writes one next to the working directory to
        // remember window positions and sizes, and this app has nothing to
        // remember: every window it opens - the canvas layer, each item's
        // chrome, the popovers, the Overview, the dock - sets its own position
        // and size explicitly on every frame, from the data model. Nothing
        // ever reads a saved position back, so the file was purely clutter
        // dropped wherever the exe happened to be launched from. Turned off
        // here rather than pointed at the config directory, because a file
        // nobody reads isn't worth relocating.
        //
        // Set from core, on the first frame, so both platform backends get
        // it from one place - and safely, since ImGui only writes the file
        // some seconds after a settings change, never during startup. The
        // ImGuiWindowFlags_NoSavedSettings already on several windows is
        // now redundant but harmless, and still documents the intent.
        ImGui::GetIO().IniFilename = nullptr;
        styleApplied_ = true;
    }
    // The accent, whenever the setting differs from what was last applied -
    // on every frame while a color is being dragged in Settings, so the
    // whole overlay recolors as it moves. The theme's accessors and the
    // ImGui style both carry it.
    if (appliedAccentRGBA_ != Cfg().accentColorRGBA) {
        theme::SetAccent(Cfg().accentColorRGBA);
        theme::ApplyAccentToStyle(ImGui::GetStyle());
        appliedAccentRGBA_ = Cfg().accentColorRGBA;
    }
    // Every display refresh, or only now and then - see
    // IOverlayWindow::SetFramePacing. Decided at the start of a frame from
    // state that changes between frames (a hotkey, a mode switch), so the
    // frame that follows a change already runs at the new pace.
    //
    // View-only - the pinned view included - shows a picture that doesn't
    // change by itself, so it is idle unless something on it moves: a
    // message fading, which is all a notice is. What it shows otherwise
    // changes only through something that arrives as a message, like a
    // capture hotkey, and a message always gets a frame. The debug overlay
    // is the exception, since it follows the pointer.
    {
        const bool toastShowing = !actionToastText_.empty() && ImGui::GetTime() < actionToastExpireAtSeconds_;
        const platform::FramePacing pacing = IsViewOnly() && !IsNoticeOnly() && !toastShowing && !Cfg().showDebugOverlay
                                                 ? platform::FramePacing::Idle
                                                 : platform::FramePacing::EveryFrame;
        if (window_ != nullptr && appliedFramePacing_ != pacing) {
            window_->SetFramePacing(pacing);
            appliedFramePacing_ = pacing;
        }
    }
    // Before anything draws: what the last frame drew and this one has not
    // asked for yet is let go of (see TextureCache::BeginFrame), and the
    // current canvas's pictures - from the library, the first time - are
    // asked for, which is what keeps them.
    Textures().BeginFrame();
    KeepCurrentCanvasTextures();

    // Live, not just at startup - see
    // CanvasManager::SyncItemsToDisplaySize's own doc comment. Cheap: a
    // no-op comparison per item whenever the display hasn't changed since
    // last frame, which is every frame but the one right after an actual
    // change.
    session_.SyncItemsToDisplaySize(displayW, displayH);

    // First run, first frame that knows how big the screen is - see
    // RequestWelcomeNote for why this waits rather than happening at
    // startup.
    if (welcomeNotePending_ && displayW > 0.0f && displayH > 0.0f) {
        welcomeNotePending_ = false;
        PlaceWelcomeNotes(displayW, displayH);
    }

    if (IsViewOnly()) {
        return;
    }

    // A drawing a stray click made goes once the hand has moved on from it
    // (see Editor::UntouchedDrawing). A press elsewhere settles it as it
    // happens (RecognizePress); this catches moving on without one.
    editor_.WatchUntouchedDrawing();

    // Nothing acts on a snippet that has gone - see Editor::Selection. The
    // keys and the wheel have been handled as they came (see OnInput).
    editor_.PruneSelection();

    // A note edit ended by a command from elsewhere - the session ends
    // whatever gesture is open before any other (see
    // Session::EndOpenGesture) - has its editor put away with it, rather
    // than typing on into an edit that is over.
    editor_.ForgetNoteEditEndedElsewhere();

    // Over a snippet a plain drag would pick up - the selection live, and
    // not in drawing mode unless Alt is held - the four-way arrow says so.
    // Only over a snippet: on empty canvas a press clears the selection.
    // Set before anything is drawn rather than after, so anything more
    // specific - a handle's own directional cursor, the dock chips' hand -
    // still wins where it applies.
    const ImGuiIO& io = ImGui::GetIO();
    if (editor_.SelectionLive() && editor_.PressPicksUp() && !io.WantCaptureMouse) {
        const ImVec2 mouse = ImGui::GetMousePos();
        if (editor_.ResolvePointerTarget(mouse.x, mouse.y).kind == PointerTarget::Kind::Body) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }

    // In the rasterized mode, the bitmaps the strokes are drawn into. Last
    // thing before anything item-shaped is drawn: everything above this
    // line is input handling, and Alt+wheel canvas stepping lives up there.
    RefreshStrokeRasters();

    // Where the panels docked against the screen's edges are this frame, and
    // how far out - before anything is drawn, since the minimized chips
    // RenderItems draws have to clear the ones on the bottom edge.
    UpdateEdgePanels(displayW, displayH);
}

void OverlayApp::DrawCanvas(float displayW, float displayH) {
    RenderCanvasLayer(displayW, displayH);
    RenderItems(displayW, displayH);
    // Over the items, and under the popups.
    RenderCanvasBar(displayW, displayH);
}

void OverlayApp::DrawPopups(float displayW, float displayH) {
    RenderItemPropertiesPopover();
    // And the menu a right-click on a snippet opens - beside the popover
    // rather than inside it: the two hold the same actions and are opened
    // different ways, and only one of them can be up at a time anyway,
    // since opening either closes whatever popup was there.
    RenderItemContextMenu();
    // And the canvas bar's, for the tile that was right-clicked - out here
    // rather than inside the bar's own window so that it is a popup at the
    // same level as every other, and so it survives a frame in which the
    // bar itself does not draw.
    RenderCanvasContextMenu();
    // And empty canvas's, the same way.
    RenderEmptyCanvasMenu();
    // At the top level every frame, so the popup always belongs to the same
    // window whichever of the two things that open it asked.
    RenderColorChooser(displayW, displayH);
}

void OverlayApp::DrawOverCanvas(float displayW, float displayH) {
    RenderRegionCaptureOverlay();
    RenderRectEraserOverlay();
    RenderBrushSizePreview();
    RenderToolModifierBadge();
    // Over everything the canvas holds, under the Overview - see its own
    // doc comment for what is in it and in which order.
    RenderScreenChrome(displayW, displayH);
}

void OverlayApp::DrawPanels(float displayW, float displayH) {
    RenderOverview(displayW, displayH);
    RenderCheatSheet(displayW, displayH);
    RenderConfirmDeletePopover();
}

void OverlayApp::DrawMessages() {
    // Drawn in view-only too, not just in edit mode where it started. A
    // hotkey that acts while the overlay is merely being looked through
    // still did something, and this is the only thing that says so - it
    // costs a frame that was being drawn anyway, and click-through means it
    // cannot get in the way of anything.
    RenderActionToast();
    RenderPersistenceWarning();
}

void OverlayApp::DrawPointer() {
    ApplyPointerShape();
    DrawSoftwareCursor();
}

void OverlayApp::Apply() {
    // Taken first, so that an action asking for another - a menu's Delete
    // canvas, where Settings says not to ask, is a command that asks for the
    // delete - waits for the next frame rather than growing the list being
    // walked.
    std::vector<ViewAction> actions;
    actions.swap(actions_);
    for (const ViewAction& action : actions) {
        Do(action);
    }

    if (IsViewOnly()) {
        // A notice exists only to carry its message, so it is over when the
        // message is - faded, or never set at all, which is the same
        // condition RenderActionToast draws nothing on. Reported once (see
        // noticeFinishedReported_); the window goes away on the other end.
        if (IsNoticeOnly() && !noticeFinishedReported_ &&
            (actionToastText_.empty() || ImGui::GetTime() >= actionToastExpireAtSeconds_)) {
            noticeFinishedReported_ = true;
            if (noticeFinishedCallback_) {
                noticeFinishedCallback_();
            }
        }
        return;
    }
    // The preview gone, the width the wheel settled on is kept - once, not
    // per notch (see drawWidthDirty_).
    if (ImGui::GetTime() >= sizePreviewExpireAtSeconds_) {
        KeepPenWidth();
    }
    UpdateInputOptionsHud();
}

namespace {
// One lambda per alternative of a variant, for std::visit.
template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;
}  // namespace

void OverlayApp::Do(const ViewAction& action) {
    std::visit(Overloaded{
                   [&](const action::RunCommand& a) { Dispatch(a.command); },
                   [&](const action::SwitchCanvas& a) { editor_.SwitchCanvas(a.canvas); },
                   [&](const action::SwitchFolder& a) {
                       session_.SwitchToFolder(a.folder);
                       deletedFolderShown_.reset();
                   },
                   [&](const action::ShowDeletedFolder& a) { deletedFolderShown_ = a.folder; },
                   [&](const action::ReorderFolder& a) { session_.ReorderFolder(a.folder, a.place); },
                   [&](const action::ReorderCanvas& a) { session_.ReorderCanvas(a.canvas, a.place); },
                   [&](const action::MoveCanvasToFolder& a) { session_.MoveCanvasToFolder(a.canvas, a.folder); },
                   [&](const action::RenameFolder& a) { session_.RenameFolder(a.folder, a.name); },
                   [&](const action::RenameCanvas& a) { session_.RenameCanvas(a.canvas, a.name); },
                   [&](const action::NewFolder&) {
                       // Named for when it was made (see TimestampName), and at
                       // the end of the sidebar - which is where the eye goes
                       // after pressing a button at the bottom of it, and matches
                       // where a new canvas lands in its own list.
                       overviewScrollToFolderId_ = session_.AddFolder(TimestampName());
                       deletedFolderShown_.reset();
                       // With a canvas already in it. A folder is where canvases
                       // live, so an empty one is a step rather than a result, and
                       // an empty folder reads as a dead end: no tile to click,
                       // nothing to drop an item onto. AddFolder has already made
                       // the new folder current, so this lands inside it - and,
                       // like New canvas, the canvas it makes is switched to.
                       editor_.SwitchCanvas(editor_.CreateCanvasInCurrentFolder());
                   },
                   [&](const action::NewCanvas&) {
                       // At the end of the folder, so the grid may have to scroll
                       // for it to be seen at all - see overviewScrollToCanvasId_,
                       // which CreateCanvasInCurrentFolder sets either way: the
                       // tile is what the click was about.
                       const CanvasId id = editor_.CreateCanvasInCurrentFolder();
                       if (pickerItemId_.has_value()) {
                           // A destination for the snippet being sent away, and not
                           // switched to: following it would take the user off the
                           // canvas they were working on.
                           SendPickedItemTo(id);
                       } else {
                           // Switched to, with the Overview staying up. Making a
                           // canvas is asking for somewhere new to draw, so leaving
                           // the app on the old one did half the job; and a panel
                           // that vanishes the instant a button is pressed is
                           // disorienting, where the grid left there can be carried
                           // on with.
                           editor_.SwitchCanvas(id);
                       }
                   },
                   [&](const action::SendPicked& a) { SendPickedItemTo(a.canvas); },
                   [&](const action::Restore& a) {
                       if (session_.Restore(a.id)) {
                           ShowActionToast(strings::kToastRestored);
                           SettleDeletedFolderShown();
                       }
                   },
                   [&](const action::Delete& a) { PerformDelete(a.target); },
                   [&](const action::RestoreMinimized& a) { session_.SetMinimized({a.item}, false); },
                   [&](const action::ClosePanel& a) {
                       if (a.panel == PanelKind::Overview) {
                           CloseOverview();
                       } else if (IsCheatSheetOpen()) {
                           editor_.Input().End(Level::Panel);
                       }
                   },
                   [&](const action::FinishNoteEdit& a) { editor_.EndEditingNote(a.text); },
               },
               action);
}

// Which shape the OS cursor should wear, decided here at the end of the
// frame rather than at the start - because ImGui only knows what it wants
// once every widget has run, and this must not overrule it. A resize
// handle's directional arrow, the dock's hand and the note editor's I-beam
// are all ImGui's to install; asking the platform for a crosshair on top of
// them is what made them flash and vanish.
//
// Asks for Default while the software pointer is drawn rather than worn by
// the OS: the OS cursor is hidden then, so this would be shaping something
// invisible.
void OverlayApp::ApplyPointerShape() {
    if (!window_) {
        return;
    }
    const ImGuiMouseCursor imguiCursor = ImGui::GetMouseCursor();
    const bool imguiOwnsIt = imguiCursor != ImGuiMouseCursor_Arrow;
    const platform::CursorShape wanted =
        (imguiOwnsIt || settings_.Live().InputOptions().SoftwarePointerDrawn()) ? platform::CursorShape::Default
                                                               : WantedPointerShape();

    // Re-asserted when either half of the decision moves, and not otherwise.
    //
    // Pushing it every frame was the previous version, and the note here
    // said it "costs one call that does nothing on the shape it is already
    // wearing". Measured, it cost 82 us of CPU per frame - a quarter of the
    // whole frame on an idle overlay - because SetCursorShape answers "is
    // the cursor still over one of my windows" with WindowFromPoint, a
    // system-wide hit test, and then installs the shape again.
    //
    // What "moved" has to mean is the whole of the difficulty, and getting it
    // wrong is not subtle: it puts the arrow back over the canvas and leaves
    // it there.
    //
    // ImGui's backend installs its own cursor from NewFrame when ImGui's
    // wanted shape differs from the one it installed last - and it reads that
    // shape *before* NewFrame resets it, so its install lands one frame after
    // the change. Watching `ImGui::GetMouseCursor()` for a change is
    // therefore a frame too early: on the frame the backend actually takes
    // the cursor away, ImGui's own answer has already been Arrow since the
    // previous frame, and a key built on it says nothing changed. That is
    // exactly how the pen stopped coming back after a resize handle. So the
    // re-assert has to cover the frame after a change as well as the frame of
    // one - hence two frames of history rather than one.
    //
    // A moved pointer re-asserts too. It costs nothing where it matters (an
    // overlay nobody is touching is the case being optimized, and there the
    // pointer is still by definition), and it covers everything that can only
    // happen while the pointer moves - including SetCursorShape declining to
    // act because its own hit test said the cursor had left our window, which
    // would otherwise be recorded as applied and never retried.
    const ImVec2 pointer = ImGui::GetMousePos();
    const bool pointerMoved = pointer.x != lastPointerX_ || pointer.y != lastPointerY_;
    const bool imguiTouchedTheCursor =
        imguiCursor != lastImGuiCursor_ || lastImGuiCursor_ != previousImGuiCursor_;
    previousImGuiCursor_ = lastImGuiCursor_;
    lastImGuiCursor_ = imguiCursor;
    lastPointerX_ = pointer.x;
    lastPointerY_ = pointer.y;

    if (!pointerMoved && !imguiTouchedTheCursor && appliedPointerShape_ == wanted) {
        return;
    }
    appliedPointerShape_ = wanted;
    window_->SetCursorShape(wanted);
}

void OverlayApp::SayDeletedForGoodAtStart(size_t count, int days) {
    if (count == 0) {
        return;
    }
    char text[160];
    if (count == 1) {
        std::snprintf(text, sizeof(text), strings::kToastPurgedOne, days);
    } else {
        std::snprintf(text, sizeof(text), strings::kToastPurgedMany, count, days);
    }
    messageForNextShow_ = text;
}

void OverlayApp::OnOverlayShown() {
    settingsPage_.OnOverlayShown();
    // Something the app did while nobody was looking - see
    // SayDeletedForGoodAtStart - said now, and for long enough to be read.
    if (!messageForNextShow_.empty() && ImGui::GetCurrentContext() != nullptr) {
        actionToastText_ = std::move(messageForNextShow_);
        messageForNextShow_.clear();
        actionToastExpireAtSeconds_ = ImGui::GetTime() + 8.0;
    }
    // Hiding is moving on too, and nothing ran while hidden to notice - see
    // Editor::UntouchedDrawing.
    editor_.SettleUntouchedDrawing();
    // Nothing is in the hand as the overlay comes up: what went down before
    // it was hidden has come up since, wherever that release went. Settled
    // already when it was put away, unless it went some other way.
    OfferLifecycle(Lifecycle::Shown);
    editor_.Settle(Scope::Hand);
    editor_.ForgetTheHand();
    // The panels docked against the edges come out for a moment, so they
    // are seen where they are - asked for here, done on the first frame.
    edgePanelsFlashPending_ = true;
    // While the overlay was away, whatever is underneath owned the pointer
    // and will have installed its own shape. What ApplyPointerShape last
    // asked for therefore says nothing about what is on screen now, and
    // skipping the push because "it hasn't changed" would leave the other
    // application's cursor over our canvas until the next mouse move.
    appliedPointerShape_.reset();
    // A showing starts from no input at all: nothing that happened while
    // the overlay was hidden is input to it.
    //
    // Not housekeeping - a key really does survive the gap. The keyboard
    // grab hands every keystroke to this window for as long as edit mode is
    // up, the hotkey that *ends* edit mode included (see
    // Win32InputGrab::OnKeyboard, which dispatches the hotkey and passes the
    // key on deliberately). That last one is posted after the final frame
    // and nothing drains it: no frame is drawn while hidden, so ImGui's
    // event queue keeps it until the next showing reads it as a fresh
    // press. Seen with Ctrl+Alt+S bound to edit mode, while the command
    // keys were still read from ImGui: the overlay came back with the
    // screenshot tool in hand, because "S" alone is that tool's key. They
    // come through the input stream now, which a hidden window hands
    // nothing (see IOverlayWindow::SetInputCallback); ImGui's queue is
    // what the panels and their widgets still read.
    //
    // The key state as well as the queue, because it goes stale the same
    // way and in both directions: what ImGui believes is held is whatever
    // the last frame before the hiding saw, however long ago that was, and
    // modifiers left latched that way would make the exact-modifier test
    // refuse a perfectly ordinary key on the way back. Nothing true is lost
    // by clearing - a modifier still physically held is re-sent on every
    // frame by the platform (see Win32OverlayWindow::RenderFrame).
    //
    // The mouse too, which ClearInputKeys leaves alone. A button held on a
    // scrollbar or a panel as the overlay went away never has its release
    // seen - under the grab, with no activation, not even as a focus loss -
    // so it came back held: the first click was taken as the release, and
    // an ImGui drag went on with no button down.
    //
    // Guarded like ShowActionToast's: the overlay can be shown by a hotkey
    // pressed before a single frame has ever been drawn.
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
    }
}

namespace {
// The rows of the input options HUD, in the order the number keys address
// them. Kept as data so the drawing and the key handling cannot disagree
// about which key means which option.
struct InputOptionRow {
    const char* label;
    // The setting the row shows and a number key flips - its row in the
    // catalog, which says where it is stored and what a profile says about
    // it.
    const ProfileSetting<BoolRule>* setting;
    // What decides whether the row can do anything - see
    // InputOptionAvailable - and whether flipping it needs edit mode
    // entered again.
    enum class Which {
        NoActivate,
        SoftwarePointer,
        DontForwardKeystrokes,
        RawMouseInput,
        CounterRawMouseInput,
        FreezeScreen,
    };
    Which which;
};

// Same names and same order as the Settings tab, so that finding the right
// combination here and then finding it again there isn't a translation
// exercise. The count travels to the platform via
// IOverlayWindow::SetInputOptionsHudDigits, so the number of digits the
// keyboard hook claims follows this table rather than a constant beside it.
//
// Ordered so that every option's prerequisites are above it, which is also
// what puts the Settings tab's tree at the top of both. "Don't steal focus"
// is what leaves the game receiving input, so the things that take it back
// follow it, contiguously, each link below the one it needs (see
// EditModeInputOptions' *CanBeUsed predicates). The last two need nothing
// here, so they sit at the bottom - the same statement the Settings tab
// makes by leaving them out of its tree.
// Which rows need edit mode re-entered to take effect at all. Freezing only
// captures the screen on entry, and no-activate decides how the window is
// shown - toggling either would otherwise read ON in the HUD while nothing
// had changed on screen. Everything else is pushed into the window live by
// OnSettingsChanged, exactly as the Settings tab does it, and needs no such
// thing - which matters because a restart is not free: see
// pendingOverlayRestart_.
constexpr bool RowNeedsOverlayRestart(InputOptionRow::Which which) {
    return which == InputOptionRow::Which::NoActivate || which == InputOptionRow::Which::FreezeScreen;
}

constexpr InputOptionRow kInputOptionRows[] = {
    {strings::kHudDontStealFocus, &setting::kDontStealFocus, InputOptionRow::Which::NoActivate},
    {strings::kHudDontForwardKeystrokes, &setting::kDontForwardKeystrokes,
     InputOptionRow::Which::DontForwardKeystrokes},
    {strings::kHudUseRawMouseInput, &setting::kRawMouseInput, InputOptionRow::Which::RawMouseInput},
    {strings::kHudCounterRawMouseInput, &setting::kCounterRawMouseInput, InputOptionRow::Which::CounterRawMouseInput},
    {strings::kHudUseSoftwarePointer, &setting::kSoftwarePointer, InputOptionRow::Which::SoftwarePointer},
    {strings::kHudFreezeScreenWhileEditing, &setting::kFreezeScreen, InputOptionRow::Which::FreezeScreen},
};
}  // namespace

bool OverlayApp::InputOptionValue(int index) const {
    if (index < 0 || index >= static_cast<int>(std::size(kInputOptionRows))) {
        return false;
    }
    // The resolved value: the HUD reports what is running, which is the
    // whole reason it exists.
    return settings_.Live().*kInputOptionRows[index].setting->value;
}

// Whether a row's option can currently do anything - the same preconditions
// the Settings tab grays its checkboxes on, read from the one place that
// states them. An unavailable row is dimmed and its number key ignored:
// storing a change that has no effect, with nothing on screen saying so, is
// how you end up believing an option is broken.
bool OverlayApp::InputOptionAvailable(int index) const {
    if (index < 0 || index >= static_cast<int>(std::size(kInputOptionRows))) {
        return false;
    }
    switch (kInputOptionRows[index].which) {
        case InputOptionRow::Which::NoActivate:
        case InputOptionRow::Which::FreezeScreen:
            return true;  // depend on nothing else here
        case InputOptionRow::Which::SoftwarePointer:
            return true;  // a matter of appearance; works with or without focus
        case InputOptionRow::Which::DontForwardKeystrokes:
            return platform::EditModeInputOptions::KeystrokesCanBeHeld(settings_.Live().dontStealFocus);
        case InputOptionRow::Which::RawMouseInput:
            return settings_.Live().InputOptions().RawMouseInputCanBeUsed(settings_.Live().dontStealFocus);
        case InputOptionRow::Which::CounterRawMouseInput:
            return settings_.Live().InputOptions().CounterRawMouseInputCanBeUsed(settings_.Live().dontStealFocus);
    }
    return false;
}

// A debugging aid, off by default - see AppConfig::showInputOptionsHud.
// Not always on, tempting as that is for something meant to be seen while
// standing in front of a misbehaving game, because of the cost: the number
// keys need a keyboard hook to reach an overlay that deliberately has no
// focus, so an always-on HUD meant digits never reached the game even with
// "Don't forward keystrokes" off. A diagnostic that quietly eats input is
// one to switch on deliberately.
void OverlayApp::DrawInputOptionsHud(ImDrawList* drawList) const {
    if (!Cfg().showInputOptionsHud || drawList == nullptr) {
        return;
    }
    constexpr float kPad = 10.0f;
    constexpr float kLineHeight = 19.0f;
    constexpr float kOriginX = 14.0f;
    constexpr float kOriginY = 14.0f;
    const int rowCount = static_cast<int>(std::size(kInputOptionRows));

    float widest = 0.0f;
    for (const InputOptionRow& row : kInputOptionRows) {
        widest = std::max(widest, ImGui::CalcTextSize(row.label).x);
    }

    char fps[160];
    // Availability, not just the stored value: with raw input grayed out
    // there is no pointer of ours being driven, so its gain and step
    // histogram would be a readout of nothing.
    if (settings_.Live().InputOptions().useRawMouseInput && settings_.Live().InputOptions().RawMouseInputCanBeUsed(settings_.Live().dontStealFocus) &&
        window_ != nullptr) {
        // Per-report step sizes against per-frame ones. All ones in the
        // first and twos in the second means the pointer arithmetic is fine
        // and it is the once-a-frame drawing that looks coarse - a
        // different problem with a different fix.
        const platform::InputGrabDiagnostics diag = window_->GetInputGrabDiagnostics();
        std::snprintf(fps, sizeof(fps), "%.0f fps  gain %.2f %s  report %d/%d/%d/%d  frame %d/%d/%d/%d",
                       ImGui::GetIO().Framerate, diag.pointerGain, diag.ballisticsEnabled ? "curve" : "flat",
                       diag.stepCounts[0], diag.stepCounts[1], diag.stepCounts[2], diag.stepCounts[3],
                       diag.frameSteps[0], diag.frameSteps[1], diag.frameSteps[2], diag.frameSteps[3]);
    } else {
        std::snprintf(fps, sizeof(fps), "%.0f fps   %.2f ms", ImGui::GetIO().Framerate,
                       1000.0f / std::max(1.0f, ImGui::GetIO().Framerate));
    }
    // What countering is actually managing, when it is on: how long the
    // game had each movement to itself before the negation arrived, and
    // which of the two injection timings produced that. The residual the
    // camera keeps is not in here and cannot be - see
    // InputGrabDiagnostics::correctionLagMsLast.
    // What the last number key did, and how much re-deriving has happened
    // since - see hudToggleCount_.
    char lastKey[160];
    lastKey[0] = '\0';
    if (hudLastToggledRow_ != 0) {
        std::snprintf(lastKey, sizeof(lastKey),
                       "last key %d: set %s, into %s  -  now prof=%s  (toggles %d, resolves %d)",
                       hudLastToggledRow_, hudLastToggledTo_ ? strings::kHotkeysOn : strings::kHotkeysOff,
                       hudLastWentToProfile_ ? strings::kProfilesNamePrefix : strings::kProfilesDefaults,
                       settings_.ActiveProfile() && *settings_.ActiveProfile() < settings_.Profiles().size()
                           ? settings_.Profiles()[*settings_.ActiveProfile()].name.c_str()
                           : "none",
                       hudToggleCount_, settings_.ResolveCount());
    }

    char counter[192];
    counter[0] = '\0';
    if (settings_.Live().InputOptions().counterRawMouseInput &&
        settings_.Live().InputOptions().CounterRawMouseInputCanBeUsed(settings_.Live().dontStealFocus) && window_ != nullptr) {
        const platform::InputGrabDiagnostics diag = window_->GetInputGrabDiagnostics();
        std::snprintf(counter, sizeof(counter), "counter lag %.2f ms (max %.2f)  n=%d",
                       diag.correctionLagMsLast, diag.correctionLagMsMax, diag.correctionsInjected);
    }

    // What is in front and where it sits relative to us - the line that
    // answers "why is nothing in this panel moving". Above us, Windows
    // delivers that application's input to no lower-integrity process at
    // all, so every number here stays where it is however the rows are
    // set, and no shortcut of the overlay's arrives either. See
    // platform::ForegroundIntegrity.
    char foreground[224];
    foreground[0] = '\0';
    {
        const platform::ForegroundApp& app = settings_.UnderlyingApplication();
        const char* what = !app.executable.empty() ? app.executable.c_str()
                           : !app.title.empty()    ? app.title.c_str()
                                                   : "(nothing identifiable)";
        switch (app.integrity) {
            case platform::ForegroundIntegrity::Above:
                std::snprintf(foreground, sizeof(foreground),
                               "over %s  -  above us, none of its input reaches here", what);
                break;
            case platform::ForegroundIntegrity::NotAbove:
                std::snprintf(foreground, sizeof(foreground), "over %s  -  not above us", what);
                break;
            case platform::ForegroundIntegrity::Unknown:
                std::snprintf(foreground, sizeof(foreground), "over %s  -  integrity unreadable", what);
                break;
        }
    }

    // Number prefix, label, then the ON/OFF column clear of the longest label.
    const float statusX = Px(kOriginX) + Px(kPad) + Px(26.0f) + widest + Px(16.0f);
    // Wide enough for the header line too - it carries the pointer
    // diagnostics and is easily longer than the rows.
    const float panelW = std::max({statusX + Px(34.0f) + Px(kPad) - Px(kOriginX),
                                    ImGui::CalcTextSize(fps).x + Px(kPad) * 2.0f,
                                    ImGui::CalcTextSize(counter).x + Px(kPad) * 2.0f,
                                    ImGui::CalcTextSize(foreground).x + Px(kPad) * 2.0f,
                                    ImGui::CalcTextSize(lastKey).x + Px(kPad) * 2.0f});
    // One extra line for the frame rate: "the overlay feels slower with the
    // grab on" is a measurement, not an impression, and this is where it can
    // be read without leaving the situation that caused it. Then one for the
    // counter readout and one for the last key, on the frames there are any.
    const int extraLines = 1 + (counter[0] != '\0' ? 1 : 0) + (foreground[0] != '\0' ? 1 : 0) +
                          (lastKey[0] != '\0' ? 1 : 0);
    const float panelH = Px(kPad) * 2.0f + Px(kLineHeight) * static_cast<float>(rowCount + extraLines);

    const ImVec2 panelMin = Px(kOriginX, kOriginY);
    const ImVec2 panelMax(panelMin.x + panelW, panelMin.y + panelH);
    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(12, 15, 20, 205), Px(6.0f));
    drawList->AddRect(panelMin, panelMax, IM_COL32(255, 255, 255, 40), Px(6.0f));

    drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad)), IM_COL32(150, 158, 172, 255), fps);

    for (int i = 0; i < rowCount; ++i) {
        const float y = Px(kOriginY) + Px(kPad) + Px(kLineHeight) * static_cast<float>(i + 1);
        // A row whose prerequisite isn't met is dimmed and reads "--" rather
        // than ON/OFF: its stored value is still there and still what it will
        // do once the row above allows it, but saying ON about something that
        // is doing nothing is the one thing a diagnostic panel must not do.
        // Its number key is ignored to match.
        const bool available = InputOptionAvailable(i);
        char key[8];
        std::snprintf(key, sizeof(key), "%d", i + 1);
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), y),
                           available ? IM_COL32(150, 158, 172, 255) : IM_COL32(96, 102, 114, 255), key);
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad) + Px(20.0f), y),
                           available ? IM_COL32(226, 230, 238, 255) : IM_COL32(120, 126, 138, 255),
                           kInputOptionRows[i].label);

        const bool on = InputOptionValue(i);
        if (!available) {
            drawList->AddText(ImVec2(statusX, y), IM_COL32(120, 126, 138, 255), "--");
            continue;
        }
        drawList->AddText(ImVec2(statusX, y), on ? IM_COL32(90, 214, 130, 255) : IM_COL32(232, 100, 100, 255),
                           on ? strings::kHotkeysOn : strings::kHotkeysOff);
    }

    float footerLine = static_cast<float>(rowCount + 1);
    if (counter[0] != '\0') {
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad) + Px(kLineHeight) * footerLine),
                           IM_COL32(150, 158, 172, 255), counter);
        footerLine += 1.0f;
    }
    if (foreground[0] != '\0') {
        // Brighter when it is the answer: above us, nothing else in this
        // panel can be trusted to mean anything.
        const bool above = settings_.UnderlyingApplication().integrity == platform::ForegroundIntegrity::Above;
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad) + Px(kLineHeight) * footerLine),
                           above ? IM_COL32(232, 100, 100, 255) : IM_COL32(150, 158, 172, 255), foreground);
        footerLine += 1.0f;
    }
    if (lastKey[0] != '\0') {
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad) + Px(kLineHeight) * footerLine),
                           IM_COL32(150, 158, 172, 255), lastKey);
    }
}

void OverlayApp::UpdateInputOptionsHud() {
    // Both halves of the same decision: the digits belong to the HUD only
    // while it is visible, and the platform needs to know so it can stop
    // holding a keyboard hook open on the HUD's behalf. Pushed only on a
    // change - installing or removing a hook is not a per-frame ask.
    const bool hudActive = Cfg().showInputOptionsHud && !IsViewOnly();
    const int hudDigits = hudActive ? static_cast<int>(std::size(kInputOptionRows)) : 0;
    if (window_ && hudDigits != appliedInputOptionsHudDigits_) {
        window_->SetInputOptionsHudDigits(hudDigits);
        appliedInputOptionsHudDigits_ = hudDigits;
    }
    if (!hudActive) {
        // A restart asked for by a row is the HUD's business; with the panel
        // gone there is nobody left to have asked, and firing it later would
        // hide and show the overlay for no reason anyone could see.
        pendingOverlayRestart_ = false;
        return;
    }

    // A restart tears the whole input path down and builds it again - the
    // window is hidden and shown, which takes the keyboard hook with it. Doing
    // that while the key that triggered it is still held loses the key-up, and
    // a key ImGui still believes is held makes the *next* press no press at
    // all. So the restart waits for every HUD digit to be up; without the
    // wait about one press in three went missing.
    if (pendingOverlayRestart_) {
        bool anyDown = false;
        for (int i = 0; i < static_cast<int>(std::size(kInputOptionRows)); ++i) {
            anyDown = anyDown || ImGui::IsKeyDown(static_cast<ImGuiKey>(ImGuiKey_1 + i));
        }
        if (!anyDown) {
            pendingOverlayRestart_ = false;
            if (restartOverlayCallback_) {
                restartOverlayCallback_();
            }
            return;  // rebuilt after this frame - see IPlatformHost::Post
        }
    }
}

// Reaches here as the Canvas level's (see CanvasLevel), so never while text
// is being typed or a panel or a popup is up: each of those takes every key.
bool OverlayApp::HandleInputOptionsHudKey(const Event& event) {
    if (!Cfg().showInputOptionsHud || IsViewOnly() || event.repeat) {
        return false;
    }
    for (int i = 0; i < static_cast<int>(std::size(kInputOptionRows)); ++i) {
        if (event.key != '1' + i) {
            continue;
        }
        if (!InputOptionAvailable(i)) {
            // Dimmed in the panel, and inert here to match. Its prerequisite
            // is one of the rows above, so it is one keypress away.
            return true;
        }
        // Into the profile that matched, if one did - the HUD is about the
        // configuration that is running, and the running configuration is
        // that profile's. Deliberately not the Settings panel's own edit
        // target, which may be some other profile entirely.
        const bool wanted = !InputOptionValue(i);
        settings_.Set(*kInputOptionRows[i].setting, wanted, settings_.ActiveProfile());
        // Recorded for the HUD, which is the only place this can be seen
        // happening - see hudToggleCount_.
        ++hudToggleCount_;
        hudLastToggledRow_ = i + 1;
        hudLastToggledTo_ = wanted;
        hudLastWentToProfile_ = settings_.ActiveProfile().has_value();
        // Only two rows need edit mode re-entered, and the restart is now
        // deferred until the key that asked for it is back up - see
        // pendingOverlayRestart_.
        if (RowNeedsOverlayRestart(kInputOptionRows[i].which)) {
            pendingOverlayRestart_ = true;
        }
        return true;
    }
    return false;
}

// Draws the pointer at ImGui's mouse position, whatever put it there - which
// is what makes this work in both configurations. Under a mouse grab that
// position is the grab's own accumulated one, so by construction the drawn
// pointer can't drift from what a click actually hits; with only
// useSoftwarePointer on, it's the real cursor's position and this is purely a
// change of appearance. Either way the OS cursor is hidden over this window
// (see Win32Dx11Renderer::NewFrame) so there is exactly one pointer visible.
//
// Gated on the settings rather than on the grab really running, which core
// has no way to ask: a backend without an input grab (the Linux dev harness)
// would otherwise draw a second, redundant pointer.
// The one place that decides what the pointer means, shared by both of them
// so they cannot disagree.
//
// An arrow unless something more specific applies - not a crosshair
// whenever a drawing tool is in hand, which put a crosshair over empty
// desktop where nothing would be drawn.

platform::CursorShape OverlayApp::WantedPointerShape() const {
    // Placing a snippet: the click puts a corner somewhere exact.
    if (ArmedCreation().has_value()) {
        return platform::CursorShape::Crosshair;
    }
    // Everything below is about what the pointer is *over*. Over the
    // overview, a popover or the selection's handles and bar, ImGui owns the
    // pointer and this must not argue with it - the arrow here is only
    // what's left when ImGui wants nothing more specific, which
    // ApplyPointerShape has already checked before this answer is used at
    // all.
    if (ImGui::GetIO().WantCaptureMouse || PanelOpen()) {
        return platform::CursorShape::Arrow;
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    const PointerTarget target = editor_.ResolvePointerTarget(mouse.x, mouse.y);
    if (target.kind != PointerTarget::Kind::Body) {
        // Open canvas, a handle or the bar: nothing here for the tool to
        // mark. A handle has already asked ImGui for its own shape
        // (RenderItems), which ApplyPointerShape lets win; a button gets
        // the arrow, as a button does.
        return platform::CursorShape::Arrow;
    }
    // A marking tool marks only the snippet in drawing mode, and not with
    // Alt held, when the press picks the snippet up instead.
    if (editor_.DrawingItem() != target.item || editor_.PressPicksUp()) {
        return platform::CursorShape::Arrow;
    }
    switch (editor_.ActiveTool()) {
        case Tool::Draw:
            return platform::CursorShape::Pen;
        case Tool::Erase:
            // No eraser glyph in either cursor set, and the exact point is
            // what matters - the brush-size ring already says how much will
            // come away.
            return platform::CursorShape::Crosshair;
        case Tool::Text:
        case Tool::Select:
        case Tool::NewDrawing:
        case Tool::NewScreenshot:
            break;
    }
    return platform::CursorShape::Arrow;
}

void OverlayApp::RenderToolModifierBadge() {
    if (PanelOpen() || ArmedCreation().has_value() ||
        (editor_.ActiveTool() != Tool::Draw && editor_.ActiveTool() != Tool::Erase)) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const Marking* stroke = editor_.Input().As<Marking>(Level::Gesture);
    const bool dragging = stroke != nullptr;
    if (!dragging) {
        // Only where a press would make one: over the snippet in drawing
        // mode, not a panel, and not with Alt held.
        const PointerTarget target = editor_.ResolvePointerTarget(io.MousePos.x, io.MousePos.y);
        if (io.WantCaptureMouse || target.kind != PointerTarget::Kind::Body || editor_.DrawingItem() != target.item ||
            editor_.PressPicksUp()) {
            return;
        }
    }
    // Mid-drag, what the gesture is making; before one, what a press would
    // make now - the modifiers held, or the bar's cycled shape.
    const Icon* icon = nullptr;
    if (editor_.ActiveTool() == Tool::Draw) {
        const DrawShape shape = dragging ? stroke->Shape() : editor_.ShapeForPress();
        if (dragging && stroke->GetKind() != Marking::Kind::Shape) {
            return;  // freehand, which needs no saying
        }
        icon = shape == DrawShape::Rectangle ? &icons::kRectangle
               : shape == DrawShape::Line    ? &icons::kLine
                                             : nullptr;
    } else if (dragging ? stroke->GetKind() == Marking::Kind::EraseRect
                        : editor_.ShapeForPress() == DrawShape::Rectangle) {
        icon = &icons::kEraserRect;
    }
    if (icon == nullptr) {
        return;
    }
    // Down and to the right of the hotspot, clear of the pen glyph and the
    // brush-size dot, on a disc of the panel color so it reads over any
    // snippet.
    const float offset = Px(18.0f);
    const float size = Px(22.0f);
    const float inset = Px(4.0f);
    const ImVec2 min(io.MousePos.x + offset, io.MousePos.y + offset);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(min, ImVec2(min.x + size, min.y + size), ImGui::ColorConvertFloat4ToU32(theme::kPanelBg),
                      Px(theme::kRadiusSm));
    DrawIcon(dl, *icon, ImVec2(min.x + inset, min.y + inset), size - inset * 2.0f,
             ImGui::ColorConvertFloat4ToU32(theme::kWhite), 1.5f);
}

void OverlayApp::DrawSoftwareCursor() const {
    if (!settings_.Live().InputOptions().SoftwarePointerDrawn()) {
        return;
    }
    // Whatever ImGui asked for, if it asked for anything: its atlas already
    // carries the resize arrows, the hand, the I-beam and NotAllowed, and
    // io.MouseDrawCursor makes it draw them at the same position this
    // pointer would be. Drawing over them instead is what made a resize
    // handle's arrow flash and turn back into a crosshair.
    if (ImGui::GetMouseCursor() != ImGuiMouseCursor_Arrow) {
        return;
    }

    // Nothing more specific, so this pointer is the one being drawn. Stops
    // ImGui drawing its own on top: the backend's io.MouseDrawCursor - the
    // flag that reliably hides the OS cursor - also asks ImGui to render a
    // pointer from its font atlas, and ImGui::EndFrame skips that only
    // while the wanted cursor is None. Set here, after every widget has had
    // its say for this frame, so nothing reinstates a shape afterwards.
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    // Whole pixels, every frame - the same integer position ImGui hit-tests
    // against, so the pointer is drawn exactly where clicks land, and crisp.
    // The grab keeps the pointer's position as a float internally (that is
    // what keeps slow movement from vanishing under a pixel) and floors it
    // once; on identical input its integer steps match the OS cursor's, so
    // there is nothing left for fractional drawing to smooth over. It was
    // tried, and it traded a crisp pointer for a soft one to hide a stepping
    // problem that turned out to be a wrong speed multiplier.
    const ImVec2 at = ImGui::GetMousePos();
    if (at.x < 0.0f || at.y < 0.0f) {
        return;
    }

    constexpr ImU32 kFill = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kEdge = IM_COL32(20, 24, 32, 235);
    constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 70);
    // At Windows' scale for the display rather than the interface scale:
    // this stands in for the system pointer, which Windows draws larger on
    // a scaled display whatever this app's own setting says.
    const float scale = window_ != nullptr ? static_cast<float>(window_->ScalePercent()) / 100.0f : 1.0f;

    const platform::CursorShape shape = WantedPointerShape();

    if (shape == platform::CursorShape::Pen) {
        // A pen, for a snippet a drawing tool would mark - the shared
        // outline (see platform::pen_glyph), placed with its nib at the
        // pointer's own position so it points at the pixel it will draw
        // on. Same two-pass outline and fill as the arrow below, and the
        // same reason: it has to stay legible over a bright game and a
        // dark one alike.
        const auto place = [&at, scale](platform::Vec2 p) { return ImVec2(at.x + p.x * scale, at.y + p.y * scale); };
        ImVec2 nibShape[std::size(platform::pen_glyph::kNib)];
        ImVec2 body[std::size(platform::pen_glyph::kBody)];
        ImVec2 shadow[std::size(platform::pen_glyph::kBody)];
        for (size_t i = 0; i < std::size(nibShape); ++i) {
            nibShape[i] = place(platform::pen_glyph::kNib[i]);
        }
        for (size_t i = 0; i < std::size(body); ++i) {
            body[i] = place(platform::pen_glyph::kBody[i]);
            shadow[i] = ImVec2(body[i].x + 1.5f * scale, body[i].y + 1.5f * scale);
        }
        const float outline = platform::pen_glyph::kOutlineWidth * scale;
        drawList->AddConvexPolyFilled(shadow, static_cast<int>(std::size(shadow)), kShadow);
        drawList->AddConvexPolyFilled(body, static_cast<int>(std::size(body)), kFill);
        drawList->AddConvexPolyFilled(nibShape, static_cast<int>(std::size(nibShape)), kFill);
        drawList->AddPolyline(body, static_cast<int>(std::size(body)), kEdge, ImDrawFlags_Closed, outline);
        drawList->AddPolyline(nibShape, static_cast<int>(std::size(nibShape)), kEdge, ImDrawFlags_Closed,
                              outline);
        return;
    }

    if (shape == platform::CursorShape::Crosshair) {
        // A crosshair for the tools where the exact point matters and an
        // arrow's body would sit on top of it. The gap in the middle leaves
        // the target pixel itself visible.
        const float arm = 11.0f * scale;
        const float gap = 3.0f * scale;
        const ImVec2 spans[4][2] = {
            {ImVec2(at.x - arm, at.y), ImVec2(at.x - gap, at.y)},
            {ImVec2(at.x + gap, at.y), ImVec2(at.x + arm, at.y)},
            {ImVec2(at.x, at.y - arm), ImVec2(at.x, at.y - gap)},
            {ImVec2(at.x, at.y + gap), ImVec2(at.x, at.y + arm)},
        };
        for (const auto& span : spans) {
            drawList->AddLine(span[0], span[1], kEdge, 3.0f * scale);
            drawList->AddLine(span[0], span[1], kFill, scale);
        }
        return;
    }

    // The ordinary arrow, hotspot at its tip so it points at what it is
    // over rather than near it. Drawn white with a dark outline (and a
    // slight shadow) so it stays legible over a bright game and a dark one
    // alike - the same reason the OS cursor is shaped this way.
    constexpr ImVec2 kArrow[7] = {
        ImVec2(0.0f, 0.0f),   ImVec2(0.0f, 17.0f),  ImVec2(4.2f, 12.8f),  ImVec2(7.0f, 18.6f),
        ImVec2(10.0f, 17.2f), ImVec2(7.2f, 11.6f),  ImVec2(12.2f, 11.6f),
    };
    ImVec2 arrow[7];
    ImVec2 shadow[7];
    for (int i = 0; i < 7; ++i) {
        arrow[i] = ImVec2(at.x + kArrow[i].x * scale, at.y + kArrow[i].y * scale);
        shadow[i] = ImVec2(arrow[i].x + 1.5f * scale, arrow[i].y + 1.5f * scale);
    }
    drawList->AddConvexPolyFilled(shadow, 7, kShadow);
    drawList->AddConvexPolyFilled(arrow, 7, kFill);
    drawList->AddPolyline(arrow, 7, kEdge, ImDrawFlags_Closed, 1.4f * scale);
}

// ================= View-only mode =================

void OverlayApp::RenderViewOnly(float displayW, float displayH) {
    ImDrawList* drawList = BeginScreenLayer("##spickzettel_view_only", displayW, displayH);
    // Nothing to show with an empty library (see CanvasManager's class
    // comment) - view-only mode has no UI of its own to offer instead, so
    // it just renders nothing, which is exactly right: a fully
    // transparent, fully click-through overlay.
    if (const Canvas* canvas = Manager().CurrentOrNull()) {
        for (const Item& item : canvas->items) {
            // A minimized snippet is drawn nowhere but its dock chip, and
            // view-only has no dock. The pinned view draws the pinned ones
            // and nothing else.
            if (Manager().IsDeleted(*canvas, item) || item.minimized || (IsPinnedOnly() && !item.pinned)) {
                continue;
            }
            const ImVec2 pMin(item.rect.x, item.rect.y);
            const ImVec2 pMax(item.rect.x + item.rect.w, item.rect.y + item.rect.h);
            drawList->PushClipRect(pMin, pMax, true);
            DrawItemContent(drawList, item, pMin, pMax, Cfg().strokeRenderMode, PictureTexture(item),
                            StrokeRasterTextureFor(item.id), /*skipNoteText=*/false, CanvasMeshSlot(),
                            PictureSampling());
            drawList->PopClipRect();
        }
    }

    if (Cfg().showDebugOverlay) {
        DrawDebugOverlay(drawList, ImGui::GetIO(), Manager(), debugHoveredResizeHandle_);
    }

    // Also here, not just in edit mode - see DrawDemoWatermark. Drawn last
    // within this one layer rather than in a layer of its own: view-only
    // mode has nothing else on screen for it to be under.
    DrawDemoWatermark(drawList, displayW, displayH);

    EndScreenLayer();
}

// Feedback for the mouse wheel's size change, which otherwise altered the
// tool silently and left "how big is it now?" to be answered by drawing a
// test stroke and undoing it. Deliberately shows the *size itself* - a dot
// of exactly the diameter the tool will mark at, under the cursor - rather
// than only a number somewhere else on screen: the question is about a
// size, so the answer should be one. The number rides along for the cases
// where the dot alone is hard to judge (1px vs 2px).
void OverlayApp::RenderBrushSizePreview() {
    const double now = ImGui::GetTime();
    if (now >= sizePreviewExpireAtSeconds_) {
        return;  // and the width it showed is kept - see Apply
    }
    if (PanelOpen()) {
        return;
    }
    const std::optional<float> diameter = editor_.ActiveToolSizePx();
    if (!diameter.has_value()) {
        return;
    }
    // Holds at full strength, then fades over the last stretch rather than
    // blinking out - a hard disappearance at the end reads as a glitch.
    const float alpha =
        static_cast<float>(std::clamp((sizePreviewExpireAtSeconds_ - now) / kSizePreviewFadeSeconds, 0.0, 1.0));

    // Follows the live cursor rather than freezing where the wheel was
    // turned: adjusting the size and then moving to where you're about to
    // draw is one continuous motion, and a preview left behind at the old
    // spot would be answering the question in the wrong place.
    const ImVec2 center = ImGui::GetIO().MousePos;
    const float radius = *diameter * 0.5f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const auto fade = [alpha](ImU32 color) {
        const ImU32 a = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };

    if (editor_.ActiveTool() == Tool::Erase) {
        // Hollow, and in RenderRectEraserOverlay's own cool blue rather
        // than the warm placement tone: the eraser takes ink away, and a
        // filled dot would read as something about to be painted.
        dl->AddCircleFilled(center, radius, fade(IM_COL32(120, 170, 255, 40)));
        dl->AddCircle(center, radius, fade(IM_COL32(120, 170, 255, 255)), 0, 2.0f);
    } else {
        // The pen's actual color, so this previews the mark itself and not
        // just its footprint.
        dl->AddCircleFilled(center, radius, fade(ToImColor(editor_.DrawColorRGBA())));
        // A hairline at exactly `radius` (not outside it - the ring must
        // not make the dot look bigger than it is) keeps a dark color, or
        // a 1px width, findable against whatever is underneath.
        dl->AddCircle(center, radius, fade(IM_COL32(255, 255, 255, 200)), 0, 1.0f);
    }

    char label[16];
    std::snprintf(label, sizeof(label), strings::kFormatPixels, *diameter);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    constexpr float kLabelPadX = 7.0f;
    constexpr float kLabelPadY = 3.0f;
    constexpr float kLabelGap = 10.0f;
    // Clear of the dot at every size in both ranges (1..24 and 8..64), so
    // the two never overlap and the label doesn't jump sides.
    const ImVec2 boxMin(center.x + radius + Px(kLabelGap), center.y - textSize.y * 0.5f - Px(kLabelPadY));
    const ImVec2 boxMax(boxMin.x + textSize.x + Px(kLabelPadX) * 2.0f, boxMin.y + textSize.y + Px(kLabelPadY) * 2.0f);
    // Same pill as RenderActionToast - over arbitrary game content, plain
    // text has no guaranteed contrast to sit against.
    dl->AddRectFilled(boxMin, boxMax, fade(IM_COL32(18, 20, 26, 235)), theme::kRadiusPill);
    dl->AddText(ImVec2(boxMin.x + Px(kLabelPadX), boxMin.y + Px(kLabelPadY)), fade(IM_COL32(240, 242, 245, 255)),
                label);
}

// The demo build's permanent mark (see build::kDemoMode). Drawn in *both*
// edit and view-only mode - it goes wherever the overlay is visible at all,
// and a mark you could drop by pressing the other hotkey wouldn't be one.
// It needs no suppression for screen capture: the platform layer leaves the
// whole overlay window out of what it grabs (see CaptureScreen), so nothing
// this draws can reach a captured image.
//
// It moves, every kDemoWatermarkMoveSeconds. A mark that lives in one
// corner is a mark you stop seeing after a minute and can work around
// permanently - putting one snippet over it and never moving that snippet
// again. Wandering, it has to be dealt with rather than arranged around,
// which is the whole point of a nag.
//
// The screen is divided into a 3x3 grid and each move picks a *different*
// cell, plus a jitter within it: pure randomness lands in nearly the same
// spot often enough to read as the mark being stuck, and picking a new
// cell by stepping 1..8 cells on cannot repeat by construction.
void OverlayApp::DrawDemoWatermark(ImDrawList* drawList, float displayW, float displayH) {
    if constexpr (!build::kDemoMode) {
        // Compiled and type-checked in every build; folded away entirely in
        // the ones where it's false. See build_config.h.in.
        return;
    } else {
        constexpr float kTextSize = 30.0f;
        constexpr float kMargin = 26.0f;
        constexpr float kLineGap = 2.0f;
        constexpr double kMoveSeconds = 10.0;
        constexpr int kGrid = 3;  // cells per axis

        // ImGui's clock only advances while frames are being drawn, so a
        // hidden overlay doesn't burn through positions it never showed -
        // the mark moves ten seconds of *being visible* after the last one.
        const auto move = static_cast<int64_t>(ImGui::GetTime() / kMoveSeconds);
        if (move != demoWatermarkMove_) {
            // One scramble, three uses: which cell to step to, and where in
            // it to sit. Cheap enough to not be worth a real generator, and
            // being a pure function of the move number keeps this
            // reproducible when something looks wrong.
            auto scramble = static_cast<uint32_t>(move) * 2654435761u;
            scramble ^= scramble >> 15;
            scramble *= 2246822519u;
            scramble ^= scramble >> 13;
            // 1..(cells-1), so the new cell is never the current one.
            constexpr int kCells = kGrid * kGrid;
            demoWatermarkCell_ = (demoWatermarkCell_ + 1 + static_cast<int>(scramble % (kCells - 1))) % kCells;
            demoWatermarkJitter_ = ImVec2(static_cast<float>((scramble >> 8) & 0xFF) / 255.0f,
                                           static_cast<float>((scramble >> 16) & 0xFF) / 255.0f);
            demoWatermarkMove_ = move;
        }

        // Faint enough to read as a mark on the glass rather than as
        // content, but not so faint it can be missed on a bright
        // background - which is the whole job.
        const ImU32 color = ToImColor(0xFFFFFFFFu, 0.20f);
        ImFont* font = ImGui::GetFont();
        const char* lines[] = {strings::kDemoTitle, strings::kDemoSubtitle};
        // Measured at the size actually being drawn, not the UI font's -
        // GetFont()->CalcTextSizeA takes the size, ImGui::CalcTextSize
        // doesn't - so the block's own width is the wider of the two lines.
        float blockW = 0.0f;
        for (const char* line : lines) {
            blockW = std::max(blockW, font->CalcTextSizeA(Px(kTextSize), FLT_MAX, 0.0f, line).x);
        }
        const float blockH = 2.0f * Px(kTextSize) + Px(kLineGap);

        // The cell grid covers the positions the block's *top-left* may
        // take, so the whole mark stays inside the margin whichever cell it
        // lands in.
        const float spanX = std::max(0.0f, displayW - 2.0f * Px(kMargin) - blockW);
        const float spanY = std::max(0.0f, displayH - 2.0f * Px(kMargin) - blockH);
        const float cellW = spanX / kGrid;
        const float cellH = spanY / kGrid;
        const float x = Px(kMargin) + static_cast<float>(demoWatermarkCell_ % kGrid) * cellW +
                        demoWatermarkJitter_.x * cellW;
        float y = Px(kMargin) + static_cast<float>(demoWatermarkCell_ / kGrid) * cellH +
                  demoWatermarkJitter_.y * cellH;
        for (const char* line : lines) {
            drawList->AddText(font, Px(kTextSize), ImVec2(x, y), color, line);
            y += Px(kTextSize) + Px(kLineGap);
        }
    }
}

// The "your clicks land here, not in the game" frame - see
// AppConfig::showEditModeBorder. Called only from RenderScreenChrome, which
// is edit-mode-only; view-only mode passes input straight through and so
// has nothing to warn about, and deliberately draws no border of its own.
//
// Inset by half its own width rather than drawn on the screen edge:
// ImDrawList::AddRect centers thickness on the path it's given, so a rect
// at the actual edge would have half of every side clipped away off-screen
// and the border would render at half the width the user asked for.
void OverlayApp::DrawEditModeBorder(ImDrawList* drawList, float displayW, float displayH) const {
    if (!Cfg().showEditModeBorder || Cfg().editModeBorderOpacity <= 0.0f || Cfg().editModeBorderWidthPx <= 0.0f) {
        return;
    }
    if (Cfg().editModeBorderOnlyWhenEmpty) {
        // "Empty" means nothing at all on this canvas - no items (even a
        // blank one still proves the overlay is up) and no un-armed ink on
        // the live layer either. No canvas at all counts as empty too, and
        // is exactly when the cue is worth the most: the screen is
        // otherwise completely blank.
        const Canvas* canvas = Manager().CurrentOrNull();
        const bool anythingShown =
            canvas != nullptr && (!session_.LiveLayer().Strokes().empty() ||
                                  std::any_of(canvas->items.begin(), canvas->items.end(),
                                              [&](const Item& item) { return !Manager().IsDeleted(*canvas, item); }));
        if (anythingShown) {
            return;
        }
    }
    const float half = Cfg().editModeBorderWidthPx * 0.5f;
    drawList->AddRect(ImVec2(half, half), ImVec2(displayW - half, displayH - half),
                       ToImColor(Cfg().editModeBorderColorRGBA, Cfg().editModeBorderOpacity), 0.0f, ImDrawFlags_None,
                       Cfg().editModeBorderWidthPx);
}

// ================= Background: live layer + armed-item overlay + debug =================

void OverlayApp::RenderCanvasLayer(float displayW, float displayH) {
    ImDrawList* drawList = BeginScreenLayer("##spickzettel_canvas", displayW, displayH);

    // First, under everything: the frozen screen, if one is held. Stretched
    // to the display rather than drawn 1:1 so a resolution change between
    // the capture and now scales instead of leaving a gap - it will look
    // soft, but a soft backdrop beats a torn one.
    if (const uint64_t frozen = session_.FrozenScreenTexture(); frozen != 0) {
        drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(frozen)), ImVec2(0.0f, 0.0f),
                            ImVec2(displayW, displayH));
    }

    if (Cfg().showDebugOverlay) {
        DrawDebugOverlay(drawList, ImGui::GetIO(), Manager(), debugHoveredResizeHandle_);
    }

    // The armed item's own in-progress live stroke is drawn as part of
    // RenderItems instead of here - see the comment there for why (a Shot
    // item's opaque fill would otherwise hide it until the stroke
    // finishes).

    EndScreenLayer();
}

// The three things that belong over the canvas rather than in it, each in
// its own layer so their heights can be stated rather than inherited from
// where in the frame they happen to be drawn. Called after everything the
// canvas holds and before the Overview, so the whole group sits between
// them - the Overview is the one panel that covers everything, because it
// is the one you go to when something on screen is in the way.
//
// Bottom to top:
//  - The input options HUD. On the foreground draw list it would sit on
//    top of the Overview - including the Settings tab holding the switch
//    that turns it off.
//  - The edit-mode border, which is the "your clicks land here" cue: a
//    frame drawn under the snippets is a frame a fullscreen snippet hides
//    completely, which is exactly when the cue matters.
//  - The demo mark, above the border and everything below it, so nothing
//    but the Overview can cover it.
//
// Nothing here takes input (see BeginScreenLayer), so none of it changes
// what can be clicked, dragged or drawn on.
void OverlayApp::RenderScreenChrome(float displayW, float displayH) {
    DrawInputOptionsHud(BeginScreenLayer("##sz_input_hud_layer", displayW, displayH));
    EndScreenLayer();

    ImDrawList* chrome = BeginScreenLayer("##sz_chrome_layer", displayW, displayH);
    DrawEditModeBorder(chrome, displayW, displayH);
    DrawDemoWatermark(chrome, displayW, displayH);
    EndScreenLayer();
}


}  // namespace sz::ui
