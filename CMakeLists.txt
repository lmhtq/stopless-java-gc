# Top-level CMakeLists.txt for stopless-java-gc.
#
# This builds the out-of-tree cap_runtime library and (optionally) its
# gtest-based unit tests for host execution. The library produced here
# is linked into HotSpot at JDK-build time via the
# patches/openjdk-jdk17/0001-cap-runtime-hook.patch hook.
#
# Usage:
#   cmake -S . -B build -DCAP_RUNTIME_BUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build --output-on-failure

cmake_minimum_required(VERSION 3.20)
project(stopless_cap_runtime
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / IDEs

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Detect whether we're building purecap (Morello) or host (x86/aarch64).
# The Morello SDK clang defines __CHERI_PURE_CAPABILITY__.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles([[
    #ifndef __CHERI_PURE_CAPABILITY__
    #error not purecap
    #endif
    int main() { return 0; }
]] STOPLESS_PURECAP)

if(STOPLESS_PURECAP)
    message(STATUS "stopless: building for CHERI purecap")
    add_compile_definitions(STOPLESS_PURECAP=1)
else()
    message(STATUS "stopless: building for host (no CHERI semantics)")
    add_compile_definitions(STOPLESS_PURECAP=0)
endif()

add_subdirectory(src/cap_runtime)
add_subdirectory(src/measurement)

option(CAP_RUNTIME_BUILD_TESTS "Build host-side unit tests for cap_runtime" OFF)
option(CAP_RUNTIME_BUILD_BENCH "Build host-side microbenchmarks"            OFF)
if(CAP_RUNTIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()
if(CAP_RUNTIME_BUILD_BENCH)
    add_subdirectory(tests/bench)
endif()
