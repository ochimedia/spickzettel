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

std::vector<TextSpan> MarkedSpans(std::string_view text, std::string_view arg) {
    std::vector<TextSpan> out = MarkedSpans(text);
    for (TextSpan& span : out) {
        if (const size_t at = span.text.find("%s"); at != std::string::npos) {
            span.text.replace(at, 2, arg);
            break;
        }
    }
    return out;
}

}  // namespace sz::ui
