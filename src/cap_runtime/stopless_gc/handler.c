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
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>
#include <cheriintrin.h>

unsigned long long stopless_handler_faults = 0;
unsigned long long stopless_handler_self_heals = 0;

/* C-9/C-10: mutator-assisted evacuation hook. When a tag-0 cap faults whose
   address is NOT yet in the forwarding table, the handler calls this (if
   registered) to evacuate the object on the spot: copy it, cas_insert the
   forwarding, and return the new cap (or NULL if the address is not a live
   GC object = a genuine fault). Lets a mutator make progress without waiting
   on the collector (NS invariant). */
typedef void *(*stopless_evacuate_fn)(uintptr_t from_addr);
static stopless_evacuate_fn g_evacuate_cb = NULL;
void stopless_set_evacuate_cb(stopless_evacuate_fn cb) { g_evacuate_cb = cb; }
unsigned long long stopless_handler_assists = 0;

static struct sigaction prev_sa;

/* C-6 L38: HotSpot's SafeFetch probes (os::is_readable_pointer etc.)
   deliberately load through a possibly-bad pointer and rely on the signal
   handler to redirect the PC to a continuation that returns a default.
   The stock handle_safefetch only recognises SIGSEGV/SIGBUS, but on CHERI
   the bad load faults with SIGPROT (PROT_CHERI_TAG/BOUNDS). These labels
   are defined in libjvm.so's safefetch_bsd_aarch64.S; cap_runtime is
   linked into the same DSO so the references resolve at link time. */
/* C-7: weak so this same .a links into BOTH the template-interpreter libjvm
   (where safefetch_bsd_aarch64.S defines them) and the Zero libjvm (os_cpu/
   bsd_zero has no such stub — the symbols stay NULL, the redirect is skipped,
   and Zero does SafeFetch in C++). */
extern char _SafeFetch32_fault[] __attribute__((weak));
extern char _SafeFetch32_continuation[] __attribute__((weak));
extern char _SafeFetchN_fault[] __attribute__((weak));
extern char _SafeFetchN_continuation[] __attribute__((weak));

static void
sigprot_handler(int sig, siginfo_t *si, void *ctx_)
{
    ucontext_t *ctx = (ucontext_t *)ctx_;

    /* C-6 L38: SafeFetch redirect FIRST (before any diag/abort) — these
       faults are expected and frequent. If the faulting PC is a SafeFetch
       load, advance PCC to the matching continuation (which returns the
       caller-supplied default) and resume. Preserve PCC bounds/tag via
       cheri_address_set. */
    {
        __uintcap_t elr = (__uintcap_t)ctx->uc_mcontext.mc_capregs.cap_elr;
        uintptr_t pc = (uintptr_t)cheri_address_get((void *)elr);
        if (_SafeFetch32_fault != NULL && pc == (uintptr_t)(void *)_SafeFetch32_fault) {
            void *cont = cheri_address_set((void *)elr,
                            (uintptr_t)(void *)_SafeFetch32_continuation);
            ctx->uc_mcontext.mc_capregs.cap_elr = (__uintcap_t)cont;
            return;
        }
        if (_SafeFetchN_fault != NULL && pc == (uintptr_t)(void *)_SafeFetchN_fault) {
            void *cont = cheri_address_set((void *)elr,
                            (uintptr_t)(void *)_SafeFetchN_continuation);
            ctx->uc_mcontext.mc_capregs.cap_elr = (__uintcap_t)cont;
            return;
        }
    }

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
        if (fwd == NULL && g_evacuate_cb != NULL) {
            /* C-9/C-10: not yet forwarded — assist by evacuating now. The cb
               copies the object + cas_inserts the forwarding (one winner) and
               returns the authoritative new cap, or NULL if addr isn't a live
               GC object (then this is a genuine fault, handled below). */
            fwd = g_evacuate_cb(addr);
            if (fwd != NULL) stopless_handler_assists++;
        }
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
        /* C-6 layer 10 diag: full cap state including bounds. */
        fprintf(stderr, "[stopless] CRITICAL caps:\n");
        __uintcap_t csp_v = (__uintcap_t)ctx->uc_mcontext.mc_capregs.cap_sp;
        __uintcap_t celr_v = (__uintcap_t)ctx->uc_mcontext.mc_capregs.cap_elr;
        __uintcap_t clr_v = (__uintcap_t)ctx->uc_mcontext.mc_capregs.cap_lr;
        fprintf(stderr, "  csp:  tag=%d addr=%#lx base=%#lx top=%#lx perms=%#lx flags=%d\n",
                (int)cheri_tag_get((void *)csp_v),
                (unsigned long)cheri_address_get((void *)csp_v),
                (unsigned long)cheri_base_get((void *)csp_v),
                (unsigned long)(cheri_base_get((void *)csp_v) + cheri_length_get((void *)csp_v)),
                (unsigned long)cheri_perms_get((void *)csp_v),
                (int)cheri_flags_get((void *)csp_v));
        fprintf(stderr, "  PCC:  tag=%d addr=%#lx base=%#lx top=%#lx perms=%#lx flags=%d\n",
                (int)cheri_tag_get((void *)celr_v),
                (unsigned long)cheri_address_get((void *)celr_v),
                (unsigned long)cheri_base_get((void *)celr_v),
                (unsigned long)(cheri_base_get((void *)celr_v) + cheri_length_get((void *)celr_v)),
                (unsigned long)cheri_perms_get((void *)celr_v),
                (int)cheri_flags_get((void *)celr_v));
        fprintf(stderr, "  clr:  tag=%d addr=%#lx base=%#lx top=%#lx perms=%#lx flags=%d\n",
                (int)cheri_tag_get((void *)clr_v),
                (unsigned long)cheri_address_get((void *)clr_v),
                (unsigned long)cheri_base_get((void *)clr_v),
                (unsigned long)(cheri_base_get((void *)clr_v) + cheri_length_get((void *)clr_v)),
                (unsigned long)cheri_perms_get((void *)clr_v),
                (int)cheri_flags_get((void *)clr_v));
        fprintf(stderr, "  -- cheri_flags: bit0=C64-mode (1=C64, 0=A64) --\n");
        fflush(stderr);
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

/* C-6 fix #11 diag: also catch SIGILL so we see WHERE the illegal
   instruction faults inside call_stub. */
static void
sigill_handler(int sig, siginfo_t *si, void *ctx_)
{
    ucontext_t *ctx = (ucontext_t *)ctx_;
    __uintcap_t celr = (__uintcap_t)ctx->uc_mcontext.mc_capregs.cap_elr;
    void *pc_cap = (void *)celr;
    unsigned long pc = (unsigned long)cheri_address_get(pc_cap);
    fprintf(stderr, "[stopless] SIGILL: si_addr=%p pc=0x%lx flags=%d\n",
            si->si_addr, pc, (int)cheri_flags_get(pc_cap));
    /* Print 4 instructions around the faulting PC. */
    uint32_t* insn = (uint32_t*)pc_cap;
    for (int i = -1; i <= 2; i++) {
        fprintf(stderr, "  pc%+d (0x%lx): 0x%08x\n",
                i, pc + i*4, insn[i]);
    }
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

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
    /* C-6 L36: the diagnostic SIGILL/SIGSEGV handlers (which read raw
       instruction bytes around the faulting PC) themselves take nested
       capability faults on CHERI and obscure the real fault. Leave SIGILL
       and SIGSEGV to default handling so the original fault surfaces
       cleanly (and gdb can catch it at the true PC). The
       sigill_handler function is retained but no longer installed. */
    (void)sigill_handler;
    fflush(stderr);
    return 0;
}

void
stopless_handler_remove(void)
{
    sigaction(34, &prev_sa, NULL);
}
