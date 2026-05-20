/* hello.c — minimal CHERI purecap smoke test.
 *
 * Compiled with the Morello SDK clang in purecap mode this program exercises:
 *   - capability-typed pointers (printf's format string and the literal)
 *   - capability load (reading argv[0])
 *   - capability bounds + permissions (the literal lives in .rodata; the
 *     loader sets up a tight cap for it)
 *
 * If this builds and links cleanly we know our Morello SDK is producing
 * valid aarch64-purecap ELFs. Running it inside the FVP gives us
 * cap-aware loader + libc smoke as a side-effect.
 */

#include <stdio.h>

#if defined(__CHERI_PURE_CAPABILITY__)
#include <cheriintrin.h>
static void print_argv0_cap(const char* argv0) {
    void* __capability cap = (void* __capability)argv0;
    printf("argv0 cap: base=%#zx len=%#zx perms=%#zx tag=%d\n",
           (size_t)cheri_base_get(cap),
           (size_t)cheri_length_get(cap),
           (size_t)cheri_perms_get(cap),
           (int)cheri_tag_get(cap));
}
#else
static void print_argv0_cap(const char* argv0) {
    printf("(not purecap) argv0=%s\n", argv0);
}
#endif

int main(int argc, char* argv[]) {
    printf("hello CHERI from stopless-java-gc smoke test\n");
    if (argc > 0 && argv[0] != NULL) {
        print_argv0_cap(argv[0]);
    }
    return 0;
}
