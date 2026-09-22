#include "core/config/app_config.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace sz::core {
namespace {

// A config file holding exactly one setting. Most of these tests are about
// one value's own reading rules, and this keeps that the visible part
// instead of a JSON document per assertion. `value` is JSON, so a string
// setting is written with its quotes: One("drawing", "renderMode",
// R"("polyline")").
std::string One(const char* group, const char* key, const std::string& value) {
    return std::string("{\"") + group + "\":{\"" + key + "\":" + value + "}}";
}

TEST(AppConfigTest, DefaultConfigMatchesHotkeyEditMode) {
    const AppConfig config = DefaultConfig();
    EXPECT_TRUE(config.hotkeyEditMode.ctrl);
    EXPECT_TRUE(config.hotkeyEditMode.alt);
    EXPECT_FALSE(config.hotkeyEditMode.shift);
    EXPECT_EQ(config.hotkeyEditMode.key, 'O');
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

TEST(AppConfigTest, EditModeNoActivateDefaultsToOn) { EXPECT_TRUE(DefaultConfig().editModeNoActivate); }

TEST(AppConfigTest, ParsesEditModeNoActivate) {
    EXPECT_TRUE(ParseConfig(One("input", "dontStealFocus", "true")).editModeNoActivate);
    EXPECT_FALSE(ParseConfig(One("input", "dontStealFocus", "false")).editModeNoActivate);
    // A value of the wrong type is a value the file failed to state, so the
    // default stands.
    EXPECT_TRUE(ParseConfig(One("input", "dontStealFocus", R"("yes")")).editModeNoActivate);
    EXPECT_TRUE(ParseConfig(One("input", "dontStealFocus", "1")).editModeNoActivate);
}

// On by default, because over an elevated application every other input
// setting is a no-op: Windows gives a lower-integrity process none of that
// application's input, so an overlay that keeps its hands off gets nothing
// at all. Off is the deliberate choice to accept that.
TEST(AppConfigTest, TakeFocusOverElevatedDefaultsToOn) {
    EXPECT_TRUE(DefaultConfig().takeFocusOverElevated);
}

TEST(AppConfigTest, ParsesTakeFocusOverElevated) {
    EXPECT_TRUE(ParseConfig(One("input", "takeFocusOverElevated", "true")).takeFocusOverElevated);
    EXPECT_FALSE(ParseConfig(One("input", "takeFocusOverElevated", "false")).takeFocusOverElevated);
    EXPECT_TRUE(ParseConfig(One("input", "takeFocusOverElevated", R"("no")")).takeFocusOverElevated);
}

TEST(AppConfigTest, EveryEditModeInputOptionDefaultsToOn) {
    const platform::EditModeInputOptions options = DefaultConfig().editModeInput;
    EXPECT_TRUE(options.useSoftwarePointer);
    EXPECT_TRUE(options.useRawMouseInput);
    EXPECT_TRUE(options.dontForwardKeystrokes);
    EXPECT_TRUE(options.counterRawMouseInput);
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
    EXPECT_FALSE(ParseConfig(One("input", "softwarePointer", "false")).editModeInput.useSoftwarePointer);
    EXPECT_FALSE(ParseConfig(One("input", "rawMouseInput", "false")).editModeInput.useRawMouseInput);
    EXPECT_FALSE(
        ParseConfig(One("input", "dontForwardKeystrokes", "false")).editModeInput.dontForwardKeystrokes);
    EXPECT_FALSE(
        ParseConfig(One("input", "counterRawMouseInput", "false")).editModeInput.counterRawMouseInput);

    // Disabling one leaves the others alone - they're separate experiments,
    // not one setting with four names.
    const platform::EditModeInputOptions onlyMouse =
        ParseConfig(One("input", "rawMouseInput", "false")).editModeInput;
    EXPECT_FALSE(onlyMouse.useRawMouseInput);
    EXPECT_TRUE(onlyMouse.useSoftwarePointer);
    EXPECT_TRUE(onlyMouse.dontForwardKeystrokes);
    EXPECT_TRUE(onlyMouse.counterRawMouseInput);
}

TEST(AppConfigTest, LibraryTreeHudDefaultsToOffAndRoundTrips) {
    EXPECT_FALSE(DefaultConfig().showLibraryTreeHud);
    EXPECT_TRUE(ParseConfig(One("diagnostics", "showLibraryTreeHud", "true")).showLibraryTreeHud);

    AppConfig config = DefaultConfig();
    config.showLibraryTreeHud = true;
    EXPECT_TRUE(ParseConfig(SerializeConfig(config)).showLibraryTreeHud);
}

TEST(AppConfigTest, InputOptionsHudDefaultsToOffAndRoundTrips) {
    EXPECT_FALSE(DefaultConfig().showInputOptionsHud);
    EXPECT_TRUE(ParseConfig(One("diagnostics", "showInputOptionsHud", "true")).showInputOptionsHud);

    AppConfig config = DefaultConfig();
    config.showInputOptionsHud = true;
    EXPECT_TRUE(ParseConfig(SerializeConfig(config)).showInputOptionsHud);
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
    EXPECT_EQ(ParseConfig(R"({"input": 5})"), DefaultConfig());
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
    config.editModeNoActivate = false;
    config.showItemBorders = false;
    config.strokeRenderMode = StrokeRenderMode::Rasterized;
    config.paintPixelsInsteadOfStrokes = true;
    config.raiseSelectedSnippet = false;
    config.overviewShowsStrokes = false;
    config.overviewShowsBitmaps = true;
    config.purgeDeleted = true;
    config.purgeDeletedAfterDays = 14;
    config.showEditModeBorder = false;
    config.editModeBorderColorRGBA = 0x5AA9FFFFu;
    config.editModeBorderOpacity = 0.4f;
    config.editModeBorderWidthPx = 16.0f;
    config.editModeBorderOnlyWhenEmpty = true;
    config.freezeScreenInEditMode = false;
    config.showInputOptionsHud = true;
    config.editModeInput.useSoftwarePointer = false;
    config.editModeInput.useRawMouseInput = true;
    config.editModeInput.dontForwardKeystrokes = false;
    config.editModeInput.counterRawMouseInput = true;

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
    config.drawingBar = {{ChromeButton::Colour, true},
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

// The colours that say which snippet is in front. Their alpha is part of
// the colour, so they are the first settings written as eight hex digits -
// and the six-digit spelling every other colour uses still has to parse,
// and still has to be what an opaque colour is written back as.
TEST(AppConfigTest, SnippetColorsRoundTripWithTheirAlpha) {
    AppConfig config = DefaultConfig();
    config.itemBorderColorFrontRGBA = 0xFF6A3DFFu;  // opaque, so six digits on disk
    config.itemBorderColorOtherRGBA = 0x101820A0u;
    config.itemBorderColorPinnedRGBA = 0x20C0FF80u;

    const std::string text = SerializeConfig(config);
    EXPECT_NE(text.find("\"#FF6A3D\""), std::string::npos) << "an opaque colour keeps the short spelling";
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

TEST(AppConfigTest, TheAccentIsTheDesignsOrangeUntilChangedAndRoundTrips) {
    AppConfig config = DefaultConfig();
    EXPECT_EQ(config.accentColorRGBA, 0xFF6A3DFFu);
    config.accentColorRGBA = 0x3D7AFFFFu;
    const std::string text = SerializeConfig(config);
    EXPECT_NE(text.find("\"#3D7AFF\""), std::string::npos);
    EXPECT_EQ(ParseConfig(text), config);
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

// Tessellated by default: it is the only one of the three that gives up
// nothing - the polyline is there to compare against, and the rasterized
// one trades sharpness at size for compositing a stroke exactly once.
TEST(AppConfigTest, StrokeRenderModeDefaultsToTessellatedAndParsesAllThree) {
    const auto mode = [](const char* text) {
        return ParseConfig(One("drawing", "renderMode", std::string("\"") + text + "\"")).strokeRenderMode;
    };
    EXPECT_EQ(DefaultConfig().strokeRenderMode, StrokeRenderMode::Tessellated);
    EXPECT_EQ(mode("polyline"), StrokeRenderMode::Polyline);
    EXPECT_EQ(mode("rasterized"), StrokeRenderMode::Rasterized);
    EXPECT_EQ(mode("Tessellated"), StrokeRenderMode::Tessellated);
    EXPECT_EQ(mode("garbage"), StrokeRenderMode::Tessellated);
}

TEST(AppConfigTest, StrokeRenderModeRoundTripsThroughText) {
    for (const StrokeRenderMode mode :
         {StrokeRenderMode::Tessellated, StrokeRenderMode::Polyline, StrokeRenderMode::Rasterized}) {
        AppConfig config = DefaultConfig();
        config.strokeRenderMode = mode;
        EXPECT_EQ(ParseConfig(SerializeConfig(config)).strokeRenderMode, mode);
    }
}

// Default off: strokes are what this app has always drawn, and painting
// pixels gives up scaling for pixel-exact erasing - a trade to opt into.
TEST(AppConfigTest, PaintPixelsInsteadOfStrokesDefaultsToOffAndParses) {
    EXPECT_FALSE(DefaultConfig().paintPixelsInsteadOfStrokes);
    EXPECT_TRUE(DefaultConfig().raiseSelectedSnippet);
    EXPECT_FALSE(ParseConfig(One("drawing", "raiseSelected", "false")).raiseSelectedSnippet);
    EXPECT_TRUE(ParseConfig(One("drawing", "paintPixels", "true")).paintPixelsInsteadOfStrokes);
    EXPECT_FALSE(ParseConfig(One("drawing", "paintPixels", "false")).paintPixelsInsteadOfStrokes);
}

TEST(AppConfigTest, OverviewPreviewTogglesRoundTrip) {
    EXPECT_TRUE(DefaultConfig().overviewShowsStrokes);
    EXPECT_FALSE(DefaultConfig().overviewShowsBitmaps);
    EXPECT_FALSE(ParseConfig(One("overview", "showStrokes", "false")).overviewShowsStrokes);
    EXPECT_TRUE(ParseConfig(One("overview", "showBitmaps", "true")).overviewShowsBitmaps);
}

// Off by default - nothing is erased unasked - with a period ready for when
// it is switched on. The days are held to a day at least and ten years at
// most, rounded if fractional, and ignored if not a number at all.
TEST(AppConfigTest, PurgingDeletedThingsDefaultsToOffAndParsesItsDays) {
    EXPECT_FALSE(DefaultConfig().purgeDeleted);
    EXPECT_EQ(DefaultConfig().purgeDeletedAfterDays, 30);
    EXPECT_TRUE(ParseConfig(One("deleted", "deleteForGoodAutomatically", "true")).purgeDeleted);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "7")).purgeDeletedAfterDays, 7);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "0")).purgeDeletedAfterDays, kPurgeDeletedAfterDaysMin);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "-5")).purgeDeletedAfterDays, kPurgeDeletedAfterDaysMin);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "1e9")).purgeDeletedAfterDays, kPurgeDeletedAfterDaysMax);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", "2.6")).purgeDeletedAfterDays, 3);
    EXPECT_EQ(ParseConfig(One("deleted", "afterDays", R"("7")")).purgeDeletedAfterDays, 30);
}

TEST(AppConfigTest, EditModeBorderDefaultsToTranslucentWhiteTenPixelsAlwaysShown) {
    const AppConfig config = DefaultConfig();
    EXPECT_TRUE(config.showEditModeBorder);
    EXPECT_EQ(config.editModeBorderColorRGBA, 0xFFFFFFFFu);
    EXPECT_GT(config.editModeBorderOpacity, 0.0f);
    EXPECT_LT(config.editModeBorderOpacity, 1.0f);
    EXPECT_FLOAT_EQ(config.editModeBorderWidthPx, 10.0f);
    EXPECT_FALSE(config.editModeBorderOnlyWhenEmpty);
}

TEST(AppConfigTest, ParsesEditModeBorderSettings) {
    const AppConfig config = ParseConfig(R"({"appearance": {"editModeBorder": {
        "show": false,
        "color": "#5AA9FF",
        "opacity": 0.75,
        "width": 20,
        "onlyWhenEmpty": true}}})");
    EXPECT_FALSE(config.showEditModeBorder);
    EXPECT_EQ(config.editModeBorderColorRGBA, 0x5AA9FFFFu);
    EXPECT_FLOAT_EQ(config.editModeBorderOpacity, 0.75f);
    EXPECT_FLOAT_EQ(config.editModeBorderWidthPx, 20.0f);
    EXPECT_TRUE(config.editModeBorderOnlyWhenEmpty);
}

// On by default. It does turn the overlay from a sheet of glass into an
// opaque page, but for annotating over a game that is the point: a screen
// that holds still is worth more than watching the one underneath move.
TEST(AppConfigTest, FreezeScreenInEditModeDefaultsToOn) {
    EXPECT_TRUE(DefaultConfig().freezeScreenInEditMode);
}

TEST(AppConfigTest, ParsesAndSerializesFreezeScreenInEditMode) {
    EXPECT_TRUE(ParseConfig(One("input", "freezeScreen", "true")).freezeScreenInEditMode);
    EXPECT_FALSE(ParseConfig(One("input", "freezeScreen", "false")).freezeScreenInEditMode);

    AppConfig config = DefaultConfig();
    config.freezeScreenInEditMode = false;
    EXPECT_FALSE(ParseConfig(SerializeConfig(config)).freezeScreenInEditMode);
}

// Both are pulled into range rather than reverted to the default - see
// their own parse comments; every value in between means something, it's
// only the ends that need holding.
TEST(AppConfigTest, ClampsOutOfRangeEditModeBorderOpacityAndWidth) {
    const auto opacity = [](const char* value) {
        return ParseConfig(std::string(R"({"appearance":{"editModeBorder":{"opacity":)") + value + "}}}")
            .editModeBorderOpacity;
    };
    const auto width = [](const char* value) {
        return ParseConfig(std::string(R"({"appearance":{"editModeBorder":{"width":)") + value + "}}}")
            .editModeBorderWidthPx;
    };
    EXPECT_FLOAT_EQ(opacity("5"), 1.0f);
    EXPECT_FLOAT_EQ(opacity("-2"), 0.0f);
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
        dir_ = std::filesystem::temp_directory_path() / (std::string("spickzettel_write_config_file_test_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
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

TEST_F(WriteConfigFileTest, ReadConfigFileReadsBackWhatWasWritten) {
    AppConfig config = DefaultConfig();
    config.strokeWidth = 9.0f;
    ASSERT_TRUE(WriteConfigFile(dir_ / "config.json", config));
    const std::optional<AppConfig> read = ReadConfigFile(dir_ / "config.json");
    ASSERT_TRUE(read.has_value());
    EXPECT_FLOAT_EQ(read->strokeWidth, 9.0f);
}

TEST_F(WriteConfigFileTest, ReadConfigFileIsNulloptWithoutAFile) {
    EXPECT_FALSE(ReadConfigFile(dir_ / "config.json").has_value()) << "a first run, not a broken file";
}

// A settings file is a few kilobytes. Whatever a file of megabytes at that
// path is, it is read as one that said nothing rather than allocated for.
TEST_F(WriteConfigFileTest, AFileTooBigToBeASettingsFileReadsAsDefaults) {
    std::filesystem::create_directories(dir_);
    {
        std::ofstream out(dir_ / "config.json", std::ios::binary);
        out << R"({"drawing": {"strokeWidth": 9}, "padding": ")" << std::string(kMaxConfigFileBytes, 'x') << R"("})";
    }
    const std::optional<AppConfig> read = ReadConfigFile(dir_ / "config.json");
    ASSERT_TRUE(read.has_value()) << "there is a file";
    EXPECT_FLOAT_EQ(read->strokeWidth, DefaultConfig().strokeWidth) << "and it said nothing";
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
