#include "core/build_info/build_info.h"

// The volatile generated headers, included here and nowhere else - see
// build_info.h for why that containment is the point.
#include "generated/about_text.h"
#include "generated/git_stamp.h"
#include "generated/notices_text.h"

namespace sz::core::build {

std::string_view GitDescribe() { return generated::kGitDescribe; }

std::string_view AboutText() { return generated::kAboutText; }

std::string_view NoticesText() { return generated::kNoticesText; }

std::string VersionLine() {
    std::string line(kVersion);
    // "0.1.0 (v0.1.0)" is noise: a description that is just the release
    // tag, with or without the conventional "v", adds nothing.
    const std::string_view describe = GitDescribe();
    const bool redundant = describe == kVersion || (describe.size() == kVersion.size() + 1 &&
                                                     describe.front() == 'v' && describe.substr(1) == kVersion);
    if (!describe.empty() && describe != "unknown" && !redundant) {
        line += " (";
        line += describe;
        line += ")";
    }
    if (kDemoMode) {
        line += " - demo";
    }
    return line;
}

}  // namespace sz::core::build
