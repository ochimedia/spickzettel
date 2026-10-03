#include "core/config/app_config.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/canvas/item.h"  // kNoteTextSizeMax
#include "core/config/config_migrations.h"
#include "support/temp_dir.h"

namespace sz::core {
namespace {

// A config file holding exactly one setting. Most of these tests are about
// one value's own reading rules, and this keeps that the visible part
// instead of a JSON document per assertion. `value` is JSON, so a string
// setting is written with its quotes: One("appearance", "imageFilter",
// R"("lanczos")").
std::string One(const char* group, const char* key, const std::string& value) {
    return std::string("{\"") + group + "\":{\"" + key + "\":" + value + "}}";
}

TEST(AppConfigTest, DefaultConfigMatchesHotkeyEditMode) {
    const AppConfig config = DefaultConfig();
    EXPECT_TRUE(config.hotkeyEditMode.ctrl);
    EXPECT_TRUE(config.hotkeyEditMode.alt);
    EXPECT_FALSE(config.hotkeyEditMode.shift);
    EXPECT_EQ(config.hotkeyEditMode.key, 'S');
}

TEST(AppConfigTest, DefaultConfigMatchesHotkeyViewMode) {
    const AppConfig config = DefaultConfig();
    EXPECT_TRUE(config.hotkeyViewMode.ctrl);
    EXPECT_TRUE(config.hotkeyViewMode.alt);
    EXPECT_FALSE(config.hotkeyViewMode.shift);
    EXPECT_EQ(config.hotkeyViewMode.key, 'V');
}

TEST(AppConfigTest, DefaultConfigMatchesHotkeyQuickCapture) {
    const AppConfig config = DefaultConfig();
    EXPECT_TRUE(config.hotkeyQuickCapture.ctrl);
    EXPECT_TRUE(config.hotkeyQuickCapture.alt);
    EXPECT_FALSE(config.hotkeyQuickCapture.shift);
    EXPECT_EQ(config.hotkeyQuickCapture.key, 'C');
}

TEST(AppConfigTest, DefaultHotkeysAreAllDistinct) {
    const AppConfig config = DefaultConfig();
    EXPECT_NE(config.hotkeyEditMode, config.hotkeyViewMode);
    EXPECT_NE(config.hotkeyEditMode, config.hotkeyQuickCapture);
    EXPECT_NE(config.hotkeyViewMode, config.hotkeyQuickCapture);
}

TEST(AppConfigTest, EditModeNoActivateDefaultsToOn) { EXPECT_TRUE(DefaultConfig().profileable.dontStealFocus); }

TEST(AppConfigTest, ParsesEditModeNoActivate) {
    EXPECT_TRUE(ParseConfig(One("behavior", "dontStealFocus", "true")).profileable.dontStealFocus);
    EXPECT_FALSE(ParseConfig(One("behavior", "dontStealFocus", "false")).profileable.dontStealFocus);
    // A value of the wrong type is a value the file failed to state, so the
    // default stands.
    EXPECT_TRUE(ParseConfig(One("behavior", "dontStealFocus", R"("yes")")).profileable.dontStealFocus);
    EXPECT_TRUE(ParseConfig(One("behavior", "dontStealFocus", "1")).profileable.dontStealFocus);
}

// On by default, because over an elevated application every other input
// setting is a no-op: Windows gives a lower-integrity process none of that
// application's input, so an overlay that keeps its hands off gets nothing
// at all. Off is the deliberate choice to accept that.
TEST(AppConfigTest, TakeFocusOverElevatedDefaultsToOn) {
    EXPECT_TRUE(DefaultConfig().profileable.takeFocusOverElevated);
}

TEST(AppConfigTest, ParsesTakeFocusOverElevated) {
    EXPECT_TRUE(ParseConfig(One("behavior", "takeFocusOverElevated", "true")).profileable.takeFocusOverElevated);
    EXPECT_FALSE(ParseConfig(One("behavior", "takeFocusOverElevated", "false")).profileable.takeFocusOverElevated);
    EXPECT_TRUE(ParseConfig(One("behavior", "takeFocusOverElevated", R"("no")")).profileable.takeFocusOverElevated);
}

TEST(AppConfigTest, EveryEditModeInputOptionButCounteringDefaultsToOn) {
    const platform::EditModeInputOptions options = DefaultConfig().profileable.InputOptions();
    EXPECT_TRUE(options.useSoftwarePointer);
    EXPECT_TRUE(options.useRawMouseInput);
    EXPECT_TRUE(options.dontForwardKeystrokes);
    EXPECT_FALSE(options.counterRawMouseInput);
}

// Switching the pointer off switches the pointer off, whatever else is on:
// read together with raw input, a grab could keep a pointer drawn that this
// option had said not to draw.
TEST(AppConfigTest, TheSoftwarePointerOptionAloneDecidesWhetherAPointerIsDrawn) {
    platform::EditModeInputOptions options;  // everything on
    EXPECT_TRUE(options.SoftwarePointerDrawn());

    options.useSoftwarePointer = false;
    ASSERT_TRUE(options.useRawMouseInput);
    EXPECT_FALSE(options.SoftwarePointerDrawn());

    // And the grab carries on: with nothing drawn, the position it keeps is
    // written to the real cursor instead (Win32InputGrab::
    // PublishVirtualCursor), so "swallowing with no pointer" is still not a
    // state that exists.
    EXPECT_TRUE(options.RawMouseInputCanBeUsed(/*gameKeepsFocus=*/true));
}

// Nothing is left to take once edit mode holds focus the ordinary way: the
// game has stopped receiving input, raw input included.
TEST(AppConfigTest, HoldingKeystrokesNeedsTheGameToKeepFocus) {
    EXPECT_TRUE(platform::EditModeInputOptions::KeystrokesCanBeHeld(/*gameKeepsFocus=*/true));
    EXPECT_FALSE(platform::EditModeInputOptions::KeystrokesCanBeHeld(/*gameKeepsFocus=*/false));
}

// Taking the mouse says nothing about which pointer is shown: the grab
// keeps the position either way and either draws it or writes it to the
// real cursor, so the software pointer is not a precondition.
TEST(AppConfigTest, TakingTheMouseDoesNotRequireDrawingThePointer) {
    platform::EditModeInputOptions options;
    options.useSoftwarePointer = false;
    EXPECT_TRUE(options.RawMouseInputCanBeUsed(/*gameKeepsFocus=*/true));
    EXPECT_FALSE(options.RawMouseInputCanBeUsed(/*gameKeepsFocus=*/false));
}

// Countering does need the mouse taken, and that one is not plumbing: the
// correction reaches the game through the same pointer arithmetic Windows
// moves its cursor with, so with Windows still driving that cursor it
// cancels the user's own slow movement along with the game's.
TEST(AppConfigTest, CounteringNeedsTheMouseTaken) {
    platform::EditModeInputOptions options;
    ASSERT_TRUE(options.useRawMouseInput);
    EXPECT_TRUE(options.CounterRawMouseInputCanBeUsed(/*gameKeepsFocus=*/true));
    EXPECT_FALSE(options.CounterRawMouseInputCanBeUsed(/*gameKeepsFocus=*/false));

    options.useRawMouseInput = false;
    EXPECT_FALSE(options.CounterRawMouseInputCanBeUsed(/*gameKeepsFocus=*/true));

    // But not a drawn pointer - that is the half that stopped being a
    // precondition.
    options.useRawMouseInput = true;
    options.useSoftwarePointer = false;
    EXPECT_TRUE(options.CounterRawMouseInputCanBeUsed(/*gameKeepsFocus=*/true));
}

// The chain is transitive: losing the root disables every link below it, so
// no combination of the leaves can re-enable one.
TEST(AppConfigTest, LosingFocusDisablesTheWholePointerChain) {
    platform::EditModeInputOptions options;  // everything on
    EXPECT_FALSE(platform::EditModeInputOptions::KeystrokesCanBeHeld(false));
    EXPECT_FALSE(options.RawMouseInputCanBeUsed(false));
    EXPECT_FALSE(options.CounterRawMouseInputCanBeUsed(false));

    // The software pointer is the exception, and deliberately so: it changes
    // what is drawn, not what the game receives.
    EXPECT_TRUE(options.SoftwarePointerDrawn());
}

TEST(AppConfigTest, ParsesEachEditModeInputOptionIndependently) {
    EXPECT_FALSE(ParseConfig(One("behavior", "softwarePointer", "false")).profileable.softwarePointer);
    EXPECT_FALSE(ParseConfig(One("behavior", "rawMouseInput", "false")).profileable.rawMouseInput);
    EXPECT_FALSE(
        ParseConfig(One("behavior", "dontForwardKeystrokes", "false")).profileable.dontForwardKeystrokes);
    EXPECT_TRUE(
        ParseConfig(One("behavior", "counterRawMouseInput", "true")).profileable.counterRawMouseInput);

    // Disabling one leaves the others alone - they're separate experiments,
    // not one setting with four names.
    const platform::EditModeInputOptions onlyMouse =
        ParseConfig(One("behavior", "rawMouseInput", "false")).profileable.InputOptions();
    EXPECT_FALSE(onlyMouse.useRawMouseInput);
    EXPECT_TRUE(onlyMouse.useSoftwarePointer);
    EXPECT_TRUE(onlyMouse.dontForwardKeystrokes);
    EXPECT_FALSE(onlyMouse.counterRawMouseInput);
}

TEST(AppConfigTest, CounterThresholdIsReadClampedAndRoundTrips) {
    using platform::EditModeInputOptions;
    EXPECT_EQ(ParseConfig(One("behavior", "counterThreshold", "42")).profileable.counterThreshold, 42);
    EXPECT_EQ(ParseConfig(One("behavior", "counterThreshold", "0")).profileable.counterThreshold,
              EditModeInputOptions::kCounterThresholdMin);
    EXPECT_EQ(ParseConfig(One("behavior", "counterThreshold", "1000000")).profileable.counterThreshold,
              EditModeInputOptions::kCounterThresholdMax);
    EXPECT_EQ(ParseConfig(One("behavior", "counterThreshold", "\"lots\"")).profileable.counterThreshold,
              EditModeInputOptions{}.counterThreshold);

    AppConfig config = DefaultConfig();
    config.profileable.counterThreshold = 123;
    EXPECT_EQ(ParseConfig(SerializeConfig(config)).profileable.counterThreshold, 123);
}

TEST(AppConfigTest, InputOptionsHudDefaultsToOffAndRoundTrips) {
    EXPECT_FALSE(DefaultConfig().showInputOptionsHud);
    EXPECT_TRUE(ParseConfig(One("diagnostics", "showInputOptionsHud", "true")).showInputOptionsHud);

    AppConfig config = DefaultConfig();
    config.showInputOptionsHud = true;
    EXPECT_TRUE(ParseConfig(SerializeConfig(config)).showInputOptionsHud);
}

TEST(AppConfigTest, FrameGraphDefaultsToOffAndRoundTrips) {
    EXPECT_FALSE(DefaultConfig().showFrameGraph);
    EXPECT_TRUE(ParseConfig(One("diagnostics", "showFrameGraph", "true")).showFrameGraph);

    AppConfig config = DefaultConfig();
    config.showFrameGraph = true;
    EXPECT_TRUE(ParseConfig(SerializeConfig(config)).showFrameGraph);
}

TEST(AppConfigTest, ParseEmptyTextYieldsDefaults) { EXPECT_EQ(ParseConfig(""), DefaultConfig()); }

TEST(AppConfigTest, ParseGarbageYieldsDefaults) {
    EXPECT_EQ(ParseConfig("{ this is not json"), DefaultConfig());
    EXPECT_EQ(ParseConfig("[1, 2, 3]"), DefaultConfig());  // valid JSON, wrong shape
    EXPECT_EQ(ParseConfig("null"), DefaultConfig());
}

TEST(AppConfigTest, AGroupOfTheWrongTypeReadsAsAbsent) {
    // Not a crash and not a partial read: a `input: 5` is a file that said
    // nothing about any input setting.
    EXPECT_EQ(ParseConfig(R"({"behavior": 5})"), DefaultConfig());
    EXPECT_EQ(ParseConfig(R"({"appearance": "wide"})"), DefaultConfig());
}

TEST(AppConfigTest, AStrokeWidthPastTheCeilingIsHeldToIt) {
    EXPECT_FLOAT_EQ(ParseConfig(One("drawing", "strokeWidth", "1000000")).strokeWidth, kMaxStrokeWidthPx);
    EXPECT_FLOAT_EQ(ParseConfig(One("drawing", "strokeWidth", "12")).strokeWidth, 12.0f) << "inside it, as typed";
}

TEST(AppConfigTest, ANumberTooLargeForAFloatReadsAsTheDefault) {
    EXPECT_FLOAT_EQ(ParseConfig(One("drawing", "strokeWidth", "1e100")).strokeWidth, DefaultConfig().strokeWidth);
}

TEST(AppConfigTest, AnUnboundHotkeySurvivesARoundTrip) {
    AppConfig config = DefaultConfig();
    config.hotkeyViewMode = platform::KeyCombo{};
    const std::string text = SerializeConfig(config);
    EXPECT_NE(text.find("\"viewMode\": null"), std::string::npos) << text;

    const AppConfig parsed = ParseConfig(text);
    EXPECT_FALSE(parsed.hotkeyViewMode.IsValid()) << "read back as unbound, not as the default";
    EXPECT_EQ(parsed, config);
}

TEST(AppConfigTest, ANullHotkeyReadsAsUnboundAndAMissingOneAsTheDefault) {
    EXPECT_FALSE(ParseConfig(One("hotkeys", "quickCapture", "null")).hotkeyQuickCapture.IsValid());
    EXPECT_EQ(ParseConfig(One("hotkeys", "viewMode", "null")).hotkeyQuickCapture,
              DefaultConfig().hotkeyQuickCapture);
}

TEST(AppConfigTest, SerializeThenParseRoundTrips) {
    AppConfig config = DefaultConfig();
    config.hotkeyEditMode = platform::KeyCombo{true, false, true, 'D'};
    config.hotkeyViewMode = platform::KeyCombo{false, true, true, 'K'};
    config.hotkeyQuickCapture = platform::KeyCombo{true, true, true, 'S'};
    config.hotkeySilentCapture = platform::KeyCombo{false, true, true, 'Q'};
    config.showToastsWhileHidden = false;
    config.strokeColorRGBA = 0x00FF00FF;
    config.strokeWidth = 6.5f;
    config.showDebugOverlay = true;
    config.profileable.dontStealFocus = false;
    config.showItemBorders = false;
    config.raiseSelectedSnippet = false;
    config.screenshotTrigger = CreationTrigger::Alt;
    config.drawingTrigger = CreationTrigger::Off;
    config.overviewShowsStrokes = false;
    config.overviewShowsBitmaps = false;
    config.purgeDeleted = false;
    config.purgeDeletedAfterDays = 21;
    config.confirmDelete = false;
    config.confirmDeleteForGood = false;
    config.showEditModeBorder = false;
    config.editModeBorderColorRGBA = 0x5AA9FF66u;
    config.editModeBorderWidthPx = 16.0f;
    config.editModeBorderOnlyWhenEmpty = true;
    config.profileable.freezeScreen = true;
    config.showInputOptionsHud = true;
    config.profileable.softwarePointer = false;
    config.profileable.rawMouseInput = true;
    config.profileable.dontForwardKeystrokes = false;
    config.profileable.counterRawMouseInput = true;  // off by default

    const std::string text = SerializeConfig(config);
    const AppConfig parsed = ParseConfig(text);

    EXPECT_EQ(parsed, config);
}

TEST(AppConfigTest, SerializedFileIsAnObjectWithAVersion) {
    // The version is written from the first release rather than added when
    // it is first needed: a migration that has to guess which shape it is
    // looking at has already lost.
    const std::string text = SerializeConfig(DefaultConfig());
    EXPECT_NE(text.find("\"version\""), std::string::npos);
    EXPECT_EQ(text.front(), '{');
}

TEST(AppConfigTest, DebugOverlayDefaultsToOff) { EXPECT_FALSE(DefaultConfig().showDebugOverlay); }

TEST(AppConfigTest, ParsesDebugOverlay) {
    EXPECT_TRUE(ParseConfig(One("diagnostics", "showDebugOverlay", "true")).showDebugOverlay);
    EXPECT_FALSE(ParseConfig(One("diagnostics", "showDebugOverlay", "false")).showDebugOverlay);
    EXPECT_FALSE(ParseConfig(One("diagnostics", "showDebugOverlay", R"("on")")).showDebugOverlay);
}

TEST(AppConfigTest, ShowItemBordersDefaultsToOn) { EXPECT_TRUE(DefaultConfig().showItemBorders); }

TEST(AppConfigTest, ParsesShowItemBorders) {
    EXPECT_TRUE(ParseConfig(One("appearance", "showItemBorders", "true")).showItemBorders);
    EXPECT_FALSE(ParseConfig(One("appearance", "showItemBorders", "false")).showItemBorders);
    EXPECT_TRUE(ParseConfig(One("appearance", "showItemBorders", R"("off")")).showItemBorders);
}

TEST(AppConfigTest, ParseIsCaseInsensitiveForModifiers) {
    const AppConfig config = ParseConfig(One("hotkeys", "editMode", R"("ctrl+ALT+shift+P")"));
    EXPECT_TRUE(config.hotkeyEditMode.ctrl);
    EXPECT_TRUE(config.hotkeyEditMode.alt);
    EXPECT_TRUE(config.hotkeyEditMode.shift);
    EXPECT_EQ(config.hotkeyEditMode.key, 'P');
}

TEST(AppConfigTest, ParsesViewModeHotkey) {
    const AppConfig config = ParseConfig(One("hotkeys", "viewMode", R"("Ctrl+Shift+L")"));
    EXPECT_TRUE(config.hotkeyViewMode.ctrl);
    EXPECT_FALSE(config.hotkeyViewMode.alt);
    EXPECT_TRUE(config.hotkeyViewMode.shift);
    EXPECT_EQ(config.hotkeyViewMode.key, 'L');
}

TEST(AppConfigTest, ParsesQuickCaptureHotkey) {
    const AppConfig config = ParseConfig(One("hotkeys", "quickCapture", R"("Ctrl+Shift+G")"));
    EXPECT_TRUE(config.hotkeyQuickCapture.ctrl);
    EXPECT_FALSE(config.hotkeyQuickCapture.alt);
    EXPECT_TRUE(config.hotkeyQuickCapture.shift);
    EXPECT_EQ(config.hotkeyQuickCapture.key, 'G');
}

TEST(AppConfigTest, ParsesBareFunctionKeyHotkeyWithNoModifier) {
    const AppConfig config = ParseConfig(One("hotkeys", "quickCapture", R"("F9")"));
    EXPECT_FALSE(config.hotkeyQuickCapture.ctrl);
    EXPECT_FALSE(config.hotkeyQuickCapture.alt);
    EXPECT_FALSE(config.hotkeyQuickCapture.shift);
    EXPECT_TRUE(config.hotkeyQuickCapture.IsFunctionKey());
    EXPECT_EQ(config.hotkeyQuickCapture.FunctionKeyNumber(), 9);
}

// A shortcut may be a mouse button - the middle one or a side one - and
// keeps it across a round trip; a global hotkey may not, since Windows
// registers keys only, and one written by hand is read as nothing said.
TEST(AppConfigTest, AShortcutMayBeAMouseButtonAndAHotkeyMayNot) {
    AppConfig config = DefaultConfig();
    config.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Draw)] =
        platform::KeyCombo{false, false, false, platform::KeyCombo::kX1Button};
    config.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Erase)] =
        platform::KeyCombo{true, false, false, platform::KeyCombo::kMiddleButton};
    config.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Text)] =
        platform::KeyCombo{false, false, true, platform::KeyCombo::kX2Button};
    const std::string text = SerializeConfig(config);
    EXPECT_NE(text.find(R"("Mouse4")"), std::string::npos) << text;
    EXPECT_NE(text.find(R"("Ctrl+Mouse3")"), std::string::npos) << text;
    EXPECT_NE(text.find(R"("Shift+Mouse5")"), std::string::npos) << text;
    EXPECT_EQ(ParseConfig(text), config);

    EXPECT_EQ(ParseConfig(One("hotkeys", "editMode", R"("Mouse4")")).hotkeyEditMode,
              DefaultConfig().hotkeyEditMode);
}

// A file from before an action existed says nothing of it, and the action
// has its default - unless the file gave that combination to another one,
// which keeps it: Ctrl+Shift+V put on Duplicate by hand before there was a
// Paste in place.
TEST(AppConfigTest, ANewActionDoesNotTakeACombinationTheFileGaveAway) {
    const auto pasteInPlace = [](const AppConfig& config) {
        return config.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::PasteInPlace)];
    };
    EXPECT_EQ(pasteInPlace(ParseConfig(One("shortcuts", "duplicate", R"("Ctrl+D")"))),
              (platform::KeyCombo{true, false, true, 'V'}))
        << "nothing in the way: the default";

    const AppConfig taken = ParseConfig(One("shortcuts", "duplicate", R"("Ctrl+Shift+V")"));
    EXPECT_EQ(taken.profileable.shortcuts[ShortcutActionIndex(ShortcutAction::Duplicate)],
              (platform::KeyCombo{true, false, true, 'V'}));
    EXPECT_FALSE(pasteInPlace(taken).IsValid());

    // Said, it is what was said, even the same combination: that is the
    // file's own clash, as a Settings edit would not have left it.
    const AppConfig both =
        ParseConfig(R"({"shortcuts": {"duplicate": "Ctrl+Shift+V", "pasteInPlace": "Ctrl+Shift+V"}})");
    EXPECT_EQ(pasteInPlace(both), (platform::KeyCombo{true, false, true, 'V'}));
}

TEST(AppConfigTest, ParsesFunctionKeyHotkeyWithModifiers) {
    const AppConfig config = ParseConfig(One("hotkeys", "editMode", R"("Ctrl+Shift+F24")"));
    EXPECT_TRUE(config.hotkeyEditMode.ctrl);
    EXPECT_TRUE(config.hotkeyEditMode.shift);
    EXPECT_TRUE(config.hotkeyEditMode.IsFunctionKey());
    EXPECT_EQ(config.hotkeyEditMode.FunctionKeyNumber(), 24);
}

TEST(AppConfigTest, RejectsOutOfRangeFunctionKeyNumber) {
    const AppConfig config = ParseConfig(One("hotkeys", "editMode", R"("F25")"));
    EXPECT_EQ(config.hotkeyEditMode, DefaultConfig().hotkeyEditMode);
}

TEST(AppConfigTest, FunctionKeyHotkeySerializeThenParseRoundTrips) {
    AppConfig config = DefaultConfig();
    config.hotkeyEditMode = platform::KeyCombo{false, false, false, platform::KeyCombo::kFunctionKeyBase + 9};
    config.hotkeyViewMode = platform::KeyCombo{true, false, false, platform::KeyCombo::kFunctionKeyBase + 1};

    const std::string text = SerializeConfig(config);
    const AppConfig parsed = ParseConfig(text);

    EXPECT_EQ(parsed, config);
}

TEST(AppConfigTest, MalformedHotkeyFallsBackToDefault) {
    const AppConfig config = ParseConfig(R"({"hotkeys":{
        "editMode": "NotAHotkey", "viewMode": "AlsoBad", "quickCapture": "StillBad"}})");
    EXPECT_EQ(config.hotkeyEditMode, DefaultConfig().hotkeyEditMode);
    EXPECT_EQ(config.hotkeyViewMode, DefaultConfig().hotkeyViewMode);
    EXPECT_EQ(config.hotkeyQuickCapture, DefaultConfig().hotkeyQuickCapture);
}

// The two bars survive the file: order, and which buttons are switched
// off.
TEST(AppConfigTest, TheBarsKeepTheirOrderAndWhatIsSwitchedOff) {
    AppConfig config = DefaultConfig();
    config.snippetBar = {{ChromeButton::Close, true},
                          {ChromeButton::Pin, false},
                          {ChromeButton::More, true},
                          {ChromeButton::Minimize, false},
                          {ChromeButton::Maximize, true}};
    config.drawingBar = {{ChromeButton::Color, true},
                          {ChromeButton::Pen, true},
                          {ChromeButton::Eraser, false},
                          {ChromeButton::Text, true}};

    const AppConfig reparsed = ParseConfig(SerializeConfig(config));

    EXPECT_EQ(reparsed.snippetBar, config.snippetBar);
    EXPECT_EQ(reparsed.drawingBar, config.drawingBar);
    EXPECT_EQ(reparsed, config);
}

// A file that says nothing about the bars keeps them as they ship, and a
// file that says something impossible is made sense of rather than
// obeyed: a name from the other bar or from no bar at all is dropped, a
// name twice is one button, and whatever the file never mentioned comes
// after what it did, shown - a button this version has and the file's
// version did not is new, and a new one arriving invisible is a feature
// that silently isn't there.
TEST(AppConfigTest, ABarReadFromTheFileEndsUpHoldingEachOfItsButtonsOnce) {
    EXPECT_EQ(ParseConfig("{}").snippetBar, DefaultSnippetBar());
    EXPECT_EQ(ParseConfig("{}").drawingBar, DefaultDrawingBar());

    const AppConfig config = ParseConfig(
        R"({"bars": {"snippet": ["close", "close", "pen", "nonsense", {"button": "pin", "shown": false}]}})");

    ASSERT_EQ(config.snippetBar.size(), kSnippetBarButtons.size());
    EXPECT_EQ(config.snippetBar[0], (BarButtonSetting{ChromeButton::Close, true})) << "a bare name is a shown button";
    EXPECT_EQ(config.snippetBar[1], (BarButtonSetting{ChromeButton::Pin, false}));
    for (size_t at = 2; at < config.snippetBar.size(); ++at) {
        EXPECT_TRUE(config.snippetBar[at].shown) << "what the file never named comes after it, shown";
    }
    EXPECT_EQ(config.drawingBar, DefaultDrawingBar()) << "the bar the file said nothing about is untouched";
}

// The bar's color button, of its drawing group, is stored as "color", in the spelling
// the rest of the file uses.
TEST(AppConfigTest, TheColorButtonIsStoredAsColor) {
    AppConfig config = DefaultConfig();
    config.drawingBar = {{ChromeButton::Color, false}, {ChromeButton::Pen, true}, {ChromeButton::Eraser, true},
                         {ChromeButton::Text, true}};
    const std::string text = SerializeConfig(config);
    EXPECT_NE(text.find("\"color\""), std::string::npos) << text;
    EXPECT_EQ(ParseConfig(text).drawingBar, config.drawingBar);
}

TEST(AppConfigTest, UnknownKeysAndGroupsAreIgnored) {
    const AppConfig config = ParseConfig(R"({
        "someFutureGroup": {"a": 1},
        "drawing": {"strokeWidth": 7, "someFutureSetting": 42}})");
    EXPECT_FLOAT_EQ(config.strokeWidth, 7.0f);
}

TEST(AppConfigTest, ParsesHexStrokeColor) {
    EXPECT_EQ(ParseConfig(One("drawing", "strokeColor", R"("#00FF00")")).strokeColorRGBA, 0x00FF00FFu);
    // Malformed colors keep the default rather than producing a stroke
    // nobody can see.
    EXPECT_EQ(ParseConfig(One("drawing", "strokeColor", R"("00FF00")")).strokeColorRGBA,
               DefaultConfig().strokeColorRGBA);
    EXPECT_EQ(ParseConfig(One("drawing", "strokeColor", "16711680")).strokeColorRGBA,
               DefaultConfig().strokeColorRGBA);
}

TEST(AppConfigTest, TheCanvasBarRoundTrips) {
    AppConfig config = DefaultConfig();
    ASSERT_TRUE(config.showCanvasBar);
    config.showCanvasBar = false;
    EXPECT_EQ(ParseConfig(SerializeConfig(config)), config);
}

TEST(AppConfigTest, TheChosenDisplayRoundTripsAndIsThePrimaryUntilChosen) {
    AppConfig config = DefaultConfig();
    ASSERT_TRUE(config.overlayDisplayId.empty());
    ASSERT_TRUE(config.overlayDisplayName.empty());
    // A real device path, backslashes, braces and all - the kind of string
    // that would come back mangled if anything along the way escaped it
    // twice.
    config.overlayDisplayId = R"(\\?\DISPLAY#AOC2401#5&24581acb&0&UID4355#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})";
    config.overlayDisplayName = "24G1WG4";
    EXPECT_EQ(ParseConfig(SerializeConfig(config)), config);
}

// Each topic's progress, by its id: an object of strings, where anything
// not a string is not there.
TEST(AppConfigTest, TheTutorialsProgressRoundTripsPerTopic) {
    AppConfig config = DefaultConfig();
    ASSERT_TRUE(config.tutorialProgress.empty());
    config.tutorialProgress = {{"basics", "finished"}, {"drawing", "started"}};
    EXPECT_EQ(ParseConfig(SerializeConfig(config)), config);
    // What builds before 0.2.3 kept to go on after a restart, which no
    // build does now: read as nothing said.
    EXPECT_EQ(ParseConfig(One("tutorial", "current", R"("drawing")")), DefaultConfig());
    EXPECT_EQ(ParseConfig(One("tutorial", "folder", R"("42")")), DefaultConfig());

    const AppConfig read = ParseConfig(One("tutorial", "progress", R"({"basics": "move", "drawing": 3})"));
    EXPECT_EQ(read.tutorialProgress, (std::map<std::string, std::string>{{"basics", "move"}}));
    EXPECT_TRUE(ParseConfig(One("tutorial", "progress", R"("basics")")).tutorialProgress.empty());
}

// The colors that say which snippet is in front. Their alpha is part of
// the color, so they are the first settings written as eight hex digits -
// and the six-digit spelling every other color uses still has to parse,
// and still has to be what an opaque color is written back as.
TEST(AppConfigTest, SnippetColorsRoundTripWithTheirAlpha) {
    AppConfig config = DefaultConfig();
    config.itemBorderColorFrontRGBA = 0xFF6A3DFFu;  // opaque, so six digits on disk
    config.itemBorderColorOtherRGBA = 0x101820A0u;
    config.itemBorderColorPinnedRGBA = 0x20C0FF80u;

    const std::string text = SerializeConfig(config);
    EXPECT_NE(text.find("\"#FF6A3D\""), std::string::npos) << "an opaque color keeps the short spelling";
    EXPECT_NE(text.find("\"#101820A0\""), std::string::npos);
    EXPECT_EQ(ParseConfig(text), config);
}

// The fourth global hotkey, and the only one that is allowed to be
// unregistered - so it needs a default of its own that doesn't collide
// with the other three.
TEST(AppConfigTest, SilentCaptureHasItsOwnDefaultHotkeyAndSaysSoByDefault) {
    const AppConfig config = DefaultConfig();
    EXPECT_TRUE(config.hotkeySilentCapture.IsValid());
    EXPECT_FALSE(config.hotkeySilentCapture == config.hotkeyEditMode);
    EXPECT_FALSE(config.hotkeySilentCapture == config.hotkeyViewMode);
    EXPECT_FALSE(config.hotkeySilentCapture == config.hotkeyQuickCapture);
    EXPECT_TRUE(config.showToastsWhileHidden);
}

TEST(AppConfigTest, TheAccentIsTealUntilChangedAndRoundTrips) {
    AppConfig config = DefaultConfig();
    EXPECT_EQ(config.accentColorRGBA, 0x2C6C7CFFu);
    config.accentColorRGBA = 0x3D7AFFFFu;
    const std::string text = SerializeConfig(config);
    EXPECT_NE(text.find("\"#3D7AFF\""), std::string::npos);
    EXPECT_EQ(ParseConfig(text), config);
}

TEST(AppConfigTest, TheUiScaleFollowsWindowsUntilSetAndRoundTrips) {
    AppConfig config = DefaultConfig();
    EXPECT_EQ(config.uiScalePercent, 0);
    EXPECT_NE(SerializeConfig(config).find(R"("uiScale": "auto")"), std::string::npos);
    EXPECT_EQ(ParseConfig(SerializeConfig(config)), config);
    config.uiScalePercent = 150;
    EXPECT_EQ(ParseConfig(SerializeConfig(config)), config);
}

TEST(AppConfigTest, AUiScaleIsHeldToItsBandAndAnythingElseIsAuto) {
    const auto scale = [](const char* value) {
        return ParseConfig(std::string(R"({"appearance":{"uiScale":)") + value + "}}").uiScalePercent;
    };
    EXPECT_EQ(scale(R"("auto")"), 0);
    EXPECT_EQ(scale("125"), 125);
    EXPECT_EQ(scale("1000"), kUiScalePercentMax);
    EXPECT_EQ(scale("10"), kUiScalePercentMin);
    EXPECT_EQ(scale(R"("large")"), 0);
    EXPECT_EQ(scale("true"), 0);
}

// What a new snippet starts with: a screenshot opaque and a drawing
// see-through, both keeping their shape, and the text size not decided
// until the app first runs (see AppConfig::noteTextSizePx) - so not written
// until then either.
TEST(AppConfigTest, SnippetDefaultsStartAsSnippetsAlwaysHaveAndRoundTrip) {
    AppConfig config = DefaultConfig();
    EXPECT_TRUE(config.screenshotDefaults.keepAspect);
    EXPECT_FLOAT_EQ(config.screenshotDefaults.backgroundOpacity, 1.0f);
    EXPECT_TRUE(config.drawingDefaults.keepAspect);
    EXPECT_FLOAT_EQ(config.drawingDefaults.backgroundOpacity, 0.0f);
    EXPECT_FLOAT_EQ(config.noteTextSizePx, 0.0f);
    EXPECT_EQ(SerializeConfig(config).find(R"("size")"), std::string::npos);
    EXPECT_EQ(ParseConfig(SerializeConfig(config)), config);

    config.screenshotDefaults = SnippetDefaults{false, 0.5f, 0.75f};
    config.drawingDefaults = SnippetDefaults{false, 0.25f, 0.6f};
    config.drawingBackgroundColorRGBA = 0x112233FFu;
    config.noteTextSizePx = 30.0f;
    config.noteTextColorRGBA = 0xFF000080u;
    EXPECT_EQ(ParseConfig(SerializeConfig(config)), config);
}

// Held to the popover's own ranges: a foreground never fully gone, a text
// size the band a snippet's is held to.
TEST(AppConfigTest, SnippetDefaultsAreHeldToThePopoversRanges) {
    const AppConfig config = ParseConfig(R"({"defaults": {
        "screenshot": {"foregroundOpacity": 0, "backgroundOpacity": 7},
        "text": {"size": 5000}
    }})");
    EXPECT_FLOAT_EQ(config.screenshotDefaults.foregroundOpacity, 0.1f);
    EXPECT_FLOAT_EQ(config.screenshotDefaults.backgroundOpacity, 1.0f);
    EXPECT_FLOAT_EQ(config.noteTextSizePx, kNoteTextSizeMax);
}

TEST(AppConfigTest, SnippetColorsHaveDefaultsThatTellFrontFromBack) {
    const AppConfig config = DefaultConfig();
    EXPECT_NE(config.itemBorderColorFrontRGBA, config.itemBorderColorOtherRGBA);
    // Same hue, different alpha, both ways round: the difference is depth,
    // not decoration.
    EXPECT_EQ(config.itemBorderColorFrontRGBA >> 8, config.itemBorderColorOtherRGBA >> 8);
    EXPECT_GT(config.itemBorderColorFrontRGBA & 0xFFu, config.itemBorderColorOtherRGBA & 0xFFu);
    // A pinned snippet is told apart by hue, since alpha already says depth.
    EXPECT_NE(config.itemBorderColorPinnedRGBA >> 8, config.itemBorderColorFrontRGBA >> 8);
}

// Bilinear by default: it is what every picture was drawn with before
// there was a choice.
TEST(AppConfigTest, ImageFilterDefaultsToBilinearAndRoundTripsAllFour) {
    EXPECT_EQ(DefaultConfig().imageFilter, platform::ImageFilter::Bilinear);
    for (const platform::ImageFilter filter : {platform::ImageFilter::Bilinear, platform::ImageFilter::Nearest,
                                               platform::ImageFilter::Bicubic, platform::ImageFilter::Lanczos}) {
        AppConfig config = DefaultConfig();
        config.imageFilter = filter;
        EXPECT_EQ(ParseConfig(SerializeConfig(config)).imageFilter, filter);
    }
    EXPECT_EQ(ParseConfig(One("appearance", "imageFilter", "\"Lanczos\"")).imageFilter,
              platform::ImageFilter::Lanczos);
    EXPECT_EQ(ParseConfig(One("appearance", "imageFilter", "\"sinc\"")).imageFilter,
              platform::ImageFilter::Bilinear);
}

TEST(AppConfigTest, RaiseSelectedSnippetDefaultsToOnAndParses) {
    EXPECT_TRUE(DefaultConfig().raiseSelectedSnippet);
    EXPECT_FALSE(ParseConfig(One("drawing", "raiseSelected", "false")).raiseSelectedSnippet);
}

// A plain press makes a screenshot and Ctrl a drawing unless told
// otherwise; one press cannot make both, so a file that says it does gets
// the defaults back - except that both may be off.
TEST(AppConfigTest, CreationTriggersDefaultParseAndNeverCoincide) {
    EXPECT_EQ(DefaultConfig().screenshotTrigger, CreationTrigger::Plain);
    EXPECT_EQ(DefaultConfig().drawingTrigger, CreationTrigger::Ctrl);
    EXPECT_EQ(ParseConfig(One("drawing", "drawingTrigger", R"("ALT")")).drawingTrigger, CreationTrigger::Alt);
    EXPECT_EQ(ParseConfig(One("drawing", "drawingTrigger", R"("shift")")).drawingTrigger, CreationTrigger::Ctrl)
        << "Shift is the selection box's";

    const AppConfig clash =
        ParseConfig(R"({"drawing": {"screenshotTrigger": "alt", "drawingTrigger": "alt"}})");
    EXPECT_EQ(clash.screenshotTrigger, CreationTrigger::Plain);
    EXPECT_EQ(clash.drawingTrigger, CreationTrigger::Ctrl);

    const AppConfig bothOff = ParseConfig(R"({"drawing": {"screenshotTrigger": "off", "drawingTrigger": "off"}})");
    EXPECT_EQ(bothOff.screenshotTrigger, CreationTrigger::Off);
    EXPECT_EQ(bothOff.drawingTrigger, CreationTrigger::Off);
}

// The version is what the file says; anything that cannot be a version is
// read as 1, what every build before 0.2.0 wrote. A file from a newer build
// is read as well as this one can, and is not a repair.
TEST(AppConfigTest, TheVersionIsReadAndIsOneWhenTheFileSaysNothingUsable) {
    const auto version = [](const std::string& text) { return TryParseConfig(text)->version; };
    EXPECT_EQ(version("{}"), 1);
    EXPECT_EQ(version(R"({"version": 1})"), 1);
    EXPECT_EQ(version(R"({"version": 7})"), 7);
    EXPECT_EQ(version(R"({"version": 18446744073709551615})"), std::numeric_limits<int>::max());
    for (const char* unusable : {R"("2")", "-3", "0", "2.5", "null", "[2]"}) {
        SCOPED_TRACE(unusable);
        EXPECT_EQ(version(std::string(R"({"version": )") + unusable + "}"), 1);
    }

    const std::optional<ParsedConfig> newer =
        TryParseConfig(R"({"version": 99, "drawing": {"strokeWidth": 9, "somethingNewer": true}})");
    ASSERT_TRUE(newer);
    EXPECT_FLOAT_EQ(newer->config.strokeWidth, 9.0f);
    EXPECT_FALSE(newer->changed);
}

// One combination cannot summon two things: a file that gives it to two
// hotkeys has the later one unbound as it is read, and the earlier keeps
// it. Two unbound hotkeys share nothing.
TEST(AppConfigTest, AFileGivingTwoHotkeysOneCombinationHasTheLaterUnbound) {
    const AppConfig config = ParseConfig(R"({"hotkeys": {
        "editMode": "Ctrl+Alt+Q", "viewMode": null, "quickCapture": "Ctrl+Alt+Q", "silentCapture": null}})");
    EXPECT_EQ(config.hotkeyEditMode, (platform::KeyCombo{true, true, false, 'Q'}));
    EXPECT_FALSE(config.hotkeyQuickCapture.IsValid());
    EXPECT_FALSE(config.hotkeyViewMode.IsValid());
    EXPECT_FALSE(config.hotkeySilentCapture.IsValid());
}

// What reading repaired is reported, for the file to be written back at
// start; a value held to its rule is not a repair, and neither is anything
// the file leaves out or says that this build does not know.
TEST(AppConfigTest, ReadingReportsEveryLoadRepairAndNothingElse) {
    for (const char* text : {
             R"({"drawing": {"screenshotTrigger": "alt", "drawingTrigger": "alt"}})",
             R"({"hotkeys": {"editMode": "Ctrl+Alt+V"}})",
             R"({"profiles": [{"name": "", "match": {"exe": ["a.exe"]}}]})",
             R"({"profiles": [{"name": "Game"}, {"name": "Game"}]})",
             R"({"version": 1})",  // migrated
         }) {
        SCOPED_TRACE(text);
        const std::optional<ParsedConfig> parsed = TryParseConfig(text);
        ASSERT_TRUE(parsed);
        EXPECT_TRUE(parsed->changed);
    }
    // At this build's version: a file without one is version 1's, and
    // migrated.
    const auto current = [](const std::string& text) {
        return std::string(R"({"version": )") + std::to_string(kConfigVersion) + (text.size() > 2 ? ", " : "") +
               text.substr(1);
    };
    for (const std::string& text : {
             current("{}"),
             current(One("drawing", "strokeWidth", "1000")),
             current(One("drawing", "strokeWidth", R"("wide")")),
             current(One("hotkeys", "viewMode", "null")),
             current(One("drawing", "somethingNewer", "true")),
             current(R"({"drawing": {"screenshotTrigger": "off", "drawingTrigger": "off"}})"),
             current(R"({"profiles": [{"name": "Game"}, {"name": "Game 2"}, 7]})"),
             SerializeConfig(DefaultConfig()),
         }) {
        SCOPED_TRACE(text);
        const std::optional<ParsedConfig> parsed = TryParseConfig(text);
        ASSERT_TRUE(parsed);
        EXPECT_FALSE(parsed->changed);
    }
}

TEST(AppConfigTest, OverviewPreviewTogglesRoundTrip) {
    EXPECT_TRUE(DefaultConfig().overviewShowsStrokes);
    EXPECT_TRUE(DefaultConfig().overviewShowsBitmaps);
    EXPECT_FALSE(ParseConfig(One("overview", "showStrokes", "false")).overviewShowsStrokes);
    EXPECT_FALSE(ParseConfig(One("overview", "showBitmaps", "false")).overviewShowsBitmaps);
}

// Off by default, with two weeks ready for when it is switched on. The days are held to a
// day at least and ten years at most, rounded if fractional, and ignored if not a number
// at all.
TEST(AppConfigTest, PurgingDeletedThingsIsOffByDefaultAndParsesItsDays) {
    EXPECT_FALSE(DefaultConfig().purgeDeleted);
    EXPECT_EQ(DefaultConfig().purgeDeletedAfterDays, 14);
    EXPECT_FALSE(ParseConfig(One("deleted", "deleteForGoodAutomatically", "false")).purgeDeleted);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "7")).purgeDeletedAfterDays, 7);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "0")).purgeDeletedAfterDays, kPurgeDeletedAfterDaysMin);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "-5")).purgeDeletedAfterDays, kPurgeDeletedAfterDaysMin);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "1e9")).purgeDeletedAfterDays, kPurgeDeletedAfterDaysMax);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "2.6")).purgeDeletedAfterDays, 3);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", R"("7")")).purgeDeletedAfterDays, 14);
}

// Retention switched on only with a period that can be read: a broken or missing one is
// not replaced by the default, which could delete far sooner than the one meant, but
// switches it off, and the file is written back to say so.
TEST(AppConfigTest, RetentionWithNoPeriodToReadIsOff) {
    // This build's version, so that nothing but the repair counts as a change.
    const auto read = [](const std::string& deleted) {
        return TryParseConfig(R"({"version": )" + std::to_string(kConfigVersion) + R"(, "deleted": )" + deleted + "}");
    };
    const std::optional<ParsedConfig> stated = read(R"({"deleteForGoodAutomatically": true, "afterDays": 365})");
    ASSERT_TRUE(stated.has_value());
    EXPECT_TRUE(stated->config.purgeDeleted);
    EXPECT_EQ(stated->config.purgeDeletedAfterDays, 365);
    EXPECT_FALSE(stated->changed);

    for (const std::string deleted : {R"({"deleteForGoodAutomatically": true, "afterDays": "365"})",
                                      R"({"deleteForGoodAutomatically": true, "afterDays": null})",
                                      R"({"deleteForGoodAutomatically": true})"}) {
        const std::optional<ParsedConfig> parsed = read(deleted);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_FALSE(parsed->config.purgeDeleted) << deleted;
        EXPECT_TRUE(parsed->changed) << deleted;
    }

    // Off, it needs no period.
    const std::optional<ParsedConfig> off = read(R"({"deleteForGoodAutomatically": false, "afterDays": "x"})");
    ASSERT_TRUE(off.has_value());
    EXPECT_FALSE(off->config.purgeDeleted);
    EXPECT_FALSE(off->changed);
}

// Deleting a folder or canvas asks first, both kinds, until told not to.
TEST(AppConfigTest, DeletingAsksFirstByDefaultAndEachCanBeTurnedOff) {
    EXPECT_TRUE(DefaultConfig().confirmDelete);
    EXPECT_TRUE(DefaultConfig().confirmDeleteForGood);
    const AppConfig notAsked = ParseConfig(One("deleted", "confirmDelete", "false"));
    EXPECT_FALSE(notAsked.confirmDelete);
    EXPECT_TRUE(notAsked.confirmDeleteForGood) << "separate settings";
    EXPECT_FALSE(ParseConfig(One("deleted", "confirmDeleteForGood", "false")).confirmDeleteForGood);
}

TEST(AppConfigTest, EditModeBorderDefaultsToTranslucentWhiteTenPixelsAlwaysShown) {
    const AppConfig config = DefaultConfig();
    EXPECT_TRUE(config.showEditModeBorder);
    EXPECT_EQ(config.editModeBorderColorRGBA, 0xFFFFFF38u);
    EXPECT_FLOAT_EQ(config.editModeBorderWidthPx, 10.0f);
    EXPECT_FALSE(config.editModeBorderOnlyWhenEmpty);
}

TEST(AppConfigTest, ParsesEditModeBorderSettings) {
    const AppConfig config = ParseConfig(R"({"version": 2, "appearance": {"editModeBorder": {
        "show": false,
        "color": "#5AA9FFBF",
        "width": 20,
        "onlyWhenEmpty": true}}})");
    EXPECT_FALSE(config.showEditModeBorder);
    EXPECT_EQ(config.editModeBorderColorRGBA, 0x5AA9FFBFu);
    EXPECT_FLOAT_EQ(config.editModeBorderWidthPx, 20.0f);
    EXPECT_TRUE(config.editModeBorderOnlyWhenEmpty);
}

// Off by default: it turns the overlay from a sheet of glass into an opaque
// page, which is for games and best switched on in their profiles.
TEST(AppConfigTest, FreezeScreenInEditModeDefaultsToOff) {
    EXPECT_FALSE(DefaultConfig().profileable.freezeScreen);
}

TEST(AppConfigTest, ParsesAndSerializesFreezeScreenInEditMode) {
    EXPECT_TRUE(ParseConfig(One("behavior", "freezeScreen", "true")).profileable.freezeScreen);
    EXPECT_FALSE(ParseConfig(One("behavior", "freezeScreen", "false")).profileable.freezeScreen);

    AppConfig config = DefaultConfig();
    config.profileable.freezeScreen = true;
    EXPECT_TRUE(ParseConfig(SerializeConfig(config)).profileable.freezeScreen);
}

// Both are pulled into range rather than reverted to the default - see
// their own parse comments; every value in between means something, it's
// only the ends that need holding.
TEST(AppConfigTest, ClampsOutOfRangeEditModeBorderWidth) {
    const auto width = [](const char* value) {
        return ParseConfig(std::string(R"({"appearance":{"editModeBorder":{"width":)") + value + "}}}")
            .editModeBorderWidthPx;
    };
    EXPECT_FLOAT_EQ(width("999"), kEditModeBorderWidthMax);
    EXPECT_FLOAT_EQ(width("0.1"), kEditModeBorderWidthMin);
    // Non-positive is rejected outright (falls back to the default) - a
    // 0px border isn't a thin border, it's the checkbox above saying "off"
    // in a worse way.
    EXPECT_FLOAT_EQ(width("0"), DefaultConfig().editModeBorderWidthPx);
}

class WriteConfigFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = sz::test::TempDir() / (std::string("spickzettel_write_config_file_test_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    static std::string ReadFile(const std::filesystem::path& path) {
        std::ifstream in(path);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    std::filesystem::path dir_;
};

TEST_F(WriteConfigFileTest, LoadingReadsBackWhatWasWritten) {
    AppConfig config = DefaultConfig();
    config.strokeWidth = 9.0f;
    ASSERT_TRUE(WriteConfigFile(dir_ / "config.json", config));
    const LoadedConfig loaded = LoadOrCreateConfig(dir_ / "config.json", "stamp");
    EXPECT_EQ(loaded.source, ConfigSource::Read);
    EXPECT_FLOAT_EQ(loaded.config.strokeWidth, 9.0f);
    EXPECT_FALSE(loaded.writeBack);
}

// A file reading repaired is to be written back at start - by the tray,
// which retries a write that fails; the loader leaves it as it was.
TEST_F(WriteConfigFileTest, AFileReadingRepairedIsToBeWrittenBack) {
    std::filesystem::create_directories(dir_);
    const std::string clash = R"({"hotkeys": {"editMode": "Ctrl+Alt+V"}})";
    std::ofstream(dir_ / "config.json", std::ios::binary) << clash;

    const LoadedConfig loaded = LoadOrCreateConfig(dir_ / "config.json", "stamp");
    EXPECT_EQ(loaded.source, ConfigSource::Read);
    EXPECT_TRUE(loaded.writeBack);
    EXPECT_FALSE(loaded.config.hotkeyViewMode.IsValid());
    EXPECT_EQ(ReadFile(dir_ / "config.json"), clash);
}

// A file a newer build wrote is read as well as this build can, and left as
// it is - not written back even where reading repaired it, since the newer
// build may have stored more than this one can see.
TEST_F(WriteConfigFileTest, AFileANewerBuildWroteIsReadAndNotToBeWrittenBack) {
    std::filesystem::create_directories(dir_);
    const std::string newer = R"({"version": )" + std::to_string(kConfigVersion + 1) +
                              R"(, "drawing": {"strokeWidth": 9}, "hotkeys": {"editMode": "Ctrl+Alt+V"}})";
    std::ofstream(dir_ / "config.json", std::ios::binary) << newer;

    const LoadedConfig loaded = LoadOrCreateConfig(dir_ / "config.json", "stamp");
    EXPECT_EQ(loaded.source, ConfigSource::Newer);
    EXPECT_FLOAT_EQ(loaded.config.strokeWidth, 9.0f);
    EXPECT_FALSE(loaded.config.hotkeyViewMode.IsValid()) << "repaired to run on, all the same";
    EXPECT_FALSE(loaded.writeBack);
    EXPECT_EQ(ReadFile(dir_ / "config.json"), newer);
}

TEST_F(WriteConfigFileTest, WithoutAFileLoadingIsAFirstRunAndWritesTheDefaults) {
    const LoadedConfig loaded = LoadOrCreateConfig(dir_ / "config.json", "stamp");
    EXPECT_EQ(loaded.source, ConfigSource::Created);
    EXPECT_EQ(loaded.config, DefaultConfig());
    EXPECT_EQ(ParseConfig(ReadFile(dir_ / "config.json")), DefaultConfig());
}

// A hand edit that left a trailing comma is not settings, and not a first
// run either: the file is renamed out of the way, never written over, and
// what was in it is still there to be found.
TEST_F(WriteConfigFileTest, AFileThatIsNotSettingsIsSetAsideNotWrittenOver) {
    std::filesystem::create_directories(dir_);
    const std::string broken = R"({"drawing": {"strokeWidth": 9,}})";
    std::ofstream(dir_ / "config.json", std::ios::binary) << broken;

    const LoadedConfig loaded = LoadOrCreateConfig(dir_ / "config.json", "2026-09-24-15-00-00");
    EXPECT_EQ(loaded.source, ConfigSource::SetAside);
    EXPECT_EQ(loaded.setAsideAs, dir_ / "config-unreadable-2026-09-24-15-00-00.json");
    EXPECT_EQ(ReadFile(loaded.setAsideAs), broken);
}

// Whether retention was on is what could not be read. The defaults put in
// the set-aside file's place have it off, so the next start - which reads
// them, not the file set aside - erases nothing either.
TEST_F(WriteConfigFileTest, WhatStandsInForAFileSetAsideKeepsRetentionOff) {
    std::filesystem::create_directories(dir_);
    std::ofstream(dir_ / "config.json", std::ios::binary) << R"({"deleted": {"afterDays": 3650},})";

    AppConfig expected = DefaultConfig();
    expected.purgeDeleted = false;
    const LoadedConfig first = LoadOrCreateConfig(dir_ / "config.json", "first");
    ASSERT_EQ(first.source, ConfigSource::SetAside);
    EXPECT_EQ(first.config, expected);

    const LoadedConfig second = LoadOrCreateConfig(dir_ / "config.json", "second");
    EXPECT_EQ(second.source, ConfigSource::Read);
    EXPECT_EQ(second.config, expected);
}

// A file that cannot be moved aside stays where it is, not written over -
// and what stands in for it has retention off all the same.
TEST_F(WriteConfigFileTest, AFileThatCannotBeSetAsideStillStartsWithRetentionOff) {
    std::filesystem::create_directories(dir_ / "config-unreadable-stamp.json");  // in the way
    const std::string broken = R"({"deleted": {"afterDays": 3650},})";
    std::ofstream(dir_ / "config.json", std::ios::binary) << broken;

    const LoadedConfig loaded = LoadOrCreateConfig(dir_ / "config.json", "stamp");
    EXPECT_EQ(loaded.source, ConfigSource::SetAside);
    EXPECT_TRUE(loaded.setAsideAs.empty());
    EXPECT_FALSE(loaded.config.purgeDeleted);
    EXPECT_EQ(ReadFile(dir_ / "config.json"), broken);
}

// A settings file is a few kilobytes. Whatever a file of megabytes at that
// path is, it is not settings, and is set aside rather than allocated for.
TEST_F(WriteConfigFileTest, AFileTooBigToBeASettingsFileIsSetAside) {
    std::filesystem::create_directories(dir_);
    {
        std::ofstream out(dir_ / "config.json", std::ios::binary);
        out << R"({"drawing": {"strokeWidth": 9}, "padding": ")" << std::string(kMaxConfigFileBytes, 'x') << R"("})";
    }
    const LoadedConfig loaded = LoadOrCreateConfig(dir_ / "config.json", "stamp");
    EXPECT_EQ(loaded.source, ConfigSource::SetAside);
    EXPECT_FLOAT_EQ(loaded.config.strokeWidth, DefaultConfig().strokeWidth);
    EXPECT_TRUE(std::filesystem::exists(dir_ / "config-unreadable-stamp.json"));
}

TEST_F(WriteConfigFileTest, WritesTextThatParsesBackToTheSameConfig) {
    AppConfig config = DefaultConfig();
    config.showDebugOverlay = true;
    config.showItemBorders = false;
    config.accentColorRGBA = 0x3D7AFFFFu;

    const std::filesystem::path path = dir_ / "config.json";
    EXPECT_TRUE(WriteConfigFile(path, config));
    EXPECT_EQ(ParseConfig(ReadFile(path)), config);
}

TEST_F(WriteConfigFileTest, CreatesParentDirectoriesThatDontExistYet) {
    const std::filesystem::path path = dir_ / "nested" / "deeper" / "config.json";
    EXPECT_TRUE(WriteConfigFile(path, DefaultConfig()));
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(WriteConfigFileTest, OverwritesWhateverWasThereBefore) {
    const std::filesystem::path path = dir_ / "config.json";
    ASSERT_TRUE(WriteConfigFile(path, DefaultConfig()));

    AppConfig changed = DefaultConfig();
    changed.showItemBorders = false;
    ASSERT_TRUE(WriteConfigFile(path, changed));

    EXPECT_EQ(ParseConfig(ReadFile(path)), changed);
}

// The temp file the atomic write goes through is the writer's own business
// and must not outlive the call: a stray config.json.tmp beside the real one
// is the kind of thing a user opens by mistake and edits.
TEST_F(WriteConfigFileTest, LeavesNoTemporaryFileBehind) {
    const std::filesystem::path path = dir_ / "config.json";
    ASSERT_TRUE(WriteConfigFile(path, DefaultConfig()));

    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        names.push_back(entry.path().filename().string());
    }
    EXPECT_EQ(names, std::vector<std::string>{"config.json"});
}

TEST_F(WriteConfigFileTest, EmptyPathIsANoOp) {
    // The "nowhere to persist to" convention shared with
    // persistence::LibraryStore's own empty-directory handling - returns
    // false rather than throwing or writing to some fallback location.
    EXPECT_FALSE(WriteConfigFile(std::filesystem::path(), DefaultConfig()));
}

}  // namespace
}  // namespace sz::core
