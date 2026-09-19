# The interface everything above the platform is written against: headers
# only, no OS dependency. The backends implement it - see win32/.
add_library(sz_platform_iface INTERFACE)
target_include_directories(sz_platform_iface INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(sz_platform_iface INTERFACE cxx_std_20)

# sz_platform_fonts: the embedded UI font, loaded into ImGui by a backend's
# Initialize() before the atlas is built - see spickzettel_fonts.h.
include(EmbedFile)
spickzettel_embed_binary_file(
    ${CMAKE_SOURCE_DIR}/assets/fonts/Manrope-Medium.ttf
    ${CMAKE_BINARY_DIR}/generated/font_manrope_medium.h
    kManropeMedium
)
add_library(sz_platform_fonts STATIC spickzettel_fonts.cpp)
target_include_directories(sz_platform_fonts PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(sz_platform_fonts PUBLIC sz_platform_iface imgui_core)
spickzettel_set_warnings(sz_platform_fonts)
