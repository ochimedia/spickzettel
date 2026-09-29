#pragma once

// The string editor's pure parts (docs/STRING_EDITOR.md): which catalog
// strings a text drawn on screen came from, whether an edit keeps what the
// code fills in, and the catalog file with one value changed.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sz::ui::string_editor {

// How well drawn text matches a catalog string: word for word; with what
// the code fills in ("%s", "{program}") taken as anything; or in part -
// the drawn text a piece of the string, or the string a piece of it.
enum class Fit { Exact, Filled, Part };

struct Matches {
    Fit fit = Fit::Exact;
    // Indices into the catalog, in its order; empty for none at all.
    std::vector<std::size_t> indices;
};

// The catalog strings `drawn` came from, the best fit only.
Matches Match(std::string_view drawn, const std::vector<std::string_view>& catalog);

// What the code fills into `text`: its printf conversions in order ("%s",
// "%.0f"; "%%" is a percent sign, not one), and its named fields
// ("{program}", "{key:undo}").
struct Fields {
    std::vector<std::string> printf;
    std::vector<std::string> named;
    // A "%%", and a '%' that starts no conversion at all.
    bool percents = false;
    bool lonePercent = false;
};
Fields FieldsOf(std::string_view text);

// Why `edited` cannot stand in for `original` - empty when it can: the same
// printf conversions in the same order, since the code passes their values
// in that order and one more reads what is not there; in a format, a
// percent sign as "%%"; and no named field the code does not fill in -
// but with `anyKey`, as in the tutorial's texts (tutorial::Expand), any
// "{key:...}" and "{trigger:...}".
std::string FieldsProblem(std::string_view original, std::string_view edited, bool anyKey = false);

// `text` as a JSON string literal, quotes included, escaped as the catalog
// file is written: quotes, backslashes and control characters, the rest
// as it is.
std::string JsonQuote(std::string_view text);

// `file`, the catalog's JSON, with `key`'s value `text` - only that value
// changed, the rest byte for byte. None when `key` is not on a line of its
// own, as `"key": "value"`, exactly once.
std::optional<std::string> WithValue(std::string_view file, std::string_view key, std::string_view text);

}  // namespace sz::ui::string_editor
