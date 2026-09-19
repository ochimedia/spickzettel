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
# Off by default because it defines IMGUI_ENABLE_TEST_ENGINE for ImGui
# itself, which every consumer then has to link. On in the debug preset.
option(SPICKZETTEL_UI_TESTS "Build the UI tests, which drive real widgets by name" OFF)

# Warnings are per target rather than global so that fetched third-party
# code is compiled with its own defaults.
function(spickzettel_set_warnings TARGET)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /W4)
    else()
        target_compile_options(${TARGET} PRIVATE -Wall -Wextra)
    endif()
endfunction()
