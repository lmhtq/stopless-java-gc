# src/cap_runtime — out-of-tree C++ that gets linked into HotSpot via
# patches/openjdk-jdk17/0001-cap-runtime-hook.patch.
#
# Builds as a static library (libstopless_cap_runtime.a) so HotSpot can
# include it without ABI/runtime coupling. Public ABI is C-compatible
# (see cap_runtime.h).

add_library(stopless_cap_runtime STATIC
    cap_runtime.cc
    side_table.cc
    forwarding_table.cc
    revoke_glue.cc
)

target_include_directories(stopless_cap_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_options(stopless_cap_runtime PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

# A small header-only helper for callers that want to assert at
# build time that they linked against the right ABI version.
set_target_properties(stopless_cap_runtime PROPERTIES
    OUTPUT_NAME stopless_cap_runtime
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)
