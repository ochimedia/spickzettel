#include "app/tray_app.h"

#include <cassert>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <optional>
#include <utility>

#include "core/config/display_choice.h"
#include "core/util/timestamp_name.h"

namespace sz::app {

TrayController::TrayController(platform::IPlatformHost& host, AppConfig config)
    : host_(host),
      settings_(std::move(config)),
      libraryStore_(host.GetLibraryPath()),
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
    // A library a newer build wrote is not this one's to open: every row
    // it saved back would lose what the newer build put there. Nor is one
    // that is there and cannot be read: a start over it would save an empty
    // library where it was. Refused before the tray icon, like a second
    // copy, and with a message of its own - see RefusedANewerLibrary.
    if (!host_.GetLibraryPath().empty()) {
        if (const auto opened = libraryStore_.Open(); opened != persistence::LibraryStore::OpenResult::Opened) {
            libraryRefusal_ = opened;
            return false;
        }
    }
    if (!host_.ShowTrayIcon()) {
        return false;
    }
    // The stand-in for a settings file set aside, written where the file
    // was - by the loader already, unless that write failed, and then
    // owed and tried again like any settings write. Not on the file: the
    // next start found none, made the defaults, and had retention back on.
    if (skipRetentionThisStart_ && !configFileKept_) {
        PersistConfig();
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
    // owns it - is left unregistered and noted (see UnregisteredHotkeys),
    // not a reason to refuse the start. Refusing cost the whole app for one
    // combination a screenshot tool happened to hold, with nothing saying
    // which, and a hand edit of config.json as the only way back in; the
    // tray menu reaches edit mode without any hotkey, and Settings > Hotkeys
    // can pick another. An unset one registers as 0 too and is not noted: a
    // hotkey given another row's combination is left unbound on purpose
    // (see ChangeHotkey).
    unregisteredHotkeys_.clear();
    const auto note = [this](HotkeySlot slot, const platform::KeyCombo& combo, int id) {
        if (id == 0 && combo.IsValid()) {
            unregisteredHotkeys_.emplace_back(slot, combo);
        }
    };
    const auto hotkey = [this](HotkeySlot slot) { return [this, slot]() { OnHotkey(slot); }; };
    editHotkeyId_ = host_.RegisterGlobalHotkey(settings_.Stored().hotkeyEditMode, hotkey(HotkeySlot::EditMode));
    note(HotkeySlot::EditMode, settings_.Stored().hotkeyEditMode, editHotkeyId_);
    viewHotkeyId_ = host_.RegisterGlobalHotkey(settings_.Stored().hotkeyViewMode, hotkey(HotkeySlot::ViewMode));
    note(HotkeySlot::ViewMode, settings_.Stored().hotkeyViewMode, viewHotkeyId_);
    quickCaptureHotkeyId_ =
        host_.RegisterGlobalHotkey(settings_.Stored().hotkeyQuickCapture, hotkey(HotkeySlot::QuickCapture));
    note(HotkeySlot::QuickCapture, settings_.Stored().hotkeyQuickCapture, quickCaptureHotkeyId_);
    silentCaptureHotkeyId_ =
        host_.RegisterGlobalHotkey(settings_.Stored().hotkeySilentCapture, hotkey(HotkeySlot::SilentCapture));
    note(HotkeySlot::SilentCapture, settings_.Stored().hotkeySilentCapture, silentCaptureHotkeyId_);

    session_.AttachWindow(&host_.GetOverlayWindow());
    overlayApp_.AttachTo(host_.GetOverlayWindow());
    host_.SetSessionEndCallback([this] { OnSessionEnding(); });
    host_.GetOverlayWindow().SetDisplaysChangedCallback([this] { OnDisplaysChanged(); });
    settings_.SetChangedCallback([this] { OnSettingsChanged(); });
    overlayApp_.SetHotkeyChangeCallback([this](HotkeySlot slot, platform::KeyCombo combo) {
        return ChangeHotkey(slot, combo);
    });
    overlayApp_.SetRestartOverlayCallback([this] { Request(OverlayRequest::Restart); });
    overlayApp_.SetDisplayListCallback([this] { return host_.ListDisplays(); });
    overlayApp_.SetNoticeFinishedCallback([this] { Request(OverlayRequest::NoticeFaded); });
    overlayApp_.SetAppCommandCallback([this](CommandId id) { RunAppCommand(id); });

    // Empty path means "this host has nowhere to persist to" (e.g. a
    // FakePlatformHost in a test that hasn't opted in) - leave the
    // session without a library store entirely: nothing is written, and
    // everything else works the same.
    bool freshInstall = false;
    if (!host_.GetLibraryPath().empty()) {
        session_.SetLibraryStore(&libraryStore_);
        if (std::optional<CanvasManagerSnapshot> snapshot = libraryStore_.Load()) {
            session_.ImportLibrary(std::move(*snapshot));
            // The retention period: only here, at startup, rather than on a
            // clock as well - an instance left running for days keeps what
            // it has until it is next started, which is soon enough.
            if (settings_.Stored().purgeDeleted && !skipRetentionThisStart_) {
                const int64_t day = 24 * 60 * 60;
                const size_t erased = session_.EraseDeletedBefore(static_cast<int64_t>(std::time(nullptr)) -
                                                                  settings_.Stored().purgeDeletedAfterDays * day);
                overlayApp_.SayDeletedForGoodAtStart(erased, settings_.Stored().purgeDeletedAfterDays);
            }
        } else {
            // Nothing to load: a genuinely first run. Distinct from a
            // library someone deliberately emptied, which loads fine as an
            // empty one - that person has already met the app and shouldn't
            // be greeted again. Load() also returns nothing for a file it
            // set aside, where showing the welcome note is the right call
            // anyway. What the app starts with - a folder and a canvas - is
            // written now, for every command after to write into.
            freshInstall = true;
            session_.WriteWholeLibrary();
        }
        // No eager display-size reconciliation here anymore - OverlayApp::OnFrame
        // now does that live, every frame, against ImGui's own DisplaySize (see
        // CanvasManager::SyncItemsToDisplaySize's own doc comment). Items
        // simply sit at their last-saved rect until the overlay is first shown
        // and a frame actually renders, which is also the first moment they'd
        // ever be visible.
    }

    // A first run shows the overlay rather than waiting to be summoned.
    // Every other start is a deliberate hotkey press, but on a first run
    // nobody knows the hotkey yet - an app that installs a tray icon and
    // then sits there invisibly, waiting for a chord it never mentioned, is
    // indistinguishable from one that didn't start. Edit mode specifically,
    // not view-only: the note explains how to interact, so interaction has
    // to be possible. Any other start is Away: pinned snippets are on
    // screen whenever the overlay is away, and having just started is one
    // of those times.
    if (freshInstall) {
        overlayApp_.RequestWelcomeNote();
    }
    OverlayFacts facts = Facts();
    facts.firstRun = freshInstall;
    Apply(Next(state_, OverlayRequest::Start, facts));

    return true;
}

void TrayController::OnHotkey(HotkeySlot slot) {
    // A command like any other, and dispatched like one: the overlay's
    // input machine offers it to what is open, which passes it on, ends
    // what the command's scope covers, and hands it back here to run (see
    // RunAppCommand).
    if (const std::optional<CommandId> command = CommandForHotkey(slot)) {
        overlayApp_.OnHotkey(*command, HotkeyCombo(settings_.Stored(), slot));
    }
}

void TrayController::RunAppCommand(CommandId id) {
    switch (id) {
        case CommandId::ToggleEditMode:
            Request(OverlayRequest::Edit);
            return;
        case CommandId::ToggleViewMode:
            Request(OverlayRequest::View);
            return;
        case CommandId::QuickCapture:
            QuickCaptureAndShow();
            return;
        case CommandId::SilentCapture:
            SilentCapture();
            return;
        default:
            return;  // the overlay's own, which never come here
    }
}

void TrayController::QuickCaptureAndShow() {
    // Idempotent and invisible if already created (see EnsureCreated's own
    // contract) - critically, this doesn't present the window, so the
    // capture below happens against whatever was on screen before this
    // hotkey fired (the hidden desktop, or the already-visible overlay),
    // never against the overlay's own edit-mode UI.
    if (!PrepareWindow()) {
        return;
    }
    overlayApp_.QuickCapture(static_cast<float>(overlayDisplay_.width), static_cast<float>(overlayDisplay_.height));
    // So the user actually notices the capture happened - always lands in
    // edit mode (never puts it away, unlike the edit hotkey itself).
    Request(OverlayRequest::QuickCapture);
}

void TrayController::SilentCapture() {
    // Same first step as the loud capture: create the window if it has
    // never been created, without showing it, so the capture below is of
    // whatever was on screen and never of us.
    if (!PrepareWindow()) {
        return;
    }
    // The capture goes onto a canvas of its own as always, but in the
    // pinned view the overlay stays on the canvas it was showing: that
    // canvas's pinned snippets are what is on screen, and switching to the
    // new one - which has none - would take them away in the middle of
    // whatever they were pinned for.
    const std::optional<CanvasId> stayOn = state_ == OverlayState::Pinned
                                               ? std::optional<CanvasId>(session_.Manager().CurrentCanvasId())
                                               : std::nullopt;
    overlayApp_.QuickCapture(static_cast<float>(overlayDisplay_.width), static_cast<float>(overlayDisplay_.height));
    if (stayOn.has_value()) {
        session_.SwitchToCanvas(*stayOn);
    }
    // Already on screen, in any state: the message lands in the next frame
    // it was going to draw anyway, and nothing about the state changes -
    // notably in view mode, where saying so costs nothing at all. Hidden,
    // it stays hidden unless a notice goes up for two seconds. The capture
    // is in the library already, written with the command that made it
    // (see Session::CreateItem).
    const OverlayTransition transition = Next(state_, OverlayRequest::SilentCapture, Facts());
    if (state_ == OverlayState::Hidden && transition.route == Route::Stay) {
        // Nothing will ever draw the message, so drop it rather than leave
        // it queued for whenever the overlay next comes up - see
        // DismissActionToast.
        overlayApp_.DismissActionToast();
        return;
    }
    Apply(transition);
}

// ================= The overlay's states =================
//
// docs/OVERLAY_STATES.md. Next is the table; this is the rest.

void TrayController::Request(OverlayRequest request) { Apply(Next(state_, request, Facts())); }

OverlayFacts TrayController::Facts() const {
    OverlayFacts facts;
    facts.pinnedHere = session_.Manager().CurrentCanvasHasPinnedItems();
    facts.messagesWhileHidden = settings_.Stored().showToastsWhileHidden;
    return facts;
}

namespace {
OverlayMode ModeFor(OverlayState state) {
    switch (state) {
        case OverlayState::Pinned:
            return OverlayMode::Pinned;
        case OverlayState::Notice:
            return OverlayMode::Notice;
        case OverlayState::View:
            return OverlayMode::View;
        case OverlayState::Hidden:
        case OverlayState::Edit:
            break;
    }
    return OverlayMode::Edit;
}

bool IsSessionState(OverlayState state) { return state == OverlayState::View || state == OverlayState::Edit; }

// What the window is for each state: edit mode takes input, every other
// state that is up is only there to be looked at.
platform::Presentation PresentationFor(OverlayState state) {
    switch (state) {
        case OverlayState::Hidden:
            return platform::Presentation::Hidden;
        case OverlayState::Edit:
            return platform::Presentation::Interactive;
        case OverlayState::Pinned:
        case OverlayState::Notice:
        case OverlayState::View:
            break;
    }
    return platform::Presentation::ClickThrough;
}
}  // namespace

void TrayController::Apply(const OverlayTransition& transition) {
    if (transition.route == Route::Stay) {
        return;
    }
    const OverlayState from = state_;
    const OverlayState to = transition.to;
    const bool comingUp = transition.route == Route::Up || transition.route == Route::ThroughHidden;

    // 1. Settle. Put away, what the hand holds is finished where it stands,
    // and so written - see SettleForPersistence; the same for a restart,
    // which ends the showing it is part of. Edit to View settles through
    // the mode, in step 5.
    const bool away = to == OverlayState::Hidden || to == OverlayState::Pinned;
    if ((IsSessionState(from) && away) || transition.restart) {
        overlayApp_.SettleForPersistence();
    }
    // 2. The frozen screen belongs to the edit mode it was taken for -
    // including one entered again, which takes it again.
    if (from == OverlayState::Edit) {
        session_.ReleaseFrozenScreen();
    }
    // 3. The session ends: what it came up over is forgotten.
    if (transition.endsSession) {
        sessionApp_.reset();
    }
    // Through hidden: down first, then up as from Hidden.
    if (transition.route == Route::ThroughHidden) {
        host_.GetOverlayWindow().Present(platform::Presentation::Hidden);
    }
    // 4. The display, and the window, when coming up. A window that cannot
    // be made leaves the overlay hidden.
    if (comingUp && !PrepareWindow()) {
        state_ = OverlayState::Hidden;
        sessionApp_.reset();
        CheckInvariants();
        return;
    }
    // 5. The mode. Hidden keeps the last one - except that a notice going
    // down is over, and leaves the plain view-only mode it was a kind of.
    if (to != OverlayState::Hidden) {
        overlayApp_.SetMode(ModeFor(to));
    } else if (from == OverlayState::Notice) {
        overlayApp_.SetMode(OverlayMode::View);
    }
    // 6. The session starts: what is underneath is asked before the window
    // comes up to be the answer, and the profile that matches decides how
    // it comes up. A restart keeps what its session came up over.
    if (transition.startsSession || transition.restart) {
        ApplyProfileForCurrentApplication(/*keepPrevious=*/transition.restart);
    }
    // 7. The window is told what to be, and gets there in the order only
    // it knows - see IOverlayWindow::Present.
    host_.GetOverlayWindow().Present(PresentationFor(to));
    state_ = to;
    // 8. A session started: anything the overlay remembers about state the
    // OS owns is stale, and a message waiting for the next showing is
    // said now - see OnOverlayShown. Whenever one starts, in place from the
    // pinned view or a notice too, and at a restart's showing.
    if (transition.startsSession || transition.restart) {
        overlayApp_.OnOverlayShown();
    }
    // 9. The still picture edit mode shows instead of the live application
    // - see AppConfig::freezeScreenInEditMode. After the window is up, so
    // the capture leaves out the overlay as it is going to be (see
    // CaptureScreen), and taken on every entry, so it is never stale.
    if (to == OverlayState::Edit && settings_.Live().freezeScreen) {
        session_.FreezeScreen(overlayDisplay_);
    }
    CheckInvariants();
}

void TrayController::CheckInvariants() const {
#ifndef NDEBUG
    const bool up = state_ != OverlayState::Hidden;
    assert(host_.GetOverlayWindow().IsVisible() == up && "the window is visible exactly when the overlay is up");
    assert((!sessionApp_.has_value() || IsSessionState(state_)) && "a session only in view or edit mode");
    assert((state_ != OverlayState::Edit || sessionApp_.has_value()) && "edit mode is always a session");
    assert((!up || overlayApp_.Mode() == ModeFor(state_)) && "the overlay's mode is the state");
#endif
}

namespace {
// How long between attempts at writing a settings file that could not be
// written. Generous: a disk that is full or a file that is held open does
// not clear itself in a hurry, and each attempt is a synchronous write on
// the app thread.
constexpr int kConfigRetryMs = 10000;
}  // namespace

void TrayController::SettleForExit() {
    // What was being typed or drawn is finished, and so written: every
    // change is written as it is made (see Session), so nothing else is
    // owed to the library. The settings file may be.
    overlayApp_.SettleForPersistence(ui::Lifecycle::SessionEnding);
    if (configWriteOwed_) {
        PersistConfig();  // owed since a settings edit; the last chance for it
    }
}

void TrayController::OnSessionEnding() { SettleForExit(); }

void TrayController::OnBackgroundTimer() {
    if (configWriteOwed_) {
        PersistConfig();
    }
    if (!configWriteOwed_) {
        host_.SetBackgroundTimer(0, nullptr);
    }
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
    // for those would be a full-screen capture for nothing.
    if (display == overlayDisplay_) {
        return;
    }
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    overlayDisplay_ = display;
    window.MoveToDisplay(display);
    // The same conditions a transition freezes under.
    if (state_ == OverlayState::Edit && settings_.Live().freezeScreen) {
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

bool TrayController::WantedEditModeNoActivate() const {
    return settings_.Live().dontStealFocus && !(sessionApp_.has_value() && MustTakeFocusFrom(*sessionApp_));
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
    liveEditModeNoActivate_ = WantedEditModeNoActivate();
    liveEditModeInput_ = settings_.Live().InputOptions();
    window.SetEditModeNoActivate(liveEditModeNoActivate_);
    window.SetEditModeInput(liveEditModeInput_);
}

void TrayController::OnSettingsChanged() {
    // The two runtime side effects, gated on an actual change so toggling
    // some unrelated setting doesn't re-poke the window or tear the input
    // hooks down and put them straight back up. Compared against what is
    // *live* rather than against what is stored, which is not the live
    // value whenever a profile is overriding it.
    platform::IOverlayWindow& window = host_.GetOverlayWindow();
    if (WantedEditModeNoActivate() != liveEditModeNoActivate_) {
        liveEditModeNoActivate_ = WantedEditModeNoActivate();
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
    if (state_ != OverlayState::Hidden) {
        MoveOverlayTo(OverlayDisplay());
    }
    PersistConfig();
}

void TrayController::PersistConfig() {
    const std::filesystem::path path = host_.GetConfigFilePath();
    if (path.empty() || configFileKept_) {
        return;  // nowhere to persist to - see FakePlatformHost - or not to be touched
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
        host_.SetBackgroundTimer(kConfigRetryMs, [this] { OnBackgroundTimer(); });
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
        {HotkeySlot::EditMode, &editHotkeyId_, &settings_.Mutable().hotkeyEditMode,
         [this] { OnHotkey(HotkeySlot::EditMode); }},
        {HotkeySlot::ViewMode, &viewHotkeyId_, &settings_.Mutable().hotkeyViewMode,
         [this] { OnHotkey(HotkeySlot::ViewMode); }},
        {HotkeySlot::QuickCapture, &quickCaptureHotkeyId_, &settings_.Mutable().hotkeyQuickCapture,
         [this] { OnHotkey(HotkeySlot::QuickCapture); }},
        {HotkeySlot::SilentCapture, &silentCaptureHotkeyId_, &settings_.Mutable().hotkeySilentCapture,
         [this] { OnHotkey(HotkeySlot::SilentCapture); }},
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

    // No actual change - unless the hotkey has no registration: one another
    // application held at the start is picked again to try again.
    if (combo == *configField && (*hotkeyId != 0 || !combo.IsValid())) {
        return true;
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
            // The edit hotkey's command, but not through OnHotkey: a click in
            // the tray menu is no key press, so it completes no hotkey
            // capture.
            overlayApp_.Dispatch(Command{CommandId::ToggleEditMode});
            break;
        case platform::TrayCommand::Exit:
            SettleForExit();
            host_.Quit(0);
            break;
    }
}

}  // namespace sz::app
