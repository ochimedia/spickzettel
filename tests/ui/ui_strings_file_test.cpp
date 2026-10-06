#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "generated/ui_strings.h"

namespace sz::ui {
namespace {

// The catalog is built by CMake's own JSON parser (see
// cmake/UiStrings.cmake), which lets a raw line break inside a string
// through - and by the time a value reaches the generator it looks the same
// as an escaped one, so the build cannot tell. Every other tool that reads
// the file can: a strict parser rejects it whole. This is the strict parse,
// so that a help text typed with real line breaks fails here rather than in
// whatever next reads the file.
TEST(UiStringsFileTest, TheCatalogIsStrictJson) {
    std::ifstream in(SPICKZETTEL_UI_STRINGS_JSON, std::ios::binary);
    ASSERT_TRUE(in.is_open()) << SPICKZETTEL_UI_STRINGS_JSON;
    std::ostringstream text;
    text << in.rdbuf();

    const nlohmann::json doc = nlohmann::json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(doc.is_discarded()) << "not strict JSON - a raw line break in a string, say; write it as \\n";
    ASSERT_TRUE(doc.is_object());
    for (const auto& [key, value] : doc.items()) {
        EXPECT_TRUE(value.is_string()) << key;
    }
    // And an escaped break still arrives in the app as a break.
    EXPECT_NE(std::string(strings::kAppearanceImageFilterHelp).find("\n\n"), std::string::npos);
}

// A {ui:name} is drawn marked by the tutorial card, which fills a card's
// texts in (tutorial::ExpandSpans), and by the help boxes and help
// tooltips, which draw the help texts (MarkedSpans). Anywhere else it
// would show as typed: a label, a title, a message, the list, or the
// tutorial's Settings row.
TEST(UiStringsFileTest, AMarkedNameIsOnlyWhereItIsDrawnMarked) {
    std::ifstream in(SPICKZETTEL_UI_STRINGS_JSON, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    std::ostringstream text;
    text << in.rdbuf();
    const nlohmann::json doc = nlohmann::json::parse(text.str());
    for (const auto& [key, value] : doc.items()) {
        if (!value.is_string() || value.get<std::string>().find("{ui:") == std::string::npos) {
            continue;
        }
        const bool cardText = key.starts_with("tutorial.") && !key.ends_with(".title") &&
                              !key.starts_with("tutorial.card.") && !key.starts_with("tutorial.list.") &&
                              !key.starts_with("tutorial.topics.") && !key.starts_with("tutorial.settings.");
        const bool helpText = key.ends_with("Help");
        EXPECT_TRUE(cardText || helpText) << key;
    }
}

}  // namespace
}  // namespace sz::ui
