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
#include <time.h>
#include <ucontext.h>
#include <cheriintrin.h>
#include <dlfcn.h>

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

/* C-6 L38 / C-9: if the faulting PC is a SafeFetch probe load, advance PCC
   to the matching continuation (which returns the caller-supplied default)
   and resume. Preserve PCC bounds/tag via cheri_address_set. These faults
   are expected and frequent (os::is_readable_pointer, OopStorage::
   block_for_ptr, ...), so EVERY handler that can receive the fault signal
   must try this redirect before treating the fault as fatal. Returns 1 if
   redirected. */
static int
stopless_safefetch_redirect(ucontext_t *ctx)
{
    __uintcap_t elr = (__uintcap_t)ctx->uc_mcontext.mc_capregs.cap_elr;
    uintptr_t pc = (uintptr_t)cheri_address_get((void *)elr);
    if (_SafeFetch32_fault != NULL && pc == (uintptr_t)(void *)_SafeFetch32_fault) {
        void *cont = cheri_address_set((void *)elr,
                        (uintptr_t)(void *)_SafeFetch32_continuation);
        ctx->uc_mcontext.mc_capregs.cap_elr = (__uintcap_t)cont;
        return 1;
    }
    if (_SafeFetchN_fault != NULL && pc == (uintptr_t)(void *)_SafeFetchN_fault) {
        void *cont = cheri_address_set((void *)elr,
                        (uintptr_t)(void *)_SafeFetchN_continuation);
        ctx->uc_mcontext.mc_capregs.cap_elr = (__uintcap_t)cont;
        return 1;
    }
    return 0;
}

static void
sigprot_handler(int sig, siginfo_t *si, void *ctx_)
{
    ucontext_t *ctx = (ucontext_t *)ctx_;

    /* SafeFetch redirect FIRST (before any diag/abort). */
    if (stopless_safefetch_redirect(ctx))
        return;

    /* A GC-revoked cap traps when the mutator loads/stores through it. CheriBSD
       has TWO revocation representations and the kernel build decides which:
         - CHERI_CAPREVOKE_CLEARTAGS defined  -> tag cleared  -> PROT_CHERI_TAG (2)
         - default (CLEARTAGS *undefined*)    -> perms cleared -> PROT_CHERI_PERM (5)
       (see cheri_revoke_is_revoked() in sys/cheri/revoke.h: revoked iff
        tag==0 || perms==0). The default build clears PERMS, so a revoked cap
       keeps tag=1 and faults as PROT_CHERI_PERM. Accept BOTH so the handler
       works regardless of how the guest kernel was built; rejecting code 5
       here was the cause of the MinMove silent re-fault spin (handler returned
       without healing or advancing PC -> instruction retried -> faulted -> ...). */
    if (si->si_code != /*PROT_CHERI_TAG=*/2 &&
        si->si_code != /*PROT_CHERI_PERM=*/5) {
        if (prev_sa.sa_sigaction) {
            prev_sa.sa_sigaction(sig, si, ctx_);
        }
        return;
    }

    /* On CHERI, si_addr = faulting PC, not the cap value. */
    struct capregs *cregs = &ctx->uc_mcontext.mc_capregs;
    uintptr_t fault_addr = 0;
    void *new_cap = NULL;
    int faulting_reg = -1;

    /* Local helper: given a candidate base register index, if it holds a
       tag-zero cap whose address forwards (or can be evacuated), record it. */
#define STOPLESS_TRY_REG(i) do {                                              \
        int _i = (i);                                                         \
        if (new_cap == NULL && _i >= 0 && _i < 30) {                          \
            __uintcap_t _c = cregs->cap_x[_i];                                \
            /* "revoked" matches the kernel's cheri_revoke_is_revoked():       \
               tag==0 (CLEARTAGS build) OR perms==0 (default perms-clear       \
               build). forward_table membership is the real gate below, so a   \
               perms-0 cap that isn't a GC oop simply won't forward. */        \
            if (!cheri_tag_get((void *)_c) ||                                 \
                cheri_perms_get((void *)_c) == 0) {                            \
                uintptr_t _a = (uintptr_t)cheri_address_get((void *)_c);      \
                void *_fwd = forward_table_lookup(_a);                        \
                if (_fwd == NULL && g_evacuate_cb != NULL) {                  \
                    _fwd = g_evacuate_cb(_a);                                 \
                    if (_fwd != NULL) stopless_handler_assists++;             \
                }                                                             \
                if (_fwd != NULL) { new_cap = _fwd; fault_addr = _a; faulting_reg = _i; } \
            }                                                                 \
        }                                                                     \
    } while (0)

    /* V2: decode the faulting load/store to find the EXACT base register,
       instead of guessing "first tag-zero reg that forwards". After a revoke
       sweep many registers hold tag-zero caps, and a wrong guess overwrites a
       live Method or istate cap -> interpreter state corruption (the C-9 spin).
       AArch64/Morello LDR/STR/LDP/STP encode the base register Rn in bits
       [9:5]; the dereferenced capability lives there. PCC (cap_elr) is
       readable, so we can fetch the instruction word. */
    {
        __uintcap_t elr = (__uintcap_t)cregs->cap_elr;
        uint32_t insn = *(volatile uint32_t *)elr;
        unsigned rn = (insn >> 5) & 0x1f;     /* base register field */
        STOPLESS_TRY_REG((int)rn);
    }

    /* Fallback (old V1 behaviour): if the decoded base didn't forward, scan
       the whole cap register file for the first tag-zero forwardable cap. */
    if (new_cap == NULL) {
        for (int i = 0; i < 30; i++) {
            STOPLESS_TRY_REG(i);
            if (new_cap != NULL) break;
        }
    }
    if (new_cap == NULL) {
        /* Last-resort: try si_addr interpreted as cap address. */
        new_cap = forward_table_lookup((uintptr_t)si->si_addr);
        fault_addr = (uintptr_t)si->si_addr;
    }
#undef STOPLESS_TRY_REG

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
static void sigill_handler(int sig, siginfo_t *si, void *ctx_);

/* C-9 boot diag: the boot SIGILL fires during EARLY VM init (stub generation,
   ~fn_id 47) BEFORE stopless_handler_install() runs, so a normal install is
   too late to catch it. A library constructor installs the SIGILL diagnostic
   the moment libjvm.so is loaded (before JNI_CreateJavaVM). HotSpot later
   installs its own SIGILL handler, but that is after this early crash point. */
__attribute__((constructor))
static void stopless_early_sigill_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigill_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    fprintf(stderr, "[stopless] early SIGILL diag installed (ctor)\n");
    fflush(stderr);
}

static void
sigill_handler(int sig, siginfo_t *si, void *ctx_)
{
    ucontext_t *ctx = (ucontext_t *)ctx_;
    struct capregs *cr = &ctx->uc_mcontext.mc_capregs;
    /* FAULT-SAFE: print ONLY capability metadata (addr/tag/bounds/perms/flags)
       via cheri_* intrinsics — NEVER dereference pc_cap to read instruction
       bytes (that takes a nested cap fault on CHERI and hides the real fault,
       per C-6 L36). flags bit0 = C64 mode (1=C64, 0=A64); a SIGILL with a
       valid-bounds PCC usually means an A64<->C64 mode mismatch on the branch. */
#define STOPLESS_DUMPCAP(name, v) do {                                        \
        void *_c = (void *)(__uintcap_t)(v);                                  \
        fprintf(stderr, "  %-5s tag=%d addr=%#lx base=%#lx top=%#lx "         \
                "perms=%#lx flags=%d(C64=%d)\n", name,                        \
                (int)cheri_tag_get(_c),                                       \
                (unsigned long)cheri_address_get(_c),                         \
                (unsigned long)cheri_base_get(_c),                            \
                (unsigned long)(cheri_base_get(_c) + cheri_length_get(_c)),   \
                (unsigned long)cheri_perms_get(_c),                           \
                (int)cheri_flags_get(_c),                                     \
                (int)(cheri_flags_get(_c) & 1));                              \
    } while (0)
    fprintf(stderr, "[stopless] SIGILL si_code=%d si_addr=%p\n",
            si->si_code, si->si_addr);
    STOPLESS_DUMPCAP("PCC", cr->cap_elr);   /* faulting PC + mode */
    STOPLESS_DUMPCAP("CLR", cr->cap_lr);    /* return addr = the cap_blr call site */
    STOPLESS_DUMPCAP("CSP", cr->cap_sp);
    STOPLESS_DUMPCAP("c0",  cr->cap_x[0]);  /* dispatch ABI: w0 = fn_id */
    STOPLESS_DUMPCAP("c1",  cr->cap_x[1]);
    STOPLESS_DUMPCAP("c16", cr->cap_x[16]); /* rscratch1 = loaded trampoline cap */
    STOPLESS_DUMPCAP("c17", cr->cap_x[17]);
#undef STOPLESS_DUMPCAP
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ===================================================================== *
 * C-9 boot-crash dumper (sigaltstack-based, robust to stack corruption)
 *
 * The boot SIGILL fires AFTER HotSpot installs its own signal handlers
 * (which override our early ctor handler) and the crash is a ret-to-stack
 * (corrupt saved-LR slot) that leaves the stack in a state where HotSpot's
 * fatal-error reporter itself dies (no hs_err, just rc=132). This dumper:
 *   - runs on a dedicated sigaltstack (SA_ONSTACK) so it executes even if
 *     CSP is unusable;
 *   - dumps ONLY the trapframe regs + a bounded window of stack words read
 *     through the (valid) ucontext CSP capability — no unchecked derefs;
 *   - must be re-armed AFTER HotSpot's signal init: call
 *     stopless_install_crash_diag() from a late hook (we call it right
 *     before java.lang.String.<clinit> in InstanceKlass::call_class_initializer).
 * ===================================================================== */
static char stopless_altstack[256 * 1024] __attribute__((aligned(16)));

/* Read one 64-bit word at `addr` THROUGH the valid CSP capability `base`
   (bounds-checked, fault-safe). Returns the word, or a sentinel if oob. */
static unsigned long
stopless_stk_rd(void *base, unsigned long addr)
{
    if (!cheri_tag_get(base)) return 0xBAD0BAD0BAD0BAD0UL;
    unsigned long b = (unsigned long)cheri_base_get(base);
    unsigned long t = b + (unsigned long)cheri_length_get(base);
    if (addr < b || addr + 8 > t) return 0xBAD1BAD1BAD1BAD1UL;
    void *p = cheri_address_set(base, addr);
    return *(volatile unsigned long *)p;
}

static void
stopless_crash_dumper(int sig, siginfo_t *si, void *ctx_)
{
    ucontext_t *ctx = (ucontext_t *)ctx_;

    /* C-9: the dumper REPLACES sigprot_handler/HotSpot handlers once armed,
       so it inherits their duty to resume expected SafeFetch probe faults
       (SIGPROT on CHERI, SEGV/BUS elsewhere) instead of dumping. This was
       the post-banner "clinit 121" wall: ServiceThread -> OopStorage::
       release -> block_for_ptr -> SafeFetchN tag-fault treated as fatal. */
    if (stopless_safefetch_redirect(ctx))
        return;

    struct capregs *cr = &ctx->uc_mcontext.mc_capregs;
    void *csp = (void *)(__uintcap_t)cr->cap_sp;
    unsigned long elr = (unsigned long)cheri_address_get((void *)(__uintcap_t)cr->cap_elr);
    unsigned long lr  = (unsigned long)cheri_address_get((void *)(__uintcap_t)cr->cap_lr);
    unsigned long spA = (unsigned long)cheri_address_get(csp);
    unsigned long fp  = (unsigned long)cheri_address_get((void *)(__uintcap_t)cr->cap_x[29]);

    fprintf(stderr, "\n[stopless] ===== CRASH DUMP (sig=%d code=%d addr=%p) =====\n",
            sig, si->si_code, si->si_addr);
    fprintf(stderr, "[stopless]   ELR(faulting PC)=%#lx  CLR(c30)=%#lx  CSP=%#lx  c29(FP)=%#lx\n",
            elr, lr, spA, fp);
    fprintf(stderr, "[stopless]   ELR tag=%d perms=%#lx C64=%d | CSP tag=%d base=%#lx top=%#lx\n",
            (int)cheri_tag_get((void *)(__uintcap_t)cr->cap_elr),
            (unsigned long)cheri_perms_get((void *)(__uintcap_t)cr->cap_elr),
            (int)(cheri_flags_get((void *)(__uintcap_t)cr->cap_elr) & 1),
            (int)cheri_tag_get(csp),
            (unsigned long)cheri_base_get(csp),
            (unsigned long)(cheri_base_get(csp) + cheri_length_get(csp)));
    /* After a corrupt-LR `cap-ldp c29,c30,[sp],#32 ; ret c30`, CSP has already
       been incremented by 32, so the crashing callee frame base was CSP-32 and
       its saved-LR slot (rfp+16) was at CSP-16. Dump a window around it. */
    fprintf(stderr, "[stopless]   stack window (via CSP cap):\n");
    for (long off = -256; off <= 160; off += 8) {
        unsigned long a = spA + off;
        unsigned long w = stopless_stk_rd(csp, a);
        const char *mark = "";
        if (off == 0)   mark = "  <== CSP=sender_sp";
        /* flag any slot that == sender_sp (0x...d010): a corrupt saved-LR */
        else if (w == spA) mark = "  <== holds CSP/sender_sp value!";
        fprintf(stderr, "[stopless]     [%#lx] (CSP%+ld) = %#018lx%s\n", a, off, w, mark);
    }
    /* Full cap-register file: tag + address for x0..x30. Reveals which
       register holds the bad stack value and what c30/c29/x9 etc. are. */
    fprintf(stderr, "[stopless]   cap regs (tag:addr):\n");
    for (int r = 0; r <= 30; r++) {
        void *c = (void *)(__uintcap_t)cr->cap_x[r];
        fprintf(stderr, "[stopless]     c%-2d tag=%d addr=%#lx%s", r,
                (int)cheri_tag_get(c), (unsigned long)cheri_address_get(c),
                (r % 2) ? "\n" : "   ");
    }
    fprintf(stderr, "\n");
    /* The intact saved-LR slot of the crashing frame, for comparison vs c30. */
    {
        unsigned long slot = stopless_stk_rd(csp, fp + 16);
        fprintf(stderr, "[stopless]   [fp+16]=saved_lr slot = %#lx  (c30=%#lx)\n",
                slot, lr);
    }
    /* dladdr symbolizer: resolves an address to module+load-base (+symbol if
       not stripped). Even on a stripped libjvm this yields fname + fbase, so
       offset = addr - base feeds addr2line on the local unstripped libjvm.
       Build a cap with the target address from PCC (dladdr uses only the
       address, not bounds). */
    /* C-9: name JVM-generated codecache blobs (dladdr can't see them). */
    extern const char* stopless_codeblob_name(void*);
    #define STOPLESS_SYM(tag, addrval) do {                                       \
        void *_a = cheri_address_set((void *)(__uintcap_t)cr->cap_elr,            \
                                     (unsigned long)(addrval));                   \
        Dl_info _di;                                                              \
        if (dladdr(_a, &_di) && _di.dli_fname) {                                  \
            unsigned long _b = (unsigned long)cheri_address_get((void*)_di.dli_fbase); \
            fprintf(stderr, "[stopless]       %s %#lx = %s @base %#lx (off %#lx) sym=%s\n", \
                    tag, (unsigned long)(addrval), _di.dli_fname, _b,             \
                    (unsigned long)(addrval) - _b,                               \
                    _di.dli_sname ? _di.dli_sname : "(stripped)");               \
        } else {                                                                  \
            const char *_bn = stopless_codeblob_name(_a);                         \
            fprintf(stderr, "[stopless]       %s %#lx = codecache blob: %s\n",    \
                    tag, (unsigned long)(addrval), _bn ? _bn : "(unknown)");      \
        }                                                                         \
    } while (0)

    /* Walk the interpreter/native frame chain via saved-FP links (bounded). */
    fprintf(stderr, "[stopless]   frame chain (saved-FP -> saved-LR):\n");
    STOPLESS_SYM("ELR", (unsigned long)cheri_address_get((void*)(__uintcap_t)cr->cap_elr));
    unsigned long f = fp;
    for (int i = 0; i < 8 && f; i++) {
        unsigned long sfp = stopless_stk_rd(csp, f);        /* [fp+0]  saved FP */
        unsigned long slr = stopless_stk_rd(csp, f + 16);   /* [fp+16] saved LR */
        fprintf(stderr, "[stopless]     #%d fp=%#lx  saved_lr=%#lx\n", i, f, slr);
        STOPLESS_SYM("lr", slr);
        if (sfp <= f || (sfp & 0xf) || sfp == 0xBAD0BAD0BAD0BAD0UL || sfp == 0xBAD1BAD1BAD1BAD1UL)
            break;
        f = sfp;
    }
    /* Disassembly aid: read code/data memory at an arbitrary address by
       picking whichever tagged register cap (incl. PCC) has bounds covering
       it — gdb can't read this process's userspace (wrong CPU context), but we
       run IN it with cap access. */
    {
        void *bases[33];
        int nb = 0;
        for (int r = 0; r <= 30; r++) bases[nb++] = (void *)(__uintcap_t)cr->cap_x[r];
        bases[nb++] = (void *)(__uintcap_t)cr->cap_elr;
        bases[nb++] = (void *)(__uintcap_t)cr->cap_sp;
        #define COVER(addr,nbytes) ({                                          \
            void *_p = NULL; unsigned long _a=(addr);                          \
            for (int _i=0;_i<nb;_i++){ void*_b=bases[_i];                      \
              /* must be tagged, UNSEALED (a sentry deref nested-faults and   \
                 kills the handler) and LOAD-capable */                        \
              if(cheri_tag_get(_b) && !cheri_is_sealed(_b) &&                  \
                 (cheri_perms_get(_b) & CHERI_PERM_LOAD)){                     \
                 unsigned long _lo=(unsigned long)cheri_base_get(_b),          \
                 _hi=_lo+(unsigned long)cheri_length_get(_b);                  \
                 if(_a>=_lo && _a+(nbytes)<=_hi){ _p=cheri_address_set(_b,_a); break; }}}\
            _p; })
        /* Disassemble around the FAULTING PC (ELR) and decode the faulting
           instruction's base register so the tag-0 capability is obvious. */
        unsigned long elr_a = (unsigned long)cheri_address_get((void *)(__uintcap_t)cr->cap_elr);
        {
            fprintf(stderr, "[stopless]   ---- code words @ ELR 0x%lx (<== is the fault) ----\n", elr_a);
            for (long off = -0x20; off <= 0x20; off += 4) {
                void *p = COVER(elr_a + off, 4);
                unsigned int insn = p ? *(volatile unsigned int *)p : 0xDEAD0000;
                fprintf(stderr, "[stopless]     0x%lx (%+ld): %08x%s\n",
                        elr_a + off, off, insn,
                        (off == 0) ? "  <== ELR" : (p ? "" : " (no-cover)"));
            }
            void *pf = COVER(elr_a, 4);
            if (pf) {
                unsigned int fi = *(volatile unsigned int *)pf;
                unsigned rn = (fi >> 5) & 0x1f;     /* base reg of ld/st = bits[9:5] */
                unsigned rt = fi & 0x1f;
                void *base = (rn <= 30) ? (void *)(__uintcap_t)cr->cap_x[rn] : NULL;
                fprintf(stderr, "[stopless]   faulting insn=%08x  Rn(base)=c%u tag=%d addr=%#lx  Rt=c%u\n",
                        fi, rn, base ? (int)cheri_tag_get(base) : -1,
                        base ? (unsigned long)cheri_address_get(base) : 0, rt);
                if (base) {
                    /* C-9: disambiguate tagged-but-faulting (bounds vs perm).
                       in-bounds = base <= addr < top. */
                    unsigned long b = (unsigned long)cheri_base_get(base);
                    unsigned long len = (unsigned long)cheri_length_get(base);
                    unsigned long a = (unsigned long)cheri_address_get(base);
                    fprintf(stderr, "[stopless]     base cap: base=%#lx top=%#lx len=%#lx perms=%#lx otype=%#lx sealed=%d %s\n",
                            b, b + len, len, (unsigned long)cheri_perms_get(base),
                            (unsigned long)cheri_type_get(base), (int)cheri_is_sealed(base),
                            (a >= b && a < b + len) ? "[addr IN bounds]" : "[addr OUT of bounds]");
                }
            }
        }
        /* C-9: name the interpreted method (rmethod=c12) + bci (rbcp=c22)
           directly — saves the offline Method*-decode dance every time. */
        {
            extern const char* stopless_method_name(void*, void*) __attribute__((weak));
            if (stopless_method_name != NULL) {
                const char *mn = stopless_method_name(
                    (void *)(__uintcap_t)cr->cap_x[12],
                    (void *)(__uintcap_t)cr->cap_x[22]);
                fprintf(stderr, "[stopless]   interpreted method (c12): %s\n",
                        mn ? mn : "(unresolvable)");
            }
        }
        /* What rbcp(c22), rmethod(c12), rcpool(c26) point at, and the dispatch table. */
        unsigned long probes[][2] = {
            { (unsigned long)cheri_address_get((void*)(__uintcap_t)cr->cap_x[22]), 22 }, /* rbcp */
            { (unsigned long)cheri_address_get((void*)(__uintcap_t)cr->cap_x[12]), 12 }, /* rmethod */
            { (unsigned long)cheri_address_get((void*)(__uintcap_t)cr->cap_x[21]), 21 }, /* rdispatch */
        };
        for (unsigned t = 0; t < 3; t++) {
            fprintf(stderr, "[stopless]   ---- mem @ c%lu=0x%lx ----\n", probes[t][1], probes[t][0]);
            for (int k = 0; k < 8; k++) {
                void *p = COVER(probes[t][0] + k*8, 8);
                unsigned long v = p ? *(volatile unsigned long *)p : 0xDEADUL;
                fprintf(stderr, "[stopless]     +%d = 0x%lx%s\n", k*8, v, p ? "" : " (no-cover)");
            }
        }
        /* C-9 receiver-strip hunt: dump the bytecode stream around rbcp(c22),
           byte-granular, BACKWARD + forward. *rbcp is the executing bytecode;
           the bytes just before it are the ones that PUSHED the (tag-0)
           operand the current bytecode is choking on — letting us name the
           producer (aaload? getfield? invoke return? ldc?) without guessing. */
        {
            unsigned long bcp = (unsigned long)cheri_address_get((void*)(__uintcap_t)cr->cap_x[22]);
            fprintf(stderr, "[stopless]   ---- bytecodes @ rbcp=0x%lx (<== is *rbcp) ----\n", bcp);
            for (int off = -24; off <= 8; off++) {
                void *p = COVER(bcp + off, 1);
                int b = p ? *(volatile unsigned char *)p : -1;
                fprintf(stderr, "[stopless]     [%+d] = 0x%02x%s%s\n",
                        off, b & 0xff, off == 0 ? "  <== *rbcp" : "",
                        p ? "" : " (no-cover)");
            }
        }
        /* C-9 receiver-strip hunt #2: print the capability TAG of the `this`
           local (rlocals=c24, slot 0) and the top expression-stack slots
           (esp=c20). If `this` is tagged (tag=1) but the receiver copied onto
           the expr stack is tag=0, the pushing bytecode template stripped it. */
        {
            struct { int reg; int n; const char *nm; } slots[] = {
                {24, 0, "rlocals[0]=this"}, {24, -16, "rlocals[-16]"},
                {24, -32, "rlocals[-32]"},
                {20, 0, "esp[0]"}, {20, 16, "esp[16]"}, {20, 32, "esp[32]"},
                {20, 48, "esp[48]"},
            };
            for (unsigned s = 0; s < sizeof(slots)/sizeof(slots[0]); s++) {
                unsigned long a = (unsigned long)cheri_address_get(
                    (void*)(__uintcap_t)cr->cap_x[slots[s].reg]) + (long)slots[s].n;
                void *p = COVER(a, 16);
                if (p) {
                    void *v = *(void * volatile *)p;
                    fprintf(stderr, "[stopless]   %-16s @0x%lx = tag=%d addr=0x%lx\n",
                            slots[s].nm, a, (int)cheri_tag_get(v),
                            (unsigned long)cheri_address_get(v));
                } else {
                    fprintf(stderr, "[stopless]   %-16s @0x%lx (no-cover)\n", slots[s].nm, a);
                }
            }
        }
        /* C-9 generic tool: STOPLESS_DUMP_RANGE="start_hex,end_hex" dumps that
           address range as 4-byte hex words via whatever register cap covers
           each word (COVER) — paste into llvm-mc to disassemble a generated
           codelet offline. Codecache layout is deterministic across runs. */
        {
            const char *rng = getenv("STOPLESS_DUMP_RANGE");
            if (rng) {
                unsigned long d_lo = 0, d_hi = 0;
                if (sscanf(rng, "%lx,%lx", &d_lo, &d_hi) == 2 && d_hi > d_lo &&
                    d_hi - d_lo <= 0x10000) {
                    fprintf(stderr, "[stopless]   ---- range dump 0x%lx..0x%lx ----\n",
                            d_lo, d_hi);
                    for (unsigned long a = d_lo; a < d_hi; a += 4) {
                        void *p = COVER(a, 4);
                        if (p) fprintf(stderr, "%08x\n", *(volatile unsigned int *)p);
                        else   fprintf(stderr, "deadc0de\n");
                    }
                    fprintf(stderr, "[stopless]   ---- end range dump ----\n");
                }
            }
        }
        #undef COVER
    }
    fprintf(stderr, "[stopless] ===== END CRASH DUMP =====\n");
    fflush(stderr);
    /* C-9 diag: optionally HANG here (env STOPLESS_HANG_ON_CRASH) so the
       process stays alive at the crash with stable addresses (run with ASLR
       off) — a system gdbstub can then attach and disassemble the live interp
       code (gdb bypasses capability checks) to pin the faulting branch. */
    if (getenv("STOPLESS_BRK_ON_CRASH")) {
        /* Trap an ALREADY-ATTACHED system gdb (qemu gdbstub :1234) right here,
           IN this process's address space (TTBR0 = java), so gdb can read/
           disassemble the live interp code and walk the real frames — which it
           cannot do once the process is descheduled. We are post-fault (in the
           SIGSEGV handler on the altstack), so this pins the frame state, not
           the faulting instruction itself; use it to identify the faulting
           ret's address, then re-run with a breakpoint there to single-step. */
        fprintf(stderr, "[stopless] BRK for gdb (pid=%d). java-context now current.\n",
                (int)getpid());
        fflush(stderr);
        __asm__ volatile("brk #0");
    }
    if (getenv("STOPLESS_HANG_ON_CRASH")) {
        fprintf(stderr, "[stopless] HANGING for gdb (pid=%d). attach :1234.\n",
                (int)getpid());
        fflush(stderr);
        for (;;) { struct timespec ts = { 3600, 0 }; nanosleep(&ts, NULL); }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Re-arm our crash dumper on an alternate stack, overriding HotSpot's
   handlers. Idempotent. Call from a late hook (post-VM-init). */
void
stopless_install_crash_diag(void)
{
    static int armed = 0;
    if (armed) return;
    armed = 1;

    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = stopless_altstack;
    ss.ss_size = sizeof(stopless_altstack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0)
        perror("stopless_install_crash_diag: sigaltstack");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = stopless_crash_dumper;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    /* NOTE: deliberately NOT SIGTRAP — the crash dumper emits `brk #0` (when
       STOPLESS_BRK_ON_CRASH is set) to stop an attached gdb IN this process's
       address space; catching SIGTRAP here would recurse instead. */
    int sigs[] = { SIGILL, SIGSEGV, SIGBUS, 34 /*SIGPROT*/ };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
    fprintf(stderr, "[stopless] crash diag re-armed on altstack (SA_ONSTACK)\n");
    fflush(stderr);
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
    /* C-6 L36 NOTE: the OLD sigill_handler dereferenced the faulting PC to
       print instruction bytes, which took a nested cap fault and hid the real
       fault. The handler is now FAULT-SAFE (metadata only, no PC dereference),
       so we install it to capture the C-9-boot SIGILL (suspected A64<->C64
       mode mismatch on the cap_blr trampoline branch). */
    {
        struct sigaction sa_ill;
        memset(&sa_ill, 0, sizeof(sa_ill));
        sa_ill.sa_sigaction = sigill_handler;
        sa_ill.sa_flags = SA_SIGINFO;
        sigemptyset(&sa_ill.sa_mask);
        if (sigaction(SIGILL, &sa_ill, NULL) != 0) {
            perror("stopless_handler_install: sigaction(SIGILL)");
        }
    }
    fflush(stderr);
    return 0;
}

void
stopless_handler_remove(void)
{
    sigaction(34, &prev_sa, NULL);
}
