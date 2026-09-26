#include "app/tray_app.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "core/persistence/library_store.h"
#include "fakes/fake_platform_host.h"
#include "support/held_library.h"
#include "support/session_test_access.h"

namespace sz::test {

using namespace ::sz::core;
using ::sz::app::TrayController;

namespace {

TEST(TrayControllerTest, InitializeShowsTrayAndRegistersAllConfiguredHotkeys) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.hotkeyEditMode = platform::KeyCombo{true, true, false, 'K'};
    config.hotkeyViewMode = platform::KeyCombo{true, true, false, 'L'};
    config.hotkeyQuickCapture = platform::KeyCombo{true, true, false, 'M'};
    config.hotkeySilentCapture = platform::KeyCombo{true, true, false, 'N'};
    TrayController controller(host, config);

    ASSERT_TRUE(controller.Initialize());

    EXPECT_TRUE(host.trayIconShown);
    ASSERT_EQ(host.registeredCombos.size(), 4u);
    EXPECT_TRUE(std::any_of(host.registeredCombos.begin(), host.registeredCombos.end(),
                             [&](const auto& kv) { return kv.second == config.hotkeySilentCapture; }));
    EXPECT_TRUE(std::any_of(host.registeredCombos.begin(), host.registeredCombos.end(),
                             [&](const auto& kv) { return kv.second == config.hotkeyEditMode; }));
    EXPECT_TRUE(std::any_of(host.registeredCombos.begin(), host.registeredCombos.end(),
                             [&](const auto& kv) { return kv.second == config.hotkeyViewMode; }));
    EXPECT_TRUE(std::any_of(host.registeredCombos.begin(), host.registeredCombos.end(),
                             [&](const auto& kv) { return kv.second == config.hotkeyQuickCapture; }));
}

TEST(TrayControllerTest, InitializeConfiguresEditModeNoActivateFromConfig) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.profileable.dontStealFocus = false;
    TrayController controller(host, config);

    ASSERT_TRUE(controller.Initialize());

    EXPECT_EQ(host.overlayWindow.setEditModeNoActivateCallCount, 1);
    EXPECT_FALSE(host.overlayWindow.editModeNoActivate);
}

TEST(TrayControllerTest, InitializeConfiguresEditModeInputFromConfig) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.profileable.rawMouseInput = true;
    config.profileable.counterRawMouseInput = true;
    config.profileable.dontForwardKeystrokes = false;
    TrayController controller(host, config);

    ASSERT_TRUE(controller.Initialize());

    // Seeded before the window is ever shown, same as edit-mode activation
    // above, so the first show already behaves as configured rather than
    // only doing so after the next settings change.
    EXPECT_EQ(host.overlayWindow.setEditModeInputCallCount, 1);
    EXPECT_TRUE(host.overlayWindow.editModeInput.useRawMouseInput);
    EXPECT_TRUE(host.overlayWindow.editModeInput.counterRawMouseInput);
    EXPECT_FALSE(host.overlayWindow.editModeInput.dontForwardKeystrokes);
}

TEST(TrayControllerTest, InitializeFailsIfTrayIconFails) {
    test::FakePlatformHost host;
    host.showTrayIconSucceeds = false;
    TrayController controller(host, DefaultConfig());

    EXPECT_FALSE(controller.Initialize());
    EXPECT_FALSE(controller.RefusedANewerLibrary()) << "a failure of its own, told as such";
}

TEST(TrayControllerTest, InitializeFailsWhenAnotherCopyIsRunning) {
    test::FakePlatformHost host;
    host.singleInstanceAvailable = false;
    TrayController controller(host, DefaultConfig());
    EXPECT_FALSE(controller.Initialize());
    EXPECT_FALSE(host.trayIconShown) << "not even a second tray icon for a moment";
    EXPECT_TRUE(host.registeredCombos.empty());
}

// A hotkey another application owns costs that hotkey, not the start: the
// tray reaches the overlay without it, and each one is named for the caller
// to say so.
TEST(TrayControllerTest, HotkeysAnotherApplicationOwnsAreNamedNotFatal) {
    test::FakePlatformHost host;
    host.registerHotkeySucceeds = false;
    AppConfig config = DefaultConfig();
    config.hotkeySilentCapture = platform::KeyCombo{};  // unbound on purpose: not a failure
    TrayController controller(host, config);

    EXPECT_TRUE(controller.Initialize());
    const auto& taken = controller.UnregisteredHotkeys();
    ASSERT_EQ(taken.size(), 3u);
    EXPECT_EQ(taken[0].first, HotkeySlot::EditMode);
    EXPECT_EQ(taken[0].second, config.hotkeyEditMode);
    EXPECT_EQ(taken[1].first, HotkeySlot::ViewMode);
    EXPECT_EQ(taken[2].first, HotkeySlot::QuickCapture);

    host.TriggerTrayCommand(platform::TrayCommand::ToggleOverlay);
    EXPECT_TRUE(host.overlayWindow.IsVisible()) << "the tray still brings it up";
}

// Finds each hotkey's id by matching its registered combo against the
// config, rather than assuming registration order - this test's whole
// point is exercising the id-per-hotkey plumbing, so it shouldn't itself
// assume anything about it beyond "each combo got registered."
int FindHotkeyId(const test::FakePlatformHost& host, const platform::KeyCombo& combo) {
    for (const auto& [id, registeredCombo] : host.registeredCombos) {
        if (registeredCombo == combo) {
            return id;
        }
    }
    return 0;
}

// ================= ChangeHotkey (runtime hotkey editing) =================

TEST(TrayControllerTest, ChangeHotkeySwapsTheRegistrationAndTriggersTheNewCombo) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int oldEditId = FindHotkeyId(host, config.hotkeyEditMode);
    ASSERT_NE(oldEditId, 0);

    const platform::KeyCombo newCombo{/*ctrl=*/true, /*alt=*/false, /*shift=*/true, /*key=*/'Z'};
    EXPECT_TRUE(controller.ChangeHotkey(HotkeySlot::EditMode, newCombo));

    // Old id no longer does anything - UnregisterGlobalHotkey erased its
    // callback (see FakePlatformHost) - while the new combo's id triggers
    // exactly what the edit hotkey always has.
    host.TriggerHotkey(oldEditId);
    EXPECT_FALSE(host.overlayWindow.IsVisible());

    const int newEditId = FindHotkeyId(host, newCombo);
    ASSERT_NE(newEditId, 0);
    host.TriggerHotkey(newEditId);
    EXPECT_TRUE(host.overlayWindow.IsVisible());

    // The view/quick-capture hotkeys are untouched.
    EXPECT_NE(FindHotkeyId(host, config.hotkeyViewMode), 0);
    EXPECT_NE(FindHotkeyId(host, config.hotkeyQuickCapture), 0);
}

TEST(TrayControllerTest, ChangeHotkeyPersistsTheNewComboToDisk) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "spickzettel_tray_app_change_hotkey_test_config.json";
    std::filesystem::remove(path);

    test::FakePlatformHost host;
    host.configFilePath = path;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    const platform::KeyCombo newCombo{/*ctrl=*/true, /*alt=*/false, /*shift=*/true, /*key=*/'Z'};
    ASSERT_TRUE(controller.ChangeHotkey(HotkeySlot::ViewMode, newCombo));

    const AppConfig written = ParseConfig([&] {
        std::ifstream in(path);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }());
    EXPECT_EQ(written.hotkeyViewMode, newCombo);

    std::filesystem::remove(path);
}

int CountRegistrations(const test::FakePlatformHost& host, const platform::KeyCombo& combo) {
    int count = 0;
    for (const auto& [id, registeredCombo] : host.registeredCombos) {
        (void)id;
        count += registeredCombo == combo ? 1 : 0;
    }
    return count;
}

TEST(TrayControllerTest, AHotkeyLeftUnboundByAnotherStaysUnboundAcrossARestart) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "spickzettel_tray_app_unbound_hotkey_test_config.json";
    std::filesystem::remove(path);

    test::FakePlatformHost host;
    host.configFilePath = path;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    // Edit mode takes view mode's combination; view mode is left unbound,
    // and the file has to say so rather than say nothing.
    ASSERT_TRUE(controller.ChangeHotkey(HotkeySlot::EditMode, config.hotkeyViewMode));
    EXPECT_EQ(CountRegistrations(host, config.hotkeyViewMode), 1);
    const AppConfig written = ParseConfig([&] {
        std::ifstream in(path);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }());
    EXPECT_EQ(written.hotkeyEditMode, config.hotkeyViewMode);
    EXPECT_FALSE(written.hotkeyViewMode.IsValid()) << "the default would collide with what edit mode took";

    // A restart from that file registers the combination exactly once, for
    // the hotkey that took it.
    test::FakePlatformHost restarted;
    restarted.configFilePath = path;
    TrayController again(restarted, written);
    ASSERT_TRUE(again.Initialize());
    EXPECT_EQ(CountRegistrations(restarted, config.hotkeyViewMode), 1);
    restarted.TriggerHotkey(FindHotkeyId(restarted, config.hotkeyViewMode));
    EXPECT_TRUE(restarted.overlayWindow.IsVisible()) << "it is the edit hotkey now";

    std::filesystem::remove(path);
}

TEST(TrayControllerTest, InitializeUnbindsALaterDuplicateOfAnEarlierHotkey) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.hotkeyQuickCapture = config.hotkeyEditMode;  // as a hand-edited file might say
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize()) << "one combination twice is not a reason to refuse to start";

    EXPECT_EQ(CountRegistrations(host, config.hotkeyEditMode), 1);
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));
    EXPECT_TRUE(host.overlayWindow.IsVisible()) << "the earlier hotkey keeps the combination";
}

TEST(TrayControllerTest, ChangeHotkeyLeavesTheOldHotkeyLiveWhenRegistrationFails) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int oldQuickCaptureId = FindHotkeyId(host, config.hotkeyQuickCapture);
    ASSERT_NE(oldQuickCaptureId, 0);

    host.registerHotkeySucceeds = false;
    EXPECT_FALSE(controller.ChangeHotkey(HotkeySlot::QuickCapture, platform::KeyCombo{true, true, false, 'Z'}));

    // The old registration is still there and still works - a rejected
    // edit must never leave the app with a broken/missing hotkey.
    EXPECT_EQ(FindHotkeyId(host, config.hotkeyQuickCapture), oldQuickCaptureId);
    host.registerHotkeySucceeds = true;  // restore, so TriggerHotkey below isn't itself affected
    host.TriggerHotkey(oldQuickCaptureId);
    EXPECT_TRUE(host.overlayWindow.IsVisible());
}

// Deliberately allowed for now (see TrayController::ChangeHotkey's own
// comment) - the user explicitly wants this left open, e.g. to try a bare
// function key like F9, even though disallowing a bare *letter/digit* may
// still be worth adding later.
TEST(TrayControllerTest, ChangeHotkeyAllowsAComboWithNoModifierKey) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    const platform::KeyCombo noModifierCombo{false, false, false, 'Z'};
    EXPECT_TRUE(controller.ChangeHotkey(HotkeySlot::EditMode, noModifierCombo));
    EXPECT_NE(FindHotkeyId(host, noModifierCombo), 0);
}

TEST(TrayControllerTest, ChangeHotkeyAllowsABareFunctionKey) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    const platform::KeyCombo f9{false, false, false, platform::KeyCombo::kFunctionKeyBase + 9};
    EXPECT_TRUE(controller.ChangeHotkey(HotkeySlot::QuickCapture, f9));
    EXPECT_NE(FindHotkeyId(host, f9), 0);
}

// A combo another of the app's own hotkeys has moves over to the edited
// one, and the hotkey that had it is left unbound - the way a tool
// shortcut's key is taken from the row that had it. Nothing stops the OS
// from registering the same physical combo twice under two different ids,
// so the other one has to be unregistered before the new registration.
TEST(TrayControllerTest, ChangeHotkeyTakesAComboFromTheOwnHotkeyThatHadIt) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int oldViewId = FindHotkeyId(host, config.hotkeyViewMode);
    ASSERT_NE(oldViewId, 0);

    EXPECT_TRUE(controller.ChangeHotkey(HotkeySlot::EditMode, config.hotkeyViewMode));

    EXPECT_EQ(controller.GetSettings().Stored().hotkeyEditMode, config.hotkeyViewMode);
    EXPECT_FALSE(controller.GetSettings().Stored().hotkeyViewMode.IsValid()) << "unbound, not kept";
    EXPECT_EQ(host.registeredCombos.count(oldViewId), 0u) << "the view hotkey's registration is gone";
    const int newEditId = FindHotkeyId(host, config.hotkeyViewMode);
    ASSERT_NE(newEditId, 0);
    EXPECT_NE(newEditId, oldViewId) << "registered afresh for the edit hotkey";
    EXPECT_EQ(FindHotkeyId(host, config.hotkeyEditMode), 0) << "the old edit combo is unregistered";
}

// ...and the app comes back up with that hotkey unbound, rather than
// refusing to start because one of its three ways in registered nothing.
TEST(TrayControllerTest, InitializeStartsWithAnUnboundViewHotkey) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.hotkeyViewMode = platform::KeyCombo{};
    TrayController controller(host, config);
    EXPECT_TRUE(controller.Initialize());
    EXPECT_NE(FindHotkeyId(host, config.hotkeyEditMode), 0);
}

TEST(TrayControllerTest, ChangeHotkeyToTheSameComboIsANoOpThatSucceeds) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);

    EXPECT_TRUE(controller.ChangeHotkey(HotkeySlot::EditMode, config.hotkeyEditMode));

    EXPECT_EQ(FindHotkeyId(host, config.hotkeyEditMode), editId);  // never re-registered
}

TEST(TrayControllerTest, EditHotkeyTogglesBetweenHiddenAndEdit) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    ASSERT_NE(editId, 0);

    host.TriggerHotkey(editId);
    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_EQ(host.overlayWindow.ensureCreatedCallCount, 1);
    EXPECT_EQ(host.overlayWindow.showCallCount, 1);

    host.TriggerHotkey(editId);
    EXPECT_FALSE(host.overlayWindow.IsVisible());
    EXPECT_EQ(host.overlayWindow.hideCallCount, 1);
}

TEST(TrayControllerTest, ViewHotkeyTogglesBetweenHiddenAndView) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int viewId = FindHotkeyId(host, config.hotkeyViewMode);
    ASSERT_NE(viewId, 0);

    host.TriggerHotkey(viewId);
    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_TRUE(controller.Overlay().IsViewOnly());
    EXPECT_TRUE(host.overlayWindow.inputPassthrough);

    host.TriggerHotkey(viewId);
    EXPECT_FALSE(host.overlayWindow.IsVisible());
}

TEST(TrayControllerTest, SwitchesDirectlyBetweenEditAndViewWithoutHiding) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    const int viewId = FindHotkeyId(host, config.hotkeyViewMode);

    host.TriggerHotkey(editId);  // hidden -> edit
    ASSERT_TRUE(host.overlayWindow.IsVisible());
    ASSERT_FALSE(controller.Overlay().IsViewOnly());
    const int showCallsAfterEdit = host.overlayWindow.showCallCount;
    const int hideCallsAfterEdit = host.overlayWindow.hideCallCount;

    host.TriggerHotkey(viewId);  // edit -> view, in place
    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_TRUE(controller.Overlay().IsViewOnly());
    EXPECT_TRUE(host.overlayWindow.inputPassthrough);
    EXPECT_EQ(host.overlayWindow.showCallCount, showCallsAfterEdit);  // no re-show
    EXPECT_EQ(host.overlayWindow.hideCallCount, hideCallsAfterEdit);  // no hide

    host.TriggerHotkey(editId);  // view -> edit, in place
    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_FALSE(host.overlayWindow.inputPassthrough);
    EXPECT_EQ(host.overlayWindow.showCallCount, showCallsAfterEdit);
    EXPECT_EQ(host.overlayWindow.hideCallCount, hideCallsAfterEdit);

    host.TriggerHotkey(editId);  // edit -> hidden (same hotkey, already in edit)
    EXPECT_FALSE(host.overlayWindow.IsVisible());
}

TEST(TrayControllerTest, QuickCaptureHotkeyAddsItemAndEntersEditMode) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int captureId = FindHotkeyId(host, config.hotkeyQuickCapture);
    ASSERT_NE(captureId, 0);
    ASSERT_TRUE(test::Model(controller.GetSession()).CurrentOrNull()->items.empty());

    host.TriggerHotkey(captureId);

    EXPECT_TRUE(host.overlayWindow.IsVisible());  // now shown, so the capture is noticed
    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_EQ(host.overlayWindow.showCallCount, 1);
    EXPECT_EQ(test::Model(controller.GetSession()).CurrentOrNull()->items.size(), 1u);
}

TEST(TrayControllerTest, QuickCaptureHotkeyWorksWhileEditModeIsShowing) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    const int captureId = FindHotkeyId(host, config.hotkeyQuickCapture);
    host.TriggerHotkey(editId);  // hidden -> edit
    ASSERT_TRUE(host.overlayWindow.IsVisible());
    const int showCallsAfterEdit = host.overlayWindow.showCallCount;

    host.TriggerHotkey(captureId);

    EXPECT_TRUE(host.overlayWindow.IsVisible());  // unchanged - still shown, still edit mode
    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_EQ(host.overlayWindow.showCallCount, showCallsAfterEdit);  // no re-show, already visible
    EXPECT_EQ(test::Model(controller.GetSession()).CurrentOrNull()->items.size(), 1u);
}

TEST(TrayControllerTest, QuickCaptureHotkeySwitchesFromViewToEditModeInPlace) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int viewId = FindHotkeyId(host, config.hotkeyViewMode);
    const int captureId = FindHotkeyId(host, config.hotkeyQuickCapture);
    host.TriggerHotkey(viewId);  // hidden -> view
    ASSERT_TRUE(host.overlayWindow.IsVisible());
    ASSERT_TRUE(controller.Overlay().IsViewOnly());
    const int showCallsAfterView = host.overlayWindow.showCallCount;
    const int hideCallsAfterView = host.overlayWindow.hideCallCount;

    host.TriggerHotkey(captureId);

    EXPECT_TRUE(host.overlayWindow.IsVisible());  // stays visible - no hide/reshow
    EXPECT_FALSE(controller.Overlay().IsViewOnly());  // switched into edit mode
    EXPECT_FALSE(host.overlayWindow.inputPassthrough);
    EXPECT_EQ(host.overlayWindow.showCallCount, showCallsAfterView);
    EXPECT_EQ(host.overlayWindow.hideCallCount, hideCallsAfterView);
    EXPECT_EQ(test::Model(controller.GetSession()).CurrentOrNull()->items.size(), 1u);
}

TEST(TrayControllerTest, TrayToggleCommandBehavesLikeEditHotkey) {
    test::FakePlatformHost host;
    TrayController controller(host, DefaultConfig());
    ASSERT_TRUE(controller.Initialize());

    host.TriggerTrayCommand(platform::TrayCommand::ToggleOverlay);

    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_FALSE(controller.Overlay().IsViewOnly());
}

TEST(TrayControllerTest, TrayExitCommandQuitsHost) {
    test::FakePlatformHost host;
    TrayController controller(host, DefaultConfig());
    ASSERT_TRUE(controller.Initialize());

    host.TriggerTrayCommand(platform::TrayCommand::Exit);

    EXPECT_TRUE(host.quitCalled);
}

// The OS can decline to give the overlay a window at all - a display
// with nothing to render on, a renderer that fails to come up. Every way
// in tries to make it and stops there when it cannot: nothing is shown,
// nothing is captured, and the next hotkey tries again rather than
// remembering a failure that may have been momentary.
TEST(TrayControllerTest, WhenTheWindowCannotBeMadeNothingIsShownOrCaptured) {
    test::FakePlatformHost host;
    host.overlayWindow.createSucceeds = false;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize()) << "the window is only made on first use";

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));
    EXPECT_FALSE(host.overlayWindow.IsVisible());
    EXPECT_EQ(host.overlayWindow.showCallCount, 0);

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyViewMode));
    EXPECT_FALSE(host.overlayWindow.IsVisible());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyQuickCapture));
    EXPECT_EQ(host.overlayWindow.captureCallCount, 0);
    EXPECT_TRUE(test::Model(controller.GetSession()).CurrentOrNull()->items.empty());
    EXPECT_FALSE(host.overlayWindow.IsVisible());

    EXPECT_EQ(host.overlayWindow.ensureCreatedCallCount, 3) << "asked again each time, not given up on";

    // And once the OS relents, the same hotkey works as it always did.
    host.overlayWindow.createSucceeds = true;
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));
    EXPECT_TRUE(host.overlayWindow.IsVisible());
}

// ================= Persistence (real temp-directory LibraryStore, no ImGui context needed) =================

class TrayControllerPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / (std::string("spickzettel_tray_app_persistence_test_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        library_ = dir_ / "library.db";
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static std::string ReadFile(const std::filesystem::path& path) {
        std::ifstream in(path);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    std::filesystem::path dir_;
    std::filesystem::path library_;
};

// A deleted canvas is loaded with the rest, hidden - and a library saved
// looking at a canvas since deleted opens on one that is shown. A deleted
// snippet is not: only undo could have brought it back, and no history
// survives a restart.
TEST_F(TrayControllerPersistenceTest, InitializeLoadsDeletedCanvasesHiddenAndErasesDeletedSnippets) {
    CanvasManagerSnapshot snapshot;
    Folder folder;
    folder.id = 1;
    folder.name = "F";
    snapshot.folders.push_back(folder);
    Canvas live;
    live.id = 2;
    live.name = "Live";
    live.folderId = 1;
    Item item;
    item.id = 3;
    item.name = "Gone";
    item.deletedAt = 100;
    live.items.push_back(item);
    snapshot.canvases.push_back(live);
    Canvas deleted;
    deleted.id = 4;
    deleted.name = "Deleted";
    deleted.folderId = 1;
    deleted.deletedAt = static_cast<int64_t>(std::time(nullptr));  // within the retention period
    snapshot.canvases.push_back(deleted);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 4;
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(snapshot));

    test::FakePlatformHost host;
    host.libraryPath = library_;
    TrayController controller(host, DefaultConfig());
    ASSERT_TRUE(controller.Initialize());

    const CanvasManager& manager = test::Model(controller.GetSession());
    EXPECT_EQ(manager.CurrentCanvasId(), 2u) << "not the deleted canvas it was saved on";
    ASSERT_NE(manager.FindCanvas(2), nullptr);
    EXPECT_TRUE(manager.FindCanvas(2)->items.empty());
    ASSERT_NE(manager.FindCanvas(4), nullptr);
    EXPECT_TRUE(manager.IsDeleted(*manager.FindCanvas(4)));
}

// The retention period runs as the library is opened: with it on, a canvas
// deleted longer ago than the period is erased, and one deleted since is
// kept; with it off, both are kept.
TEST_F(TrayControllerPersistenceTest, InitializeErasesWhatWasDeletedLongerAgoThanTheRetentionPeriod) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t day = 24 * 60 * 60;
    CanvasManagerSnapshot snapshot;
    Folder folder;
    folder.id = 1;
    folder.name = "F";
    snapshot.folders.push_back(folder);
    const auto canvas = [&snapshot](CanvasId id, int64_t deletedAt) {
        Canvas c;
        c.id = id;
        c.name = "C" + std::to_string(id);
        c.folderId = 1;
        c.deletedAt = deletedAt;
        snapshot.canvases.push_back(c);
    };
    canvas(2, 0);
    canvas(3, now - 40 * day);
    canvas(4, now - 20 * day);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 2;

    for (const bool purge : {false, true}) {
        std::filesystem::remove_all(dir_);
        ASSERT_TRUE(persistence::LibraryStore(library_).Save(snapshot));
        AppConfig config = DefaultConfig();
        config.purgeDeleted = purge;
        config.purgeDeletedAfterDays = 30;
        test::FakePlatformHost host;
        host.libraryPath = library_;
        TrayController controller(host, config);
        ASSERT_TRUE(controller.Initialize());

        const CanvasManager& manager = test::Model(controller.GetSession());
        EXPECT_NE(manager.FindCanvas(2), nullptr);
        EXPECT_EQ(manager.FindCanvas(3) == nullptr, purge) << (purge ? "past the period" : "retention is off");
        EXPECT_NE(manager.FindCanvas(4), nullptr) << "not deleted long enough ago";
    }
}

// Settings standing in for a config.json that could not be read: nothing is
// erased by a retention period nobody could read, and with the file kept
// where it was, nothing is written over it.
TEST_F(TrayControllerPersistenceTest, StandInSettingsEraseNothingAndLeaveTheFileAlone) {
    CanvasManagerSnapshot snapshot;
    Folder folder;
    folder.id = 1;
    folder.name = "F";
    snapshot.folders.push_back(folder);
    Canvas live;
    live.id = 2;
    live.name = "Live";
    live.folderId = 1;
    snapshot.canvases.push_back(live);
    Canvas old;
    old.id = 3;
    old.name = "Old";
    old.folderId = 1;
    old.deletedAt = static_cast<int64_t>(std::time(nullptr)) - 400 * 24 * 60 * 60;
    snapshot.canvases.push_back(old);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 2;
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(snapshot));
    std::ofstream(dir_ / "config.json") << "not settings";

    test::FakePlatformHost host;
    host.libraryPath = library_;
    host.configFilePath = dir_ / "config.json";
    AppConfig config = DefaultConfig();
    ASSERT_TRUE(config.purgeDeleted);
    TrayController controller(host, config);
    controller.StartOnStandInSettings(/*keepFile=*/true);
    ASSERT_TRUE(controller.Initialize());
    EXPECT_NE(test::Model(controller.GetSession()).FindCanvas(3), nullptr);

    controller.GetSettings().Mutable().strokeWidth = 12.0f;
    controller.GetSettings().Commit();
    EXPECT_EQ(ReadFile(dir_ / "config.json"), "not settings");
}

// A library a newer build wrote refuses the start, before the tray icon,
// and says so apart from any other failure; the library is left as it was.
TEST_F(TrayControllerPersistenceTest, InitializeRefusesALibraryWrittenByANewerVersion) {
    CanvasManagerSnapshot snapshot;
    Folder folder;
    folder.id = 1;
    folder.name = "F";
    snapshot.folders.push_back(folder);
    Canvas canvas;
    canvas.id = 2;
    canvas.name = "C";
    canvas.folderId = 1;
    snapshot.canvases.push_back(canvas);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 2;
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(snapshot));
    {
        sqlite3* db = nullptr;
        const std::u8string name = library_.u8string();
        sqlite3_open(std::string(name.begin(), name.end()).c_str(), &db);
        const std::string newer =
            "PRAGMA user_version = " + std::to_string(persistence::LibraryStore::kFormatVersion + 1);
        ASSERT_EQ(sqlite3_exec(db, newer.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }
    const std::string before = ReadFile(library_);

    test::FakePlatformHost host;
    host.libraryPath = library_;
    TrayController controller(host, DefaultConfig());
    EXPECT_FALSE(controller.Initialize());
    EXPECT_TRUE(controller.RefusedANewerLibrary());
    EXPECT_FALSE(controller.RefusedAnUnreadableLibrary());
    EXPECT_FALSE(host.trayIconShown);
    EXPECT_EQ(ReadFile(library_), before);
}

// A library that cannot be read - held by a program that shares it with
// nobody - refuses the start as well, and is told apart: trying again
// later may work.
TEST_F(TrayControllerPersistenceTest, InitializeRefusesALibraryFileItCannotRead) {
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(CanvasManager().ExportSnapshot()));
    HeldLibrary held(library_, /*readers=*/false);
    test::FakePlatformHost host;
    host.libraryPath = library_;
    TrayController controller(host, DefaultConfig());
    EXPECT_FALSE(controller.Initialize());
    EXPECT_TRUE(controller.RefusedAnUnreadableLibrary());
    EXPECT_FALSE(host.trayIconShown);
}

// A file that is not a library at all is kept beside it, and the app
// starts on an empty one - with the caller told where the file went.
TEST_F(TrayControllerPersistenceTest, InitializeSetsAsideAFileThatIsNotALibrary) {
    std::filesystem::create_directories(dir_);
    std::ofstream(library_) << "not a library";
    test::FakePlatformHost host;
    host.libraryPath = library_;
    TrayController controller(host, DefaultConfig());
    ASSERT_TRUE(controller.Initialize());
    ASSERT_FALSE(controller.LibrarySetAsideAs().empty());
    EXPECT_EQ(ReadFile(controller.LibrarySetAsideAs()), "not a library");
}

TEST_F(TrayControllerPersistenceTest, InitializeLoadsAPreviouslySavedLibrary) {
    CanvasManagerSnapshot snapshot;
    Folder folder;
    folder.id = 1;
    folder.name = "Loaded Folder";
    snapshot.folders.push_back(folder);
    Canvas canvas;
    canvas.id = 2;
    canvas.name = "Loaded Canvas";
    canvas.folderId = 1;
    snapshot.canvases.push_back(canvas);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 2;
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(snapshot));

    test::FakePlatformHost host;
    host.libraryPath = library_;
    TrayController controller(host, DefaultConfig());

    ASSERT_TRUE(controller.Initialize());

    ASSERT_EQ(test::Model(controller.GetSession()).Canvases().size(), 1u);
    EXPECT_EQ(test::Model(controller.GetSession()).CurrentOrNull()->name, "Loaded Canvas");
}

// A capture that comes back with pixels - which is what a real backend's
// does - is written to the library as the snippet's image, and shown from
// the pixels captured rather than read back. The fake's default capture
// fails, and every other test here is content with that.
TEST_F(TrayControllerPersistenceTest, ACaptureWithPixelsIsSavedAsTheSnippetsImage) {
    test::FakePlatformHost host;
    host.libraryPath = library_;
    host.overlayWindow.uploadsSucceed = true;
    host.overlayWindow.captureReturnsWidth = 4;
    host.overlayWindow.captureReturnsHeight = 3;
    host.overlayWindow.captureReturnsPixelsRGBA.assign(4u * 3u * 4u, 0x80);
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyQuickCapture));

    const Canvas* canvas = test::Model(controller.GetSession()).CurrentOrNull();
    ASSERT_NE(canvas, nullptr);
    ASSERT_EQ(canvas->items.size(), 1u);
    const Item& shot = canvas->items.front();
    const std::optional<uint64_t> texture =
        controller.GetSession().Textures().Find(TextureKey{TextureKey::Kind::Picture, shot.id});
    ASSERT_TRUE(texture.has_value());
    EXPECT_TRUE(host.overlayWindow.IsDrawable(*texture));
    ASSERT_TRUE(shot.picture.stored) << "the pixels went to the library";
    const std::optional<persistence::DecodedImage> saved = persistence::LibraryStore(library_).LoadImage(shot.id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->width, 4);
    EXPECT_EQ(saved->height, 3);
    EXPECT_EQ(saved->pixelsRGBA, host.overlayWindow.captureReturnsPixelsRGBA);
}

// A silent capture while the overlay is hidden, with notices off, shows
// nothing - and no frame follows it. It is in the library all the same,
// picture and all, written as it was made.
TEST_F(TrayControllerPersistenceTest, ASilentCaptureWhileHiddenIsOnDiskWithoutAFrame) {
    // A library on disk already, so this is not a first run - which would
    // show the overlay with its welcome note rather than start hidden.
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(CanvasManager().ExportSnapshot()));
    test::FakePlatformHost host;
    host.libraryPath = library_;
    host.overlayWindow.captureReturnsWidth = 2;
    host.overlayWindow.captureReturnsHeight = 1;
    host.overlayWindow.captureReturnsPixelsRGBA = {1, 2, 3, 255, 4, 5, 6, 255};
    AppConfig config = DefaultConfig();
    config.showToastsWhileHidden = false;
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeySilentCapture));
    EXPECT_FALSE(host.overlayWindow.IsVisible());
    EXPECT_EQ(host.backgroundTimerIntervalMs, 0) << "nothing to retry";

    persistence::LibraryStore reopened(library_);
    const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
    ASSERT_TRUE(loaded.has_value());
    const Item* shot = nullptr;
    for (const Canvas& canvas : loaded->canvases) {
        for (const Item& item : canvas.items) {
            shot = &item;
        }
    }
    ASSERT_NE(shot, nullptr);
    ASSERT_TRUE(shot->picture.stored);
    const std::optional<persistence::DecodedImage> saved = reopened.LoadImage(shot->id);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->pixelsRGBA, host.overlayWindow.captureReturnsPixelsRGBA);
}

// ===== When the library cannot be written =====

// A capture while hidden whose write fails is not made - nothing that looks
// captured and is not in the library - and the overlay says so when it is
// next up. Nothing is left to retry: the file holds what the app holds.
TEST_F(TrayControllerPersistenceTest, ACaptureWhileHiddenThatCannotBeWrittenIsNotMade) {
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(CanvasManager().ExportSnapshot()));
    test::FakePlatformHost host;
    host.libraryPath = library_;
    host.overlayWindow.captureReturnsWidth = 1;
    host.overlayWindow.captureReturnsHeight = 1;
    host.overlayWindow.captureReturnsPixelsRGBA = {9, 9, 9, 255};
    AppConfig config = DefaultConfig();
    config.showToastsWhileHidden = false;
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    {
        HeldLibrary held(library_, /*readers=*/true);
        host.TriggerHotkey(FindHotkeyId(host, config.hotkeySilentCapture));
    }
    size_t items = 0;
    for (const Canvas& canvas : controller.GetSession().Manager().Canvases()) {
        items += canvas.items.size();
    }
    EXPECT_EQ(items, 0u) << "not made";
    EXPECT_TRUE(controller.GetSession().LastWriteFailed());
    EXPECT_FALSE(controller.Overlay().PersistenceWarning().empty());
    EXPECT_EQ(host.backgroundTimerIntervalMs, 0) << "nothing owed";
}

// A capture is on disk as it is made; the OS ending the session finds
// nothing owed.
TEST_F(TrayControllerPersistenceTest, TheOSEndingTheSessionFindsTheLibraryWritten) {
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(CanvasManager().ExportSnapshot()));
    test::FakePlatformHost host;
    host.libraryPath = library_;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyQuickCapture));

    host.TriggerSessionEnd();  // logoff, with no frame between the capture and it
    const std::optional<CanvasManagerSnapshot> loaded = persistence::LibraryStore(library_).Load();
    ASSERT_TRUE(loaded.has_value());
    size_t items = 0;
    for (const Canvas& canvas : loaded->canvases) {
        items += canvas.items.size();
    }
    EXPECT_EQ(items, 1u);
}

TEST_F(TrayControllerPersistenceTest, ASettingsFileThatCannotBeWrittenIsSaidOnScreenUntilItIs) {
    std::filesystem::create_directories(dir_ / "config.json");  // a directory where the file goes
    test::FakePlatformHost host;
    host.configFilePath = dir_ / "config.json";
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    EXPECT_TRUE(controller.Overlay().PersistenceWarning().empty()) << "nothing written yet, nothing failed";

    ASSERT_TRUE(controller.ChangeHotkey(HotkeySlot::ViewMode, platform::KeyCombo{true, false, true, 'Z'}));
    EXPECT_NE(controller.Overlay().PersistenceWarning().find("config.json"), std::string::npos)
        << "applied in memory, and said to be unsaved";
    ASSERT_GT(host.backgroundTimerIntervalMs, 0) << "owed, so tried again without waiting for another edit";

    host.FireBackgroundTimer();
    EXPECT_FALSE(controller.Overlay().PersistenceWarning().empty()) << "still in the way";
    EXPECT_GT(host.backgroundTimerIntervalMs, 0);

    std::filesystem::remove_all(dir_ / "config.json");
    host.FireBackgroundTimer();
    EXPECT_TRUE(controller.Overlay().PersistenceWarning().empty()) << "cleared by the write that landed";
    EXPECT_EQ(host.backgroundTimerIntervalMs, 0) << "nothing owed any more";
    EXPECT_TRUE(std::filesystem::is_regular_file(dir_ / "config.json"));
    EXPECT_EQ(ParseConfig(ReadFile(dir_ / "config.json")).hotkeyViewMode, (platform::KeyCombo{true, false, true, 'Z'}));
}

// A settings file set aside whose stand-in could not be written in its
// place: the write is owed and tried again, retention off and all - left
// unwritten, the next start found no file, made the defaults, and had
// retention back on.
TEST_F(TrayControllerPersistenceTest, TheStandInForASettingsFileSetAsideIsWrittenOnceItCanBe) {
    std::filesystem::create_directories(dir_ / "config.json");  // in the way of the stand-in
    test::FakePlatformHost host;
    host.configFilePath = dir_ / "config.json";
    AppConfig config = DefaultConfig();
    config.purgeDeleted = false;  // as LoadOrCreateConfig hands the stand-in over
    TrayController controller(host, config);
    controller.StartOnStandInSettings(/*keepFile=*/false);
    ASSERT_TRUE(controller.Initialize());
    EXPECT_FALSE(controller.Overlay().PersistenceWarning().empty());
    ASSERT_GT(host.backgroundTimerIntervalMs, 0) << "owed";

    std::filesystem::remove_all(dir_ / "config.json");
    host.FireBackgroundTimer();
    ASSERT_TRUE(std::filesystem::is_regular_file(dir_ / "config.json"));
    EXPECT_FALSE(ParseConfig(ReadFile(dir_ / "config.json")).purgeDeleted);
}

TEST_F(TrayControllerPersistenceTest, ExitWritesASettingsFileStillOwed) {
    std::filesystem::create_directories(dir_ / "config.json");
    test::FakePlatformHost host;
    host.configFilePath = dir_ / "config.json";
    TrayController controller(host, DefaultConfig());
    ASSERT_TRUE(controller.Initialize());
    ASSERT_TRUE(controller.ChangeHotkey(HotkeySlot::ViewMode, platform::KeyCombo{true, false, true, 'Z'}));
    ASSERT_FALSE(controller.Overlay().PersistenceWarning().empty());

    std::filesystem::remove_all(dir_ / "config.json");  // writable again, and no timer has fired since
    host.TriggerTrayCommand(platform::TrayCommand::Exit);
    EXPECT_TRUE(host.quitCalled);
    EXPECT_TRUE(std::filesystem::is_regular_file(dir_ / "config.json")) << "the last chance was taken";
    EXPECT_EQ(ParseConfig(ReadFile(dir_ / "config.json")).hotkeyViewMode, (platform::KeyCombo{true, false, true, 'Z'}));
}

// The actual rescale math (position/size/fullscreen/clamping behavior) is
// covered exhaustively in canvas_manager_test.cpp against CanvasManager
// directly. This just guards the wiring decision that Initialize() itself
// never touches an item's rect at all - OverlayApp::OnFrame does that
// live, every frame, against ImGui's own DisplaySize (see
// CanvasManager::SyncItemsToDisplaySize's own doc comment) - which can't
// be exercised here without a real ImGui context. host's display is
// deliberately different from the loaded item's own saved rect below to
// prove Initialize() doesn't react to it at all.
TEST_F(TrayControllerPersistenceTest, InitializeLeavesItemRectsUntouchedUntilTheFirstFrameActuallyRenders) {
    CanvasManagerSnapshot snapshot;
    Folder folder;
    folder.id = 1;
    folder.name = "F";
    snapshot.folders.push_back(folder);
    Canvas canvas;
    canvas.id = 2;
    canvas.name = "C";
    canvas.folderId = 1;
    Item item;
    item.id = 3;
    item.name = "A";
    item.rect = Rect{200, 150, 300, 200};
    canvas.items.push_back(item);
    snapshot.canvases.push_back(canvas);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 2;
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(snapshot));

    test::FakePlatformHost host;
    host.libraryPath = library_;
    // Nothing at Initialize acts on the display size; items are laid out
    // against it on the first frame instead.
    host.displays.front().width = 2000;
    host.displays.front().height = 1000;
    TrayController controller(host, DefaultConfig());

    ASSERT_TRUE(controller.Initialize());

    ASSERT_EQ(test::Model(controller.GetSession()).CurrentOrNull()->items.size(), 1u);
    EXPECT_EQ(test::Model(controller.GetSession()).CurrentOrNull()->items.front().rect, (Rect{200, 150, 300, 200}));
}

TEST_F(TrayControllerPersistenceTest, InitializeLeavesDefaultStateWhenNothingSavedYet) {
    test::FakePlatformHost host;
    host.libraryPath = library_;  // not there yet
    TrayController controller(host, DefaultConfig());

    ASSERT_TRUE(controller.Initialize());

    ASSERT_EQ(test::Model(controller.GetSession()).Canvases().size(), 1u);
    // Named for the moment it was made, like every canvas nobody has named
    // - "2026-09-07 22:36:14", the one shape a test can check.
    const std::string& name = test::Model(controller.GetSession()).CurrentOrNull()->name;
    EXPECT_EQ(name.size(), 19u) << name;
    EXPECT_EQ(name[4], '-') << name;
    EXPECT_EQ(name[10], ' ') << name;
}

// A first run writes the library it begins with, and every change after
// is in the file as it is made - a capture before the overlay is hidden.
TEST_F(TrayControllerPersistenceTest, AFirstRunsLibraryAndACaptureAreOnDiskAsTheyAreMade) {
    test::FakePlatformHost host;
    host.libraryPath = library_;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    ASSERT_EQ(persistence::LibraryStore(library_).Load()->canvases.size(), 1u) << "the library it began with";

    // QuickCapture creates an item (a real CanvasManager mutation) purely
    // through plain data/platform calls - no ImGui context needed, unlike
    // most of OverlayApp's rendering, so this is safe to drive directly
    // from a FakePlatformHost-based test - see ShowActionToast's own
    // ImGui::GetCurrentContext() guard.
    const int captureId = FindHotkeyId(host, config.hotkeyQuickCapture);
    host.TriggerHotkey(captureId);
    ASSERT_EQ(test::Model(controller.GetSession()).CurrentOrNull()->items.size(), 1u);

    const std::optional<CanvasManagerSnapshot> reloaded = persistence::LibraryStore(library_).Load();
    ASSERT_TRUE(reloaded.has_value());
    // The capture made a canvas of its own, at the end of the folder - so
    // two now, and the shot is on the second (see OverlayApp::QuickCapture).
    ASSERT_EQ(reloaded->canvases.size(), 2u);
    EXPECT_TRUE(reloaded->canvases[0].items.empty());
    EXPECT_EQ(reloaded->canvases[1].items.size(), 1u);
    // Named for when it was made (see TimestampName), so the shape is what
    // can be asserted here: "2026-09-07 22:36:14". The first canvas is
    // named the same way now, and two made in the same second share a
    // name - names are not identity - so it is the ids that are distinct.
    EXPECT_EQ(reloaded->canvases[1].name.size(), 19u) << "was: " << reloaded->canvases[1].name;
    EXPECT_NE(reloaded->canvases[1].id, reloaded->canvases[0].id);
}

TEST_F(TrayControllerPersistenceTest, TrayExitLeavesTheLibraryWritten) {
    test::FakePlatformHost host;
    host.libraryPath = library_;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    const int captureId = FindHotkeyId(host, config.hotkeyQuickCapture);
    host.TriggerHotkey(captureId);

    host.TriggerTrayCommand(platform::TrayCommand::Exit);

    EXPECT_TRUE(host.quitCalled);
    const std::optional<CanvasManagerSnapshot> reloaded = persistence::LibraryStore(library_).Load();
    ASSERT_TRUE(reloaded.has_value());
    ASSERT_EQ(reloaded->canvases.size(), 2u);  // the capture's own canvas - see QuickCapture
    EXPECT_EQ(reloaded->canvases[1].items.size(), 1u);
}

TEST_F(TrayControllerPersistenceTest, NothingIsWrittenWhenTheLibraryPathIsEmpty) {
    test::FakePlatformHost host;  // libraryPath left empty - see its own doc comment
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    const int captureId = FindHotkeyId(host, config.hotkeyQuickCapture);
    host.TriggerHotkey(captureId);
    host.TriggerTrayCommand(platform::TrayCommand::Exit);

    EXPECT_TRUE(host.quitCalled);
    // Nothing to assert on disk (there's no meaningful path to check) -
    // this test's real job is making sure none of the above crashes when
    // no library store is attached at all.
}

// ===== Per-application profiles =====

namespace {
AppConfig ConfigWithGameProfile() {
    AppConfig config = DefaultConfig();
    config.profileable.counterRawMouseInput = true;
    config.profileable.freezeScreen = true;

    Profile profile;
    profile.name = "The Game";
    profile.match.executables.push_back("game.exe");
    profile.overrides.counterRawMouseInput = false;
    config.profiles.push_back(std::move(profile));
    return config;
}
}  // namespace

TEST(TrayControllerProfileTest, TheMatchingProfilesSettingsAreWhatTheWindowIsGiven) {
    test::FakePlatformHost host;
    host.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "The Game"};
    const AppConfig config = ConfigWithGameProfile();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));

    EXPECT_TRUE(host.overlayWindow.visible);
    EXPECT_FALSE(host.overlayWindow.editModeInput.counterRawMouseInput);
}

// Edit mode from a notice switches no mode in place: the notice applied no
// profile, and edit mode comes up as from hidden, with the game's.
TEST(TrayControllerProfileTest, EditModeFromANoticeRunsTheMatchingProfile) {
    test::FakePlatformHost host;
    host.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "The Game"};
    const AppConfig config = ConfigWithGameProfile();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeySilentCapture));
    ASSERT_TRUE(host.overlayWindow.IsVisible()) << "the notice";
    ASSERT_TRUE(host.overlayWindow.editModeInput.counterRawMouseInput) << "no profile for a notice";

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));

    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_FALSE(host.overlayWindow.editModeInput.counterRawMouseInput);
}

// The same from the pinned view by way of view mode, which it switches to
// in place.
TEST(TrayControllerProfileTest, EditModeFromThePinnedViewByWayOfViewModeRunsTheMatchingProfile) {
    test::FakePlatformHost host;
    const AppConfig config = ConfigWithGameProfile();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    host.TriggerHotkey(editId);
    CanvasManager& manager = test::Model(controller.GetSession());
    manager.FindItemAnywhere(manager.CreateItem(false, Rect{100, 100, 300, 200}, "Pinned"))->pinned = true;
    host.TriggerHotkey(editId);
    ASSERT_TRUE(controller.Overlay().IsPinnedOnly());

    host.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "The Game"};
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyViewMode));
    ASSERT_FALSE(controller.Overlay().IsPinnedOnly());
    host.TriggerHotkey(editId);

    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_FALSE(host.overlayWindow.editModeInput.counterRawMouseInput);
}

// And from the pinned view that view mode was put away into in place - the
// window never went down, and still carried the profile view mode came up
// with, for whatever was underneath then.
TEST(TrayControllerProfileTest, EditModeFromThePinnedViewViewModeWasPutAwayIntoRunsTheMatchingProfile) {
    test::FakePlatformHost host;
    host.overlayWindow.underlyingApp = platform::ForegroundApp{"notepad.exe", "Untitled"};
    const AppConfig config = ConfigWithGameProfile();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    CanvasManager& manager = test::Model(controller.GetSession());
    manager.FindItemAnywhere(manager.CreateItem(false, Rect{100, 100, 300, 200}, "Pinned"))->pinned = true;
    const int viewId = FindHotkeyId(host, config.hotkeyViewMode);
    host.TriggerHotkey(viewId);
    host.TriggerHotkey(viewId);
    ASSERT_TRUE(controller.Overlay().IsPinnedOnly());
    ASSERT_TRUE(host.overlayWindow.IsVisible()) << "put away in place";

    host.overlayWindow.underlyingApp = platform::ForegroundApp{"game.exe", "The Game"};
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));

    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_FALSE(host.overlayWindow.editModeInput.counterRawMouseInput) << "the game's profile";
}

TEST(TrayControllerProfileTest, NothingUnderneathMeansTheDefaultsRun) {
    test::FakePlatformHost host;
    host.overlayWindow.underlyingApp = platform::ForegroundApp{"notepad.exe", "Untitled"};
    const AppConfig config = ConfigWithGameProfile();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));

    EXPECT_TRUE(host.overlayWindow.editModeInput.counterRawMouseInput);
}

// ===== Taking focus from an elevated application =====

namespace {
platform::ForegroundApp AppAt(const char* executable, platform::ForegroundIntegrity integrity) {
    platform::ForegroundApp app;
    app.executable = executable;
    app.title = "A Window";
    app.integrity = integrity;
    return app;
}

bool ShowEditModeOver(test::FakePlatformHost& host, const AppConfig& config,
                       const platform::ForegroundApp& app) {
    host.overlayWindow.underlyingApp = app;
    TrayController controller(host, config);
    EXPECT_TRUE(controller.Initialize());
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));
    EXPECT_TRUE(host.overlayWindow.visible);
    return host.overlayWindow.editModeNoActivate;
}
}  // namespace

// Windows gives a lower-integrity process none of an elevated
// application's input, so leaving it focused costs the overlay not only
// the grab but every shortcut it has. Focus is taken instead.
TEST(TrayControllerProfileTest, AnElevatedApplicationHasFocusTakenFromIt) {
    test::FakePlatformHost host;
    EXPECT_FALSE(ShowEditModeOver(host, DefaultConfig(),
                                   AppAt("taskmgr.exe", platform::ForegroundIntegrity::Above)));
}

// The other two answers change nothing, and Unknown is the one that
// matters: a game behind an anti-cheat driver refuses the integrity query
// exactly as an elevated tool would, and taking focus from it is the one
// thing this whole mode exists to prevent. So only a positive reading acts.
TEST(TrayControllerProfileTest, AnApplicationThatIsNotAboveUsKeepsItsFocus) {
    test::FakePlatformHost host;
    EXPECT_TRUE(ShowEditModeOver(host, DefaultConfig(),
                                  AppAt("notepad.exe", platform::ForegroundIntegrity::NotAbove)));
}

TEST(TrayControllerProfileTest, AnApplicationThatRefusesTheQuestionKeepsItsFocus) {
    test::FakePlatformHost host;
    EXPECT_TRUE(ShowEditModeOver(host, DefaultConfig(),
                                  AppAt("game.exe", platform::ForegroundIntegrity::Unknown)));
}

// Off globally, an elevated application is treated like any other: the
// overlay stays hands-off and is simply deaf, which is a choice a person
// is allowed to make.
TEST(TrayControllerProfileTest, TheSettingTurnedOffLeavesAnElevatedApplicationAlone) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.profileable.takeFocusOverElevated = false;
    EXPECT_TRUE(ShowEditModeOver(host, config, AppAt("taskmgr.exe", platform::ForegroundIntegrity::Above)));
}

// And off for one application only, which is the case the setting is
// profileable for: an elevated game, where a deaf overlay still shows
// pinned snippets and still captures.
TEST(TrayControllerProfileTest, AProfileMayKeepHandsOffOneElevatedApplication) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    Profile profile;
    profile.name = "Game";
    profile.match.executables.push_back("game.exe");
    profile.overrides.takeFocusOverElevated = false;
    config.profiles.push_back(profile);

    EXPECT_TRUE(ShowEditModeOver(host, config, AppAt("game.exe", platform::ForegroundIntegrity::Above)));

    test::FakePlatformHost other;
    EXPECT_FALSE(ShowEditModeOver(other, config,
                                   AppAt("taskmgr.exe", platform::ForegroundIntegrity::Above)))
        << "the profile speaks for its own application only";
}

// The decision is made once per session rather than once per mode, and
// that is what makes view-only into edit mode work: the two switch in
// place within a session, without coming up from hidden and without
// asking again. Deciding this per mode would leave the overlay deaf in exactly
// the case it is here to fix, two keypresses in.
TEST(TrayControllerProfileTest, EnteringEditModeFromViewOnlyKeepsTheFocusItTook) {
    test::FakePlatformHost host;
    host.overlayWindow.underlyingApp = AppAt("taskmgr.exe", platform::ForegroundIntegrity::Above);
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyViewMode));
    ASSERT_TRUE(host.overlayWindow.visible);
    ASSERT_FALSE(host.overlayWindow.editModeNoActivate);

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));

    EXPECT_TRUE(host.overlayWindow.visible);
    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_FALSE(host.overlayWindow.editModeNoActivate) << "switched in place, still holding focus";
    EXPECT_FALSE(host.overlayWindow.inputPassthrough);
}

// What is stored stays as the user set it: the Settings row goes on
// showing their answer rather than silently rewriting itself because of
// what happened to be in front of the overlay once.
TEST(TrayControllerProfileTest, TakingFocusDoesNotRewriteTheStoredSetting) {
    test::FakePlatformHost host;
    host.overlayWindow.underlyingApp = AppAt("taskmgr.exe", platform::ForegroundIntegrity::Above);
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));

    EXPECT_FALSE(host.overlayWindow.editModeNoActivate) << "focus taken for this showing";
    EXPECT_TRUE(controller.GetSettings().Stored().profileable.dontStealFocus) << "but not written down";
    EXPECT_TRUE(controller.GetSettings().Live().dontStealFocus);
}

// ===== Pinned snippets =====

namespace {
ItemId PinASnippet(TrayController& controller) {
    CanvasManager& manager = test::Model(controller.GetSession());
    const ItemId id = manager.CreateItem(false, Rect{100, 100, 300, 200}, "Pinned");
    manager.FindItemAnywhere(id)->pinned = true;
    return id;
}
}  // namespace

TEST(TrayControllerPinnedTest, PuttingTheOverlayAwayLeavesThePinnedSnippetsUp) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    host.TriggerHotkey(editId);
    PinASnippet(controller);

    host.TriggerHotkey(editId);

    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_TRUE(controller.Overlay().IsPinnedOnly());
    EXPECT_TRUE(controller.Overlay().IsViewOnly());
    EXPECT_TRUE(host.overlayWindow.inputPassthrough) << "the pinned view takes no input";
    EXPECT_EQ(host.overlayWindow.hideCallCount, 0) << "in place: the pinned snippets never blink";
}

TEST(TrayControllerPinnedTest, UnpinningIsHowThePinnedViewGoes) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    host.TriggerHotkey(editId);
    const ItemId id = PinASnippet(controller);
    host.TriggerHotkey(editId);
    ASSERT_TRUE(controller.Overlay().IsPinnedOnly());

    host.TriggerHotkey(editId);
    test::Model(controller.GetSession()).FindItemAnywhere(id)->pinned = false;
    host.TriggerHotkey(editId);

    EXPECT_FALSE(host.overlayWindow.IsVisible());
    EXPECT_FALSE(controller.Overlay().IsPinnedOnly());
}

TEST(TrayControllerPinnedTest, TheEditHotkeyLeavesThePinnedViewAsIfFromHidden) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    host.TriggerHotkey(editId);
    PinASnippet(controller);
    host.TriggerHotkey(editId);
    ASSERT_TRUE(controller.Overlay().IsPinnedOnly());
    const int hidesBefore = host.overlayWindow.hideCallCount;
    const int showsBefore = host.overlayWindow.showCallCount;

    host.TriggerHotkey(editId);

    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_FALSE(controller.Overlay().IsPinnedOnly());
    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_FALSE(host.overlayWindow.inputPassthrough);
    EXPECT_EQ(host.overlayWindow.hideCallCount, hidesBefore + 1);
    EXPECT_EQ(host.overlayWindow.showCallCount, showsBefore + 1);
}

TEST(TrayControllerPinnedTest, TheViewHotkeySwitchesBetweenThePinnedViewAndEverySnippetInPlace) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    PinASnippet(controller);
    const int viewId = FindHotkeyId(host, config.hotkeyViewMode);
    host.TriggerHotkey(viewId);  // hidden -> view: every snippet
    ASSERT_TRUE(controller.Overlay().IsViewOnly());
    ASSERT_FALSE(controller.Overlay().IsPinnedOnly());
    const int hidesBefore = host.overlayWindow.hideCallCount;

    host.TriggerHotkey(viewId);  // view -> pinned view
    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_TRUE(controller.Overlay().IsPinnedOnly());

    host.TriggerHotkey(viewId);  // pinned view -> view
    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_TRUE(controller.Overlay().IsViewOnly());
    EXPECT_FALSE(controller.Overlay().IsPinnedOnly());
    EXPECT_EQ(host.overlayWindow.hideCallCount, hidesBefore) << "neither switch should hide the window";
}

TEST(TrayControllerPinnedTest, ASilentCaptureInThePinnedViewKeepsThePinnedCanvasOnScreen) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    host.TriggerHotkey(editId);
    PinASnippet(controller);
    host.TriggerHotkey(editId);
    ASSERT_TRUE(controller.Overlay().IsPinnedOnly());
    const CanvasId pinnedCanvas = test::Model(controller.GetSession()).CurrentCanvasId();

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeySilentCapture));

    EXPECT_EQ(test::Model(controller.GetSession()).Canvases().size(), 2u) << "the capture still has a canvas of its own";
    EXPECT_EQ(test::Model(controller.GetSession()).CurrentCanvasId(), pinnedCanvas);
    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_TRUE(controller.Overlay().IsPinnedOnly());
}

// The capture lands on a canvas that is not current, so its texture is
// pure cost: nothing draws it, and it goes with the frame after next -
// without anyone saying so.
TEST_F(TrayControllerPersistenceTest, ASilentCaptureInThePinnedViewGivesItsTextureBack) {
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(CanvasManager().ExportSnapshot()));
    test::FakePlatformHost host;
    host.libraryPath = library_;
    host.overlayWindow.uploadsSucceed = true;
    host.overlayWindow.captureReturnsWidth = 2;
    host.overlayWindow.captureReturnsHeight = 1;
    host.overlayWindow.captureReturnsPixelsRGBA = {1, 2, 3, 255, 4, 5, 6, 255};
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    host.TriggerHotkey(editId);
    PinASnippet(controller);
    host.TriggerHotkey(editId);
    ASSERT_TRUE(controller.Overlay().IsPinnedOnly());
    const CanvasId pinnedCanvas = test::Model(controller.GetSession()).CurrentCanvasId();

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeySilentCapture));

    ASSERT_EQ(test::Model(controller.GetSession()).CurrentCanvasId(), pinnedCanvas);
    const Item* shot = nullptr;
    for (const Canvas& canvas : test::Model(controller.GetSession()).Canvases()) {
        if (canvas.id == pinnedCanvas) {
            continue;
        }
        for (const Item& item : canvas.items) {
            shot = &item;
        }
    }
    ASSERT_NE(shot, nullptr);
    ASSERT_TRUE(shot->picture.stored) << "in the library, so the texture is not the only copy";
    TextureCache& textures = controller.GetSession().Textures();
    const TextureKey key{TextureKey::Kind::Picture, shot->id};
    const std::optional<uint64_t> texture = textures.Find(key);
    ASSERT_TRUE(texture.has_value()) << "made from the pixels captured";
    textures.BeginFrame();
    textures.BeginFrame();  // the pinned view draws the canvas it shows, and nothing of this one
    EXPECT_FALSE(textures.Find(key).has_value());
    EXPECT_EQ(host.overlayWindow.liveTextures.count(*texture), 0u) << "not resident on a canvas nobody is looking at";
}

TEST_F(TrayControllerPersistenceTest, PinnedSnippetsAreOnScreenFromTheStart) {
    CanvasManagerSnapshot snapshot;
    Folder folder;
    folder.id = 1;
    folder.name = "F";
    snapshot.folders.push_back(folder);
    Canvas canvas;
    canvas.id = 2;
    canvas.name = "C";
    canvas.folderId = 1;
    Item item;
    item.id = 3;
    item.name = "Pinned";
    item.rect = Rect{200, 150, 300, 200};
    item.pinned = true;
    canvas.items.push_back(item);
    snapshot.canvases.push_back(canvas);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 2;
    ASSERT_TRUE(persistence::LibraryStore(library_).Save(snapshot));

    test::FakePlatformHost host;
    host.libraryPath = library_;
    TrayController controller(host, DefaultConfig());
    ASSERT_TRUE(controller.Initialize());

    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_TRUE(controller.Overlay().IsPinnedOnly());
    EXPECT_TRUE(host.overlayWindow.inputPassthrough);
}

// ===== Displays =====

namespace {
// A second display, to the left of the primary and listed before it - the
// order a real listing gives them - so "the first display" and "the
// primary display" are different answers.
void AttachDisplayOnTheLeft(test::FakePlatformHost& host) {
    host.displays.insert(host.displays.begin(),
                         platform::DisplayInfo{"fake-left", "Left Display", -2560, 0, 2560, 1440, false, 60, 100});
}

platform::DisplayInfo& PrimaryOf(test::FakePlatformHost& host) {
    return *std::find_if(host.displays.begin(), host.displays.end(), [](const auto& d) { return d.primary; });
}
}  // namespace

TEST(TrayControllerDisplayTest, TheOverlayComesUpOnThePrimaryDisplayByDefault) {
    test::FakePlatformHost host;
    AttachDisplayOnTheLeft(host);
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));

    EXPECT_EQ(host.overlayWindow.onDisplay.id, "fake-primary");
}

TEST(TrayControllerDisplayTest, AChosenDisplayIsWhereTheOverlayComesUp) {
    test::FakePlatformHost host;
    AttachDisplayOnTheLeft(host);
    AppConfig config = DefaultConfig();
    config.overlayDisplayId = "fake-left";
    config.overlayDisplayName = "Left Display";
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));

    EXPECT_EQ(host.overlayWindow.onDisplay.id, "fake-left");
}

TEST(TrayControllerDisplayTest, WhileTheChosenDisplayIsNotConnectedThePrimaryStandsIn) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.overlayDisplayId = "fake-left";
    config.overlayDisplayName = "Left Display";
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));
    EXPECT_EQ(host.overlayWindow.onDisplay.id, "fake-primary");

    // Plugged back in while the overlay is up, it is taken back there - and
    // the choice was never forgotten meanwhile.
    AttachDisplayOnTheLeft(host);
    host.overlayWindow.displaysChangedCallback();
    EXPECT_EQ(host.overlayWindow.onDisplay.id, "fake-left");
    EXPECT_EQ(controller.GetSettings().Stored().overlayDisplayId, "fake-left");
}

TEST(TrayControllerDisplayTest, AQuickCaptureWhileHiddenIsTakenFromTheChosenDisplay) {
    test::FakePlatformHost host;
    AttachDisplayOnTheLeft(host);
    AppConfig config = DefaultConfig();
    config.overlayDisplayId = "fake-left";
    config.overlayDisplayName = "Left Display";
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyQuickCapture));

    EXPECT_EQ(host.overlayWindow.onDisplay.id, "fake-left");
    ASSERT_GE(host.overlayWindow.captureCallCount, 1);
    EXPECT_FLOAT_EQ(host.overlayWindow.lastCaptureRect.w, 2560.0f);
}

TEST(TrayControllerDisplayTest, ChoosingAnotherDisplayWhileTheOverlayIsUpMovesItThere) {
    test::FakePlatformHost host;
    AttachDisplayOnTheLeft(host);
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));
    ASSERT_EQ(host.overlayWindow.onDisplay.id, "fake-primary");

    controller.GetSettings().Mutable().overlayDisplayId = "fake-left";
    controller.GetSettings().Mutable().overlayDisplayName = "Left Display";
    controller.GetSettings().Commit();
    host.RunPostedTasks();  // after the frame the choice was made in

    EXPECT_EQ(host.overlayWindow.onDisplay.id, "fake-left");
}

TEST(TrayControllerDisplayTest, WhenItsDisplayChangesTheOverlayFollows) {
    test::FakePlatformHost host;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));
    ASSERT_EQ(host.overlayWindow.onDisplay.width, 1920);

    PrimaryOf(host).width = 2560;
    PrimaryOf(host).height = 1440;
    ASSERT_TRUE(host.overlayWindow.displaysChangedCallback);
    host.overlayWindow.displaysChangedCallback();

    EXPECT_EQ(host.overlayWindow.onDisplay.width, 2560);
    EXPECT_EQ(host.overlayWindow.onDisplay.height, 1440);
}

TEST(TrayControllerDisplayTest, AFrozenScreenIsTakenAgainWhenItsDisplayChanges) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.profileable.freezeScreen = true;
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));
    ASSERT_EQ(host.overlayWindow.captureCallCount, 1);

    PrimaryOf(host).width = 2560;
    PrimaryOf(host).height = 1440;
    host.overlayWindow.displaysChangedCallback();

    EXPECT_EQ(host.overlayWindow.captureCallCount, 2);
    EXPECT_FLOAT_EQ(host.overlayWindow.lastCaptureRect.w, 2560.0f);
}

TEST(TrayControllerDisplayTest, AChangeToAnotherDisplayLeavesAFrozenScreenAlone) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.profileable.freezeScreen = true;
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));
    ASSERT_EQ(host.overlayWindow.captureCallCount, 1);

    AttachDisplayOnTheLeft(host);
    host.overlayWindow.displaysChangedCallback();

    EXPECT_EQ(host.overlayWindow.captureCallCount, 1);
    EXPECT_EQ(host.overlayWindow.onDisplay.id, "fake-primary");
}


}  // namespace
}  // namespace sz::test
