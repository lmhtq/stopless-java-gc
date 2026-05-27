/*
 * test_basic.c — end-to-end exercise of CHERI-Stopless primitives.
 *
 * Scenario:
 *   1. Create arena A of 1 MiB.
 *   2. Allocate object O (256 bytes) at A[0..256].
 *   3. Take cap c_old pointing at O.
 *   4. Allocate destination region D of 1 MiB.
 *   5. memcpy O's contents into D[0..256] -> c_new.
 *   6. Insert forwarding c_old.addr -> c_new.
 *   7. Mark O's range for revocation in A's shadow bitmap.
 *   8. Trigger cheri_revoke. c_old is now tag-zero.
 *   9. Set stopless_v1_pending_slot = &c_old.
 *  10. Load via c_old -> SIGPROT -> handler -> self-heal slot ->
 *      retry -> success.
 *  11. Verify the load got the same byte value from the new location.
 */

#include "../api.h"
#include "../revoke.h"
#include "../forward_table.h"
#include "../handler.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cheri/cherireg.h>
#include <cheriintrin.h>

extern __thread volatile void **stopless_v1_pending_slot;

int
main(void)
{
    write(2, "step 1: init\n", 13);
    forward_table_init();
    if (stopless_handler_install() != 0) {
        write(2, "FAIL: handler install\n", 22);
        return 1;
    }
    write(2, "step 2: arena_init src\n", 23);

    /* Two arenas (source + destination). */
    stopless_arena_t a_src, a_dst;
    if (stopless_arena_init(&a_src, 1 << 20) != 0) return 1;
    write(2, "step 3: arena_init dst\n", 23);
    if (stopless_arena_init(&a_dst, 1 << 20) != 0) return 1;
    write(2, "step 4: allocate obj\n", 21);

    /* Carve an object out of a_src. The mmap-returned cap carries
       CHERI_PERM_SW_VMEM, which marks it as "VM allocator metadata"
       and EXEMPTS it from kernel revocation sweep. We must strip
       that permission so the object cap is subject to revocation. */
    const size_t obj_size = 256;
    char *obj_old_init = (char *)cheri_bounds_set((char *)a_src.base, obj_size);
    obj_old_init = (char *)cheri_perms_and(obj_old_init, ~CHERI_PERM_SW_VMEM);
    memset(obj_old_init, 0xAB, obj_size);

    /* Move: copy into a_dst, get new cap. */
    char *obj_new = (char *)cheri_bounds_set((char *)a_dst.base, obj_size);
    obj_new = (char *)cheri_perms_and(obj_new, ~CHERI_PERM_SW_VMEM);
    memcpy(obj_new, obj_old_init, obj_size);

    uintptr_t old_addr = (uintptr_t)cheri_address_get(obj_old_init);
    forward_table_insert(old_addr, obj_new);

    /* Store obj_old in a HEAP slot (mmap'd separately, not in either
       arena). The kernel revocation sweep walks heap memory and will
       clear the tag here when we revoke a_src's object range. */
    char **obj_old_slot =
        mmap(NULL, sysconf(_SC_PAGESIZE),
             PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_PRIVATE, -1, 0);
    if (obj_old_slot == MAP_FAILED) return 1;
    *obj_old_slot = obj_old_init;
    fprintf(stderr, "DBG slot[0] tag before revoke = %d\n",
            (int)cheri_tag_get(*obj_old_slot));

    fprintf(stderr, "DBG arena_src.base=%#p obj_old=%#p old_addr=0x%lx obj_new=%#p slot=%#p\n",
            a_src.base, (void *)obj_old_init, (unsigned long)old_addr,
            (void *)obj_new, (void *)obj_old_slot);

    /* Mark for revocation + sweep. */
    write(2, "step 5: mark_revoke (cap-based)\n", 32);
    int mrk = stopless_mark_revoke_cap(&a_src, obj_old_init);
    fprintf(stderr, "DBG mark_revoke_cap rc=%d\n", mrk);
    write(2, "step 6: revoke_now\n", 19);
    if (stopless_revoke_now() != 0) {
        write(2, "FAIL: revoke_now\n", 17);
        return 1;
    }
    write(2, "step 7: after revoke\n", 21);
    char *obj_old = *obj_old_slot;  /* re-read from heap slot */
    int tag_slot = (int)cheri_tag_get(*obj_old_slot);
    int tag_loc  = (int)cheri_tag_get(obj_old_init);  /* local register copy */
    int tag = (int)cheri_tag_get(obj_old);
    fprintf(stderr, "DBG after revoke: tag(slot)=%d tag(local)=%d tag(reread)=%d\n",
            tag_slot, tag_loc, tag);
    write(2, tag ? "obj_old still tagged\n" : "obj_old tag cleared\n", 21);

    /* Now obj_old should be tag-zero. Verify. */
    if (cheri_tag_get(obj_old) != 0) {
        fprintf(stderr, "FAIL: obj_old still tagged after revoke\n");
        return 1;
    }

    /* Set up the self-heal slot side-channel + perform a load. */
    volatile void *slot = obj_old;  /* stale cap */
    stopless_v1_pending_slot = (volatile void **)&slot;

    /* Load. This should SIGPROT, handler heals slot to obj_new, PC
       advances past the load. We then verify slot now contains
       obj_new and the value at slot[0] is 0xAB. */
    volatile char *p = (volatile char *)slot;
    /* The next dereference triggers fault: */
    char observed = *p;

    if (observed != (char)0xAB) {
        fprintf(stderr, "FAIL: observed 0x%02x, expected 0xAB\n",
                (unsigned char)observed);
        return 1;
    }
    if (stopless_handler_faults < 1) {
        fprintf(stderr, "FAIL: no fault was handled (faults=%llu)\n",
                stopless_handler_faults);
        return 1;
    }

    printf("OK  faults=%llu self_heals=%llu fwd_size=%zu\n",
           stopless_handler_faults, stopless_handler_self_heals,
           forward_table_size());

    stopless_handler_remove();
    stopless_arena_fini(&a_src);
    stopless_arena_fini(&a_dst);
    return 0;
}
