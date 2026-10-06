#include "ui/string_editor/string_match.h"

#include <algorithm>
#include <cstdio>

namespace sz::ui::string_editor {

namespace {

// The printf conversion at `text[at]`, a '%', as long as it is - 0 for a
// '%' that starts none, "%%" among them.
std::size_t ConversionLength(std::string_view text, std::size_t at) {
    std::size_t i = at + 1;
    const auto more = [&] { return i < text.size(); };
    while (more() && std::string_view("-+ #0").find(text[i]) != std::string_view::npos) {
        ++i;
    }
    while (more() && (text[i] == '*' || (text[i] >= '0' && text[i] <= '9'))) {
        ++i;
    }
    if (more() && text[i] == '.') {
        ++i;
        while (more() && (text[i] == '*' || (text[i] >= '0' && text[i] <= '9'))) {
            ++i;
        }
    }
    while (more() && std::string_view("hlzjtL").find(text[i]) != std::string_view::npos) {
        ++i;
    }
    if (!more() || std::string_view("diouxXfFeEgGaAcsp").find(text[i]) == std::string_view::npos) {
        return 0;
    }
    return i + 1 - at;
}

// The marked name at `text[at]`, a '{', as long as it is - 0 for none:
// "{ui:Show deleted}", drawn as the name alone (ui/text_spans.h).
std::size_t MarkLength(std::string_view text, std::size_t at) {
    if (text.substr(at, 4) != "{ui:") {
        return 0;
    }
    const std::size_t close = text.find('}', at);
    return close == std::string_view::npos ? 0 : close + 1 - at;
}

// The named field at `text[at]`, a '{', as long as it is - 0 for none:
// "{program}", or with what it names, "{key:undo}".
std::size_t NamedLength(std::string_view text, std::size_t at) {
    const auto word = [&](std::size_t i) {
        while (i < text.size() && ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
            ++i;
        }
        return i;
    };
    std::size_t i = word(at + 1);
    if (i == at + 1) {
        return 0;
    }
    if (i < text.size() && text[i] == ':') {
        const std::size_t after = word(i + 1);
        if (after == i + 1) {
            return 0;
        }
        i = after;
    }
    return i < text.size() && text[i] == '}' ? i + 1 - at : 0;
}

// `text` cut at its fields: the words between them, one more than there
// are fields, with "%%" as the '%' it prints.
std::vector<std::string> Literals(std::string_view text) {
    std::vector<std::string> literals(1);
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == '%') {
            literals.back() += '%';
            i += 2;
        } else if (const std::size_t conversion = text[i] == '%' ? ConversionLength(text, i) : 0) {
            literals.emplace_back();
            i += conversion;
        } else if (const std::size_t mark = text[i] == '{' ? MarkLength(text, i) : 0) {
            literals.back() += text.substr(i + 4, mark - 5);
            i += mark;
        } else if (const std::size_t named = text[i] == '{' ? NamedLength(text, i) : 0) {
            literals.emplace_back();
            i += named;
        } else {
            literals.back() += text[i];
            ++i;
        }
    }
    return literals;
}

// Whether `drawn` is `literals` with anything in each gap between them.
bool Fills(std::string_view drawn, const std::vector<std::string>& literals) {
    const std::string& first = literals.front();
    const std::string& last = literals.back();
    if (literals.size() == 1) {
        return drawn == first;
    }
    if (drawn.size() < first.size() + last.size() || !drawn.starts_with(first) || !drawn.ends_with(last)) {
        return false;
    }
    std::size_t at = first.size();
    const std::size_t end = drawn.size() - last.size();
    for (std::size_t i = 1; i + 1 < literals.size(); ++i) {
        const std::size_t found = drawn.find(literals[i], at);
        if (found == std::string_view::npos || found + literals[i].size() > end) {
            return false;
        }
        at = found + literals[i].size();
    }
    return true;
}

std::string_view Trimmed(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") + 1 - begin);
}

// The shortest piece of text a part match is made on: shorter, and a word
// like "of" is a piece of half the catalog.
constexpr std::size_t kShortestPart = 4;
// How many part matches are worth listing.
constexpr std::size_t kMostParts = 12;

// What a conversion takes and reads: its '*'s, its length and its letter -
// "%.0f" and "%.1f" the same, "%d" and "%s" not.
std::string Kind(std::string_view conversion) {
    std::string kind;
    for (const char c : conversion.substr(1)) {
        if (c == '*' || std::string_view("hlzjtL").find(c) != std::string_view::npos) {
            kind += c;
        }
    }
    kind += conversion.back();
    return kind;
}

}  // namespace

Matches Match(std::string_view drawn, const std::vector<std::string_view>& catalog) {
    Matches matches;
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        if (catalog[i] == drawn) {
            matches.indices.push_back(i);
        }
    }
    if (!matches.indices.empty()) {
        return matches;
    }

    std::vector<std::vector<std::string>> literals;
    literals.reserve(catalog.size());
    for (const std::string_view text : catalog) {
        literals.push_back(Literals(text));
    }
    matches.fit = Fit::Filled;
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        std::size_t words = 0;
        for (const std::string& literal : literals[i]) {
            words += literal.size();
        }
        // A string that is nothing but a field fits anything. One whose
        // only fields are marked names is drawn as its one literal.
        const bool filled = literals[i].size() > 1 || literals[i].front() != catalog[i];
        if (filled && words >= 2 && Fills(drawn, literals[i])) {
            matches.indices.push_back(i);
        }
    }
    if (!matches.indices.empty()) {
        return matches;
    }

    // In part, the longest piece in common first.
    matches.fit = Fit::Part;
    const std::string_view piece = Trimmed(drawn);
    std::vector<std::pair<std::size_t, std::size_t>> parts;  // (length in common, index)
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        std::size_t common = 0;
        for (const std::string& literal : literals[i]) {
            const std::string_view words = Trimmed(literal);
            if (piece.size() >= kShortestPart && words.find(piece) != std::string_view::npos) {
                common = std::max(common, piece.size());
            } else if (words.size() >= kShortestPart && piece.find(words) != std::string_view::npos) {
                common = std::max(common, words.size());
            }
        }
        if (common > 0) {
            parts.emplace_back(common, i);
        }
    }
    std::stable_sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (parts.size() > kMostParts) {
        parts.resize(kMostParts);
    }
    for (const auto& part : parts) {
        matches.indices.push_back(part.second);
    }
    std::sort(matches.indices.begin(), matches.indices.end());
    return matches;
}

Fields FieldsOf(std::string_view text) {
    Fields fields;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == '%') {
            fields.percents = true;
            i += 2;
        } else if (const std::size_t conversion = text[i] == '%' ? ConversionLength(text, i) : 0) {
            fields.printf.emplace_back(text.substr(i, conversion));
            i += conversion;
        } else if (text[i] == '%') {
            fields.lonePercent = true;
            ++i;
        } else if (const std::size_t mark = text[i] == '{' ? MarkLength(text, i) : 0) {
            fields.marks = true;
            i += mark;
        } else if (const std::size_t named = text[i] == '{' ? NamedLength(text, i) : 0) {
            fields.named.emplace_back(text.substr(i, named));
            i += named;
        } else {
            ++i;
        }
    }
    return fields;
}

std::string FieldsProblem(std::string_view original, std::string_view edited, bool anyKey, bool marks) {
    const Fields before = FieldsOf(original);
    const Fields after = FieldsOf(edited);
    std::string wanted;
    std::string got;
    for (const std::string& conversion : before.printf) {
        wanted += wanted.empty() ? "" : " ";
        wanted += conversion;
    }
    for (const std::string& conversion : after.printf) {
        got += got.empty() ? "" : " ";
        got += conversion;
    }
    bool same = before.printf.size() == after.printf.size();
    for (std::size_t i = 0; same && i < before.printf.size(); ++i) {
        same = Kind(before.printf[i]) == Kind(after.printf[i]);
    }
    if (!same) {
        return "Needs the same % fields in the same order: " + (wanted.empty() ? std::string("none") : wanted) +
               " (has " + (got.empty() ? std::string("none") : got) + "). Write a percent sign as %%.";
    }
    if (after.lonePercent && (!before.printf.empty() || before.percents)) {
        return "A percent sign is written %% here.";
    }
    if (after.marks && !marks) {
        return "Only the tutorial's cards and the help texts draw {ui:} names.";
    }
    for (const std::string& named : after.named) {
        const bool key = named.starts_with("{key:") || named.starts_with("{trigger:");
        if (!(anyKey && key) && std::find(before.named.begin(), before.named.end(), named) == before.named.end()) {
            return "Nothing fills in " + named + " here.";
        }
    }
    return {};
}

std::string JsonQuote(std::string_view text) {
    std::string quoted = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':
                quoted += "\\\"";
                break;
            case '\\':
                quoted += "\\\\";
                break;
            case '\n':
                quoted += "\\n";
                break;
            case '\r':
                quoted += "\\r";
                break;
            case '\t':
                quoted += "\\t";
                break;
            case '\b':
                quoted += "\\b";
                break;
            case '\f':
                quoted += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    quoted += escaped;
                } else {
                    quoted += c;
                }
        }
    }
    quoted += '"';
    return quoted;
}

std::optional<std::string> WithValue(std::string_view file, std::string_view key, std::string_view text) {
    const std::string name = "\"" + std::string(key) + "\"";
    std::optional<std::pair<std::size_t, std::size_t>> value;  // [begin, end), quotes included
    for (std::size_t line = 0; line < file.size();) {
        std::size_t lineEnd = file.find('\n', line);
        lineEnd = lineEnd == std::string_view::npos ? file.size() : lineEnd;
        std::size_t i = file.find_first_not_of(" \t", line);
        if (i < lineEnd && file.substr(i).starts_with(name)) {
            i = file.find_first_not_of(" \t", i + name.size());
            if (i < lineEnd && file[i] == ':') {
                i = file.find_first_not_of(" \t", i + 1);
            } else {
                i = lineEnd;
            }
            if (i < lineEnd && file[i] == '"') {
                std::size_t end = i + 1;
                while (end < lineEnd && file[end] != '"') {
                    end += file[end] == '\\' ? 2 : 1;
                }
                if (end >= lineEnd || value.has_value()) {
                    return std::nullopt;
                }
                value.emplace(i, end + 1);
            }
        }
        line = lineEnd + 1;
    }
    if (!value.has_value()) {
        return std::nullopt;
    }
    std::string changed(file.substr(0, value->first));
    changed += JsonQuote(text);
    changed += file.substr(value->second);
    return changed;
}

}  // namespace sz::ui::string_editor
