# The interface everything above the platform is written against: headers
# only, no OS dependency. The backends implement it - see win32/.
add_library(sz_platform_iface INTERFACE)
target_include_directories(sz_platform_iface INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(sz_platform_iface INTERFACE cxx_std_20)
