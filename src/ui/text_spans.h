#pragma once

// Text in runs, some of them marked: the names of what to press, tick or
// pick, which the tutorial's cards and the help boxes draw in a color of
// their own (WrappedSpans, in widgets.h).

#include <string>
#include <string_view>
#include <vector>

namespace sz::ui {

// A run of text, and whether it names something to press, tick or pick - a
// key, a button, a row.
struct TextSpan {
    std::string text;
    bool marked = false;
};

// `text` in runs, each {ui:<name>} the name as it is, marked. Anything
// else in braces is left as it is. The tutorial's cards fill in more
// (tutorial::ExpandSpans).
std::vector<TextSpan> MarkedSpans(std::string_view text);
// The same, with `arg` - the user's own words, a profile's name - put
// where `text` says %s once the names are found, so that a brace in it is
// shown as typed rather than read as a mark. All of it, however long:
// snprintf into an array cut a long name by bytes, through the middle of
// a character. `text` holds that one %s and no other conversion.
std::vector<TextSpan> MarkedSpans(std::string_view text, std::string_view arg);

}  // namespace sz::ui
