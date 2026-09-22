# Everything baked into the binary at build time: the version, the
# compile-time feature flags, the git description, and the embedded About
# and third-party-notices text. All of it lands in
# ${CMAKE_BINARY_DIR}/generated/ and is included as "generated/<name>.h";
# the include root is CMAKE_BINARY_DIR, which sz_core exports.
include(EmbedFile)

function(spickzettel_configure_build_info)
    set(_generated_dir "${CMAKE_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_generated_dir}")

    # Configure time: the version and the flags only change when cmake is
    # re-run, which is exactly when they can change.
    if(SPICKZETTEL_DEMO_MODE)
        set(SPICKZETTEL_DEMO_MODE_CXX "true")
    else()
        set(SPICKZETTEL_DEMO_MODE_CXX "false")
    endif()
    if(SPICKZETTEL_PRERELEASE_NOTICE)
        set(SPICKZETTEL_PRERELEASE_NOTICE_CXX "true")
    else()
        set(SPICKZETTEL_PRERELEASE_NOTICE_CXX "false")
    endif()
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/core/build_info/build_config.h.in"
        "${_generated_dir}/build_config.h"
        @ONLY
    )

    # Configure time as well, and listed as configure dependencies so that
    # editing either file re-runs cmake by itself.
    spickzettel_embed_text_file(
        "${CMAKE_SOURCE_DIR}/ABOUT.md"
        "${_generated_dir}/about_text.h"
        kAboutText
        sz::core::build::generated
    )
    spickzettel_embed_text_file(
        "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
        "${_generated_dir}/notices_text.h"
        kNoticesText
        sz::core::build::generated
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/ABOUT.md"
        "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
        "${CMAKE_SOURCE_DIR}/VERSION"
    )

    # Once now, so the header exists before anything compiles against it...
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DGIT_SRC_DIR=${CMAKE_SOURCE_DIR}"
        "-DOUT_FILE=${_generated_dir}/git_stamp.h"
        -P "${CMAKE_SOURCE_DIR}/cmake/GitStamp.cmake")
    # ...and before every build, which is what keeps it current. sz_core
    # depends on this target so it always runs first.
    add_custom_target(sz_git_stamp ALL
        COMMAND "${CMAKE_COMMAND}"
                "-DGIT_SRC_DIR=${CMAKE_SOURCE_DIR}"
                "-DOUT_FILE=${_generated_dir}/git_stamp.h"
                -P "${CMAKE_SOURCE_DIR}/cmake/GitStamp.cmake"
        BYPRODUCTS "${_generated_dir}/git_stamp.h"
        COMMENT "Stamping git description into generated/git_stamp.h"
        VERBATIM
    )
endfunction()
