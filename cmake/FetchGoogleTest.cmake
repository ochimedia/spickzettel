include(FetchContent)

FetchContent_Declare(
    googletest
    # v1.15.2
    URL https://github.com/google/googletest/archive/b514bdc898e2951020cbdca1304b75f5950d1f59.tar.gz
    URL_HASH SHA256=9257316d65d6259f6596bc4fa5464092305ab3f3d9dc6d7e5780ab462d8baa64
)
# Match the project's runtime library on MSVC, or the test binaries fail to
# link: the static one (see CMAKE_MSVC_RUNTIME_LIBRARY), which is what
# googletest picks by itself when not forced onto the shared one.
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
