# Dear ImGui's own test engine, which is what lets a test click a widget by
# name instead of by guessed screen coordinates - and, just as usefully,
# fail when a widget is present but *unreachable*, which is the shape of
# every z-order bug this UI has had.
#
# Off by default and on only in the debug preset: switching it on defines
# IMGUI_ENABLE_TEST_ENGINE for ImGui itself, which makes ImGui reference the
# engine's hook symbols, which every consumer then has to link. That is fine
# for a build nobody ships and wrong for one that is.
include(FetchContent)

FetchContent_Declare(
    imgui_test_engine
    GIT_REPOSITORY https://github.com/ocornut/imgui_test_engine.git
    GIT_TAG v1.92.1
)
FetchContent_MakeAvailable(imgui_test_engine)

add_library(imgui_test_engine STATIC
    ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_context.cpp
    ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_coroutine.cpp
    ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_engine.cpp
    ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_exporters.cpp
    ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_perftool.cpp
    # imgui_te_ui.cpp is deliberately absent. It draws the engine's own
    # interactive test browser, which a unit-test binary has nobody to show
    # it to - and it reaches into ImGui internals (ImGuiContext::
    # DebugHookIdInfo) that this pinned ImGui no longer has, since the
    # engine's releases trail ImGui's by a few versions.
    ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_utils.cpp
    ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_capture_tool.cpp
)
# Headers only, not the imgui_core *target*: imgui_core links this one back
# (it needs the hook symbols), and naming each other as targets would be a
# cycle. Both end up in the same final link either way.
target_include_directories(imgui_test_engine PUBLIC
    ${imgui_test_engine_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
)
# std::function test bodies, so a test can capture the fixture it belongs
# to instead of smuggling it through a void*.
target_compile_definitions(imgui_test_engine PUBLIC IMGUI_ENABLE_TEST_ENGINE
                            IMGUI_TEST_ENGINE_ENABLE_STD_FUNCTION=1
                            # The engine runs a test body on its own
                            # coroutine so it can yield a frame mid-test;
                            # std::thread is its portable implementation and
                            # has to be asked for explicitly.
                            IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1)
target_compile_features(imgui_test_engine PUBLIC cxx_std_17)
