# sz_core: the model, the persistence, the settings and the session. No OS
# headers and no Dear ImGui, on purpose - a build that cannot reach imgui.h
# is what keeps this layer usable by a different UI, or by none, as the
# tests do.
add_library(sz_core STATIC
    build_info/build_info.cpp
    canvas/canvas_manager.cpp
    canvas/item_geometry.cpp
    config/app_config.cpp
    config/bar_layout.cpp
    config/display_choice.cpp
    config/profile.cpp
    config/shortcut_action.cpp
    drawing/canvas_state.cpp
    drawing/draw_tool.cpp
    drawing/painted_image.cpp
    drawing/stroke_mesh.cpp
    drawing/stroke_mesh_cache.cpp
    drawing/stroke_clip.cpp
    drawing/stroke_smoothing.cpp
    persistence/image_codec.cpp
    persistence/library_store.cpp
    util/slug.cpp
    util/timestamp_name.cpp
    util/uid.cpp
)

target_include_directories(sz_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
# PUBLIC: build_info.h includes "generated/build_config.h", and the
# include root for generated headers is CMAKE_BINARY_DIR.
target_include_directories(sz_core PUBLIC ${CMAKE_BINARY_DIR})
# The git stamp is regenerated before anything here compiles - see
# cmake/GitStamp.cmake.
add_dependencies(sz_core sz_git_stamp)
target_compile_features(sz_core PUBLIC cxx_std_20)
target_link_libraries(sz_core PUBLIC sz_platform_iface stb qoi nlohmann_json::nlohmann_json)
spickzettel_set_warnings(sz_core)
