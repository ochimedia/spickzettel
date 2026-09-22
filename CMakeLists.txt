cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# The one number in VERSION feeds both project() and the generated build
# header, so the two cannot drift. Read before project() because project()
# needs it.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" SPICKZETTEL_VERSION)
string(STRIP "${SPICKZETTEL_VERSION}" SPICKZETTEL_VERSION)
if(NOT SPICKZETTEL_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must be MAJOR.MINOR.PATCH, got '${SPICKZETTEL_VERSION}'")
endif()

project(spickzettel VERSION ${SPICKZETTEL_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPICKZETTEL_BUILD_TESTS "Build the unit tests" ON)
# Compile-time flag rather than a setting: a demo watermark that can be
# switched off in config.json is not a watermark. See docs/ARCHITECTURE.md.
option(SPICKZETTEL_DEMO_MODE "Build a demo binary with a permanent 'Demo Version' mark" OFF)
# The same reasoning: a build handed out ahead of a release, which says so at
# every start and wherever it states its version.
option(SPICKZETTEL_PRERELEASE_NOTICE "Build a prerelease binary that shows a not-for-redistribution notice at every start" OFF)
# Off by default because it defines IMGUI_ENABLE_TEST_ENGINE for ImGui
# itself, which every consumer then has to link. On in the debug preset.
option(SPICKZETTEL_UI_TESTS "Build the UI tests, which drive real widgets by name" OFF)
# Where a finished Spickzettel.exe is also copied to: dist/<name>/ at the
# repo root, next to nothing else, instead of deep in the build tree. Each
# Windows preset sets its own name; empty copies nowhere.
set(SPICKZETTEL_DIST_NAME "" CACHE STRING "Copy Spickzettel.exe to dist/<name>/ after each build (empty: don't)")

# Warnings are per target rather than global so that fetched third-party
# code is compiled with its own defaults.
function(spickzettel_set_warnings TARGET)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /W4)
    else()
        target_compile_options(${TARGET} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Third-party code, all fetched and pinned - see docs/ARCHITECTURE.md.
include(FetchImGui)
if(SPICKZETTEL_UI_TESTS)
    include(FetchImGuiTestEngine)
    # ImGui itself has to know, or it compiles none of the hooks the engine
    # listens on; PUBLIC so everything that includes imgui.h agrees, since
    # the hook macros are in the header.
    target_compile_definitions(imgui_core PUBLIC IMGUI_ENABLE_TEST_ENGINE)
    target_link_libraries(imgui_core PUBLIC imgui_test_engine)
endif()
include(FetchJson)
include(FetchStb)
include(FetchQoi)
if(SPICKZETTEL_BUILD_TESTS)
    include(FetchGoogleTest)
    enable_testing()
endif()

# generated/build_config.h, about_text.h, notices_text.h and the per-build
# git_stamp.h - see cmake/BuildInfo.cmake. After the options it reads.
include(BuildInfo)
spickzettel_configure_build_info()

# Every word the app shows, compiled in from one editable file - see
# cmake/UiStrings.cmake. A configure dependency like the About text: editing
# what the app says is editing a source file.
include(UiStrings)
spickzettel_generate_ui_strings(
    "${CMAKE_SOURCE_DIR}/assets/ui_strings.json"
    "${CMAKE_BINARY_DIR}/generated/ui_strings.h"
    sz::strings
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/ui_strings.json")

add_subdirectory(src/platform)
add_subdirectory(src/core)
add_subdirectory(src/ui)
add_subdirectory(src/app)

if(WIN32)
    add_subdirectory(src/platform/win32)
    add_subdirectory(src/app_main)
endif()

if(SPICKZETTEL_BUILD_TESTS)
    add_subdirectory(tests)
    # Developer tools that are never shipped. Tied to the tests option: they
    # exist for the same reason the tests do.
    add_subdirectory(tools/perf_library)
endif()
