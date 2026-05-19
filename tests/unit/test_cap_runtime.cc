// test_cap_runtime.cc — smoke tests for the public C ABI.
//
// These run on the host (no CHERI). On Morello builds the same tests
// exercise the same ABI; the underlying types switch to cap_t but the
// observable behavior is the same.

#include "cap_runtime.h"

#include <gtest/gtest.h>

TEST(CapRuntime, AbiVersionIsExpected) {
    EXPECT_EQ(stopless_cap_runtime_abi_version(),
              STOPLESS_CAP_RUNTIME_ABI_VERSION);
}

TEST(CapRuntime, HeapCreateAndDestroy) {
    stopless_heap_t* h = stopless_heap_create(1 << 20);  // 1 MiB
    ASSERT_NE(h, nullptr);
    stopless_heap_destroy(h);
}

TEST(CapRuntime, HeapCreateZeroBytesReturnsNull) {
    EXPECT_EQ(stopless_heap_create(0), nullptr);
}

TEST(CapRuntime, DestroyNullIsSafe) {
    stopless_heap_destroy(nullptr);
    SUCCEED();
}
