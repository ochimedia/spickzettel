# The portable suite: everything that needs neither a window nor an OS.
# Runs on Windows and, through the linux-tests preset, on Linux.
add_executable(sz_core_tests
    app/command_line_test.cpp
    app/headless_frame_test.cpp
    app/overlay_states_test.cpp
    app/perf_bench_test.cpp
    app/tray_app_test.cpp
    app/tutorial_app_test.cpp
    app/view_layer_test.cpp
    core/build_info_test.cpp
    core/app_config_test.cpp
    core/atomic_file_test.cpp
    core/config_files_test.cpp
    core/config_migrations_test.cpp
    core/canvas_manager_test.cpp
    core/canvas_state_test.cpp
    core/display_choice_test.cpp
    core/draw_tool_test.cpp
    core/history_test.cpp
    core/image_codec_test.cpp
    core/item_geometry_test.cpp
    core/library_store_test.cpp
    core/profile_test.cpp
    core/session_test.cpp
    core/settings_catalog_test.cpp
    core/settings_test.cpp
    core/shortcut_action_test.cpp
    core/stroke_clip_test.cpp
    core/stroke_mesh_test.cpp
    core/stroke_mesh_cache_test.cpp
    core/stroke_smoothing_test.cpp
    core/texture_cache_test.cpp
    core/timeline_test.cpp
    core/uid_test.cpp
    platform/pen_glyph_test.cpp
    platform/pointer_ballistics_test.cpp
    platform/presentation_test.cpp
    ui/icon_draw_test.cpp
    ui/item_painting_test.cpp
    ui/tooltip_test.cpp
    ui/cheat_sheet_test.cpp
    ui/command_test.cpp
    ui/editor_test.cpp
    ui/interaction_cases_test.cpp
    ui/machine_test.cpp
    ui/selection_layout_test.cpp
    ui/text_spans_test.cpp
    ui/tutorial_test.cpp
    ui/ui_strings_file_test.cpp
    ui/chains_test.cpp
    support/shared_library.cpp
)
target_include_directories(sz_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# sz_app, not only sz_core: the headless frame tests and the tray controller
# tests drive the whole app.
# sqlite too, for the store tests that reach into the file the way another
# program would.
target_link_libraries(sz_core_tests PRIVATE sz_app sqlite GTest::gtest_main)
target_compile_features(sz_core_tests PRIVATE cxx_std_20)
spickzettel_set_warnings(sz_core_tests)

# The benchmarks read their scenario from the environment, and the
# renderer's side-by-side picture where to write it; MSVC deprecates getenv
# for a shared-state reason that does not apply to a test.
set_source_files_properties(app/perf_bench_test.cpp PROPERTIES COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS)
set_source_files_properties(platform/win32_dx11_renderer_test.cpp PROPERTIES COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS)

# The string catalog as it is in the source tree, for the test that parses
# it strictly - see ui/ui_strings_file_test.cpp.
set_source_files_properties(ui/ui_strings_file_test.cpp PROPERTIES COMPILE_DEFINITIONS
    "SPICKZETTEL_UI_STRINGS_JSON=\"${CMAKE_SOURCE_DIR}/assets/ui_strings.json\"")

# Settings files as released builds wrote them and as this one writes them,
# read from the source tree - see core/config_files_test.cpp.
set_source_files_properties(core/config_files_test.cpp PROPERTIES COMPILE_DEFINITIONS
    "SPICKZETTEL_CONFIG_FILES=\"${CMAKE_CURRENT_SOURCE_DIR}/core/config_files\"")

spickzettel_use_utf8_code_page(sz_core_tests)

include(GoogleTest)
gtest_discover_tests(sz_core_tests)

# The UI tests drive real widgets by name and so need the test engine - see
# cmake/FetchImGuiTestEngine.cmake for why that is opt-in. A separate binary
# so the ordinary suite stays buildable and runnable without any of it.
if(SPICKZETTEL_UI_TESTS)
    add_executable(sz_ui_tests
        ui/canvas_bar_ui_test.cpp
        ui/context_menu_ui_test.cpp
        ui/overview_ui_test.cpp
        ui/settings_ui_test.cpp
        ui/tutorial_ui_test.cpp
        ui/view_layer_ui_test.cpp
        support/shared_library.cpp
    )
    target_include_directories(sz_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(sz_ui_tests PRIVATE sz_app imgui_test_engine GTest::gtest_main)
    target_compile_features(sz_ui_tests PRIVATE cxx_std_20)
    spickzettel_set_warnings(sz_ui_tests)
    spickzettel_use_utf8_code_page(sz_ui_tests)
    gtest_discover_tests(sz_ui_tests)
endif()

# The Win32 backend against the real OS: the input grab's registrations,
# and whatever displays the machine has. Its own binary so the portable
# suite stays buildable on Linux.
if(WIN32)
    add_executable(sz_win32_tests
        platform/win32_crash_dump_test.cpp
        platform/win32_displays_test.cpp
        platform/win32_dx11_renderer_test.cpp
        platform/win32_input_grab_test.cpp
        platform/win32_integrity_test.cpp
        platform/win32_overlay_window_test.cpp
        platform/win32_platform_host_test.cpp
        platform/win32_screen_capture_test.cpp
    )
    target_link_libraries(sz_win32_tests PRIVATE sz_platform_win32 sz_ui GTest::gtest_main)
    target_include_directories(sz_win32_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(sz_win32_tests PRIVATE cxx_std_20)
    spickzettel_set_warnings(sz_win32_tests)
    spickzettel_use_utf8_code_page(sz_win32_tests)
    # One at a time: these install desktop-wide hooks and register raw
    # input for the whole process, which two copies would fight over. The
    # timeout is there because a hook thread that never stops would
    # otherwise hang the suite rather than fail it.
    gtest_discover_tests(sz_win32_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)
endif()
