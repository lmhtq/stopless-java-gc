# src/measurement — perf-counter sampling helpers used by the JVM (via
# the hook patch) and by the integration tests / benchmark harness.

add_library(stopless_measurement STATIC
    pmu_sample.cc
)

target_include_directories(stopless_measurement PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(stopless_measurement PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
