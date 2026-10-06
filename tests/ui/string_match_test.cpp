#include "ui/string_editor/string_match.h"

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace sz::ui::string_editor {
namespace {

using Indices = std::vector<std::size_t>;

TEST(StringMatchTest, TheSameWordsAreAnExactFitForEveryStringThatHasThem) {
    const std::vector<std::string_view> catalog = {"Copy", "Paste", "Copy", "Copy to %s"};
    const Matches matches = Match("Copy", catalog);
    EXPECT_EQ(matches.fit, Fit::Exact);
    EXPECT_EQ(matches.indices, (Indices{0, 2}));
}

TEST(StringMatchTest, FilledInFieldsFitAnything) {
    const std::vector<std::string_view> catalog = {"Moved to %s.", "Opacity %.0f%%", "Open {program} first",
                                                   "%s", "{canvas}"};
    EXPECT_EQ(Match("Moved to Notes.", catalog).fit, Fit::Filled);
    EXPECT_EQ(Match("Moved to Notes.", catalog).indices, (Indices{0}));
    EXPECT_EQ(Match("Opacity 40%", catalog).indices, (Indices{1}));
    EXPECT_EQ(Match("Open Paint first", catalog).indices, (Indices{2}));
    EXPECT_EQ(Match("Press Ctrl+Z to undo", {"Press {key:undo} to undo"}).fit, Fit::Filled);
    EXPECT_EQ(Match("Moved to .", catalog).indices, (Indices{0})) << "a field filled with nothing";
    const Matches unfilled = Match("Moved to Notes", catalog);
    EXPECT_EQ(unfilled.fit, Fit::Part) << "the period missing";
    EXPECT_EQ(unfilled.indices, (Indices{0})) << "nothing but a field fits nothing";
}

// A help text or a card's text is drawn with its names, braces and all
// left out; it is found by those words.
TEST(StringMatchTest, AMarkedNameIsMatchedAsItIsDrawn) {
    const std::vector<std::string_view> catalog = {"Turn on {ui:Show deleted} first.", "Show deleted"};
    EXPECT_EQ(Match("Turn on Show deleted first.", catalog).indices, (Indices{0}));
    EXPECT_NE(Match("Turn on Show deleted first.", catalog).fit, Fit::Part);
}

TEST(StringMatchTest, APieceOfAStringOrAStringInAPieceIsAPartFit) {
    const std::vector<std::string_view> catalog = {"First paragraph.\n\nSecond paragraph.", "Basics", "of"};
    const Matches piece = Match("Second paragraph.", catalog);
    EXPECT_EQ(piece.fit, Fit::Part);
    EXPECT_EQ(piece.indices, (Indices{0}));
    EXPECT_EQ(Match("Basics - step 4 of 10", catalog).indices, (Indices{1})) << "\"of\" is too short to count";
    EXPECT_TRUE(Match("Something else", catalog).indices.empty());
}

TEST(StringMatchTest, FieldsAreTheConversionsInOrderAndTheNamedOnes) {
    const Fields fields = FieldsOf("%d of %s at %.0f%% in {program}");
    EXPECT_EQ(fields.printf, (std::vector<std::string>{"%d", "%s", "%.0f"}));
    EXPECT_EQ(fields.named, (std::vector<std::string>{"{program}"}));
}

TEST(StringMatchTest, AnEditMustKeepTheConversionsAndFillNothingNew) {
    EXPECT_EQ(FieldsProblem("%d of %s", "%d out of %s"), "");
    EXPECT_EQ(FieldsProblem("%.0f%%", "%.1f percent"), "") << "a precision reads the same value";
    EXPECT_NE(FieldsProblem("%d of %s", "%s of %d"), "");
    EXPECT_NE(FieldsProblem("%d of %s", "%d"), "");
    EXPECT_NE(FieldsProblem("100%%", "100%"), "") << "a lone % is not a percent sign";
    EXPECT_EQ(FieldsProblem("Open {program}", "Start {program} now"), "");
    EXPECT_EQ(FieldsProblem("Open {program}", "Open it"), "");
    EXPECT_NE(FieldsProblem("Open {program}", "Open {canvas}"), "");
    EXPECT_EQ(FieldsOf("Press {key:undo} or {trigger:screenshot}").named,
              (std::vector<std::string>{"{key:undo}", "{trigger:screenshot}"}));
    EXPECT_NE(FieldsProblem("Press Ctrl+Z", "Press {key:undo}"), "");
    EXPECT_EQ(FieldsProblem("Press Ctrl+Z", "Press {key:undo}", /*anyKey=*/true), "") << "the tutorial's";
    EXPECT_NE(FieldsProblem("Press Next", "Press {ui:Next}"), "") << "drawn with its braces here";
    EXPECT_EQ(FieldsProblem("Press Next", "Press {ui:Next}", false, /*marks=*/true), "");
    EXPECT_EQ(FieldsProblem("Turn on {ui:Show deleted}", "Turn on Show deleted"), "") << "a name is no field";
    EXPECT_NE(FieldsProblem("Open {program}", "Open {canvas}", /*anyKey=*/true), "");
}

TEST(StringMatchTest, AValueIsQuotedAsTheCatalogWritesIt) {
    EXPECT_EQ(JsonQuote("Say \"hi\"\n\\ \t"), "\"Say \\\"hi\\\"\\n\\\\ \\t\"");
    EXPECT_EQ(JsonQuote("\x01"), "\"\\u0001\"");
    EXPECT_EQ(JsonQuote("caf\xc3\xa9"), "\"caf\xc3\xa9\"") << "UTF-8 as it is";
    const std::string text = "a \"b\"\n\\c\td\x02 caf\xc3\xa9";
    EXPECT_EQ(nlohmann::json::parse(JsonQuote(text)).get<std::string>(), text);
}

TEST(StringMatchTest, OnlyTheOneValueChanges) {
    const std::string file =
        "{\n"
        "  \"_note\": \"a note\",\n"
        "\n"
        "  \"tool.draw\": \"Draw\",\n"
        "  \"tool.drawTip\": \"Draw - \\\"drag\\\"\",\n"
        "  \"tool.erase\": \"Erase\"\n"
        "}\n";
    const std::optional<std::string> changed = WithValue(file, "tool.drawTip", "Pen: drag\nto draw");
    ASSERT_TRUE(changed.has_value());
    EXPECT_EQ(*changed,
              "{\n"
              "  \"_note\": \"a note\",\n"
              "\n"
              "  \"tool.draw\": \"Draw\",\n"
              "  \"tool.drawTip\": \"Pen: drag\\nto draw\",\n"
              "  \"tool.erase\": \"Erase\"\n"
              "}\n");
    EXPECT_NE(WithValue(file, "tool.erase", "Rub")->find("  \"tool.erase\": \"Rub\"\n}\n"), std::string::npos)
        << "the last, with no comma";
    EXPECT_FALSE(WithValue(file, "tool.pen", "x").has_value());
    EXPECT_FALSE(WithValue(file + "  \"tool.draw\": \"Again\"\n", "tool.draw", "x").has_value()) << "twice";
}

// Every value in the real catalog can be written back, and writing one
// back as it is leaves the file as it was, byte for byte.
TEST(StringMatchTest, EveryValueInTheCatalogWritesBackAsItWas) {
    std::ifstream in(SPICKZETTEL_UI_STRINGS_JSON, std::ios::binary);
    ASSERT_TRUE(in.is_open()) << SPICKZETTEL_UI_STRINGS_JSON;
    std::ostringstream read;
    read << in.rdbuf();
    const std::string file = read.str();
    const nlohmann::json doc = nlohmann::json::parse(file);
    for (const auto& [key, value] : doc.items()) {
        const std::optional<std::string> same = WithValue(file, key, value.get<std::string>());
        ASSERT_TRUE(same.has_value()) << key;
        EXPECT_EQ(*same, file) << key;
    }
}

}  // namespace
}  // namespace sz::ui::string_editor
