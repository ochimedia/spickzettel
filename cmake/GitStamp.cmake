# Writes generated/git_stamp.h with `git describe` for the working tree.
#
# Run with `cmake -P`: once at configure time so the header exists, and
# again from an always-run target on every build. The second half is the
# point - a plain execute_process(git ...) in CMakeLists.txt bakes the hash
# at configure time, and this project configures once per preset and then
# builds for days, so the About tab would show whichever commit the build
# directory was created on.
#
# Costs one git call per build. It does not cost a rebuild: the header is
# written to a temp file and copied only if it differs, so the one
# translation unit that includes it recompiles only when the commit did
# change.
#
# Expects -DGIT_SRC_DIR=<repo root> -DOUT_FILE=<header path>.

# --tags   prefer a release tag when there is one
# --always fall back to a bare short hash otherwise
# --dirty  mark a build made from uncommitted changes
set(_describe_args describe --tags --always --dirty)

# "unknown" rather than an error: a source archive without .git, or a
# machine without git, is a legitimate way to build this.
set(_git_describe "unknown")

find_package(Git QUIET)
if(GIT_EXECUTABLE AND EXISTS "${GIT_SRC_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${_describe_args}
        WORKING_DIRECTORY "${GIT_SRC_DIR}"
        OUTPUT_VARIABLE _describe_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _describe_result
    )
    if(_describe_result EQUAL 0 AND NOT _describe_output STREQUAL "")
        set(_git_describe "${_describe_output}")
    endif()
endif()

file(WRITE "${OUT_FILE}.tmp"
"// Generated on every build by cmake/GitStamp.cmake - do not edit.
// Included by exactly one translation unit (build_info.cpp): this changes
// with every commit and must not sit anywhere that would rebuild the
// project because of it.
#pragma once

#include <string_view>

namespace sz::core::build::generated {
inline constexpr std::string_view kGitDescribe{\"${_git_describe}\"};
}  // namespace sz::core::build::generated
")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT_FILE}.tmp" "${OUT_FILE}")
file(REMOVE "${OUT_FILE}.tmp")
