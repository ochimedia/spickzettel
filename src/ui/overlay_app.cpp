#include "ui/overlay_app.h"

#include "core/util/timestamp_name.h"
#include "generated/ui_strings.h"
#include "ui/icons_generated.h"
#include "ui/theme.h"
#include "ui/widgets.h"

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
    messages_.NewNotice();  // a notice entered now is reported when its own message fades
    if (IsViewOnly() == wasViewOnly) {
        return;
    }
    // Every mode but Edit, the pinned view and a notice included. Those two
    // are the overlay put away, as hidden is, which keeps what edit mode
    // left up - but unlike hidden they draw frames, and ImGui closes a
    // popup that a frame does not draw: a focused window that was not
    // active in the last frame loses focus at the next NewFrame, and losing
    // it closes the popups over it. Ended here, everything ends together,
    // rather than the popup alone behind the machine's back. See
    // docs/OVERLAY_STATES.md, section 10.
    if (IsViewOnly()) {
        // Nothing should stay "in progress" while merely viewing: edit
        // mode starts clean later rather than resuming whatever happened to
        // be up.
        Settle(Scope::All);
        // Normally cleared at the top of every CanvasView::RenderItems call - which
        // view-only mode never runs, so without this the debug overlay's
        // "resize handle:" line would keep showing whatever handle
        // happened to be hovered on the last edit-mode frame for the
        // whole view-only session.
        canvasView_.ForgetHoveredHandle();
    } else {
        // Edit again, and nothing in the hand, as when the overlay comes
        // up: view-only is click-through, and a button let go of there was
        // let go of on whatever is underneath. From View it is the same
        // session and nothing else forgets: a press held into View was
        // still held here, and its Spent swallowed the wheel and every
        // right click until the next left press.
        editor_.ForgetTheHand();
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

void OverlayApp::Settle(Scope scope) {
    // Ending everything above the canvas ends a popup, which asks for
    // itself to be closed once edit mode draws again (see
    // Popup::Interrupt): what was asked of ImGui before that is let go of
    // first, for a frame that will not come in edit mode.
    if (scope == Scope::All) {
        popups_.ForgetEffects();
    }
    // Top down: the gesture first, while drawing mode still says which
    // snippet a stroke in flight belongs to - kept, as a command's scope
    // keeps it; then a note being typed, committed, whose editor may never
    // be drawn again to hear that it closed; and, for All, a popup, a
    // panel, drawing mode or the creation tool in hand.
    editor_.Settle(scope);
    // A slider or swatch in the middle of a drag is not drawn again to say
    // it was let go of, which is where its preview is committed - not
    // before the next showing, and not at all before an exit or in
    // view-only mode: committed now, as its end would have. And the pen
    // as the hand left it: its width before its preview has faded, its
    // color with the chooser up.
    settings_.CommitPreviews();
    KeepPen();
}

bool OverlayApp::PointerOverView() const {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
}

bool OverlayApp::PopupOpen() const {
    return ImGui::GetCurrentContext() != nullptr && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
}

// ================= Input, and the first run =================

namespace {
// The backing a practice snippet gets. Black: it is there to be seen on
// whatever the desktop happens to show, pale or not.
constexpr uint32_t kNoteBackgroundColorRGBA = 0x000000FFu;
constexpr float kNoteBackgroundOpacity = 0.5f;
}  // namespace

void OverlayApp::WelcomeAtStart(LibraryAtStart library) {
    const std::string& progress = settings_.Get(setting::kTutorialWelcome);
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
    if (progress.empty()) {
        welcomePending_ = Welcome::Offer;
    } else if (progress == "offered" || progress == "finished" || progress == "skipped") {
        welcomePending_ = Welcome::Nothing;
    } else {
        welcomePending_ = Welcome::Resume;  // a step's id
    }
}

void OverlayApp::OnHotkey(CommandId command, const platform::KeyCombo& combo) {
    Event event;
    event.kind = EventKind::Hotkey;
    event.command = command;
    event.combo = combo;
    event.modifiers = editor_.Held();
    editor_.Input().Offer(event);
}

void OverlayApp::OnInput(const platform::InputEvent& event) {
    // Real OS-level click-through (see IOverlayWindow::Present)
    // means view-only mode receives no input on Windows; guarded here too so
    // it is read-only on every backend, not just the real one.
    if (IsViewOnly()) {
        return;
    }
    editor_.SetHeld(event.modifiers);
    editor_.SetNow(event.seconds);
    // The display as the last frame saw it, which is what the event's
    // position is in.
    if (ImGui::GetCurrentContext() != nullptr) {
        editor_.SetDisplaySize(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    }
    editor_.Input().Offer(Event::FromInput(event));
}

// ================= What the owners and the editor ask =================

std::optional<ImVec2> OverlayApp::SelectionBarButtonCenter(ChromeButton button) const {
    const std::optional<platform::Vec2> center = editor_.SelectionBarButtonCenter(button);
    return center.has_value() ? std::optional<ImVec2>(ImVec2(center->x, center->y)) : std::nullopt;
}

void OverlayApp::AskToDelete(DeleteTarget target) {
    // Not asked at all where Settings > Behavior says not to: deleted after
    // the draw, as the confirmation's own Delete would be.
    const bool forGood = target.forGood || target.kind == DeleteTarget::Kind::DeletedCanvasesIn;
    if (!(forGood ? Cfg().confirmDeleteForGood : Cfg().confirmDelete)) {
        Act(action::Delete{std::move(target)});
        return;
    }
    popups_.OpenConfirmDelete(std::move(target));
}

void OverlayApp::PerformDelete(const DeleteTarget& target) {
    if (!editor_.Settle(Scope::Canvas)) {  // a command - see Scope
        return;
    }
    const bool forGood = target.forGood || target.kind == DeleteTarget::Kind::DeletedCanvasesIn;
    const bool deletedIn = target.kind == DeleteTarget::Kind::DeletedCanvasesIn;
    // Its textures go as it leaves the screen, either way (see
    // TextureCache), and its history only with the thing itself, for good.
    if (forGood) {
        if (deletedIn ? session_.DeleteMarkedCanvasesPermanently(target.id) : session_.DeletePermanently(target.id)) {
            messages_.Say(strings::kToastDeletedForGood);
        }
    } else if (session_.Delete(target.id)) {
        messages_.Say(strings::kToastDeleted);
    }
}

void OverlayApp::OpenOverview() {
    overview_.Open();
    settingsPage_.OnPanelOpened();
}

void OverlayApp::OpenSettings() {
    overview_.OpenSettings();
    settingsPage_.OnPanelOpened();
}

void OverlayApp::AskToDeleteCanvas(CanvasId canvas) {
    const Canvas* found = Manager().FindCanvas(canvas);
    AskToDelete(DeleteTarget{DeleteTarget::Kind::Canvas, canvas, found != nullptr ? found->name : std::string()});
}

void OverlayApp::OpenPicker(ItemId itemId, bool isCopy) { overview_.OpenPicker(itemId, isCopy); }

void OverlayApp::ToggleCheatSheet() { cheatSheet_.Toggle(); }

void OverlayApp::ClosePanel(PanelKind kind) {
    switch (kind) {
        case PanelKind::Overview:
            overview_.Close();
            return;
        case PanelKind::CheatSheet:
            return;  // nothing of its own to put away
    }
}

void OverlayApp::KeepPen() {
    pointer_.KeepPenWidth();
    popups_.KeepChooserColor();
}

// ================= Frame =================

void OverlayApp::OnFrame(float /*deltaSeconds*/) {
    // The mesh caches' frame, closed on every path out of here - including
    // view-only mode's own early return: a cache left thinking its frame is
    // still running never drops what that frame didn't draw.
    struct MeshCacheFrame {
        CanvasView& view;
        explicit MeshCacheFrame(CanvasView& canvasView) : view(canvasView) { view.BeginFrame(); }
        ~MeshCacheFrame() { view.EndFrame(); }
        MeshCacheFrame(const MeshCacheFrame&) = delete;
        MeshCacheFrame& operator=(const MeshCacheFrame&) = delete;
    };
    const MeshCacheFrame meshCacheFrame(canvasView_);
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    Prepare(display.x, display.y);
    if (IsViewOnly()) {
        // A notice is this same click-through mode with the canvas left
        // out: nothing of the library on screen, only the message. See
        // OverlayMode::Notice.
        if (!IsNoticeOnly()) {
            canvasView_.DrawViewOnly(display.x, display.y, IsPinnedOnly(), [&](ImDrawList* layer) {
                chrome_.DrawDemoMark(layer, display.x, display.y);
            });
        }
        DrawMessages();
        Apply();
        return;
    }
    DrawCanvas(display.x, display.y);
    // 3. Open: what was asked for that only a frame can do - see Popups::Effect -
    // just before the popups it opens are drawn.
    popups_.ApplyEffects();
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
    Front("##tutorial_card");
    Front(AppPopupWindow(PopupKind::ConfirmDelete));
}

void OverlayApp::Prepare(float displayW, float displayH) {
    // The display the canvas is on, for the editor - which draws nothing
    // and so has no other way to know it.
    editor_.SetDisplaySize(displayW, displayH);
    // Nothing is marked on screen until it is drawn in this frame.
    anchors_.Clear();

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
    // message fading, which is all a notice is, or a failed write's warning
    // kept up on a clock (see Messages::Timed). What it shows otherwise
    // changes only through something that arrives as a message, like a
    // capture hotkey, and a message always gets a frame. The debug overlay
    // is the exception, since it follows the pointer.
    {
        const bool timed = messages_.Timed();
        const platform::FramePacing pacing = IsViewOnly() && !IsNoticeOnly() && !timed && !Cfg().showDebugOverlay
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
    canvasView_.KeepTextures();

    // Live, not just at startup - see
    // CanvasManager::SyncItemsToDisplaySize's own doc comment. Cheap: a
    // no-op comparison per item whenever the display hasn't changed since
    // last frame, which is every frame but the one right after an actual
    // change.
    session_.SyncItemsToDisplaySize(displayW, displayH);

    if (IsViewOnly()) {
        // The strokes' bitmaps, as below, since view-only draws them too.
        // Going view-only settles what edit mode left in progress - a
        // stroke in flight is filed, an erase the write refused is rolled
        // back - after edit mode's last frame, and with this skipped the
        // view drew the bitmap from before that: the last stroke missing,
        // until edit mode came back.
        canvasView_.RefreshStrokeRasters();
        return;
    }


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
    canvasView_.RefreshStrokeRasters();

    // Where the panels docked against the screen's edges are this frame, and
    // how far out - before anything is drawn, since the minimized chips
    // the canvas view draws have to clear the ones on the bottom edge.
    canvasBar_.Update(displayW, displayH, popups_.Up(PopupKind::CanvasMenu));

    // What the start decided for the tutorial, on the first frame of edit
    // mode (see WelcomeAtStart) - asked for as an action, like the card's
    // buttons.
    switch (std::exchange(welcomePending_, Welcome::Nothing)) {
        case Welcome::Nothing:
            break;
        case Welcome::Start:
            Act(action::StartTutorial{});
            break;
        case Welcome::Resume:
            Act(action::ResumeTutorial{});
            break;
        case Welcome::Offer:
            tutorialCard_.Offer();
            break;
    }

    // The tutorial's step brought up to date with what the input above
    // did - its own state, and nothing else.
    tutorialCard_.Update(ImGui::GetTime());
}

void OverlayApp::DrawCanvas(float displayW, float displayH) {
    canvasView_.Draw(displayW, displayH, popups_.ItemOf(PopupKind::ItemProperties), canvasBar_.BottomPanelsTop());
    // Over the items, and under the popups.
    canvasBar_.Draw(displayW, displayH);
}

void OverlayApp::DrawPopups(float displayW, float displayH) { popups_.DrawOverCanvas(displayW, displayH); }

void OverlayApp::DrawOverCanvas(float displayW, float displayH) {
    pointer_.DrawOverCanvas();
    // Over everything the canvas holds, under the Overview - see its own
    // doc comment for what is in it and in which order.
    chrome_.Draw(displayW, displayH);
}

void OverlayApp::DrawPanels(float displayW, float displayH) {
    overview_.Draw(displayW, displayH, [this] { settingsPage_.Draw(); });
    cheatSheet_.Draw(displayW, displayH);
    // Above the panels, so a step can talk about them, and below the
    // delete confirmation, which must stay reachable.
    tutorialCard_.Draw(displayW, displayH);
    popups_.DrawConfirmDelete();
}

void OverlayApp::DrawMessages() {
    // Drawn in view-only too, not just in edit mode where it started. A
    // hotkey that acts while the overlay is merely being looked through
    // still did something, and this is the only thing that says so - it
    // costs a frame that was being drawn anyway, and click-through means it
    // cannot get in the way of anything.
    messages_.Draw();
    // Like the messages, it changes nothing - and only where the card is.
    if (!IsViewOnly()) {
        tutorialCard_.DrawSpotlight();
    }
}

void OverlayApp::DrawPointer() {
    pointer_.Draw();
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
    KeepTutorialProgress();

    if (IsViewOnly()) {
        // A notice exists only to carry its message, so it is over when the
        // message is. Reported once; the window goes away on the other end.
        if (IsNoticeOnly() && messages_.NoticeJustFinished() && noticeFinishedCallback_) {
            noticeFinishedCallback_();
        }
        return;
    }
    pointer_.Apply();
    chrome_.Update(/*editMode=*/true);
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
                       overview_.ForgetDeletedFolderShown();
                   },
                   [&](const action::ShowDeletedFolder& a) { overview_.ShowDeletedFolder(a.folder); },
                   [&](const action::ReorderFolder& a) { session_.ReorderFolder(a.folder, a.place); },
                   [&](const action::ReorderCanvas& a) { session_.ReorderCanvas(a.canvas, a.place); },
                   [&](const action::MoveCanvasToFolder& a) { session_.MoveCanvasToFolder(a.canvas, a.folder); },
                   [&](const action::RenameFolder& a) { session_.RenameFolder(a.folder, a.name); },
                   [&](const action::RenameCanvas& a) { session_.RenameCanvas(a.canvas, a.name); },
                   [&](const action::NewFolder&) {
                       // Named for when it was made (see TimestampName), and at
                       // the end of the sidebar - which is where the eye goes
                       // after pressing a button at the bottom of it, and matches
                       // where a new canvas lands in its own list. Nothing more
                       // when it could not be written: the canvas below would
                       // land in the folder browsed before.
                       const FolderId folder = session_.AddFolder(TimestampName());
                       if (folder == 0) {
                           return;
                       }
                       overview_.ScrollToFolder(folder);
                       overview_.ForgetDeletedFolderShown();
                       // With a canvas already in it. A folder is where canvases
                       // live, so an empty one is a step rather than a result, and
                       // an empty folder reads as a dead end: no tile to click,
                       // nothing to drop an item onto. AddFolder has already made
                       // the new folder current, so this lands inside it - and,
                       // like New canvas, the canvas it makes is switched to.
                       if (const CanvasId canvas = editor_.CreateCanvasInCurrentFolder(); canvas != 0) {
                           editor_.SwitchCanvas(canvas);
                       }
                   },
                   [&](const action::NewCanvas&) {
                       // At the end of the folder, so the grid may have to scroll
                       // for it to be seen at all - see CanvasMade, which
                       // CreateCanvasInCurrentFolder calls either way: the tile
                       // is what the click was about.
                       const CanvasId id = editor_.CreateCanvasInCurrentFolder();
                       if (id == 0) {
                           return;  // not written
                       }
                       if (overview_.Picking()) {
                           // A destination for the snippet being sent away, and not
                           // switched to: following it would take the user off the
                           // canvas they were working on.
                           overview_.SendPickedItemTo(id);
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
                   [&](const action::SendPicked& a) { overview_.SendPickedItemTo(a.canvas); },
                   [&](const action::Restore& a) {
                       if (session_.Restore(a.id)) {
                           messages_.Say(strings::kToastRestored);
                           overview_.SettleDeletedFolderShown();
                       }
                   },
                   [&](const action::Delete& a) { PerformDelete(a.target); },
                   [&](const action::RestoreMinimized& a) { session_.SetMinimized({a.item}, false); },
                   [&](const action::ClosePanel& a) {
                       if (a.panel == PanelKind::Overview) {
                           overview_.Close();
                       } else if (IsCheatSheetOpen()) {
                           editor_.Input().End(Level::Panel);
                       }
                   },
                   [&](const action::FinishNoteEdit& a) { editor_.EndEditingNote(a.text); },
                   [&](const action::TutorialPress& a) {
                       // Either answer to the offer is kept, so it is made once.
                       if (a.button == TutorialButton::NoThanks) {
                           settings_.Set(setting::kTutorialWelcome, std::string("offered"));
                       }
                       const FolderId folder = tutorialCard_.Runner().Folder();
                       const bool on = tutorialCard_.Runner().On();
                       tutorialCard_.Press(a.button);
                       // Done ends the tutorial with its folder in the trash,
                       // asked first where Settings says to, as any folder's
                       // Delete is (question 9). Done, keep the folder keeps it.
                       if (a.button == TutorialButton::Done && on && !tutorialCard_.Runner().On()) {
                           if (const Folder* found = Manager().FindFolder(folder);
                               found != nullptr && !Manager().IsDeleted(*found)) {
                               AskToDelete(DeleteTarget{DeleteTarget::Kind::Folder, folder, found->name});
                           }
                       }
                   },
                   [&](const action::StartTutorial& a) {
                       // From Settings, which is in the Overview: the tutorial
                       // is about the canvas.
                       overview_.Close();
                       const tutorial::Topic* topic = tutorial::FindTopic(a.topic);
                       if (topic == nullptr) {
                           topic = tutorial::FindTopic(tutorial::kBasicsTopic);
                       }
                       if (const FolderId folder = MakeTutorialFolder(*topic); folder != 0) {
                           tutorialCard_.Start(*topic, folder);
                       }
                   },
                   [&](const action::ResumeTutorial&) {
                       if (const FolderId folder = GoToTutorialFolder(settings_.Get(setting::kTutorialFolder));
                           folder != 0) {
                           tutorialCard_.Resume(tutorialCard_.CurrentTopic(), settings_.Get(setting::kTutorialWelcome),
                                                folder);
                       }
                   },
                   [&](const action::BackToTutorial&) {
                       const FolderId folder = tutorialCard_.Runner().Folder();
                       if (const FolderId now = GoToTutorialFolder(folder); now != 0 && now != folder) {
                           tutorialCard_.MoveTo(now);
                       }
                   },
                   [&](const action::PracticeSnippet&) { PlacePracticeSnippet(); },
               },
               action);
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

FolderId OverlayApp::GoToTutorialFolder(FolderId folder) {
    for (const Canvas& canvas : Manager().Canvases()) {
        if (folder != 0 && canvas.folderId == folder && !Manager().IsDeleted(canvas)) {
            editor_.SwitchCanvas(canvas.id);
            return folder;
        }
    }
    // Gone - deleted, or never in this library - and a new one to go on in.
    return MakeTutorialFolder(tutorialCard_.CurrentTopic());
}

void OverlayApp::KeepTutorialProgress() {
    // Where the runner is once it has moved on for the frame - by itself in
    // Prepare, or at a button just done. Only a change is set: a Set is a
    // commit, which the tray writes to the file. A runner that has never
    // run says nothing, and leaves "offered" as it is.
    const tutorial::Tutorial& runner = tutorialCard_.Runner();
    if (std::string progress = runner.Progress();
        !progress.empty() && progress != settings_.Get(setting::kTutorialWelcome)) {
        settings_.Set(setting::kTutorialWelcome, std::move(progress));
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

void OverlayApp::OnOverlayShown() {
    tutorialWorld_.CountShowing();
    settingsPage_.OnOverlayShown();
    // Something the app did while nobody was looking - see
    // SayDeletedForGoodAtStart.
    messages_.OnOverlayShown();
    // Nothing is in the hand as the overlay comes up: what went down before
    // it was hidden has come up since, wherever that release went. Settled
    // already when it was put away, unless it went some other way.
    Settle(Scope::Hand);
    editor_.ForgetTheHand();
    // The panels docked against the edges come out for a moment, so they
    // are seen where they are - asked for here, done on the first frame.
    canvasBar_.Flash();
    // While the overlay was away, whatever is underneath owned the pointer
    // and will have installed its own shape. What ApplyPointerShape last
    // asked for therefore says nothing about what is on screen now, and
    // skipping the push because "it hasn't changed" would leave the other
    // application's cursor over our canvas until the next mouse move.
    pointer_.OnOverlayShown();
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
    // Guarded like Messages::Say: the overlay can be shown by a hotkey
    // pressed before a single frame has ever been drawn.
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
    }
}


}  // namespace sz::ui
