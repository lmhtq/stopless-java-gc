// tests/bench/microbench_pointer_chase.cc — pointer-chase microbenchmark
// used to isolate per-load barrier cost. Compiled standalone (no JDK
// involvement) so we can compare:
//
//   (a) raw aarch64 load: lower bound on memory latency
//   (b) ZGC-equivalent software color check inserted by hand
//   (c) CHERI cap-load on Morello (Phase 2 only)
//
// This is a *companion* to in-JVM benchmarks: it isolates the per-load
// cost without GC noise. In-JVM perf for DaCapo/Renaissance is captured
// by scripts/run_benchmarks.sh.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <vector>

namespace {

constexpr size_t kCount     = 1 << 24;   // 16M nodes
constexpr size_t kIters     = 1 << 24;   // 16M chases per run

struct Node {
    Node* next;
    char  pad[56];                       // 64B / cacheline
};

// Build a randomly-permuted singly-linked list of `count` nodes so the
// chase defeats hardware prefetch.
Node* build_list(std::vector<Node>& storage) {
    size_t count = storage.size();
    std::vector<size_t> perm(count);
    for (size_t i = 0; i < count; ++i) perm[i] = i;
    for (size_t i = count - 1; i > 0; --i) {
        size_t j = static_cast<size_t>(std::rand()) % (i + 1);
        std::swap(perm[i], perm[j]);
    }
    for (size_t i = 0; i + 1 < count; ++i) {
        storage[perm[i]].next = &storage[perm[i + 1]];
    }
    storage[perm[count - 1]].next = &storage[perm[0]];
    return &storage[perm[0]];
}

uint64_t chase_raw(Node* head, size_t iters) {
    Node* p = head;
    for (size_t i = 0; i < iters; ++i) p = p->next;
    return reinterpret_cast<uintptr_t>(p);     // prevent optimization
}

// Simulated ZGC software barrier: AND against a bad mask, branch to a
// "fixup" path. The fixup is never taken in this benchmark but we still
// pay the AND + CBNZ + speculation overhead.
uint64_t chase_zgc_sim(Node* head, size_t iters, uintptr_t bad_mask) {
    Node* p = head;
    for (size_t i = 0; i < iters; ++i) {
        uintptr_t raw = reinterpret_cast<uintptr_t>(p->next);
        if (__builtin_expect((raw & bad_mask) != 0, 0)) {
            std::abort();                       // unreachable in this bench
        }
        p = reinterpret_cast<Node*>(raw);
    }
    return reinterpret_cast<uintptr_t>(p);
}

template <typename Fn>
double measure_ns_per_load(Fn fn, size_t iters) {
    auto t0 = std::chrono::high_resolution_clock::now();
    auto sink = fn();
    auto t1 = std::chrono::high_resolution_clock::now();
    std::fprintf(stderr, "  sink=%p\n", reinterpret_cast<void*>(sink));
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return double(ns) / double(iters);
}

}  // namespace

int main() {
    std::vector<Node> storage(kCount);
    Node* head = build_list(storage);

    std::printf("microbench: %zu nodes, %zu chases per run\n",
                kCount, kIters);

    double ns_raw = measure_ns_per_load(
        [&] { return chase_raw(head, kIters); }, kIters);
    std::printf("  raw load          : %.2f ns/load\n", ns_raw);

    double ns_sim = measure_ns_per_load(
        [&] { return chase_zgc_sim(head, kIters, 0xFFFE000000000000ULL); }, kIters);
    std::printf("  zgc-software-sim  : %.2f ns/load\n", ns_sim);

    std::printf("  delta             : %.2f ns/load\n", ns_sim - ns_raw);
    return 0;
}
