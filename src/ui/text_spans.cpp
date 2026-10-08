#include "ui/text_spans.h"

namespace sz::ui {

std::vector<TextSpan> MarkedSpans(std::string_view text) {
    std::vector<TextSpan> out;
    auto add = [&out](std::string_view piece, bool marked) {
        if (piece.empty()) {
            return;
        }
        if (!marked && !out.empty() && !out.back().marked) {
            out.back().text.append(piece);
        } else {
            out.push_back(TextSpan{std::string(piece), marked});
        }
    };
    size_t at = 0;
    while (at < text.size()) {
        const size_t open = text.find("{ui:", at);
        const size_t close = open == std::string_view::npos ? open : text.find('}', open);
        if (close == std::string_view::npos) {
            add(text.substr(at), false);
            break;
        }
        add(text.substr(at, open - at), false);
        add(text.substr(open + 4, close - open - 4), true);
        at = close + 1;
    }
    return out;
}

std::string WithValue(std::string_view format, std::string_view value) {
    const size_t at = format.find("%s");
    if (at == std::string_view::npos) {
        return std::string(format);
    }
    std::string text(format.substr(0, at));
    text += value;
    text += format.substr(at + 2);
    return text;
}

}  // namespace sz::ui
