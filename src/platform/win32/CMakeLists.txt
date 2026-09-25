# The Windows backend: the only place <windows.h> and <d3d11.h> are included.
add_library(sz_platform_win32 STATIC
    win32_platform_host.cpp
    win32_displays.cpp
    win32_overlay_window.cpp
    win32_dx11_renderer.cpp
    win32_input_grab.cpp
    win32_crash_dump.cpp
    win32_screen_capture.cpp
    ${IMGUI_BACKENDS_DIR}/imgui_impl_win32.cpp
    ${IMGUI_BACKENDS_DIR}/imgui_impl_dx11.cpp
)

target_include_directories(sz_platform_win32 PUBLIC ${CMAKE_SOURCE_DIR}/src)
# generated/ui_strings.h, for the tray menu's own words.
target_include_directories(sz_platform_win32 PRIVATE ${CMAKE_BINARY_DIR})
target_compile_features(sz_platform_win32 PUBLIC cxx_std_20)
# Deliberately NOT defining UNICODE: this code calls the explicit "A" and
# "W" Win32 functions, and `#ifdef UNICODE` is true as soon as the macro is
# defined at all, which would switch MAKEINTRESOURCE-based constants to
# their wide form and break the "A" calls.
target_compile_definitions(sz_platform_win32 PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(sz_platform_win32
    PUBLIC
        sz_platform_iface
        imgui_core
    PRIVATE
        sz_platform_fonts
        d3d11
        dxgi
        # imgui_impl_dx11.cpp pulls this in through a #pragma comment(lib)
        # that only MSVC honors; listed so any toolchain links it.
        d3dcompiler
        dwmapi
        shcore   # GetDpiForMonitor
        shell32
        user32
        gdi32
        # OpenProcessToken/GetTokenInformation, for the foreground process's
        # integrity level (win32_integrity.h).
        advapi32
        dbghelp  # MiniDumpWriteDump (win32_crash_dump.h)
)
spickzettel_set_warnings(sz_platform_win32)
