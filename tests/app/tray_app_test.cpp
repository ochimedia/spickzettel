#include "app/tray_app.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "core/persistence/library_store.h"
#include "fakes/fake_platform_host.h"

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
    config.editModeNoActivate = false;
    TrayController controller(host, config);

    ASSERT_TRUE(controller.Initialize());

    EXPECT_EQ(host.overlayWindow.setEditModeNoActivateCallCount, 1);
    EXPECT_FALSE(host.overlayWindow.editModeNoActivate);
}

TEST(TrayControllerTest, InitializeConfiguresEditModeInputFromConfig) {
    test::FakePlatformHost host;
    AppConfig config = DefaultConfig();
    config.editModeInput.useRawMouseInput = true;
    config.editModeInput.counterRawMouseInput = true;
    config.editModeInput.dontForwardKeystrokes = false;
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
}

TEST(TrayControllerTest, InitializeFailsIfHotkeyRegistrationFails) {
    test::FakePlatformHost host;
    host.registerHotkeySucceeds = false;
    TrayController controller(host, DefaultConfig());

    EXPECT_FALSE(controller.Initialize());
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
        std::filesystem::temp_directory_path() / "spickzettel_tray_app_change_hotkey_test_config.txt";
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
    ASSERT_TRUE(controller.GetSession().Manager().CurrentOrNull()->items.empty());

    host.TriggerHotkey(captureId);

    EXPECT_TRUE(host.overlayWindow.IsVisible());  // now shown, so the capture is noticed
    EXPECT_FALSE(controller.Overlay().IsViewOnly());
    EXPECT_EQ(host.overlayWindow.showCallCount, 1);
    EXPECT_EQ(controller.GetSession().Manager().CurrentOrNull()->items.size(), 1u);
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
    EXPECT_EQ(controller.GetSession().Manager().CurrentOrNull()->items.size(), 1u);
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
    EXPECT_EQ(controller.GetSession().Manager().CurrentOrNull()->items.size(), 1u);
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
    EXPECT_TRUE(controller.GetSession().Manager().CurrentOrNull()->items.empty());
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
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

// What is deleted is loaded with the rest, hidden - and a library saved
// looking at a canvas since deleted opens on one that is shown.
TEST_F(TrayControllerPersistenceTest, InitializeLoadsDeletedThingsHiddenInPlace) {
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
    deleted.deletedAt = 200;
    snapshot.canvases.push_back(deleted);
    snapshot.currentFolderId = 1;
    snapshot.currentCanvasId = 4;
    ASSERT_TRUE(persistence::LibraryStore(dir_).Save(snapshot));

    test::FakePlatformHost host;
    host.dataDirectoryPath = dir_;
    TrayController controller(host, DefaultConfig());
    ASSERT_TRUE(controller.Initialize());

    const CanvasManager& manager = controller.GetSession().Manager();
    EXPECT_EQ(manager.CurrentCanvasId(), 2u) << "not the deleted canvas it was saved on";
    EXPECT_TRUE(manager.IsItemDeleted(3));
    ASSERT_NE(manager.FindCanvas(4), nullptr);
    EXPECT_TRUE(manager.IsDeleted(*manager.FindCanvas(4)));
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
    ASSERT_TRUE(persistence::LibraryStore(dir_).Save(snapshot));

    test::FakePlatformHost host;
    host.dataDirectoryPath = dir_;
    TrayController controller(host, DefaultConfig());

    ASSERT_TRUE(controller.Initialize());

    ASSERT_EQ(controller.GetSession().Manager().Canvases().size(), 1u);
    EXPECT_EQ(controller.GetSession().Manager().CurrentOrNull()->name, "Loaded Canvas");
}

// A capture that comes back with pixels - which is what a real backend's
// does - is written to the library as the snippet's image, beside the
// texture it is shown with. The fake's default capture has a handle and
// nothing to save, and every other test here is content with that.
TEST_F(TrayControllerPersistenceTest, ACaptureWithPixelsIsSavedAsTheSnippetsImage) {
    test::FakePlatformHost host;
    host.dataDirectoryPath = dir_;
    host.overlayWindow.captureReturnsHandle = 7;
    host.overlayWindow.captureReturnsWidth = 4;
    host.overlayWindow.captureReturnsHeight = 3;
    host.overlayWindow.captureReturnsPixelsRGBA.assign(4u * 3u * 4u, 0x80);
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyQuickCapture));

    const Canvas* canvas = controller.GetSession().Manager().CurrentOrNull();
    ASSERT_NE(canvas, nullptr);
    ASSERT_EQ(canvas->items.size(), 1u);
    const Item& shot = canvas->items.front();
    ASSERT_NE(shot.ImageLayer(), nullptr);
    EXPECT_EQ(shot.ImageLayer()->textureHandle, 7u);
    ASSERT_FALSE(shot.ImageLayer()->imageFile.empty()) << "the pixels went to the library";
    const std::optional<persistence::DecodedImage> saved =
        persistence::LibraryStore(dir_).LoadImage(shot.id, shot.ImageLayer()->imageFile);
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->width, 4);
    EXPECT_EQ(saved->height, 3);
    EXPECT_EQ(saved->pixelsRGBA, host.overlayWindow.captureReturnsPixelsRGBA);
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
    ASSERT_TRUE(persistence::LibraryStore(dir_).Save(snapshot));

    test::FakePlatformHost host;
    host.dataDirectoryPath = dir_;
    // Nothing at Initialize acts on the display size; items are laid out
    // against it on the first frame instead.
    host.displays.front().width = 2000;
    host.displays.front().height = 1000;
    TrayController controller(host, DefaultConfig());

    ASSERT_TRUE(controller.Initialize());

    ASSERT_EQ(controller.GetSession().Manager().CurrentOrNull()->items.size(), 1u);
    EXPECT_EQ(controller.GetSession().Manager().CurrentOrNull()->items.front().rect, (Rect{200, 150, 300, 200}));
}

TEST_F(TrayControllerPersistenceTest, InitializeLeavesDefaultStateWhenNothingSavedYet) {
    test::FakePlatformHost host;
    host.dataDirectoryPath = dir_;  // exists (SetUp doesn't create it) but has no library.json
    TrayController controller(host, DefaultConfig());

    ASSERT_TRUE(controller.Initialize());

    ASSERT_EQ(controller.GetSession().Manager().Canvases().size(), 1u);
    // Named for the moment it was made, like every canvas nobody has named
    // - "2026-09-07 22:36:14", the one shape a test can check.
    const std::string& name = controller.GetSession().Manager().CurrentOrNull()->name;
    EXPECT_EQ(name.size(), 19u) << name;
    EXPECT_EQ(name[4], '-') << name;
    EXPECT_EQ(name[10], ' ') << name;
}

TEST_F(TrayControllerPersistenceTest, HidingTheOverlayFlushesAPendingChangeToDisk) {
    test::FakePlatformHost host;
    host.dataDirectoryPath = dir_;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());
    ASSERT_FALSE(std::filesystem::exists(dir_ / "library.json"));

    // QuickCapture creates an item (a real CanvasManager mutation) purely
    // through plain data/platform calls - no ImGui context needed, unlike
    // most of OverlayApp's rendering, so this is safe to drive directly
    // from a FakePlatformHost-based test - see ShowActionToast's own
    // ImGui::GetCurrentContext() guard.
    const int captureId = FindHotkeyId(host, config.hotkeyQuickCapture);
    host.TriggerHotkey(captureId);
    ASSERT_EQ(controller.GetSession().Manager().CurrentOrNull()->items.size(), 1u);

    // QuickCapture itself already lands in edit mode (see its own test
    // above) - toggle the edit hotkey again to hide, which is one of the
    // two explicit flush points (see TrayController::ToggleMode).
    const int editId = FindHotkeyId(host, config.hotkeyEditMode);
    host.TriggerHotkey(editId);
    ASSERT_FALSE(host.overlayWindow.IsVisible());

    ASSERT_TRUE(std::filesystem::exists(dir_ / "library.json"));
    const std::optional<CanvasManagerSnapshot> reloaded = persistence::LibraryStore(dir_).Load();
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

TEST_F(TrayControllerPersistenceTest, TrayExitFlushesAPendingChangeToDisk) {
    test::FakePlatformHost host;
    host.dataDirectoryPath = dir_;
    const AppConfig config = DefaultConfig();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    const int captureId = FindHotkeyId(host, config.hotkeyQuickCapture);
    host.TriggerHotkey(captureId);

    host.TriggerTrayCommand(platform::TrayCommand::Exit);

    EXPECT_TRUE(host.quitCalled);
    ASSERT_TRUE(std::filesystem::exists(dir_ / "library.json"));
    const std::optional<CanvasManagerSnapshot> reloaded = persistence::LibraryStore(dir_).Load();
    ASSERT_TRUE(reloaded.has_value());
    ASSERT_EQ(reloaded->canvases.size(), 2u);  // the capture's own canvas - see QuickCapture
    EXPECT_EQ(reloaded->canvases[1].items.size(), 1u);
}

TEST_F(TrayControllerPersistenceTest, NothingIsWrittenWhenDataDirectoryPathIsEmpty) {
    test::FakePlatformHost host;  // dataDirectoryPath left empty - see its own doc comment
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
    config.editModeInput.counterRawMouseInput = true;
    config.freezeScreenInEditMode = true;

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

TEST(TrayControllerProfileTest, NothingUnderneathMeansTheDefaultsRun) {
    test::FakePlatformHost host;
    host.overlayWindow.underlyingApp = platform::ForegroundApp{"notepad.exe", "Untitled"};
    const AppConfig config = ConfigWithGameProfile();
    TrayController controller(host, config);
    ASSERT_TRUE(controller.Initialize());

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeyEditMode));

    EXPECT_TRUE(host.overlayWindow.editModeInput.counterRawMouseInput);
}

// ===== Pinned snippets =====

namespace {
ItemId PinASnippet(TrayController& controller) {
    CanvasManager& manager = controller.GetSession().Manager();
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
    EXPECT_TRUE(host.overlayWindow.inputPassthrough);
    EXPECT_EQ(host.overlayWindow.showWithoutActivatingCallCount, 1) << "the pinned view never takes focus";
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
    controller.GetSession().Manager().FindItemAnywhere(id)->pinned = false;
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
    const CanvasId pinnedCanvas = controller.GetSession().Manager().CurrentCanvasId();

    host.TriggerHotkey(FindHotkeyId(host, config.hotkeySilentCapture));

    EXPECT_EQ(controller.GetSession().Manager().Canvases().size(), 2u) << "the capture still has a canvas of its own";
    EXPECT_EQ(controller.GetSession().Manager().CurrentCanvasId(), pinnedCanvas);
    EXPECT_TRUE(host.overlayWindow.IsVisible());
    EXPECT_TRUE(controller.Overlay().IsPinnedOnly());
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
    ASSERT_TRUE(persistence::LibraryStore(dir_).Save(snapshot));

    test::FakePlatformHost host;
    host.dataDirectoryPath = dir_;
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
    host.overlayWindow.captureReturnsHandle = 7;
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
    host.overlayWindow.captureReturnsHandle = 7;
    AppConfig config = DefaultConfig();
    config.freezeScreenInEditMode = true;
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
    host.overlayWindow.captureReturnsHandle = 7;
    AppConfig config = DefaultConfig();
    config.freezeScreenInEditMode = true;
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
