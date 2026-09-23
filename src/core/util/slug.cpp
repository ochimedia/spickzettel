#include "core/util/slug.h"

#include <algorithm>

#include "core/util/uid.h"

namespace sz::core {

namespace {
// Long enough for a default name whole - TimestampName's
// "2026-09-07 22:36:14" is 19 - and short because every level of the tree
// pays it: a snippet's files sit three slugs below the library root, and
// past MAX_PATH a record is neither written nor read back.
constexpr size_t kMaxBaseLength = 20;
}  // namespace

std::string MakeSlug(const std::string& name, uint64_t id) {
    // ASCII letters/digits pass through (lowercased); everything else -
    // punctuation, whitespace, and every byte of a multi-byte UTF-8
    // sequence alike - collapses into a single '-' separator rather than
    // being transliterated, which keeps this dependency-free at the cost
    // of dropping non-ASCII text down to whatever ASCII remains.
    std::string base;
    base.reserve(std::min(name.size(), kMaxBaseLength));
    bool lastWasSeparator = true;  // starts true so a leading separator is dropped, not emitted
    for (unsigned char ch : name) {
        if (ch >= 'a' && ch <= 'z') {
            base.push_back(static_cast<char>(ch));
            lastWasSeparator = false;
        } else if (ch >= 'A' && ch <= 'Z') {
            base.push_back(static_cast<char>(ch - 'A' + 'a'));
            lastWasSeparator = false;
        } else if (ch >= '0' && ch <= '9') {
            base.push_back(static_cast<char>(ch));
            lastWasSeparator = false;
        } else if (!lastWasSeparator) {
            base.push_back('-');
            lastWasSeparator = true;
        }
        if (base.size() >= kMaxBaseLength) {
            break;
        }
    }
    while (!base.empty() && base.back() == '-') {
        base.pop_back();
    }
    if (base.empty()) {
        base = "untitled";
    }
    return base + "-" + FormatUid(id);
}

}  // namespace sz::core
