# The portable suite: everything that needs neither a window nor an OS.
# Runs on Windows and, through the linux-tests preset, on Linux.
add_executable(sz_core_tests
    core/build_info_test.cpp
    core/app_config_test.cpp
    core/canvas_manager_test.cpp
    core/canvas_state_test.cpp
    core/display_choice_test.cpp
    core/draw_tool_test.cpp
    core/image_codec_test.cpp
    core/item_geometry_test.cpp
    core/library_store_test.cpp
    core/painted_image_test.cpp
    core/profile_test.cpp
    core/session_test.cpp
    core/settings_test.cpp
    core/shortcut_action_test.cpp
    core/slug_test.cpp
    core/stroke_clip_test.cpp
    core/stroke_mesh_test.cpp
    core/stroke_mesh_cache_test.cpp
    core/stroke_smoothing_test.cpp
    core/uid_test.cpp
    platform/pen_glyph_test.cpp
    ui/icon_draw_test.cpp
    ui/shortcut_targets_test.cpp
)
target_include_directories(sz_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sz_core_tests PRIVATE sz_ui GTest::gtest_main)
target_compile_features(sz_core_tests PRIVATE cxx_std_20)
spickzettel_set_warnings(sz_core_tests)

include(GoogleTest)
gtest_discover_tests(sz_core_tests)
