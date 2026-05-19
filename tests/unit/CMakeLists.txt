# tests/unit — gtest-based unit tests for src/cap_runtime/.
# Fetched googletest is built locally (no system-wide install required).

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
# Don't override the parent project's compiler/linker settings.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(cap_runtime_tests
    test_cap_runtime.cc
    test_side_table.cc
    test_forwarding_table.cc
)

target_link_libraries(cap_runtime_tests PRIVATE
    stopless_cap_runtime
    GTest::gtest_main
)

target_compile_options(cap_runtime_tests PRIVATE
    -Wall -Wextra -Wpedantic
)

include(GoogleTest)
gtest_discover_tests(cap_runtime_tests)
