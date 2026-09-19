include(FetchContent)

# stb_image / stb_image_write, header-only, for PNG. stb has no version
# tags, so the pin is a commit; bump deliberately.
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 5c205738c191bcb0abc65c4febfa9bd25ff35234
)
FetchContent_MakeAvailable(stb)

# SYSTEM: stb's own code trips -Wall/-Wextra, and vendored source is not
# ours to make warning-clean.
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
