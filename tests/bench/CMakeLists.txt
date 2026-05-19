# tests/bench — microbenchmarks built standalone (no JDK / no gtest).

add_executable(microbench_pointer_chase
    microbench_pointer_chase.cc
)

target_compile_options(microbench_pointer_chase PRIVATE
    -O3 -Wall -Wextra
)
