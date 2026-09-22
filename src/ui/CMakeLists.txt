# sz_ui: the overlay as it is drawn - OverlayApp, its panels, the item
# chrome, and the drawing helpers they share. A view of the session in
# sz_core: it reads the models, edits them, and asks the session for
# everything that has to stay in step with them.
#
# The only library above the platform interface that links Dear ImGui.
add_library(sz_ui STATIC
    context_menu.cpp
    icon_draw.cpp
    overlay_app.cpp
    overlay_app_deleted.cpp
    overlay_app_docks.cpp
    overlay_app_input.cpp
    overlay_app_items.cpp
    overlay_app_overview.cpp
    overlay_app_paint.cpp
    overlay_app_popovers.cpp
    overlay_app_undo.cpp
)

target_include_directories(sz_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
# generated/ui_strings.h, and the build info the About tab shows.
target_include_directories(sz_ui PUBLIC ${CMAKE_BINARY_DIR})
target_compile_features(sz_ui PUBLIC cxx_std_20)
target_link_libraries(sz_ui PUBLIC sz_core imgui_core)
spickzettel_set_warnings(sz_ui)
