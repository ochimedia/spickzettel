#include "core/config/shortcut_action.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "core/config/app_config.h"

namespace sz::core {
namespace {

platform::KeyCombo Plain(char key) {
    return platform::KeyCombo{/*ctrl=*/false, /*alt=*/false, /*shift=*/false, /*key=*/key};
}

platform::KeyCombo WithCtrl(char key) {
    return platform::KeyCombo{/*ctrl=*/true, /*alt=*/false, /*shift=*/false, /*key=*/key};
}

platform::KeyCombo WithCtrlShift(char key) {
    return platform::KeyCombo{/*ctrl=*/true, /*alt=*/false, /*shift=*/true, /*key=*/key};
}

const platform::KeyCombo& BindingFor(const ShortcutBindings& bindings, ShortcutAction action) {
    return bindings[ShortcutActionIndex(action)];
}

TEST(ShortcutActionTest, DefaultsBindTheFourReachedForMost) {
    const ShortcutBindings bindings = DefaultShortcuts();
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::NewScreenshot), Plain('S'));
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::NewDrawing), Plain('D'));
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::Erase), Plain('E'));
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::Draw), Plain('P'));
}

// The clipboard's three on the chords every application on the machine
// has already taught the hand.
TEST(ShortcutActionTest, TheClipboardShipsOnTheUsualChords) {
    const ShortcutBindings bindings = DefaultShortcuts();
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::Copy), WithCtrl('C'));
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::Cut), WithCtrl('X'));
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::Paste), WithCtrl('V'));
}

// Duplicate on the chord it has everywhere else, and the canvas that
// takes the selection with it on the "new" chord with Shift - both chords
// rather than letters, so neither can fire from ordinary typing. Ctrl+D
// sits beside a plain D for a new drawing, which the exact-modifier match
// in HandleToolShortcuts keeps apart.
TEST(ShortcutActionTest, DuplicateAndTheCanvasThatTakesTheSelectionShipBound) {
    const ShortcutBindings bindings = DefaultShortcuts();
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::Duplicate), WithCtrl('D'));
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::NewCanvasWithSelection), WithCtrlShift('N'));
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::NewDrawing), Plain('D'));
    EXPECT_EQ(BindingFor(bindings, ShortcutAction::NewCanvas), platform::KeyCombo{})
        << "the plain new canvas keeps no key of its own";
}

TEST(ShortcutActionTest, EverythingElseStartsUnbound) {
    const ShortcutBindings bindings = DefaultShortcuts();
    for (const ShortcutAction action : kAllShortcutActions) {
        const bool bound = action == ShortcutAction::NewScreenshot || action == ShortcutAction::NewDrawing ||
                           action == ShortcutAction::Erase || action == ShortcutAction::Draw ||
                           action == ShortcutAction::Copy || action == ShortcutAction::Cut ||
                           action == ShortcutAction::Paste || action == ShortcutAction::Duplicate ||
                           action == ShortcutAction::NewCanvasWithSelection;
        EXPECT_EQ(BindingFor(bindings, action).key != 0, bound)
            << "action " << ShortcutActionKey(action);
    }
}

TEST(ShortcutActionTest, EveryActionHasItsOwnPersistedName) {
    std::set<std::string> names;
    for (const ShortcutAction action : kAllShortcutActions) {
        const std::string name(ShortcutActionKey(action));
        EXPECT_FALSE(name.empty());
        EXPECT_TRUE(names.insert(name).second) << "duplicate name " << name;
        EXPECT_EQ(ShortcutActionFromKey(name), action);
    }
}

TEST(ShortcutActionTest, UnknownNameIsRejectedRatherThanGuessed) {
    EXPECT_FALSE(ShortcutActionFromKey("").has_value());
    EXPECT_FALSE(ShortcutActionFromKey("highlighter").has_value());
}

// Draw and Erase are stored under the names of the buttons they light.
TEST(ShortcutActionTest, DrawAndEraseAreStoredAsPenAndEraser) {
    EXPECT_EQ(ShortcutActionFromKey("pen"), ShortcutAction::Draw);
    EXPECT_EQ(ShortcutActionFromKey("eraser"), ShortcutAction::Erase);
    EXPECT_EQ(ShortcutActionKey(ShortcutAction::Select), "select");
}

// A config written while there were seven tools and a text-note action
// names things that are gone. They are skipped, and nothing else is
// disturbed by their being there.
TEST(ShortcutActionTest, TheNamesOfToolsThatWentAreIgnored) {
    const AppConfig config = ParseConfig(
        R"({"shortcuts": {"rectangle": "R", "line": "L", "rectEraser": "X", "newTextNote": "N", "pen": "Q"}})");
    EXPECT_EQ(config.toolShortcuts[ShortcutActionIndex(ShortcutAction::Draw)], Plain('Q'));
    EXPECT_EQ(config.toolShortcuts[ShortcutActionIndex(ShortcutAction::Erase)], Plain('E'));
    for (const ShortcutAction action : kAllShortcutActions) {
        EXPECT_NE(config.toolShortcuts[ShortcutActionIndex(action)], Plain('R'));
        EXPECT_NE(config.toolShortcuts[ShortcutActionIndex(action)], Plain('N'));
    }
}

TEST(ShortcutActionTest, ConfigRoundTripsBindingsAndUnbindings) {
    AppConfig config = DefaultConfig();
    config.toolShortcuts[ShortcutActionIndex(ShortcutAction::Select)] =
        platform::KeyCombo{/*ctrl=*/true, /*alt=*/false, /*shift=*/false, /*key=*/'M'};
    // The case a naive "skip empty values" parser gets wrong: an action
    // that ships bound, unbound on purpose, has to come back unbound.
    config.toolShortcuts[ShortcutActionIndex(ShortcutAction::Draw)] = platform::KeyCombo{};

    const AppConfig reparsed = ParseConfig(SerializeConfig(config));
    EXPECT_EQ(reparsed.toolShortcuts, config.toolShortcuts);
    EXPECT_EQ(reparsed, config);
}

TEST(ShortcutActionTest, ConfigWithNoShortcutsObjectKeepsTheDefaults) {
    // A file that says nothing about shortcuts must not read as
    // "everything unbound" - absent means inherit, and only an explicit
    // null means unset.
    const AppConfig config = ParseConfig(R"({"hotkeys": {"editMode": "F5"}})");
    EXPECT_EQ(config.toolShortcuts, DefaultShortcuts());

    const AppConfig partial = ParseConfig(R"({"shortcuts": {"text": "T"}})");
    EXPECT_EQ(partial.toolShortcuts[ShortcutActionIndex(ShortcutAction::Text)], Plain('T'));
    // Untouched by a file that only mentioned one of them.
    EXPECT_EQ(partial.toolShortcuts[ShortcutActionIndex(ShortcutAction::Draw)], Plain('P'));
}

TEST(ShortcutActionTest, ExplicitNullIsUnbound) {
    const AppConfig config = ParseConfig(R"({"shortcuts": {"pen": null}})");
    EXPECT_EQ(config.toolShortcuts[ShortcutActionIndex(ShortcutAction::Draw)].key, 0);
}

TEST(ShortcutActionTest, FunctionKeysAndModifiersSurviveTheFile) {
    AppConfig config = DefaultConfig();
    config.toolShortcuts[ShortcutActionIndex(ShortcutAction::NewCanvas)] =
        platform::KeyCombo{/*ctrl=*/false, /*alt=*/true, /*shift=*/true,
                           /*key=*/platform::KeyCombo::kFunctionKeyBase + 9};
    const AppConfig reparsed = ParseConfig(SerializeConfig(config));
    EXPECT_EQ(reparsed.toolShortcuts[ShortcutActionIndex(ShortcutAction::NewCanvas)],
               config.toolShortcuts[ShortcutActionIndex(ShortcutAction::NewCanvas)]);
}

// Which shape a press makes, from the modifiers held as it starts - Ctrl
// is the rectangle for Draw and Erase alike, and wins when both are held.
TEST(ShortcutActionTest, ModifiersPickTheShape) {
    EXPECT_EQ(DrawShapeFor(/*ctrl=*/false, /*shift=*/false), DrawShape::Freehand);
    EXPECT_EQ(DrawShapeFor(false, true), DrawShape::Line);
    EXPECT_EQ(DrawShapeFor(true, false), DrawShape::Rectangle);
    EXPECT_EQ(DrawShapeFor(true, true), DrawShape::Rectangle);
    EXPECT_TRUE(ErasesRectangle(/*ctrl=*/true));
    EXPECT_FALSE(ErasesRectangle(false));
}

}  // namespace
}  // namespace sz::core
