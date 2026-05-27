/*
 * revoke.c — implementation of the cheri_revoke / shadow-bitmap wrappers.
 */

#include "revoke.h"

#include <sys/mman.h>
#include <cheri/revoke.h>
#include <cheri/libcaprevoke.h>
#include <cheriintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>

/* Process-wide CHERI revocation info struct. Lazily acquired from the
   kernel via cheri_revoke_get_shadow(SHADOW_INFO_STRUCT). Holds the
   `base_mem_nomap` ptraddr that all caprev_shadow_nomap_* calls need. */
static volatile const struct cheri_revoke_info *g_cri;

static int
ensure_cri(void)
{
    if (g_cri != NULL) return 0;
    void *p = NULL;
    int rc = cheri_revoke_get_shadow(CHERI_REVOKE_SHADOW_INFO_STRUCT, NULL, &p);
    if (rc != 0) {
        fprintf(stderr, "ensure_cri: cheri_revoke_get_shadow(INFO) failed: %s\n",
                strerror(errno));
        return -1;
    }
    g_cri = (const struct cheri_revoke_info *)p;
    return 0;
}

int
stopless_arena_init(stopless_arena_t *out, size_t size)
{
    if (out == NULL || size == 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    /* Map the arena. PROT_MAX wired wide so we can later mprotect for
       write barriers (§3.6 of design). */
    void *p = mmap(NULL, size,
                   PROT_READ | PROT_WRITE | PROT_MAX(PROT_READ | PROT_WRITE),
                   MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "stopless_arena_init: mmap failed: %s\n", strerror(errno));
        return -1;
    }
    out->base = p;
    out->size = size;

    if (ensure_cri() != 0) {
        munmap(p, size);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    out->shadow_base_addr = g_cri->base_mem_nomap;

    /* Acquire the shadow bitmap for this arena. */
    int rc = cheri_revoke_get_shadow(CHERI_REVOKE_SHADOW_NOVMEM,
                                     p, (void **)&out->shadow);
    if (rc != 0) {
        fprintf(stderr, "stopless_arena_init: cheri_revoke_get_shadow failed: %s\n",
                strerror(errno));
        munmap(p, size);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

void
stopless_arena_fini(stopless_arena_t *a)
{
    if (a == NULL || a->base == NULL) return;
    munmap(a->base, a->size);
    /* Shadow bitmap is kernel-owned; nothing to free. */
    memset(a, 0, sizeof(*a));
}

void
stopless_mark_revoke(stopless_arena_t *a, uintptr_t obj_addr, size_t len)
{
    if (a == NULL || a->shadow == NULL || len == 0) return;
    /* Use the raw variant (no user_obj safety hook). Matches the
       libmalloc_simple pattern: caprev_shadow_nomap_set_raw(sbase, sb,
       (ptraddr_t)addr, size). */
    caprev_shadow_nomap_set_raw(a->shadow_base_addr, a->shadow,
                                obj_addr, len);
}

void
stopless_unmark_revoke(stopless_arena_t *a, uintptr_t obj_addr, size_t len)
{
    if (a == NULL || a->shadow == NULL || len == 0) return;
    caprev_shadow_nomap_clear_raw(a->shadow_base_addr, a->shadow,
                                  obj_addr, len);
}

int
stopless_revoke_now(void)
{
    if (ensure_cri() != 0) return -1;

    /* Run a full revocation pass. Follow the libc tls_malloc pattern:
       sample current enqueue epoch, then call LAST_PASS until dequeue
       reaches that epoch. */
    atomic_thread_fence(memory_order_acq_rel);
    cheri_revoke_epoch_t start_epoch = g_cri->epochs.enqueue;
    while (!cheri_revoke_epoch_clears(g_cri->epochs.dequeue, start_epoch)) {
        int rc = cheri_revoke(CHERI_REVOKE_LAST_PASS, start_epoch, NULL);
        if (rc != 0) {
            fprintf(stderr, "stopless_revoke_now: cheri_revoke failed: %s\n",
                    strerror(errno));
            return rc;
        }
    }
    return 0;
}
