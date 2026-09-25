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

project(spickzettel VERSION ${SPICKZETTEL_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The C++ runtime linked in rather than loaded from MSVCP140.dll and
# VCRUNTIME140*.dll: a machine without the Visual C++ Redistributable
# cannot start the exe at all, and one with an older copy of it can crash
# a binary built with a newer toolset. Before any target is made, the
# fetched ones included, so that everything links against the same one.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# A release build with symbols, so that a crash dump from someone else's
# machine can be read (see win32_crash_dump.h). /Z7 keeps the debug info
# in the objects, so parallel compiles share no PDB; /DEBUG turns off
# /OPT:REF and /OPT:ICF, which are turned back on so the exe is what it was;
# /PDBALTPATH names the PDB without the path of the machine that built it.
# Everything from here on, the fetched code included - a stack through
# ImGui is read with ImGui's symbols.
if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/Z7>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>
                     $<$<CONFIG:Release>:/PDBALTPATH:%_PDB%>)
endif()

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
# What the exe is called - "Spickzettel", or "Spickzettel Demo" for the
# demo build - so that builds of different kinds can sit side by side in
# dist/ and a person can tell which one they are running. The PDB takes
# the same name, which is the name the exe records for it.
set(SPICKZETTEL_EXE_NAME "Spickzettel" CACHE STRING "The exe's file name, without .exe")
# Whether the finished exe is also copied to dist/ at the repo root, next
# to the other presets' exes instead of deep in the build tree, and its
# PDB to dist/symbols/. On in the presets whose build is handed out.
option(SPICKZETTEL_COPY_TO_DIST "Copy the exe to dist/ and its PDB to dist/symbols/ after each build" OFF)

# Warnings are per target rather than global so that fetched third-party
# code is compiled with its own defaults.
function(spickzettel_set_warnings TARGET)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /W4)
    else()
        target_compile_options(${TARGET} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Every executable runs with UTF-8 as its ANSI code page - the tests too, so
# they meet paths the way the app does. See utf8.manifest.
function(spickzettel_use_utf8_code_page TARGET)
    if(WIN32)
        target_sources(${TARGET} PRIVATE "${CMAKE_SOURCE_DIR}/src/platform/win32/resources/utf8.manifest")
    endif()
endfunction()

# Third-party code, all fetched and pinned - see docs/ARCHITECTURE.md.
#
# Each is an archive checked against its SHA-256. An extracted file gets
# the time it was extracted, not the one stored in the archive, so that a
# new archive for a dependency rebuilds what uses it; CMake 3.24 and later
# warn until told which (CMP0135), and older ones only ever do this.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
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
include(FetchQoi)
include(FetchSqlite)
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
