include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59 # v1.15.2
)
# Match the project's runtime library on MSVC, or the test binaries fail to
# link: the static one (see CMAKE_MSVC_RUNTIME_LIBRARY), which is what
# googletest picks by itself when not forced onto the shared one.
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
