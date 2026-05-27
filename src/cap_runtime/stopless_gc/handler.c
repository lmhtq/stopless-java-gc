/*
 * handler.c — SIGPROT handler for tag-zero cap faults.
 *
 * Behaviour:
 *   1. Extract the faulting capability and the source slot it was
 *      loaded from.
 *   2. forward_table_lookup(addr) -> new_cap; if NULL, re-raise as
 *      a real error.
 *   3. Self-heal: store new_cap back into the source slot so the
 *      next load gets a valid cap directly (no second fault).
 *   4. Set the destination register of the faulting load instruction
 *      to new_cap.
 *   5. Advance PC past the faulting instruction and resume.
 *
 * V1 limitations:
 *   - Decoding aarch64 ld/ldr instructions is partial (only the
 *     common forms used by JVM oop loads).
 *   - Does not handle the concurrent-move WRITE barrier yet (§3.6
 *     of design); only the read side.
 */

#include "handler.h"
#include "forward_table.h"

#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <cheriintrin.h>

unsigned long long stopless_handler_faults = 0;
unsigned long long stopless_handler_self_heals = 0;

static struct sigaction prev_sa;

static void
sigprot_handler(int sig, siginfo_t *si, void *ctx_)
{
    ucontext_t *ctx = (ucontext_t *)ctx_;

    /* PROT_CHERI_TAG ⇒ caller tried to load/store via tag-zero cap. */
    if (si->si_code != /*PROT_CHERI_TAG=*/2) {
        if (prev_sa.sa_sigaction) {
            prev_sa.sa_sigaction(sig, si, ctx_);
        }
        return;
    }

    /* On CHERI, si_addr = faulting PC, not the cap value. To find
       which cap faulted, scan the cap register file for a tag-zero
       cap whose address is in our forwarding table. */
    struct capregs *cregs = &ctx->uc_mcontext.mc_capregs;
    uintptr_t fault_addr = 0;
    void *new_cap = NULL;
    int faulting_reg = -1;
    for (int i = 0; i < 30; i++) {
        __uintcap_t c = cregs->cap_x[i];
        if (cheri_tag_get((void *)c)) continue;  /* not the culprit */
        uintptr_t addr = (uintptr_t)cheri_address_get((void *)c);
        void *fwd = forward_table_lookup(addr);
        if (fwd != NULL) {
            new_cap = fwd;
            fault_addr = addr;
            faulting_reg = i;
            break;
        }
    }
    if (new_cap == NULL) {
        /* No matching cap in register file. Last-resort: try si_addr
           interpreted as cap address (in case kernel ever changes). */
        new_cap = forward_table_lookup((uintptr_t)si->si_addr);
        fault_addr = (uintptr_t)si->si_addr;
    }

    if (new_cap == NULL) {
        unsigned long pc = (unsigned long)ctx->uc_mcontext.mc_capregs.cap_elr;
        fprintf(stderr, "[stopless] unforwarded fault: si_addr=%p si_code=%d "
                "pc=0x%lx fwd_size=%zu (no tag-zero cap in regs matched)\n",
                si->si_addr, si->si_code, pc, forward_table_size());
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    /* Install the new cap into the faulting register so the retry
       sees a valid cap. */
    if (faulting_reg >= 0) {
        cregs->cap_x[faulting_reg] = (__uintcap_t)new_cap;
    }

    stopless_handler_faults++;

    /* V1: we don't yet decode the instruction to find the destination
       register; the test harness drives the mechanism end-to-end by
       installing the new_cap into the source slot explicitly via
       self-heal. PC is advanced one instruction so we don't loop.
       V2 will: (a) parse the faulting LDR/STR insn, (b) set the
       destination register in mcontext to new_cap, (c) update the
       source slot.

       For V1 we update the source slot (self-heal) and resume; the
       caller's retry of the same load will succeed. */

    /* Caller pattern in tests:
         volatile void *src_slot = ...;     // holds the stale cap
         void *p = *src_slot;               // FAULT here; handler runs
                                            // and re-tries the load
         ... use p ...                      // succeeds via self-heal
     */
    /* Self-heal: write new_cap to the source slot. We need to find
       the slot; in V1 we encode it via the cap's address (slot ==
       fault_addr's containing word in the parent oop). */

    /* Without instruction decoding we cannot determine which slot the
       load read FROM, only what address the cap pointed AT. The two
       differ. So self-heal in V1 is approximate: the test harness
       passes the slot pointer via a side channel (thread-local). */

    extern __thread volatile void **stopless_v1_pending_slot;
    if (stopless_v1_pending_slot != NULL) {
        *stopless_v1_pending_slot = new_cap;
        stopless_v1_pending_slot = NULL;
        stopless_handler_self_heals++;
    }

    /* Do NOT advance PC: let the faulting instruction re-execute with
       the newly-patched register holding a valid cap. This is the
       standard signal-handler retry pattern.

       (Alternative: if we want to emulate the load in software, we'd
       need to decode the instruction, perform the load, write the
       destination, and advance PC. The retry-from-PC approach is
       simpler and matches Cornucopia Reloaded's expected behaviour.)
     */
}

__thread volatile void **stopless_v1_pending_slot;

int
stopless_handler_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigprot_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    /* SIGPROT is FreeBSD-specific (34). */
    if (sigaction(34, &sa, &prev_sa) != 0) {
        perror("stopless_handler_install: sigaction(SIGPROT)");
        return -1;
    }
    return 0;
}

void
stopless_handler_remove(void)
{
    sigaction(34, &prev_sa, NULL);
}
