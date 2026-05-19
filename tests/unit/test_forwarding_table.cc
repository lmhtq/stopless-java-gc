// test_forwarding_table.cc — exercise install/lookup correctness and a
// modest concurrent-lookup race.

#include "cap_runtime.h"

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

stopless_cap_t fake_cap(uintptr_t off) {
    return reinterpret_cast<stopless_cap_t>(off);
}

class ForwardingTest : public ::testing::Test {
protected:
    void SetUp() override {
        heap_ = stopless_heap_create(8 << 20);  // 8 MiB
        ASSERT_NE(heap_, nullptr);
    }
    void TearDown() override {
        stopless_heap_destroy(heap_);
    }
    stopless_heap_t* heap_ = nullptr;
};

TEST_F(ForwardingTest, UninstalledLookupReturnsIdentity) {
    stopless_cap_t c = fake_cap(0x1000);
    EXPECT_EQ(stopless_forwarding_lookup(heap_, c), c);
}

TEST_F(ForwardingTest, InstallThenLookup) {
    stopless_cap_t from = fake_cap(0x2000);
    stopless_cap_t to   = fake_cap(0x9000);
    stopless_forwarding_install(heap_, from, to);
    EXPECT_EQ(stopless_forwarding_lookup(heap_, from), to);
}

TEST_F(ForwardingTest, MultipleDistinctForwardings) {
    for (uintptr_t i = 0; i < 100; ++i) {
        stopless_cap_t f = fake_cap(0x10000 + i * 64);
        stopless_cap_t t = fake_cap(0x80000 + i * 64);
        stopless_forwarding_install(heap_, f, t);
    }
    for (uintptr_t i = 0; i < 100; ++i) {
        stopless_cap_t f = fake_cap(0x10000 + i * 64);
        stopless_cap_t expected = fake_cap(0x80000 + i * 64);
        EXPECT_EQ(stopless_forwarding_lookup(heap_, f), expected)
            << "i=" << i;
    }
}

TEST_F(ForwardingTest, ConcurrentLookupsAreSafe) {
    // Install a handful of forwardings, then race many reader threads.
    constexpr int N = 64;
    for (int i = 0; i < N; ++i) {
        stopless_forwarding_install(heap_,
                                    fake_cap(0x20000 + i * 64),
                                    fake_cap(0xA0000 + i * 64));
    }

    constexpr int THREADS = 8;
    constexpr int ITERS   = 5000;
    std::atomic<int> errors{0};

    auto reader = [&] {
        for (int j = 0; j < ITERS; ++j) {
            int i = j % N;
            stopless_cap_t f = fake_cap(0x20000 + i * 64);
            stopless_cap_t expected = fake_cap(0xA0000 + i * 64);
            if (stopless_forwarding_lookup(heap_, f) != expected) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) threads.emplace_back(reader);
    for (auto& th : threads) th.join();

    EXPECT_EQ(errors.load(), 0);
}

}  // namespace
