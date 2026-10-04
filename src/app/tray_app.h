#pragma once

#include <array>
#include <iterator>
#include <optional>

#include "app/overlay_states.h"
#include "ui/overlay_app.h"
#include "core/config/app_config.h"
#include "core/persistence/library_store.h"
#include "platform/i_platform_host.h"

namespace sz::app {

// The controller speaks in the core's vocabulary and owns the one UI,
// whose commands it gives and runs the global ones of.
using namespace ::sz::core;
using ::sz::ui::Command;
using ::sz::ui::CommandForHotkey;
using ::sz::ui::CommandId;
using ::sz::ui::HotkeyCombo;
using ::sz::ui::OverlayApp;
using ::sz::ui::OverlayMode;

// Top-level application logic: wires tray/hotkey events from IPlatformHost
// to the overlay's states. This is the only place that knows "what a
// hotkey does" - everything else is either pure drawing logic (OverlayApp
// and the Session) or pure OS glue (the platform backend).
//
// The overlay is in one of five states - hidden, the pinned view, a
// notice, view and edit - and moves between them as one machine:
// docs/OVERLAY_STATES.md. Its table is Next (overlay_states.h), and this
// holds the state and carries out a transition (Apply), in the one order
// that document's section 6 gives. Every request that moves the overlay
// is Apply(Next(state, request, facts)).
//
// A capture hotkey takes its capture first, so that it is never a
// screenshot of the overlay's own edit mode: the quick capture then asks
// for edit mode (up, or in place), and the silent one for a notice when
// the overlay is hidden. See OverlayApp::QuickCapture for why a capture
// gets a canvas of its own.
class TrayController {
public:
    TrayController(platform::IPlatformHost& host, AppConfig config);

    // Loads any previously-saved library from host_.GetLibraryPath() (a
    // no-op if that path is empty - see FakePlatformHost's own doc comment
    // - or if nothing's been saved there yet, in which case OverlayApp
    // just keeps the fresh default state CanvasManager already starts
    // with), then puts up the tray icon and registers all four global
    // hotkeys. Also attaches the library store, which every command is
    // written to from here on (see Session). Returns false if another copy
    // is running, or the library cannot be opened (see
    // LibraryStore::Open). A hotkey another application owns is not one
    // of those - see UnregisteredHotkeys - and neither is a tray icon the
    // taskbar is not up to take yet. Leaves the overlay hidden: see Start.
    bool Initialize();
    // Brings the overlay to where a start puts it (OverlayRequest::Start):
    // up in edit mode with the welcome notes on a first run, otherwise
    // Away. After a successful Initialize, and after the caller has said
    // what it found there - a library set aside, hotkeys taken. A message
    // box shown once a first run's overlay is up sits under it, which is
    // fullscreen and topmost and has the keyboard, and draws no frame
    // before the event loop runs: a box that can be neither seen nor
    // answered, over a screen that does not move. A start on a library
    // set aside is always a first run.
    void Start();
    // Holds back the hotkeys and the tray icon's show and hide until
    // Start, for a caller that shows something in between - WinMain's
    // message boxes. Their modal loops hand on hotkeys and tray clicks,
    // and one run there brought up an edit overlay that no frame drew
    // until the box was closed: the grab holding the mouse and keyboard
    // over a screen that did not move. Exit is not held.
    void HoldUntilStart() { held_ = true; }
    // After a failed Initialize: whether it was the library that refused,
    // and why - a newer build wrote it, or it could not be read - which the
    // person has to be told apart from a hotkey held elsewhere, and from
    // each other: trying again later may well work for the second.
    bool RefusedANewerLibrary() const {
        return libraryRefusal_ == persistence::LibraryStore::OpenResult::WrittenByANewerVersion;
    }
    bool RefusedAnUnreadableLibrary() const {
        return libraryRefusal_ == persistence::LibraryStore::OpenResult::Unreadable;
    }
    const std::filesystem::path& LibraryPath() const { return libraryStore_.File(); }
    // After Initialize: where a library file that could not be read was set
    // aside, to start with an empty one (see LibraryStore::SetAsideAs) -
    // for the caller to say so. Empty when nothing was.
    const std::filesystem::path& LibrarySetAsideAs() const { return libraryStore_.SetAsideAs(); }
    // After Initialize: the hotkeys set to a combination another application
    // already owns, which were left unregistered rather than refusing the
    // start - for the caller to name. Empty when every one registered.
    const std::vector<std::pair<HotkeySlot, platform::KeyCombo>>& UnregisteredHotkeys() const {
        return unregisteredHotkeys_;
    }

    // Before Initialize, when the config this was given is the defaults
    // standing in for a config.json that could not be read, or what this
    // build could read of one a newer build wrote (see LoadOrCreateConfig);
    // `source` says which. The retention period is not applied at this
    // start: whether the person had it on, and for how long, is exactly
    // what could not be read. And with keepFile, nothing is written over
    // that file for as long as this runs - it is still where it was, and
    // may be the only copy of their settings - and Settings says that what
    // is changed there is not saved. Without it, the file was set aside,
    // and the stand-in is written in its place at Initialize, and tried
    // again until it lands.
    void StartOnStandInSettings(core::ConfigSource source, bool keepFile);
    // Before Initialize, when reading config.json changed what it says - a
    // load repair (see LoadedConfig::writeBack). The file is written once at
    // Initialize, so that it says what runs, and a write that fails is owed
    // like any other. Not over a file kept (StartOnStandInSettings).
    void WriteConfigAtStart() { writeConfigAtStart_ = true; }

    // Which of the five the overlay is in - see docs/OVERLAY_STATES.md.
    OverlayState State() const { return state_; }
    // The app started again while this copy runs: the overlay comes up in
    // edit mode, and stays if it is there already.
    void OnOpenedAgain();
    // The start of every frame - see OverlayApp::SetFrameStartCallback.
    // In view mode and the pinned view, once a frame of the state is on
    // screen, the library's checkpoint (see CheckpointLibrary); in edit
    // mode, the one made while the hand is still (CheckpointWhenStill).
    void OnFrameStart();
    // Edit mode's checkpoint is made once what is unflushed has waited
    // `afterSeconds` and the hand has been still for `stillSeconds`.
    static constexpr double kEditCheckpointAfterSeconds = 60.0;
    static constexpr double kEditCheckpointStillSeconds = 5.0;
    void SetEditCheckpointTimesForTesting(double afterSeconds, double stillSeconds) {
        editCheckpointAfterSeconds_ = afterSeconds;
        editCheckpointStillSeconds_ = stillSeconds;
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
    // unregistered, and the combination is stored by an edit like any
    // other setting's (Settings::Set) - whose repair unbinds the hotkey it
    // was taken from, and whose commit writes the file.
    bool ChangeHotkey(HotkeySlot slot, platform::KeyCombo combo);

private:
    // A global hotkey: its command, offered to the overlay's input machine
    // (see OverlayApp::OnHotkey) - where a Settings row waiting for a combo
    // takes it as the answer instead (see KeyCapture).
    void OnHotkey(HotkeySlot slot);
    // Where the overlay hands the hotkeys' commands back to, once it has
    // settled what they cover (see OverlayApp::SetAppCommandCallback).
    void RunAppCommand(CommandId id);
    // A fullscreen capture onto a canvas of its own, then edit mode, so
    // the capture is noticed - AppConfig::hotkeyQuickCapture.
    void QuickCaptureAndShow();
    // The same capture, without the overlay coming up for it - see
    // AppConfig::hotkeySilentCapture. Followed by a notice when the overlay
    // is hidden and messages are allowed there; when it is already up, the
    // message simply appears in the frames it is already drawing.
    void SilentCapture();

    // ===== The overlay's states (docs/OVERLAY_STATES.md) =====

    // A request, carried out: Apply(Next(...)) with the facts as they are.
    void Request(OverlayRequest request);
    // Makes what the library holds in its WAL durable (see
    // LibraryStore::Checkpoint), at a moment nothing on screen waits for
    // it: in Apply when the overlay is hidden, and in OnFrameStart in view
    // mode and the pinned view. Nothing to do when nothing was written
    // since.
    void CheckpointLibrary();
    // Edit mode's checkpoint, which would be a hitch anywhere else: only
    // once what it moves has waited a minute unflushed, and only while
    // the hand is still - no input for a few seconds, no button held -
    // so that a frame it makes late is one nobody is waiting on. Without
    // it, an hour in edit mode was an hour a power cut could take.
    void CheckpointWhenStill();
    // What the table needs besides the state and the request.
    OverlayFacts Facts() const;
    // Section 6: one transition, every step in its order. A Stay does
    // nothing.
    void Apply(const OverlayTransition& transition);
    // Section 3's invariants, checked after every transition in a debug
    // build.
    void CheckInvariants() const;

    // For the moment the app has to go - Exit from the tray, or the OS
    // ending the session: what the hand is in the middle of is finished,
    // and so written, and a settings file still owed is tried once more.
    // The library owes nothing: every change is written as it is made.
    void SettleForExit();
    // Wired to IPlatformHost::SetSessionEndCallback in Initialize().
    void OnSessionEnding();
    // The one way the settings file is written, so that a write that
    // fails is reported to the overlay once, and cleared when one lands -
    // and remembered as owed (configWriteOwed_), so that it is tried again
    // from the background timer and before the app goes, rather than only
    // when the next settings edit happens to write the file.
    void PersistConfig();
    // The background timer's tick: another attempt at the settings file,
    // and the timer stopped once it is written.
    void OnBackgroundTimer();
    void OnTrayCommand(platform::TrayCommand command);
    // Wired to Settings::SetChangedCallback in Initialize(). Writes the
    // settings to host_.GetConfigFilePath() via WriteConfigFile - a no-op if
    // that path is empty, same "nowhere to persist to" convention
    // host_.GetLibraryPath() already has for libraryStore_ (see
    // Initialize()) - and has ApplySettingsToWindow run after the frame.
    void OnSettingsChanged();
    // Applies to the window whatever of the settings it acts on actually
    // changed, lets go of a frozen screen they no longer want, and moves the
    // overlay to the display they now choose.
    void ApplySettingsToWindow();
    // Whether this showing - the whole showing, not one mode of it - has
    // to take focus whatever the settings say
    // about leaving it alone, because Windows would otherwise deliver us
    // none of that application's input - see ForegroundIntegrity and
    // ProfileableSettings::takeFocusOverElevated. Only the live
    // no-activate value is affected; what is stored stays as the user set
    // it, so the Settings row keeps showing their answer rather than
    // silently rewriting itself for one application.
    //
    // Asked once, as the session starts, and deliberately not per mode.
    // View-only needs none of that input and would rather leave the
    // foreground alone, but view and edit mode switch in place within one
    // session - so a decision made for view-only is the one edit mode
    // inherits, and scoping this to edit mode would leave the overlay deaf
    // in exactly the case it exists to fix.
    bool MustTakeFocusFrom(const platform::ForegroundApp& app) const;
    // Whether edit mode is shown without taking focus: the setting, unless
    // the application underneath is one MustTakeFocusFrom. One expression
    // for the way up and for a settings change alike - the second compared
    // the setting alone, so any setting saved while the overlay was up over
    // an elevated application gave that application its focus back.
    bool WantedEditModeNoActivate() const;
    // Asks what the overlay is about to be shown over, picks the profile
    // that matches, and applies the resolved settings to both OverlayApp
    // and the window. Called as a session starts, and at a restart.
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
    // Which of docs/OVERLAY_STATES.md's five states the overlay is in.
    // Changed by Apply and nowhere else.
    OverlayState state_ = OverlayState::Hidden;
    // The display the overlay is on. Decided when it comes up from hidden,
    // and kept while it is up: switching between edit and view, or a capture
    // hotkey pressed while it is showing, happens where the overlay already
    // is. Changed while up only when the displays themselves change.
    platform::DisplayInfo overlayDisplay_;
    // What the session came up over: held exactly while there is a session
    // (docs/OVERLAY_STATES.md, section 2) - in view and edit mode - and
    // forgotten when it ends.
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
    // See UnregisteredHotkeys.
    std::vector<std::pair<HotkeySlot, platform::KeyCombo>> unregisteredHotkeys_;
    // See StartOnStandInSettings.
    bool skipRetentionThisStart_ = false;
    bool configFileKept_ = false;
    // See WriteConfigAtStart.
    bool writeConfigAtStart_ = false;
    // Whether Initialize found no library to load, which Start greets; and
    // whether it loaded one, which the tutorial may be offered to or
    // resumed in (see OverlayApp::WelcomeAtStart).
    bool firstRun_ = false;
    bool libraryLoaded_ = false;
    // See HoldUntilStart.
    bool held_ = false;
    // Frames started since the last transition, counted to one - see
    // OnFrameStart.
    int framesSinceTransition_ = 0;
    // When, on the input stream's clock, a frame in edit mode first found
    // the library holding something unflushed - see CheckpointWhenStill.
    std::optional<double> unflushedSince_;
    double editCheckpointAfterSeconds_ = kEditCheckpointAfterSeconds;
    double editCheckpointStillSeconds_ = kEditCheckpointStillSeconds;
    // Constructed up front (from host.GetLibraryPath(), possibly empty) but
    // only ever used - Load()'d from, attached to overlayApp_ - when that
    // path is non-empty; see Initialize().
    persistence::LibraryStore libraryStore_;
    // See RefusedANewerLibrary.
    std::optional<persistence::LibraryStore::OpenResult> libraryRefusal_;
    // What the app is working on - the library, what is on disk
    // and on the GPU. The overlay is a view of it.
    Session session_;
    OverlayApp overlayApp_;
    // Each summon hotkey's registration, by slot: 0 for one that is unbound
    // or that another application holds (see UnregisteredHotkeys).
    std::array<int, std::size(kAllHotkeySlots)> hotkeyIds_{};
    int& HotkeyId(HotkeySlot slot) { return hotkeyIds_[static_cast<size_t>(slot)]; }
    // What a summon hotkey does when pressed, for its registration.
    platform::HotkeyCallback HotkeyCallback(HotkeySlot slot) {
        return [this, slot] { OnHotkey(slot); };
    }
};

}  // namespace sz::app
