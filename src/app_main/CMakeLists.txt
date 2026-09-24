# The composition root: the executable, and nothing else.
if(WIN32)
    # app_icon.rc embeds assets/app_icon/spickzettel.ico into the .exe. A
    # resource script has to be compiled into the final executable rather
    # than a static library to end up in the PE resource section.
    add_executable(spickzettel WIN32
        main_win32.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/win32/resources/app_icon.rc
    )
    # Spickzettel.exe, not spickzettel.exe: a proper noun wherever a person
    # sees it (Explorer, the taskbar, Task Manager). OUTPUT_NAME rather than
    # renaming the target, so `spickzettel` stays the lowercase identifier
    # everywhere in the build.
    set_target_properties(spickzettel PROPERTIES OUTPUT_NAME Spickzettel)
    target_compile_features(spickzettel PRIVATE cxx_std_20)
    target_link_libraries(spickzettel PRIVATE sz_app sz_platform_win32)
    spickzettel_set_warnings(spickzettel)
    spickzettel_use_utf8_code_page(spickzettel)

    # See SPICKZETTEL_DIST_NAME. A target of its own rather than a
    # POST_BUILD step, so that it runs on every build - a copy deleted or
    # overwritten by hand comes back without a relink - and copies only
    # when the exe differs.
    if(SPICKZETTEL_DIST_NAME)
        set(_dist_dir "${CMAKE_SOURCE_DIR}/dist/${SPICKZETTEL_DIST_NAME}")
        set(_dist_symbols_dir "${CMAKE_SOURCE_DIR}/dist/symbols/${SPICKZETTEL_DIST_NAME}")
        add_custom_target(spickzettel_dist ALL
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dist_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:spickzettel>" "${_dist_dir}/"
            # Its symbols, kept apart so that the folder can be handed on
            # as it is: they are for reading a crash dump from this very
            # build, and nobody else needs them.
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dist_symbols_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:spickzettel>" "${_dist_symbols_dir}/"
            COMMENT "Copying Spickzettel.exe to dist/${SPICKZETTEL_DIST_NAME}/ and its PDB to dist/symbols/"
            VERBATIM
        )
        add_dependencies(spickzettel_dist spickzettel)
    endif()
endif()
