#include "app/command_line.h"

#include <cstdio>
#include <string_view>
#include <system_error>

#include "generated/ui_strings.h"

namespace sz::app {

namespace {

// UTF-8, whatever the code page: a folder named in any script is a folder.
std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

struct Option {
    std::string_view name;
    // What follows it, as the help shows it.
    std::string_view value;
    const char* help;
    // False when the value is no use - empty, say.
    bool (*apply)(CommandLine& options, std::string_view value);
    // Whether this run's options already have it.
    bool (*given)(const CommandLine& options);
};

const Option kOptions[] = {
    {"--data-dir", "<folder>", strings::kCommandLineDataDir,
     [](CommandLine& options, std::string_view value) {
         if (value.empty()) {
             return false;
         }
         // Absolute, so that it means the same folder whatever the
         // working directory is when something asks for it.
         std::error_code ec;
         std::filesystem::path folder = std::filesystem::absolute(PathFromUtf8(value), ec);
         options.dataDir = ec ? PathFromUtf8(value) : folder.lexically_normal();
         return true;
     },
     [](const CommandLine& options) { return options.dataDir.has_value(); }},
};

const Option* Find(std::string_view name) {
    for (const Option& option : kOptions) {
        if (option.name == name) {
            return &option;
        }
    }
    return nullptr;
}

std::string Formatted(const char* format, std::string_view value) {
    char text[512];
    std::snprintf(text, sizeof(text), format, std::string(value).c_str());
    return text;
}

}  // namespace

ParsedCommandLine ParseCommandLine(const std::vector<std::string>& args) {
    ParsedCommandLine parsed;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        std::optional<std::string_view> value;
        if (const size_t equals = arg.find('='); arg.starts_with("--") && equals != std::string_view::npos) {
            value = arg.substr(equals + 1);
            arg = arg.substr(0, equals);
        }
        const Option* option = Find(arg);
        if (option == nullptr) {
            parsed.error = Formatted(strings::kCommandLineUnknown, args[i]);
            return parsed;
        }
        if (option->given(parsed.options)) {
            parsed.error = Formatted(strings::kCommandLineTwice, option->name);
            return parsed;
        }
        if (!value.has_value()) {
            if (i + 1 >= args.size()) {
                parsed.error = Formatted(strings::kCommandLineNoValue, option->name);
                return parsed;
            }
            value = args[++i];
        }
        if (!option->apply(parsed.options, *value)) {
            parsed.error = Formatted(strings::kCommandLineNoValue, option->name);
            return parsed;
        }
    }
    return parsed;
}

std::string CommandLineUsage() {
    std::string usage;
    for (const Option& option : kOptions) {
        usage += "\n    ";
        usage += option.name;
        usage += ' ';
        usage += option.value;
        usage += "\n        ";
        usage += option.help;
    }
    return usage;
}

}  // namespace sz::app
