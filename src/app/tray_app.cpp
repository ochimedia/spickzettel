#include "app/tray_app.h"

#include <iterator>
#include <optional>
#include <utility>

#include "core/config/display_choice.h"
#include "core/util/timestamp_name.h"

namespace sz::app {

TrayController::TrayController(platform::IPlatformHost& host, AppConfig config)
    : host_(host),
      settings_(std::move(config)),
      libraryStore_(host.GetDataDirectoryPath()),
      overlayApp_(settings_, session_) {}

bool TrayController::Initialize() {
    host_.SetTrayCommandCallback([this](platform::TrayCommand cmd) { OnTrayCommand(cmd); });

    // Also fine to call again later, after the window exists - see
    // IOverlayWindow::SetEditModeNoActivate - but seeding it here, before
    // the window is ever created (the first EnsureCreated() call,
    // triggered by any of the hotkeys below), means edit mode already
    // behaves correctly on its very first show rather than only from the
    // next settings change onward.
    liveEditModeNoActivate_ = settings_.Live().dontStealFocus;
    host_.GetOverlayWindow().SetEditModeNoActivate(liveEditModeNoActivate_);
    // Same reasoning: seeded before the window exists so the very first
    // show already behaves as configured. Nothing is grabbed until the
    // overlay is actually visible in edit mode - the window decides when
    // these apply, this only says what they are. A matching profile
    // replaces both on the way up (ApplyProfileForCurrentApplication);
    // these are what runs when none does.
    liveEditModeInput_ = settings_.Live().InputOptions();
    host_.GetOverlayWindow().SetEditModeInput(liveEditModeInput_);

    // Before anything else - before the tray icon, which would show two
    // icons for a moment, and long before the library is loaded, which is
    // what this protects: two copies of the app would each write the
    // library from a picture of it the other keeps changing.
    if (!host_.AcquireSingleInstance()) {
        return false;
    }
    if (!host_.ShowTrayIcon()) {
        return false;
    }

    // Two of the app's own hotkeys on one combination would register once
    // and fail once, and a failure below refuses to start - so a later
    // duplicate of an earlier combination is unbound first, and the file is
    // corrected so it stays that way. What ChangeHotkey does for an edit
    // made in the app, done here for a file edited by hand.
    {
        AppConfig& stored = settings_.Mutable();
        platform::KeyCombo* slots[] = {&stored.hotkeyEditMode, &stored.hotkeyViewMode, &stored.hotkeyQuickCapture,
                                       &stored.hotkeySilentCapture};
        bool unboundAny = false;
        for (size_t later = 1; later < std::size(slots); ++later) {
            for (size_t earlier = 0; earlier < later; ++earlier) {
                if (slots[later]->IsValid() && *slots[later] == *slots[earlier]) {
                    *slots[later] = platform::KeyCombo{};
                    unboundAny = true;
                }
            }
        }
        if (unboundAny) {
            PersistConfig();
        }
    }

    // A set combination that cannot be registered - another application
    // owns it - is fatal for the three that bring the overlay up: there is
    // no point starting without them. An unset one registers as 0 too and
    // is not a failure: a hotkey given another row's combination is left
    // unbound (see ChangeHotkey), and the app has to come back up with it
    // that way. The tray menu still reaches edit mode.
    const auto registered = [](const platform::KeyCombo& combo, int id) { return id != 0 || !combo.IsValid(); };
    editHotkeyId_ = host_.RegisterGlobalHotkey(settings_.Stored().hotkeyEditMode, [this]() { OnEditHotkey(); });
    if (!registered(settings_.Stored().hotkeyEditMode, editHotkeyId_)) {
        return false;
    }
    viewHotkeyId_ = host_.RegisterGlobalHotkey(settings_.Stored().hotkeyViewMode, [this]() { OnViewHotkey(); });
    if (!registered(settings_.Stored().hotkeyViewMode, viewHotkeyId_)) {
        return false;
    }
    quickCaptureHotkeyId_ =
        host_.RegisterGlobalHotkey(settings_.Stored().hotkeyQuickCapture, [this]() { OnQuickCaptureHotkey(); });
    if (!registered(settings_.Stored().hotkeyQuickCapture, quickCaptureHotkeyId_)) {
        return false;
    }
    // Not checked at all - this one is an extra whose combination another
    // application may already own. Leaving it unregistered costs that one
    // hotkey; refusing to start costs the whole app.
    silentCaptureHotkeyId_ =
        host_.RegisterGlobalHotkey(settings_.Stored().hotkeySilentCapture, [this]() { OnSilentCaptureHotkey(); });

    session_.AttachWindow(&host_.GetOverlayWindow());
    overlayApp_.AttachTo(host_.GetOverlayWindow());
    host_.SetSessionEndCallback([this] { OnSessionEnding(); });
    host_.GetOverlayWindow().SetDisplaysChangedCallback([this] { OnDisplaysChanged(); });
    settings_.SetChangedCallback([this] { OnSettingsChanged(); });
    overlayApp_.SetHotkeyChangeCallback([this](HotkeySlot slot, platform::KeyCombo combo) {
        return ChangeHotkey(slot, combo);
    });
    overlayApp_.SetRestartOverlayCallback([this] { RestartOverlay(); });
    overlayApp_.SetDisplayListCallback([this] { return host_.ListDisplays(); });
    overlayApp_.SetNoticeFinishedCallback([this] { HideNoticeIfDone(); });

    // Empty path means "this host has nowhere to persist to" (e.g. a
    // FakePlatformHost in a test that hasn't opted in) - leave the
    // session without a library store entirely, matching how it
    // behaves before this feature existed (no autosave, no image writes).
    bool freshInstall = false;
    if (!host_.GetDataDirectoryPath().empty()) {
        session_.SetLibraryStore(&libraryStore_);
        if (std::optional<CanvasManagerSnapshot> snapshot = libraryStore_.Load()) {
            session_.ImportLibrary(std::move(*snapshot));
        } else {
            // Nothing on disk to load: a genuinely first run. Distinct from
            // a library someone deliberately emptied, which loads fine and
            // reports itself as empty (see LibraryStore::IsValid) - that
            // person has already met the app and shouldn't be greeted
            // again. Load() also returns nothing for a corrupt file, where
            // showing the welcome note is the right call anyway.
            freshInstall = true;
        }
        // No eager display-size reconciliation here anymore - OverlayApp::OnFrame
        // now does that live, every frame, against ImGui's own DisplaySize (see
        // CanvasManager::SyncItemsToDisplaySize's own doc comment). Items
        // simply sit at their last-saved rect until the overlay is first shown
        // and a frame actually renders, which is also the first moment they'd
        // ever be visible.
    }

    if (freshInstall) {
        // Show ourselves rather than waiting to be summoned. Every other
        // start is a deliberate hotkey press, but on a first run nobody
        // knows the hotkey yet - an app that installs a tray icon and then
        // sits there invisibly, waiting for a chord it never mentioned, is
        // indistinguishable from one that didn't start. Edit mode
        // specifically, not view-only: the note explains how to interact,
        // so interaction has to be possible.
        overlayApp_.RequestWelcomeNote();
        EnsureMode(/*viewOnly=*/false);
    } else {
        // Pinned snippets are on screen whenever the overlay is away, and
        // having just started is one of those times.
        ShowPinnedView();
    }

    return true;
}

bool TrayController::CompletesAHotkeyCapture(const platform::KeyCombo& combo) {
    if (!overlayApp_.IsCapturingHotkey()) {
        return false;
    }
    overlayApp_.CompleteHotkeyCapture(combo);
    return true;
}

void TrayController::OnEditHotkey() {
    if (CompletesAHotkeyCapture(settings_.Stored().hotkeyEditMode)) {
        return;
    }
    ToggleMode(/*viewOnly=*/false);
}

void TrayController::OnViewHotkey() {
    if (CompletesAHotkeyCapture(settings_.Stored().hotkeyViewMode)) {
        return;
    }
    ToggleMode(/*viewOnly=*/true);
}

void TrayController::OnQuickCaptureHotkey() {
    if (CompletesAHotkeyCapture(settings_.Stored().hotkeyQuickCapture)) {
        return;
    }
    // Idempotent and invisible if already created (see EnsureCreated's own
    // contract) - critically, this doesn't call Show() itself, so the
    // capture below happens against whatever was on screen before this
    // hotkey fired (the hidden desktop, or the already-visible overlay),
    // never against the overlay's own edit-mode UI.
    if (!PrepareWindow()) {
        return;
    }
    overlayApp_.QuickCapture(static_cast<float>(overlayDisplay_.width), static_cast<float>(overlayDisplay_.height));
    // So the user actually notices the capture happened - always lands in
    // edit mode (never toggles it off, unlike the edit hotkey itself).
    // Hardcoded for now; could become configurable later if silent
    // capture is wanted sometimes too.
    EnsureMode(/*viewOnly=*/false);
}

void TrayController::OnSilentCaptureHotkey() {
    if (CompletesAHotkeyCapture(settings_.Stored().hotkeySilentCapture)) {
        return;
    }
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    // Same first step as the loud capture: create the window if it has
    // never been created, without showing it, so the capture below is of
    // whatever was on screen and never of us.
    if (!PrepareWindow()) {
        return;
    }
    const bool wasVisible = window.IsVisible();
    // The capture goes onto a canvas of its own as always, but in the
    // pinned view the overlay stays on the canvas it was showing: that
    // canvas's pinned snippets are what is on screen, and switching to the
    // new one - which has none - would take them away in the middle of
    // whatever they were pinned for.
    const std::optional<CanvasId> stayOn = overlayApp_.IsPinnedOnly()
                                               ? std::optional<CanvasId>(session_.Manager().CurrentCanvasId())
                                               : std::nullopt;
    overlayApp_.QuickCapture(static_cast<float>(overlayDisplay_.width), static_cast<float>(overlayDisplay_.height));
    if (stayOn.has_value()) {
        session_.Manager().SwitchToCanvas(*stayOn);
        // The capture's texture is on a canvas nobody is looking at now.
        // The frame's own sync only runs when the current canvas changes,
        // and from its point of view it has not: this is the same canvas
        // it last synced.
        session_.SyncTexturesToCurrentCanvas();
    }
    if (wasVisible) {
        // Already on screen, in either mode: the message lands in the next
        // frame it was going to draw anyway, and nothing about the mode or
        // the window changes. Notably this is the view-only case, where
        // saying so costs nothing at all.
        return;
    }
    // Hidden, and staying hidden unless a notice goes up for two seconds:
    // the picture is on disk (see Session::CaptureShotItem) but the record
    // that names it is not, and no frame is coming to run the autosave.
    // Written now, so a crash before the overlay is next shown does not
    // lose the capture - or, worse, leave the picture as an orphan the
    // next save sets aside.
    FlushOrRetryLater();
    if (!settings_.Stored().showToastsWhileHidden) {
        // Nothing will ever draw it, so drop it rather than leave it
        // queued for whenever the overlay next comes up - see
        // DismissActionToast.
        overlayApp_.DismissActionToast();
        return;
    }
    ShowNotice();
}

void TrayController::ShowNotice() {
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    if (window.IsVisible()) {
        return;
    }
    if (!PrepareWindow()) {
        return;
    }
    overlayApp_.SetViewOnly(true);
    overlayApp_.SetNoticeOnly(true);
    // Shown first, click-through second - the same order view-only mode
    // uses, and it is not a free choice. Turning passthrough on adds
    // WS_EX_LAYERED, and a window that gets that style before it has ever
    // been shown draws nothing at all: it comes up, stays up for its two
    // seconds and goes away again, with no message on it. Found exactly
    // that way round.
    //
    // Which leaves focus, the thing the other order was for: showing a
    // window activates it on Windows, so a plain Show would take focus
    // from whatever the user is typing in. Hence the separate
    // ShowWithoutActivating - and no focus handoff is needed afterwards,
    // since the focus never moved.
    //
    // No ApplyProfileForCurrentApplication either: a notice is not a
    // session with the application underneath, it draws one message and
    // leaves, so there is nothing for a profile's input settings to apply
    // to - and swapping them would tear input hooks up and down for it.
    window.ShowWithoutActivating();
    window.SetInputPassthrough(true);
}

void TrayController::HideNoticeIfDone() {
    if (!overlayApp_.IsNoticeOnly()) {
        return;  // a real mode took over while the message was up
    }
    overlayApp_.SetNoticeOnly(false);
    // The capture that caused this notice is a library change, and frames
    // stop the moment the window goes - so the same explicit flush every
    // other way out of the overlay does (see ToggleMode).
    FlushOrRetryLater();
    // Called from inside OverlayApp::OnFrame, which is inside the frame
    // callback: safe, because the renderer ends the ImGui frame after that
    // callback returns whether or not the window is still visible.
    host_.GetOverlayWindow().Hide();
}

void TrayController::ToggleMode(bool viewOnly) {
    platform::IOverlayWindow& window = host_.GetOverlayWindow();

    // Already showing in exactly the requested mode - that mode's own
    // hotkey puts it away, per the state table in the header.
    //
    // A notice does not count as being in view-only mode for this, though
    // it is one underneath (see OverlayApp::SetNoticeOnly): pressing the
    // view hotkey while a message happens to be up asks for view mode, and
    // getting the overlay hidden instead - because a two-second window the
    // user never asked for was on screen - would be nonsense. The pinned
    // view doesn't count either: it is the overlay put away already.
    if (window.IsVisible() && !overlayApp_.IsNoticeOnly() && !overlayApp_.IsPinnedOnly() &&
        overlayApp_.IsViewOnly() == viewOnly) {
        PutAway();
        return;
    }
    EnsureMode(viewOnly);
}

void TrayController::EnsureMode(bool viewOnly, bool keepProfileContext) {
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    // Whatever brought us here supersedes a notice that happens to be up -
    // otherwise its own "the message has faded" would hide the overlay a
    // moment after someone deliberately opened it. Cleared before Show
    // below, so the frames this mode draws are never notice frames.
    overlayApp_.SetNoticeOnly(false);
    // The pinned view is the overlay put away, so leaving it for edit mode
    // is coming up from hidden - with a profile, focus taken if it is to be,
    // and everything else a real show does - rather than switching in place
    // the way view-only does. View-only itself is the exception: the pinned
    // view already is view-only and click-through, and has only to start
    // drawing the rest of the canvas.
    if (overlayApp_.IsPinnedOnly()) {
        overlayApp_.SetPinnedOnly(false);
        if (!viewOnly) {
            window.Hide();
        }
    }
    if (!window.IsVisible()) {
        if (!PrepareWindow()) {
            return;
        }
        ApplyProfileForCurrentApplication(keepProfileContext);
        window.Show();
        // Only on the branch that actually brought it back from hidden - the
        // other one is already on screen and has lost nothing. See
        // OverlayApp::OnOverlayShown.
        overlayApp_.OnOverlayShown();
    }
    // Otherwise already visible in the *other* mode (or already the
    // requested one) - switch/stay in place, no hide/reshow (avoids the
    // flicker and refocus that would cause).
    overlayApp_.SetViewOnly(viewOnly);
    window.SetInputPassthrough(viewOnly);
    // After SetViewOnly, which decides whether freezing applies at all, and
    // after Show, so the capture's own hide/show cycle starts from the
    // state we're actually going to be in. Re-captured on every entry, so
    // what you see frozen is always what was on screen a moment ago.
    // The still picture edit mode shows instead of the live application -
    // see AppConfig::freezeScreenInEditMode. Re-captured on every entry, so
    // it is never stale; never in view-only mode, which is click-through
    // and has to show what is really underneath.
    session_.ReleaseFrozenScreen();
    if (settings_.Live().freezeScreen && !viewOnly) {
        session_.FreezeScreen(overlayDisplay_);
    }
}

void TrayController::PutAway() {
    // OnFrame (and so the debounced autosave check inside it) only runs
    // while visible - and in the pinned view nothing is edited - so going
    // away is a safe point that needs its own explicit flush: the next
    // debounce window might otherwise never arrive. Settled first, so the
    // flush has what was being typed or drawn - see SettleForPersistence.
    overlayApp_.SettleForPersistence();
    FlushOrRetryLater();
    session_.ReleaseFrozenScreen();
    if (overlayApp_.IsViewOnly() && session_.Manager().CurrentCanvasHasPinnedItems()) {
        // Already click-through and unfocused: only what is drawn changes.
        overlayApp_.SetPinnedOnly(true);
        return;
    }
    host_.GetOverlayWindow().Hide();
    ShowPinnedView();
}

namespace {
// How long between attempts at a save that failed while the overlay is
// hidden. Generous: a disk that is full or a file that is held open does
// not clear itself in a hurry, and each attempt is a synchronous write on
// the app thread.
constexpr int kHiddenSaveRetryMs = 10000;
}  // namespace

void TrayController::FlushOrRetryLater() {
    // Landed and nothing owed - a removal still pending counts as owed,
    // though the flush that recorded it counted; so does a settings file
    // that could not be written - or scheduled again.
    if (session_.Flush() && !session_.HasUnsavedChanges() && !configWriteOwed_) {
        host_.SetBackgroundTimer(0, nullptr);
        return;
    }
    host_.SetBackgroundTimer(kHiddenSaveRetryMs, [this] { OnBackgroundTimer(); });
}

bool TrayController::FlushForShutdown() {
    overlayApp_.SettleForPersistence();
    if (configWriteOwed_) {
        PersistConfig();  // owed since a settings edit; the last chance for it too
    }
    // Twice: the first attempt may have been what cleared the way - a
    // pending removal finished, a picture's directory made - and a second
    // is cheap against what the alternative costs.
    if (session_.Flush() || session_.Flush()) {
        return true;
    }
    const std::optional<std::filesystem::path> recovery = RecoveryCopyPath();
    const bool recovered = recovery.has_value() && session_.WriteRecoveryCopy(*recovery);
    // ACCEPTED OUTCOME: when the library cannot be written twice over and
    // the recovery copy beside it cannot be written either - a full
    // volume, an unwritable parent - what is in memory is lost when the
    // caller goes on to quit. That is a decision, not an oversight: the
    // exit was asked for, the OS's session end cannot be held up, the
    // tray has no window of its own to ask in, and holding a process open
    // against an explicit exit was judged worse than losing what three
    // attempts at two destinations could not write. The result is
    // returned so a caller can say so where it has somewhere to say it.
    return recovered;
}

void TrayController::OnSessionEnding() { FlushForShutdown(); }

std::optional<std::filesystem::path> TrayController::RecoveryCopyPath() const {
    const std::filesystem::path library = host_.GetDataDirectoryPath();
    if (library.empty()) {
        return std::nullopt;
    }
    // "library-recovery-2026-09-19-22-36-14", beside "library": the same
    // place, which is where someone looking for their work will look, and
    // spelled so that it sorts after the library and reads as what it is.
    std::string stamp = TimestampName();
    for (char& c : stamp) {
        if (c == ' ' || c == ':') {
            c = '-';
        }
    }
    return library.parent_path() / (library.filename().string() + "-recovery-" + stamp);
}

void TrayController::OnBackgroundTimer() {
    if (configWriteOwed_) {
        PersistConfig();
    }
    // The library only while hidden: up again, frames are running and the
    // autosave's own clock, with its backoff, is the one to use.
    const bool visible = host_.GetOverlayWindow().IsVisible();
    if (!visible && session_.HasUnsavedChanges()) {
        session_.Flush();
    }
    if (!configWriteOwed_ && (visible || !session_.HasUnsavedChanges())) {
        host_.SetBackgroundTimer(0, nullptr);
    }
}

bool TrayController::ShowPinnedView() {
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    if (window.IsVisible() || !session_.Manager().CurrentCanvasHasPinnedItems()) {
        return false;
    }
    if (!PrepareWindow()) {
        return false;
    }
    overlayApp_.SetNoticeOnly(false);
    overlayApp_.SetViewOnly(true);
    overlayApp_.SetPinnedOnly(true);
    // Shown first, click-through second, and without taking focus - the
    // order and the call a notice uses, for the reasons given there.
    window.ShowWithoutActivating();
    window.SetInputPassthrough(true);
    return true;
}

platform::DisplayInfo TrayController::OverlayDisplay() const {
    return ChooseDisplay(host_.ListDisplays(), settings_.Stored().overlayDisplayId,
                         settings_.Stored().overlayDisplayName);
}

bool TrayController::PrepareWindow() {
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    // The width check is for a window shown before anything here placed it.
    if (!window.IsVisible() || overlayDisplay_.width <= 0) {
        overlayDisplay_ = OverlayDisplay();
    }
    if (!window.EnsureCreated(overlayDisplay_)) {
        return false;
    }
    window.MoveToDisplay(overlayDisplay_);
    return true;
}

void TrayController::OnDisplaysChanged() {
    // Decided again whether the overlay is up or not: the display it is on
    // may be gone or a different size now, and the one it belongs on may
    // just have come back. A hidden window is moved too, so the next capture
    // taken without showing it is already in the right place.
    MoveOverlayTo(OverlayDisplay());
}

void TrayController::MoveOverlayTo(const platform::DisplayInfo& display) {
    // Most changes are to some other display, and retaking a frozen screen
    // for those would flash the overlay off and on for nothing.
    if (display == overlayDisplay_) {
        return;
    }
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    overlayDisplay_ = display;
    window.MoveToDisplay(display);
    // The same conditions EnsureMode freezes under.
    if (window.IsVisible() && !overlayApp_.IsNoticeOnly() && !overlayApp_.IsViewOnly() &&
        settings_.Live().freezeScreen) {
        session_.FreezeScreen(display);
    }
}

// Resolved once, on the way up, and not again while the overlay is
// showing. The alternative - re-resolving whenever the foreground changes -
// would tear the input hooks down and put them back mid-session, and
// "which profile am I in" would stop being a question with one answer for
// as long as the panel is open.
bool TrayController::MustTakeFocusFrom(const platform::ForegroundApp& app) const {
    // Only a *positive* reading. Unknown is the answer for a process that
    // refuses the question, and the two things that refuse it want
    // opposite treatment: an elevated tool would want focus taken, a game
    // behind an anti-cheat driver must never have it taken. Leaving
    // Unknown alone keeps the game right and costs the tool nothing it
    // had, since without this it had nothing anyway.
    return settings_.Live().takeFocusOverElevated &&
           app.integrity == platform::ForegroundIntegrity::Above;
}

void TrayController::ApplyProfileForCurrentApplication(bool keepPrevious) {
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    // Asked before Show, which is what makes it the *underlying*
    // application rather than this window - except on a restart, which is
    // still the same showing and keeps the answer it already had. See
    // sessionApp_ for what asking twice cost.
    if (!keepPrevious || !sessionApp_.has_value()) {
        sessionApp_ = window.UnderlyingApplication();
    }
    // Handing it to the settings is what matches a profile and derives the
    // values that will run - see Settings::SetUnderlyingApplication.
    settings_.SetUnderlyingApplication(*sessionApp_);
    // Both have a real runtime effect beyond what OverlayApp renders, and
    // both are set before the window is shown: no-activate decides how it
    // is shown at all, and the input options decide what gets hooked.
    liveEditModeNoActivate_ = settings_.Live().dontStealFocus && !MustTakeFocusFrom(*sessionApp_);
    liveEditModeInput_ = settings_.Live().InputOptions();
    window.SetEditModeNoActivate(liveEditModeNoActivate_);
    window.SetEditModeInput(liveEditModeInput_);
}

void TrayController::RestartOverlay() {
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    if (!window.IsVisible()) {
        return;
    }
    // Same teardown a deliberate hide does - the frozen image belongs to
    // the session being ended, and hiding is the safe point autosave needs.
    const bool viewOnly = overlayApp_.IsViewOnly();
    overlayApp_.SettleForPersistence();
    session_.Flush();
    session_.ReleaseFrozenScreen();
    window.Hide();
    // The one show that isn't one: same session, same profile, and
    // deliberately no second look at what is underneath - see sessionApp_.
    EnsureMode(viewOnly, /*keepProfileContext=*/true);
}

void TrayController::OnSettingsChanged() {
    // The two runtime side effects, gated on an actual change so toggling
    // some unrelated setting doesn't re-poke the window or tear the input
    // hooks down and put them straight back up. Compared against what is
    // *live* rather than against what is stored, which is not the live
    // value whenever a profile is overriding it.
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    if (settings_.Live().dontStealFocus != liveEditModeNoActivate_) {
        liveEditModeNoActivate_ = settings_.Live().dontStealFocus;
        window.SetEditModeNoActivate(liveEditModeNoActivate_);
    }
    if (settings_.Live().InputOptions() != liveEditModeInput_) {
        liveEditModeInput_ = settings_.Live().InputOptions();
        window.SetEditModeInput(liveEditModeInput_);
    }
    // A monitor chosen in Settings takes the overlay there at once - and the
    // overlay is always up when that happens, since its own Settings panel is
    // where the choice is made. For every other setting the display this
    // resolves to is the one the overlay is already on, and nothing moves.
    if (window.IsVisible()) {
        MoveOverlayTo(OverlayDisplay());
    }
    PersistConfig();
}

void TrayController::PersistConfig() {
    const std::filesystem::path path = host_.GetConfigFilePath();
    if (path.empty()) {
        return;  // nowhere to persist to - see FakePlatformHost
    }
    // Said on screen while it stays true: a setting that appears applied
    // and is back to its old value at the next start is the kind of thing
    // nobody connects to a full disk a week later. And tried again from
    // the timer, whether or not the overlay is up: a settings edit is
    // rare, and one that failed used to stay unwritten until the next.
    const bool written = WriteConfigFile(path, settings_.Stored());
    overlayApp_.SetConfigWriteFailed(written ? std::nullopt : std::optional<std::string>(path.string()));
    configWriteOwed_ = !written;
    if (configWriteOwed_) {
        host_.SetBackgroundTimer(kHiddenSaveRetryMs, [this] { OnBackgroundTimer(); });
    }
}

bool TrayController::ChangeHotkey(HotkeySlot slot, platform::KeyCombo combo) {
    // No modifier-required check here (deliberately, for now - see
    // RenderHotkeyEditor's own doc comment): a bare key like F9 does hijack
    // every press of that key system-wide, but the user explicitly wants
    // that left open to experiment with rather than blocked outright.
    // Disallowing a *plain letter/digit* with no modifier (as opposed to a
    // function key, which has no other use bound to it) may still be worth
    // adding later.
    // One row per slot rather than a case per slot naming the others by
    // hand: with four of them, "everything except me" written out three
    // times each was four chances to forget one, and the collision check
    // below wants that set anyway.
    struct HotkeyRow {
        HotkeySlot slot;
        int* id;
        platform::KeyCombo* configField;
        platform::HotkeyCallback callback;
    };
    const HotkeyRow rows[] = {
        {HotkeySlot::EditMode, &editHotkeyId_, &settings_.Mutable().hotkeyEditMode, [this] { OnEditHotkey(); }},
        {HotkeySlot::ViewMode, &viewHotkeyId_, &settings_.Mutable().hotkeyViewMode, [this] { OnViewHotkey(); }},
        {HotkeySlot::QuickCapture, &quickCaptureHotkeyId_, &settings_.Mutable().hotkeyQuickCapture,
         [this] { OnQuickCaptureHotkey(); }},
        {HotkeySlot::SilentCapture, &silentCaptureHotkeyId_, &settings_.Mutable().hotkeySilentCapture,
         [this] { OnSilentCaptureHotkey(); }},
    };

    int* hotkeyId = nullptr;
    platform::KeyCombo* configField = nullptr;
    platform::HotkeyCallback callback;
    for (const HotkeyRow& row : rows) {
        if (row.slot != slot) {
            continue;
        }
        hotkeyId = row.id;
        configField = row.configField;
        callback = row.callback;
    }
    if (hotkeyId == nullptr) {
        return false;  // unreachable: every slot has a row
    }

    if (combo == *configField) {
        return true;  // no actual change
    }
    // A combo one of the app's own other hotkeys has moves over: that one
    // is unbound, the way a tool shortcut's key is taken from the row that
    // had it (see OverlayApp::SetToolShortcut). Refusing it instead leaves
    // the user to go and free the combo by hand - and since Windows hands a
    // registered combination to its hotkey and to nothing else, pressing it
    // while the editor waited would do the other hotkey's job rather than
    // be captured at all. Nothing stops the OS
    // from registering the same physical combo twice under two ids, so it
    // is unregistered here first rather than left to collide. An unset
    // combination is exempt - it registers nothing, so it can collide with
    // nothing.
    const HotkeyRow* taken = nullptr;
    for (const HotkeyRow& row : rows) {
        if (row.slot != slot && combo.IsValid() && combo == *row.configField) {
            taken = &row;
        }
    }
    if (taken != nullptr) {
        host_.UnregisterGlobalHotkey(*taken->id);
        *taken->id = 0;
    }

    const int newId = host_.RegisterGlobalHotkey(combo, std::move(callback));
    if (newId == 0) {
        // Rejected by the OS (e.g. already taken by another app) - the old
        // hotkey stays live, and one taken from another row goes back.
        if (taken != nullptr) {
            *taken->id = host_.RegisterGlobalHotkey(*taken->configField, taken->callback);
        }
        return false;
    }
    if (taken != nullptr) {
        *taken->configField = platform::KeyCombo{};
    }
    host_.UnregisterGlobalHotkey(*hotkeyId);
    *hotkeyId = newId;
    *configField = combo;
    PersistConfig();
    return true;
}

void TrayController::OnTrayCommand(platform::TrayCommand command) {
    switch (command) {
        case platform::TrayCommand::ToggleOverlay:
            // Not OnEditHotkey: a click in the tray menu is no key press, so
            // it completes no hotkey capture.
            ToggleMode(/*viewOnly=*/false);
            break;
        case platform::TrayCommand::Exit:
            FlushForShutdown();
            host_.Quit(0);
            break;
    }
}

}  // namespace sz::app
