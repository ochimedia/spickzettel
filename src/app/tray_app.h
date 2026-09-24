#pragma once

#include <optional>

#include "ui/overlay_app.h"
#include "core/config/app_config.h"
#include "core/persistence/library_store.h"
#include "platform/i_platform_host.h"

namespace sz::app {

// The controller speaks in the core's vocabulary and owns the one UI.
using namespace ::sz::core;
using ::sz::ui::OverlayApp;

// Top-level application logic: wires tray/hotkey events from IPlatformHost
// to the overlay's show/hide/mode behavior. This is the only place that
// knows "what a hotkey does" — everything else is either pure drawing
// logic (OverlayApp and the Session) or pure OS glue (the platform
// backend).
//
// Two hotkeys drive three states - hidden, edit (the full interactive
// canvas), and view-only (the current canvas
// displayed read-only, click-through to whatever's underneath):
//
//   hidden    --edit hotkey--> edit
//   hidden    --view hotkey--> view
//   edit      --edit hotkey--> hidden
//   edit      --view hotkey--> view      (no hide/reshow - stays visible)
//   view      --view hotkey--> hidden
//   view      --edit hotkey--> edit      (no hide/reshow - stays visible)
//
// i.e. each hotkey toggles its own mode off (back to hidden) when that
// mode is already active, and switches straight to its mode otherwise -
// including directly between edit and view, without hiding in between.
//
// "Hidden" is the pinned view instead whenever the current canvas has a
// pinned snippet (Item::pinned): putting the overlay away leaves those
// snippets on screen, click-through, and nothing else. It stands in for
// hidden everywhere in the table above - either hotkey leaves it for its
// own mode - and there is deliberately no hotkey that hides it: unpinning
// is how pinned snippets go. See PutAway and ShowPinnedView.
//
// A third hotkey, independent of the state machine above, captures a
// fullscreen screenshot onto a canvas of its own - see
// OverlayApp::QuickCapture's own doc comment for why (a fallback for
// grabbing a shot of the game before entering edit mode might cost it a
// frame or two, and a canvas each so the shots can be told apart later) -
// and then always switches to edit mode afterward
// (showing it if hidden, switching in place if it was in view-only mode,
// a no-op if already there), so the capture is never silent. The capture
// itself happens first, before that switch, so it's never a screenshot of
// the overlay's own edit-mode UI.
//
// Deliberately stateless about which of the three states it's in: there's
// no `mode_` member to go stale. "Hidden vs. visible" comes from
// `window.IsVisible()`, and "edit vs. view" (only meaningful while
// visible) comes from `overlayApp_.IsViewOnly()` - both already the
// actual ground truth the platform layer and OverlayApp maintain for
// their own reasons, so this can't desync from it if something else ever
// shows the window directly.
class TrayController {
public:
    TrayController(platform::IPlatformHost& host, AppConfig config);

    // Registers the tray icon and all four global hotkeys, then loads any
    // previously-saved library from host_.GetDataDirectoryPath() (a no-op
    // if that path is empty - see FakePlatformHost's own doc comment - or
    // if nothing's been saved there yet, in which case OverlayApp just
    // keeps the fresh default state CanvasManager already starts with).
    // Also attaches the library store so the Session's debounced autosave
    // (see Session::SetLibraryStore) and CaptureShotItem's synchronous image
    // writes actually persist anything from here on. Returns false if any
    // hotkey/tray registration fails (e.g. a hotkey combination is
    // already taken), or if the library was written by a newer build.
    bool Initialize();
    // After a failed Initialize: whether it was the library that refused -
    // a newer build wrote it (see LibraryStore::WrittenByANewerVersion) -
    // which the person has to be told apart from a hotkey held elsewhere.
    bool RefusedANewerLibrary() const { return refusedANewerLibrary_; }

    // Before Initialize, when the config this was given is the defaults
    // standing in for a config.json that could not be read (see
    // LoadOrCreateConfig). The retention period is not applied at this
    // start: whether the person had it on, and for how long, is exactly
    // what could not be read. And with keepFile, nothing is written over
    // that file for as long as this runs - it is still where it was, and
    // may be the only copy of their settings.
    void StartOnStandInSettings(bool keepFile) {
        skipRetentionThisStart_ = true;
        configFileKept_ = keepFile;
    }

    const OverlayApp& Overlay() const { return overlayApp_; }
    // Non-const for the tests that have to *arrange* a world before driving
    // it - an empty library, a canvas full of items. Deliberately not for
    // performing the action under test: that goes in through the input the
    // real thing would have received, or the test proves nothing about the
    // path a user takes.
    OverlayApp& Overlay() { return overlayApp_; }
    // The settings as the app holds them - stored and resolved. Read by
    // tests checking where an edit went; non-const for arranging one.
    const Settings& GetSettings() const { return settings_; }
    Settings& GetSettings() { return settings_; }
    // What is being worked on - the library. Read by tests
    // checking where something went; non-const for arranging a world
    // before driving it, same as Overlay().
    const Session& GetSession() const { return session_; }
    Session& GetSession() { return session_; }

    // Attempts to change one of the four global hotkeys to `combo` -
    // wired up as overlayApp_'s hotkey-change callback in Initialize() (see
    // OverlayApp::SetHotkeyChangeCallback's own doc comment for why this is
    // "ask first, commit after" rather than the plain "tell the host after"
    // shape every other setting uses), and public here so it's directly
    // callable/testable without an actual Settings-panel edit driving it.
    // Refuses the change (returns false, leaves the existing hotkey
    // registered and the settings untouched) if the OS rejects the
    // registration (e.g. already taken by another app). A combo one of
    // this app's own other hotkeys has is taken from it: that hotkey is
    // unregistered and left unbound. On success, the previous hotkey is
    // unregistered, the stored hotkey and the live registration both move
    // to `combo`, and the settings are persisted to disk immediately - so
    // a caller never needs a separate "now save it" step.
    bool ChangeHotkey(HotkeySlot slot, platform::KeyCombo combo);

private:
    // Whether a hotkey row in Settings is waiting for a combo, in which
    // case the hotkey that just fired is the answer and does nothing else -
    // see OverlayApp::IsCapturingHotkey. Each hotkey handler asks first.
    bool CompletesAHotkeyCapture(const platform::KeyCombo& combo);
    void OnEditHotkey();
    void OnViewHotkey();
    void OnQuickCaptureHotkey();
    // The same capture, without the overlay coming up for it - see
    // AppConfig::hotkeySilentCapture. Followed by a notice (ShowNotice)
    // when the overlay is hidden and messages are allowed there; when it
    // is already up, in either mode, the message simply appears in the
    // frames it is already drawing and nothing else happens.
    void OnSilentCaptureHotkey();
    // Puts the overlay up carrying nothing but the message a hotkey just
    // set, click-through and without taking focus, and takes it away again
    // when that message fades (OverlayApp::SetNoticeFinishedCallback). A
    // no-op if the overlay is already visible, which needs none of this.
    //
    // Deliberately not EnsureMode: that path freezes the screen when the
    // setting asks for it, which for a two-second message would mean a
    // full-screen still of the desktop flashing up behind it.
    void ShowNotice();
    // The other end of ShowNotice, called by OverlayApp when the message
    // has faded. Does nothing unless a notice is actually what is on
    // screen, since a real mode entered meanwhile has taken the overlay
    // over and must keep it.
    void HideNoticeIfDone();
    // What a mode's own hotkey does to it: flush, drop the frozen screen,
    // and go - to the pinned view when the current canvas has pinned
    // snippets (in place from view-only, which it already is), to hidden
    // otherwise.
    void PutAway();
    // The flush for a moment after which no frame follows: the overlay
    // going away, or a capture taken while it is away. What it cannot
    // write now it arranges to try again from the host's background
    // timer, since the autosave's own retry clock runs on frames and there
    // are none while hidden - see OnBackgroundTimer.
    void FlushOrRetryLater();
    // The flush for the moment the app has to go - Exit from the tray, or
    // the OS ending the session - after which there is no retry. What
    // cannot be written to the library is written to a recovery copy
    // beside it (see Session::WriteRecoveryCopy and RecoveryCopyPath), so
    // that the changes exist somewhere rather than nowhere. True when one
    // of the two landed whole. False - the library and the copy both
    // unwritable - is an accepted outcome, and the caller exits anyway;
    // see the .cpp.
    bool FlushForShutdown();
    // Wired to IPlatformHost::SetSessionEndCallback in Initialize().
    void OnSessionEnding();
    // Where a recovery copy goes: a sibling of the library, named after it
    // and the moment. Nullopt without a library on disk.
    std::optional<std::filesystem::path> RecoveryCopyPath() const;
    // The one way the settings file is written, so that a write that
    // fails is reported to the overlay once, and cleared when one lands -
    // and remembered as owed (configWriteOwed_), so that it is tried again
    // from the background timer and before the app goes, rather than only
    // when the next settings edit happens to write the file.
    void PersistConfig();
    // The background timer's tick: another attempt at whatever is owed to
    // the disk - the library while the overlay is hidden (frames retry it
    // while it is up), the settings file either way - and the timer is
    // stopped once nothing is.
    void OnBackgroundTimer();
    // Puts the pinned view up - view-only, click-through, never focused,
    // drawing the current canvas's pinned snippets - if the overlay is
    // hidden and there are any. False if it did not. The same way up as a
    // notice, for the same reasons (see ShowNotice): it is not a session
    // with the application underneath, so no profile, no focus.
    bool ShowPinnedView();
    // Shared by both mode hotkey handlers: shows (creating the window first
    // if needed) in the given mode, or - if already shown in that exact
    // mode - hides instead.
    void ToggleMode(bool viewOnly);
    // The "show/switch in place" half of ToggleMode, without the toggle-
    // off-if-already-there check - used where landing in a specific mode
    // must never hide the overlay, e.g. after a quick capture.
    // `keepProfileContext` is for the one caller that is not a new show at
    // all: a restart the overlay does to itself, which has to stay in the
    // profile it was already in - see ApplyProfileForCurrentApplication.
    void EnsureMode(bool viewOnly, bool keepProfileContext = false);
    void OnTrayCommand(platform::TrayCommand command);
    // Wired to Settings::SetChangedCallback in Initialize(). Applies to the
    // window whatever of the settings it acts on actually changed, then
    // writes the settings to host_.GetConfigFilePath() via WriteConfigFile -
    // a no-op if that path is empty, same "nowhere to persist to"
    // convention host_.GetDataDirectoryPath() already has for
    // libraryStore_ (see Initialize()).
    void OnSettingsChanged();
    // Whether this showing - the whole showing, not one mode of it - has
    // to take focus whatever the settings say
    // about leaving it alone, because Windows would otherwise deliver us
    // none of that application's input - see ForegroundIntegrity and
    // ProfileableSettings::takeFocusOverElevated. Only the live
    // no-activate value is affected; what is stored stays as the user set
    // it, so the Settings row keeps showing their answer rather than
    // silently rewriting itself for one application.
    //
    // Asked once, on the way up, and deliberately not per mode. View-only
    // needs none of that input and would rather leave the foreground
    // alone, but EnsureMode switches it and edit mode in place without
    // coming up from hidden - so a decision made for view-only is the one
    // edit mode inherits, and scoping this to edit mode would leave the
    // overlay deaf in exactly the case it exists to fix.
    bool MustTakeFocusFrom(const platform::ForegroundApp& app) const;
    // Hides the overlay and immediately shows it again in the same mode,
    // for settings that are only read on the way in - see
    // OverlayApp::SetRestartOverlayCallback. No-op while hidden.
    void RestartOverlay();
    // Asks what the overlay is about to be shown over, picks the profile
    // that matches, and applies the resolved settings to both OverlayApp
    // and the window. Called on the way up, once per show.
    //
    // `keepPrevious` re-uses the answer this showing already had rather than
    // asking again - see sessionApp_.
    void ApplyProfileForCurrentApplication(bool keepPrevious = false);
    // The display the overlay belongs on - see core::ChooseDisplay. Asked
    // afresh each time, since displays come and go while the app runs.
    platform::DisplayInfo OverlayDisplay() const;
    // Creates the window if it does not exist yet and puts it on its
    // display, ready to be shown or captured from. False if the window
    // could not be created. See overlayDisplay_ for when that display is
    // decided again.
    bool PrepareWindow();
    // Wired to IOverlayWindow::SetDisplaysChangedCallback in Initialize().
    void OnDisplaysChanged();
    // Puts the overlay on `display`, taking the frozen screen again if it
    // was showing one - that is a picture of the display it has just left.
    void MoveOverlayTo(const platform::DisplayInfo& display);
    // The display the overlay is on. Decided when it comes up from hidden,
    // and kept while it is up: switching between edit and view, or a capture
    // hotkey pressed while it is showing, happens where the overlay already
    // is. Changed while up only when the displays themselves change.
    platform::DisplayInfo overlayDisplay_;
    // What the overlay decided it was up over when it came up, kept for as
    // long as that showing lasts and forgotten when it is hidden for real.
    //
    // A restart is not a new show. The overlay hides and shows itself to
    // apply a setting that is only read on the way in, and asking again in
    // the middle of that gets a different answer: it has just hidden itself,
    // and may have taken the foreground on the way (turning "Don't steal
    // focus" off does exactly that). The profile that matched then stops
    // matching, and a HUD toggle that went into that profile reads as if it
    // had switched itself back.
    std::optional<platform::ForegroundApp> sessionApp_;

    platform::IPlatformHost& host_;
    // Every setting, as written and as resolved against the profile that
    // matched what the overlay came up over - see Settings. Owned here so
    // that the UI views it rather than holding a copy, and this persists it
    // whenever it is committed.
    Settings settings_;
    // The last values handed to the window, so a settings edit can tell a
    // real change from a no-op - the stored value is not it whenever a
    // profile overrides it.
    bool liveEditModeNoActivate_ = false;
    platform::EditModeInputOptions liveEditModeInput_;
    // Whether the settings as held differ from the file because a write
    // failed - see PersistConfig. Retried from the background timer.
    bool configWriteOwed_ = false;
    // See StartOnStandInSettings.
    bool skipRetentionThisStart_ = false;
    bool configFileKept_ = false;
    // Constructed up front (from host.GetDataDirectoryPath(), possibly
    // empty) but only ever used - Load()'d from, attached to overlayApp_ -
    // when that path is non-empty; see Initialize().
    persistence::LibraryStore libraryStore_;
    // See RefusedANewerLibrary.
    bool refusedANewerLibrary_ = false;
    // What the app is working on - the library, what is on disk
    // and on the GPU. The overlay is a view of it.
    Session session_;
    OverlayApp overlayApp_;
    int editHotkeyId_ = 0;
    int viewHotkeyId_ = 0;
    int quickCaptureHotkeyId_ = 0;
    // 0 when the silent-capture hotkey could not be registered, which -
    // unlike the three above - is survivable: see
    // AppConfig::hotkeySilentCapture for why this one alone is optional.
    int silentCaptureHotkeyId_ = 0;
};

}  // namespace sz::app
