# sz_app: TrayController - what a hotkey or the tray does. Owns the settings,
# the session and the overlay, loads the library into the session, and moves
# the window between hidden, edit and view-only. The one place that knows
# about all three layers at once, and so the one place the executable links.
add_library(sz_app STATIC
    tray_app.cpp
)

target_include_directories(sz_app PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(sz_app PUBLIC cxx_std_20)
target_link_libraries(sz_app PUBLIC sz_ui)
spickzettel_set_warnings(sz_app)
