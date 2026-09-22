#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "generated/ui_strings.h"

namespace sz::ui {
namespace {

// The catalogue is built by CMake's own JSON parser (see
// cmake/UiStrings.cmake), which lets a raw line break inside a string
// through - and by the time a value reaches the generator it looks the same
// as an escaped one, so the build cannot tell. Every other tool that reads
// the file can: a strict parser rejects it whole. This is the strict parse,
// so that a help text typed with real line breaks fails here rather than in
// whatever next reads the file.
TEST(UiStringsFileTest, TheCatalogueIsStrictJson) {
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
    EXPECT_NE(std::string(strings::kBarsHelp).find("\n\n"), std::string::npos);
}

}  // namespace
}  // namespace sz::ui
