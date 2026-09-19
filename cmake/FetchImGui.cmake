include(FetchContent)

# Pinned to the v1.92.9 tag's commit; bump deliberately.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 01380c579715e62fb9a8d6ec0502c4ea83bfde6e # v1.92.9
)
FetchContent_MakeAvailable(imgui)

# Only the OS-independent part of Dear ImGui. The Win32/D3D11 backend
# sources are compiled into sz_platform_win32 from IMGUI_BACKENDS_DIR, so
# nothing above the platform layer can reach an OS header through ImGui.
add_library(imgui_core STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
)
target_include_directories(imgui_core SYSTEM PUBLIC ${imgui_SOURCE_DIR})
target_compile_features(imgui_core PUBLIC cxx_std_17)

set(IMGUI_BACKENDS_DIR ${imgui_SOURCE_DIR}/backends CACHE INTERNAL "")
