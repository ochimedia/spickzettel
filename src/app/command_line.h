#pragma once

// The options the executable takes on its command line - see
// docs/ARCHITECTURE.md, "Command-line options". Each is a row of one table
// (kOptions in command_line.cpp): its name, what follows it, a line of
// help, and what it sets here. A person starting the app has no reason to
// pass any; they are for the scripts that test and measure it.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sz::app {

struct CommandLine {
    // --data-dir <folder>: the settings, the library and the crash dumps
    // in this one folder rather than the user's own - so that a test or a
    // measurement can neither read nor change them.
    std::optional<std::filesystem::path> dataDir;
    // --edit-mode: the overlay up in edit mode once started, as if started
    // again while running - so that a script reaches it without pressing a
    // global hotkey, which goes to whichever copy holds it.
    bool editMode = false;
};

struct ParsedCommandLine {
    CommandLine options;
    // Empty when every argument was understood; otherwise what was wrong,
    // for the message the app shows before it gives up.
    std::string error;
};

// `args` without the program's own name, as UTF-8. An option's value
// follows it as the next argument or after an equals sign (--data-dir=x);
// a flag (--edit-mode) has none. An option unknown, given twice, without
// its value, or a flag given one is an error, and so is anything that is
// not an option: a start with arguments nobody understood is not a start
// of what was meant.
ParsedCommandLine ParseCommandLine(const std::vector<std::string>& args);

// The options and their help, a line each, for that message.
std::string CommandLineUsage();

}  // namespace sz::app
