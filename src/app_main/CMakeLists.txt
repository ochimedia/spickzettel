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
endif()
