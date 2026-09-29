# The composition root: the executable, and nothing else.
if(WIN32)
    # app_icon.rc embeds assets/app_icon/spickzettel.ico into the .exe. A
    # resource script has to be compiled into the final executable rather
    # than a static library to end up in the PE resource section.
    # version.rc gives it its version resource, configured from VERSION and
    # the build's kind: the product version names a demo or a prerelease
    # as VersionLine does, and a prerelease or debug build says so in its
    # flags as well.
    set(SPICKZETTEL_VERSION_TEXT "${SPICKZETTEL_VERSION}")
    set(_kinds "")
    if(SPICKZETTEL_DEMO_MODE)
        list(APPEND _kinds "demo")
    endif()
    if(SPICKZETTEL_PRERELEASE_NOTICE)
        list(APPEND _kinds "prerelease")
    endif()
    if(_kinds)
        list(JOIN _kinds ", " _kinds)
        string(APPEND SPICKZETTEL_VERSION_TEXT " - ${_kinds}")
    endif()
    set(_flags "")
    if(SPICKZETTEL_PRERELEASE_NOTICE)
        list(APPEND _flags "VS_FF_PRERELEASE")
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND _flags "VS_FF_DEBUG")
    endif()
    if(_flags)
        list(JOIN _flags "|" SPICKZETTEL_VERSION_FLAGS)
    else()
        set(SPICKZETTEL_VERSION_FLAGS "0")
    endif()
    configure_file(version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)

    add_executable(spickzettel WIN32
        main_win32.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win32/resources/app_icon.rc
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    )
    # Spickzettel.exe, not spickzettel.exe: a proper noun wherever a person
    # sees it (Explorer, the taskbar, Task Manager). OUTPUT_NAME rather than
    # renaming the target, so `spickzettel` stays the lowercase identifier
    # everywhere in the build. See SPICKZETTEL_EXE_NAME.
    set_target_properties(spickzettel PROPERTIES OUTPUT_NAME "${SPICKZETTEL_EXE_NAME}")
    target_compile_features(spickzettel PRIVATE cxx_std_20)
    target_link_libraries(spickzettel PRIVATE sz_app sz_platform_win32)
    spickzettel_set_warnings(spickzettel)
    spickzettel_use_utf8_code_page(spickzettel)

    # See SPICKZETTEL_COPY_TO_DIST. A target of its own rather than a
    # POST_BUILD step, so that it runs on every build - a copy deleted or
    # overwritten by hand comes back without a relink - and copies only
    # when the exe differs.
    if(SPICKZETTEL_COPY_TO_DIST)
        set(_dist_dir "${CMAKE_SOURCE_DIR}/dist")
        set(_dist_symbols_dir "${CMAKE_SOURCE_DIR}/dist/symbols")
        add_custom_target(spickzettel_dist ALL
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dist_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:spickzettel>" "${_dist_dir}/"
            # Its symbols, kept apart so that the folder can be handed on
            # as it is: they are for reading a crash dump from this very
            # build, and nobody else needs them.
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dist_symbols_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:spickzettel>" "${_dist_symbols_dir}/"
            COMMENT "Copying ${SPICKZETTEL_EXE_NAME}.exe to dist/ and its PDB to dist/symbols/"
            VERBATIM
        )
        add_dependencies(spickzettel_dist spickzettel)
    endif()
endif()
