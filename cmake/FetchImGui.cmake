include(FetchContent)

# Pinned to the v1.92.9 tag's commit; bump deliberately.
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/01380c579715e62fb9a8d6ec0502c4ea83bfde6e.tar.gz
    URL_HASH SHA256=c7bc489afefa2461c40a84812118e6dff86e7eadfc4b7e2f851a8c94ade8910d
)
FetchContent_MakeAvailable(imgui)

# Only the OS-independent part of Dear ImGui. The Win32/D3D11 backend
# sources are compiled into sz_platform_win32 from IMGUI_BACKENDS_DIR, so
# nothing above the platform layer can reach an OS header through ImGui.
set(_imgui_draw ${imgui_SOURCE_DIR}/imgui_draw.cpp)
# The string editor's build (docs/STRING_EDITOR.md) needs to know what text
# was drawn where, and every piece of text ImGui draws, a widget's or an
# AddText's, goes through ImFont::RenderText. A copy of imgui_draw.cpp with
# one call added at its start, to SzTextDrawn (ui/string_editor/
# text_ledger.cpp); the fetched source stays as it is. The copy's includes
# are found on imgui_core's include path, as they are next to the original.
if(SPICKZETTEL_STRING_EDITOR)
    file(READ ${_imgui_draw} _source)
    set(_render_text "void ImFont::RenderText(ImDrawList* draw_list, float size, const ImVec2& pos, ImU32 col, const ImVec4& clip_rect, const char* text_begin, const char* text_end, float wrap_width, ImDrawTextFlags flags)\n{\n")
    string(FIND "${_source}" "${_render_text}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "spickzettel: ImFont::RenderText is not where the string editor's hook expects it - "
                            "update cmake/FetchImGui.cmake with ImGui")
    endif()
    string(REPLACE "${_render_text}"
        "void SzTextDrawn(ImDrawList* draw_list, ImFont* font, float size, const ImVec2& pos, const ImVec4& clip_rect, const char* text_begin, const char* text_end, float wrap_width);\n${_render_text}    SzTextDrawn(draw_list, this, size, pos, clip_rect, text_begin, text_end, wrap_width);\n"
        _source "${_source}")
    # Written only when it differs, so a configure does not rebuild it.
    file(WRITE ${CMAKE_BINARY_DIR}/imgui_text_hook/imgui_draw.cpp.new "${_source}")
    configure_file(${CMAKE_BINARY_DIR}/imgui_text_hook/imgui_draw.cpp.new
                   ${CMAKE_BINARY_DIR}/imgui_text_hook/imgui_draw.cpp COPYONLY)
    set(_imgui_draw ${CMAKE_BINARY_DIR}/imgui_text_hook/imgui_draw.cpp)
endif()
add_library(imgui_core STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${_imgui_draw}
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
)
target_include_directories(imgui_core SYSTEM PUBLIC ${imgui_SOURCE_DIR})
target_compile_features(imgui_core PUBLIC cxx_std_17)
# 32-bit indices, in every target that includes imgui.h - see
# docs/ARCHITECTURE.md, "Tessellation, and its cache". A variable because
# the test engine takes ImGui's headers without linking this target, and
# has to agree on the type.
set(IMGUI_DRAW_INDEX_DEFINITION "ImDrawIdx=unsigned int" CACHE INTERNAL "")
target_compile_definitions(imgui_core PUBLIC "${IMGUI_DRAW_INDEX_DEFINITION}")
# Without ImGui's obsolete names, so that a call written against one does
# not compile - see docs/ARCHITECTURE.md, the gotcha on AddRect. Everywhere
# imgui.h is included, the test engine too, for the same reason as the
# index type: some of what it leaves out is in ImGui's own structs.
set(IMGUI_NO_OBSOLETE_DEFINITION "IMGUI_DISABLE_OBSOLETE_FUNCTIONS" CACHE INTERNAL "")
target_compile_definitions(imgui_core PUBLIC "${IMGUI_NO_OBSOLETE_DEFINITION}")

set(IMGUI_BACKENDS_DIR ${imgui_SOURCE_DIR}/backends CACHE INTERNAL "")
