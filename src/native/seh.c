/* Structured Exception Handling for x86-64.
 *
 * An implementation of the Windows x64 exception model, written from
 * Microsoft's published description of it ("x64 exception handling"):
 * RtlCaptureContext, RtlLookupFunctionEntry, RtlVirtualUnwind, RtlUnwindEx,
 * RaiseException / RtlRaiseException, vectored handlers, the unhandled
 * exception filter and __C_specific_handler. CPU faults (SIGSEGV, SIGFPE,
 * SIGILL, SIGBUS) become Windows exceptions, so __try/__except, MinGW C++
 * exceptions (libgcc's SEH personality) and crash handlers all work.
 *
 * LSIW's own functions are ordinary Linux code without .pdata unwind
 * tables. Every entry point that starts a dispatch or an unwind therefore
 * captures the context of its Windows caller first, and records the
 * stretch of host stack it occupies. When a stack walk reaches host code it
 * continues from the matching record, which mirrors what Windows does when
 * a walk crosses the dispatcher (back to the faulting context) or an
 * in-progress unwind (a "collided unwind").
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "seh.h"
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Windows structures                                                   */
/* ------------------------------------------------------------------ */


/* UNWIND_INFO */
#define UWOP_PUSH_NONVOL     0
#define UWOP_ALLOC_LARGE     1
#define UWOP_ALLOC_SMALL     2
#define UWOP_SET_FPREG       3
#define UWOP_SAVE_NONVOL     4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_EPILOG          6
#define UWOP_SPARE           7
#define UWOP_SAVE_XMM128     8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME  10

#define UNW_FLAG_NHANDLER  0
#define UNW_FLAG_EHANDLER  1
#define UNW_FLAG_UHANDLER  2
#define UNW_FLAG_CHAININFO 4

struct unwind_info {
    uint8_t version_flags;      /* Version:3, Flags:5 */
    uint8_t prolog_size;
    uint8_t count;
    uint8_t frame;              /* FrameRegister:4, FrameOffset:4 */
    uint16_t codes[];           /* CodeOffset:8, UnwindOp:4, OpInfo:4 */
};
#define UI_VERSION(u) ((u)->version_flags & 7)
#define UI_FLAGS(u)   ((u)->version_flags >> 3)
#define UI_FREG(u)    ((u)->frame & 15)
#define UI_FOFF(u)    ((u)->frame >> 4)
#define UC_OFFSET(c)  ((c) & 0xff)
#define UC_OP(c)      (((c) >> 8) & 15)
#define UC_INFO(c)    ((c) >> 12)

static uint64_t *reg_slot(CONTEXT *c, int r)
{
    return &(&c->Rax)[r];       /* Rax..R15 are consecutive in reg-number order */
}

/* ------------------------------------------------------------------ */
/* Context capture and restore (assembly)                               */
/* ------------------------------------------------------------------ */

/* Store every integer register except RSP/RIP, plus XMM0-15 and MXCSR,
 * into the CONTEXT at offset 0 from \base. Clobbers nothing. */
#define SAVE_REGS(base) \
    "movq %rax, 0x78(" base ")\n" "movq %rcx, 0x80(" base ")\n" \
    "movq %rdx, 0x88(" base ")\n" "movq %rbx, 0x90(" base ")\n" \
    "movq %rbp, 0xA0(" base ")\n" "movq %rsi, 0xA8(" base ")\n" \
    "movq %rdi, 0xB0(" base ")\n" "movq %r8,  0xB8(" base ")\n" \
    "movq %r9,  0xC0(" base ")\n" "movq %r10, 0xC8(" base ")\n" \
    "movq %r11, 0xD0(" base ")\n" "movq %r12, 0xD8(" base ")\n" \
    "movq %r13, 0xE0(" base ")\n" "movq %r14, 0xE8(" base ")\n" \
    "movq %r15, 0xF0(" base ")\n" \
    "movl $0x10000B, 0x30(" base ")\n" \
    "stmxcsr 0x34(" base ")\n" \
    "stmxcsr 0x118(" base ")\n" \
    "fnstcw 0x100(" base ")\n" \
    "movw $0x33, 0x38(" base ")\n" "movw $0x2b, 0x42(" base ")\n" \
    "movl $0x202, 0x44(" base ")\n" \
    "movdqu %xmm0, 0x1A0(" base ")\n" "movdqu %xmm1, 0x1B0(" base ")\n" \
    "movdqu %xmm2, 0x1C0(" base ")\n" "movdqu %xmm3, 0x1D0(" base ")\n" \
    "movdqu %xmm4, 0x1E0(" base ")\n" "movdqu %xmm5, 0x1F0(" base ")\n" \
    "movdqu %xmm6, 0x200(" base ")\n" "movdqu %xmm7, 0x210(" base ")\n" \
    "movdqu %xmm8, 0x220(" base ")\n" "movdqu %xmm9, 0x230(" base ")\n" \
    "movdqu %xmm10, 0x240(" base ")\n" "movdqu %xmm11, 0x250(" base ")\n" \
    "movdqu %xmm12, 0x260(" base ")\n" "movdqu %xmm13, 0x270(" base ")\n" \
    "movdqu %xmm14, 0x280(" base ")\n" "movdqu %xmm15, 0x290(" base ")\n"

/* Entry stubs. Each reserves a CONTEXT on the stack, records the caller's
 * state in it (RSP and RIP as they will be once the stub "returns"), then
 * calls a C function with the System V convention. */
#define FRAME 0x4D8   /* CONTEXT (0x4D0) rounded so RSP stays 16-aligned */

__asm__(
    ".text\n"
    ".globl lsiw_RtlCaptureContext\n"
    "lsiw_RtlCaptureContext:\n"
    SAVE_REGS("%rcx")
    "  pushq %rax\n"
    "  leaq 16(%rsp), %rax\n"   "  movq %rax, 0x98(%rcx)\n"
    "  movq 8(%rsp), %rax\n"    "  movq %rax, 0xF8(%rcx)\n"
    "  popq %rax\n"
    "  ret\n"

    ".globl lsiw_RaiseException\n"
    "lsiw_RaiseException:\n"
    "  subq $0x4D8, %rsp\n"
    SAVE_REGS("%rsp")
    "  leaq 0x4E0(%rsp), %rax\n" "movq %rax, 0x98(%rsp)\n"
    "  movq 0x4D8(%rsp), %rax\n" "movq %rax, 0xF8(%rsp)\n"
    "  movl %ecx, %edi\n"
    "  movl %edx, %esi\n"
    "  movl %r8d, %edx\n"
    "  movq %r9, %rcx\n"
    "  movq %rsp, %r8\n"
    "  call lsiw_raise_entry\n"
    "  ud2\n"

    ".globl lsiw_RtlRaiseException\n"
    "lsiw_RtlRaiseException:\n"
    "  subq $0x4D8, %rsp\n"
    SAVE_REGS("%rsp")
    "  leaq 0x4E0(%rsp), %rax\n" "movq %rax, 0x98(%rsp)\n"
    "  movq 0x4D8(%rsp), %rax\n" "movq %rax, 0xF8(%rsp)\n"
    "  movq %rcx, %rdi\n"
    "  movq %rsp, %rsi\n"
    "  call lsiw_rtlraise_entry\n"
    "  ud2\n"

    ".globl lsiw_RtlUnwindEx\n"
    "lsiw_RtlUnwindEx:\n"
    "  subq $0x4D8, %rsp\n"
    SAVE_REGS("%rsp")
    "  leaq 0x4E0(%rsp), %rax\n" "movq %rax, 0x98(%rsp)\n"
    "  movq 0x4D8(%rsp), %rax\n" "movq %rax, 0xF8(%rsp)\n"
    "  movq %rcx, %rdi\n"
    "  movq %rdx, %rsi\n"
    "  movq %r8, %rdx\n"
    "  movq %r9, %rcx\n"
    "  movq 0x500(%rsp), %r8\n"      /* 5th argument: ContextRecord */
    "  movq %rsp, %r9\n"
    "  call lsiw_unwind_entry\n"
    "  ud2\n"

    ".globl lsiw_RtlUnwind\n"
    "lsiw_RtlUnwind:\n"
    "  subq $0x4D8, %rsp\n"
    SAVE_REGS("%rsp")
    "  leaq 0x4E0(%rsp), %rax\n" "movq %rax, 0x98(%rsp)\n"
    "  movq 0x4D8(%rsp), %rax\n" "movq %rax, 0xF8(%rsp)\n"
    "  movq %rcx, %rdi\n"
    "  movq %rdx, %rsi\n"
    "  movq %r8, %rdx\n"
    "  movq %r9, %rcx\n"
    "  xorl %r8d, %r8d\n"
    "  movq %rsp, %r9\n"
    "  call lsiw_unwind_entry\n"
    "  ud2\n"

    /* Load a CONTEXT (rdi) and jump to it. Only the 8 bytes just below the
     * target RSP are written (the return slot), which is dead stack for
     * Windows code since x64 Windows has no red zone. */
    ".globl lsiw_restore_context\n"
    "lsiw_restore_context:\n"
    "  movq %rdi, %rax\n"
    "  ldmxcsr 0x34(%rax)\n"
    "  movdqu 0x1A0(%rax), %xmm0\n"  "movdqu 0x1B0(%rax), %xmm1\n"
    "  movdqu 0x1C0(%rax), %xmm2\n"  "movdqu 0x1D0(%rax), %xmm3\n"
    "  movdqu 0x1E0(%rax), %xmm4\n"  "movdqu 0x1F0(%rax), %xmm5\n"
    "  movdqu 0x200(%rax), %xmm6\n"  "movdqu 0x210(%rax), %xmm7\n"
    "  movdqu 0x220(%rax), %xmm8\n"  "movdqu 0x230(%rax), %xmm9\n"
    "  movdqu 0x240(%rax), %xmm10\n" "movdqu 0x250(%rax), %xmm11\n"
    "  movdqu 0x260(%rax), %xmm12\n" "movdqu 0x270(%rax), %xmm13\n"
    "  movdqu 0x280(%rax), %xmm14\n" "movdqu 0x290(%rax), %xmm15\n"
    "  movq 0x98(%rax), %rcx\n"      /* target RSP */
    "  movq 0xF8(%rax), %rdx\n"      /* target RIP */
    "  movq %rdx, -8(%rcx)\n"
    "  movq 0x90(%rax), %rbx\n"
    "  movq 0xA0(%rax), %rbp\n"
    "  movq 0xA8(%rax), %rsi\n"
    "  movq 0xB0(%rax), %rdi\n"
    "  movq 0xB8(%rax), %r8\n"
    "  movq 0xC0(%rax), %r9\n"
    "  movq 0xC8(%rax), %r10\n"
    "  movq 0xD0(%rax), %r11\n"
    "  movq 0xD8(%rax), %r12\n"
    "  movq 0xE0(%rax), %r13\n"
    "  movq 0xE8(%rax), %r14\n"
    "  movq 0xF0(%rax), %r15\n"
    "  leaq -8(%rcx), %rsp\n"
    "  movq 0x80(%rax), %rcx\n"
    "  movq 0x88(%rax), %rdx\n"
    "  movq 0x78(%rax), %rax\n"
    "  ret\n"

);


API void RtlRestoreContext(CONTEXT *c, EXCEPTION_RECORD *rec) { (void)rec; lsiw_restore_context(c); }

/* ------------------------------------------------------------------ */
/* Host-region records                                                  */
/* ------------------------------------------------------------------ */

enum { REC_DISPATCH, REC_UNWIND, REC_CONSOLIDATE };

struct host_region {
    int kind;
    uintptr_t top;              /* RSP of the Windows context that entered LSIW */
    CONTEXT *entry;             /* that context                                 */
    /* REC_UNWIND: the frame currently being unwound */
    CONTEXT *cur;
    DISPATCHER_CONTEXT *dc;
    /* REC_CONSOLIDATE: notified when a walk crosses the region */
    lsiw_region_hook hook;
    void *hook_arg;
};

#define MAX_REGIONS 64
static __thread struct host_region t_regions[MAX_REGIONS];
static __thread int t_nregions;

static void regions_prune(uintptr_t live_rsp)
{
    /* Regions whose entry point lies below the live stack pointer belong to
     * frames that no longer exist. */
    while (t_nregions && t_regions[t_nregions - 1].top < live_rsp) t_nregions--;
}

static struct host_region *region_push(int kind, CONTEXT *entry)
{
    regions_prune(entry->Rsp);
    if (t_nregions >= MAX_REGIONS) {
        fprintf(stderr, "lsiw: exception nesting too deep\n");
        lsiw_exit_process(STATUS_STACK_OVERFLOW);
    }
    struct host_region *r = &t_regions[t_nregions++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->top = entry->Rsp;
    r->entry = entry;
    return r;
}

static void region_pop(struct host_region *r)
{
    int i = (int)(r - t_regions);
    if (i >= 0 && i < t_nregions) t_nregions = i;
}

/* The region a stack walk enters when, at stack position `sp`, it returns
 * into LSIW code: the innermost record whose entry lies above `sp`. */
static struct host_region *region_at(uintptr_t sp)
{
    for (int i = t_nregions - 1; i >= 0; i--)
        if (t_regions[i].top >= sp) return &t_regions[i];
    return NULL;
}

/* The region enclosing `r` as seen from stack position `sp`: used when a
 * walk starts in the host code that owns `r` itself (an unwind requested
 * by a language handler LSIW implements), so it continues in the operation
 * that called that handler. */
static struct host_region *region_outside(struct host_region *r, uintptr_t sp)
{
    for (int i = (int)(r - t_regions) - 1; i >= 0; i--)
        if (t_regions[i].top >= sp) return &t_regions[i];
    return NULL;
}

static void cross_hook(struct host_region *r, EXCEPTION_RECORD *rec, int unwinding)
{
    if (r->kind != REC_CONSOLIDATE || !r->hook) return;
    lsiw_region_hook fn = r->hook;
    if (unwinding) r->hook = NULL;          /* the catch block is gone */
    fn(r->hook_arg, rec, unwinding);
}

void lsiw_seh_set_consolidate_hook(lsiw_region_hook fn, void *arg)
{
    for (int i = t_nregions - 1; i >= 0; i--)
        if (t_regions[i].kind == REC_CONSOLIDATE) {
            t_regions[i].hook = fn;
            t_regions[i].hook_arg = arg;
            return;
        }
}

/* ------------------------------------------------------------------ */
/* Function tables                                                      */
/* ------------------------------------------------------------------ */

struct dyn_table {
    RUNTIME_FUNCTION *functions;
    DWORD count;
    ULONG_PTR base;
    ULONG_PTR start, end;          /* for callback-based tables */
    void *callback, *context;
    struct dyn_table *next;
};
static struct dyn_table *g_dyn_tables;
static pthread_mutex_t g_dyn_lock = PTHREAD_MUTEX_INITIALIZER;

typedef RUNTIME_FUNCTION *(WINAPI *get_fn_callback)(ULONG_PTR pc, void *context);

static RUNTIME_FUNCTION *search_table(RUNTIME_FUNCTION *fns, DWORD count, ULONG_PTR base, ULONG_PTR pc)
{
    if (pc < base) return NULL;
    uint64_t rva64 = pc - base;
    if (rva64 > 0xFFFFFFFFu) return NULL;
    uint32_t rva = (uint32_t)rva64;
    long lo = 0, hi = (long)count - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (rva < fns[mid].BeginAddress) hi = mid - 1;
        else if (rva >= fns[mid].EndAddress) lo = mid + 1;
        else return &fns[mid];
    }
    return NULL;
}

/* Returns 1 if `pc` is Windows code LSIW knows how to unwind. */
static int is_windows_code(ULONG_PTR pc)
{
    struct lsiw_module *m = lsiw_module_from_address((void *)pc);
    if (m && !m->is_builtin) return 1;
    int found = 0;
    pthread_mutex_lock(&g_dyn_lock);
    for (struct dyn_table *t = g_dyn_tables; t && !found; t = t->next) {
        if (t->callback) found = pc >= t->start && pc < t->end;
        else found = search_table(t->functions, t->count, t->base, pc) != NULL;
    }
    pthread_mutex_unlock(&g_dyn_lock);
    return found;
}

API RUNTIME_FUNCTION *RtlLookupFunctionEntry(ULONG_PTR pc, ULONG_PTR *image_base, void *history)
{
    (void)history;
    struct lsiw_module *m = lsiw_module_from_address((void *)pc);
    if (m && !m->is_builtin) {
        IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(m->base + ((IMAGE_DOS_HEADER *)m->base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (image_base) *image_base = (ULONG_PTR)m->base;
        if (!dd.VirtualAddress || !dd.Size) return NULL;
        return search_table((RUNTIME_FUNCTION *)(m->base + dd.VirtualAddress), dd.Size / sizeof(RUNTIME_FUNCTION),
                            (ULONG_PTR)m->base, pc);
    }
    RUNTIME_FUNCTION *r = NULL;
    pthread_mutex_lock(&g_dyn_lock);
    for (struct dyn_table *t = g_dyn_tables; t && !r; t = t->next) {
        if (t->callback) {
            if (pc >= t->start && pc < t->end) {
                pthread_mutex_unlock(&g_dyn_lock);
                r = ((get_fn_callback)t->callback)(pc, t->context);
                if (image_base) *image_base = t->base;
                return r;
            }
        } else if ((r = search_table(t->functions, t->count, t->base, pc)) && image_base) *image_base = t->base;
    }
    pthread_mutex_unlock(&g_dyn_lock);
    if (!r && image_base) *image_base = 0;
    return r;
}

RUNTIME_FUNCTION *lsiw_lookup_function_entry(ULONG_PTR pc, ULONG_PTR *image_base)
{
    return RtlLookupFunctionEntry(pc, image_base, NULL);
}

API BOOL RtlAddFunctionTable(RUNTIME_FUNCTION *fns, DWORD count, ULONG_PTR base)
{
    struct dyn_table *t = calloc(1, sizeof *t);
    t->functions = fns; t->count = count; t->base = base;
    pthread_mutex_lock(&g_dyn_lock);
    t->next = g_dyn_tables; g_dyn_tables = t;
    pthread_mutex_unlock(&g_dyn_lock);
    return TRUE;
}

API BOOL RtlDeleteFunctionTable(RUNTIME_FUNCTION *fns)
{
    pthread_mutex_lock(&g_dyn_lock);
    for (struct dyn_table **pp = &g_dyn_tables; *pp; pp = &(*pp)->next) {
        uintptr_t id = (uintptr_t)(*pp)->functions;
        if ((*pp)->functions == fns || (id & 3) == 3) {
            if ((*pp)->functions != fns) continue;
            struct dyn_table *d = *pp; *pp = d->next;
            pthread_mutex_unlock(&g_dyn_lock);
            free(d);
            return TRUE;
        }
    }
    pthread_mutex_unlock(&g_dyn_lock);
    return FALSE;
}

API BOOL RtlInstallFunctionTableCallback(ULONG_PTR id, ULONG_PTR base, DWORD len, void *cb, void *ctx, const WCHAR *dll)
{
    (void)dll;
    if ((id & 3) != 3) return FALSE;
    struct dyn_table *t = calloc(1, sizeof *t);
    t->functions = (RUNTIME_FUNCTION *)id;
    t->base = base; t->start = base; t->end = base + len;
    t->callback = cb; t->context = ctx;
    pthread_mutex_lock(&g_dyn_lock);
    t->next = g_dyn_tables; g_dyn_tables = t;
    pthread_mutex_unlock(&g_dyn_lock);
    return TRUE;
}

API DWORD RtlAddGrowableFunctionTable(void **out, RUNTIME_FUNCTION *fns, DWORD count, DWORD max, ULONG_PTR base, ULONG_PTR end)
{
    (void)max; (void)end;
    struct dyn_table *t = calloc(1, sizeof *t);
    t->functions = fns; t->count = count; t->base = base;
    pthread_mutex_lock(&g_dyn_lock);
    t->next = g_dyn_tables; g_dyn_tables = t;
    pthread_mutex_unlock(&g_dyn_lock);
    if (out) *out = t;
    return 0;
}

API void RtlGrowFunctionTable(void *h, DWORD count) { pthread_mutex_lock(&g_dyn_lock); ((struct dyn_table *)h)->count = count; pthread_mutex_unlock(&g_dyn_lock); }

API void RtlDeleteGrowableFunctionTable(void *h)
{
    pthread_mutex_lock(&g_dyn_lock);
    for (struct dyn_table **pp = &g_dyn_tables; *pp; pp = &(*pp)->next)
        if (*pp == h) { *pp = ((struct dyn_table *)h)->next; free(h); break; }
    pthread_mutex_unlock(&g_dyn_lock);
}

API void *RtlPcToFileHeader(void *pc, void **base)
{
    struct lsiw_module *m = lsiw_module_from_address(pc);
    void *b = m ? (void *)m->base : NULL;
    if (base) *base = b;
    return b;
}

/* ------------------------------------------------------------------ */
/* RtlVirtualUnwind                                                     */
/* ------------------------------------------------------------------ */

static int code_slots(uint16_t c, int version)
{
    switch (UC_OP(c)) {
    case UWOP_ALLOC_LARGE:     return UC_INFO(c) ? 3 : 2;
    case UWOP_SAVE_NONVOL:     return 2;
    case UWOP_SAVE_NONVOL_FAR: return 3;
    case UWOP_EPILOG:          return version >= 2 ? 1 : 2;   /* v1: old SAVE_XMM */
    case UWOP_SPARE:           return version >= 2 ? 1 : 3;   /* v1: old SAVE_XMM_FAR */
    case UWOP_SAVE_XMM128:     return 2;
    case UWOP_SAVE_XMM128_FAR: return 3;
    default:                   return 1;
    }
}

/* Recognise and simulate an epilog starting at pc, following the rules of
 * the Windows x64 unwinder:
 *   [add rsp, imm | lea rsp, [frame+disp]] pop* (ret | jmp)
 * A direct jmp whose target lies inside the function is an ordinary branch:
 * it is followed and the scan continues at the target (an epilog may be
 * split). Only a direct jmp leaving the function, or an indirect
 * rip-relative jmp, ends an epilog as a tail call. Without this, a return
 * address followed by "jmp <inside function>" was taken for the last
 * instruction of an epilog and the frame was mis-unwound. */
static int try_epilog(const uint8_t *pc, ULONG_PTR base, const RUNTIME_FUNCTION *fn,
                      struct unwind_info *info, CONTEXT *ctx)
{
    const uint8_t *p = pc;
    uint64_t rsp = ctx->Rsp;
    int frame_reg = UI_FREG(info);
    const uint8_t *fstart = (const uint8_t *)(base + fn->BeginAddress);
    const uint8_t *fend = (const uint8_t *)(base + fn->EndAddress);

    /* add $imm, %rsp */
    if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xC4) { rsp += (int8_t)p[3]; p += 4; }
    else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xC4) { rsp += *(int32_t *)(p + 3); p += 7; }
    /* lea disp(%frame), %rsp */
    else if ((p[0] & 0xF8) == 0x48 && p[1] == 0x8D) {
        int modrm = p[2], mod = modrm >> 6, rm = (modrm & 7) | ((p[0] & 1) << 3), reg = ((modrm >> 3) & 7) | ((p[0] & 4) << 1);
        if (reg != 4 || !frame_reg || rm != frame_reg || (modrm & 7) == 4) return 0;
        if (mod == 1) { rsp = *reg_slot(ctx, rm) + (int8_t)p[3]; p += 4; }
        else if (mod == 2) { rsp = *reg_slot(ctx, rm) + *(int32_t *)(p + 3); p += 7; }
        else return 0;
    }
    /* Scan pops up to the terminating ret/jmp before touching anything. */
    int pops[32], npops = 0, hops = 0;
    for (;;) {
        if ((p[0] & 0xF8) == 0x58) { if (npops < 32) pops[npops++] = p[0] & 7; p++; continue; }
        if ((p[0] & 0xF0) == 0x40 && (p[1] & 0xF8) == 0x58) {
            if (npops < 32) pops[npops++] = (p[1] & 7) | ((p[0] & 1) << 3);
            p += 2; continue;
        }
        if (p[0] == 0xC3 || p[0] == 0xC2 || (p[0] == 0xF3 && p[1] == 0xC3)) break;   /* ret */
        if (p[0] == 0xE9 || p[0] == 0xEB) {
            const uint8_t *t = p[0] == 0xE9 ? p + 5 + *(int32_t *)(p + 1) : p + 2 + (int8_t)p[1];
            if (t >= fstart && t < fend) {               /* branch within the function */
                if (++hops > 8) return 0;
                p = t;
                continue;
            }
            break;                                       /* tail call */
        }
        {
            const uint8_t *q = (p[0] & 0xF0) == 0x40 ? p + 1 : p;
            if (q[0] == 0xFF && q[1] == 0x25) break;    /* jmp *disp32(%rip): tail call */
        }
        return 0;
    }
    if (npops >= 32) return 0;
    ctx->Rsp = rsp;
    for (int i = 0; i < npops; i++) {
        *reg_slot(ctx, pops[i]) = *(uint64_t *)ctx->Rsp;
        ctx->Rsp += 8;
    }
    ctx->Rip = *(uint64_t *)ctx->Rsp;
    ctx->Rsp += 8;
    return 1;
}

API void *RtlVirtualUnwind(DWORD type, ULONG_PTR base, ULONG_PTR pc, RUNTIME_FUNCTION *fn,
                           CONTEXT *ctx, void **handler_data, ULONG_PTR *establisher, void *ctx_ptrs)
{
    (void)ctx_ptrs;
    uint64_t frame = ctx->Rsp;
    if (establisher) *establisher = frame;
    int in_prolog = 0, machframe = 0;
    struct unwind_info *info = NULL;
    uint32_t prolog_off = 0;

    for (int depth = 0; depth < 32; depth++) {
        info = (struct unwind_info *)(base + fn->UnwindData);
        int version = UI_VERSION(info);

        if (UI_FREG(info)) frame = *reg_slot(ctx, UI_FREG(info)) - UI_FOFF(info) * 16;

        uint64_t start = base + fn->BeginAddress;
        if (depth == 0 && pc >= start && pc < start + info->prolog_size) {
            in_prolog = 1;
            prolog_off = (uint32_t)(pc - start);
        } else if (depth == 0) {
            in_prolog = 0;
            if (try_epilog((const uint8_t *)pc, base, fn, info, ctx)) {
                if (establisher) *establisher = frame;
                if (handler_data) *handler_data = NULL;
                return NULL;
            }
        }

        for (int i = 0; i < info->count; i += code_slots(info->codes[i], version)) {
            uint16_t c = info->codes[i];
            if (depth == 0 && in_prolog && prolog_off < UC_OFFSET(c)) continue;   /* not executed yet */
            int r = UC_INFO(c);
            switch (UC_OP(c)) {
            case UWOP_PUSH_NONVOL:
                *reg_slot(ctx, r) = *(uint64_t *)ctx->Rsp;
                ctx->Rsp += 8;
                break;
            case UWOP_ALLOC_LARGE:
                if (r) ctx->Rsp += info->codes[i + 1] | ((uint32_t)info->codes[i + 2] << 16);
                else ctx->Rsp += (uint32_t)info->codes[i + 1] * 8;
                break;
            case UWOP_ALLOC_SMALL:
                ctx->Rsp += (uint32_t)r * 8 + 8;
                break;
            case UWOP_SET_FPREG:
                ctx->Rsp = frame;
                if (establisher) *establisher = frame;
                break;
            case UWOP_SAVE_NONVOL:
                *reg_slot(ctx, r) = *(uint64_t *)(frame + (uint32_t)info->codes[i + 1] * 8);
                break;
            case UWOP_SAVE_NONVOL_FAR:
                *reg_slot(ctx, r) = *(uint64_t *)(frame + (info->codes[i + 1] | ((uint32_t)info->codes[i + 2] << 16)));
                break;
            case UWOP_SAVE_XMM128:
                memcpy(XMM(ctx, r), (void *)(frame + (uint32_t)info->codes[i + 1] * 16), 16);
                break;
            case UWOP_SAVE_XMM128_FAR:
                memcpy(XMM(ctx, r), (void *)(frame + (info->codes[i + 1] | ((uint32_t)info->codes[i + 2] << 16))), 16);
                break;
            case UWOP_PUSH_MACHFRAME:
                if (r) ctx->Rsp += 8;
                ctx->Rip = *(uint64_t *)ctx->Rsp;
                ctx->Rsp = *(uint64_t *)(ctx->Rsp + 24);
                machframe = 1;
                break;
            default:
                break;
            }
        }
        if (!(UI_FLAGS(info) & UNW_FLAG_CHAININFO)) break;
        fn = (RUNTIME_FUNCTION *)&info->codes[(info->count + 1) & ~1];
    }

    if (!machframe) {
        ctx->Rip = *(uint64_t *)ctx->Rsp;
        ctx->Rsp += 8;
    }
    if (handler_data) *handler_data = NULL;
    if (in_prolog || !(UI_FLAGS(info) & type)) return NULL;
    uint32_t *h = (uint32_t *)&info->codes[(info->count + 1) & ~1];
    if (handler_data) *handler_data = h + 1;
    return (void *)(base + h[0]);
}

/* One step of a stack walk. Fills dc (ControlPc, ImageBase, FunctionEntry,
 * EstablisherFrame, LanguageHandler, HandlerData) for the frame in *cur and
 * leaves *next as the caller's context. Returns 0 at the end of the stack,
 * 1 for a normal Windows frame, 2 if *cur was in LSIW code and the walk was
 * redirected (dc then describes nothing; *next is where to continue). */
static int walk_step(DWORD type, CONTEXT *cur, CONTEXT *next, DISPATCHER_CONTEXT *dc, struct host_region **crossed)
{
    *crossed = NULL;
    *next = *cur;
    if (!cur->Rip || cur->Rsp < 0x1000) return 0;
    if (!is_windows_code(cur->Rip)) {
        struct host_region *r = region_at(cur->Rsp);
        if (!r) return 0;
        *crossed = r;
        return 2;
    }
    ULONG_PTR base = 0;
    RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(cur->Rip, &base, NULL);
    dc->ControlPc = cur->Rip;
    dc->ImageBase = base;
    dc->FunctionEntry = fn;
    dc->LanguageHandler = NULL;
    dc->HandlerData = NULL;
    if (!fn) {
        /* Leaf function: no prolog, return address on top of the stack. */
        dc->EstablisherFrame = cur->Rsp;
        next->Rip = *(uint64_t *)cur->Rsp;
        next->Rsp = cur->Rsp + 8;
        if (lsiw_verbose > 2) lsiw_log("    walk_step LEAF pc=%#llx -> next.Rip=%#llx\n", (unsigned long long)cur->Rip, (unsigned long long)next->Rip);
        return 1;
    }
    ULONG_PTR est = 0;
    dc->LanguageHandler = RtlVirtualUnwind(type, base, cur->Rip, fn, next, &dc->HandlerData, &est, NULL);
    dc->EstablisherFrame = est;
    if (lsiw_verbose > 2) lsiw_log("    walk_step pc=%#llx base=%#llx fn=%p -> next.Rip=%#llx next.Rsp=%#llx est=%#llx\n", (unsigned long long)cur->Rip, (unsigned long long)base, (void*)fn, (unsigned long long)next->Rip, (unsigned long long)next->Rsp, (unsigned long long)est);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Vectored handlers, unhandled filter                                  */
/* ------------------------------------------------------------------ */

typedef LONG (WINAPI *veh_fn)(struct exc_pointers *);

struct veh { veh_fn fn; struct veh *next; int refs; };
static struct veh *g_veh, *g_vch;
static pthread_mutex_t g_veh_lock = PTHREAD_MUTEX_INITIALIZER;
static veh_fn g_unhandled_filter;

static int call_vectored(struct veh *list, EXCEPTION_RECORD *rec, CONTEXT *ctx)
{
    struct exc_pointers p = { rec, ctx };
    veh_fn fns[64];
    int n = 0;
    pthread_mutex_lock(&g_veh_lock);
    for (struct veh *v = list; v && n < 64; v = v->next) fns[n++] = v->fn;
    pthread_mutex_unlock(&g_veh_lock);
    for (int i = 0; i < n; i++)
        if (fns[i](&p) == EXCEPTION_CONTINUE_EXECUTION) return 1;
    return 0;
}

static void *add_handler(struct veh **list, ULONG first, veh_fn fn)
{
    struct veh *v = calloc(1, sizeof *v);
    v->fn = fn;
    pthread_mutex_lock(&g_veh_lock);
    if (first || !*list) { v->next = *list; *list = v; }
    else { struct veh *t = *list; while (t->next) t = t->next; t->next = v; }
    pthread_mutex_unlock(&g_veh_lock);
    return v;
}

static ULONG remove_handler(struct veh **list, void *h)
{
    pthread_mutex_lock(&g_veh_lock);
    for (struct veh **pp = list; *pp; pp = &(*pp)->next)
        if (*pp == h) { struct veh *v = *pp; *pp = v->next; pthread_mutex_unlock(&g_veh_lock); free(v); return 1; }
    pthread_mutex_unlock(&g_veh_lock);
    return 0;
}

API void *AddVectoredExceptionHandler(ULONG first, veh_fn fn) { return add_handler(&g_veh, first, fn); }
API ULONG RemoveVectoredExceptionHandler(void *h) { return remove_handler(&g_veh, h); }
API void *AddVectoredContinueHandler(ULONG first, veh_fn fn) { return add_handler(&g_vch, first, fn); }
API ULONG RemoveVectoredContinueHandler(void *h) { return remove_handler(&g_vch, h); }
API void *SetUnhandledExceptionFilter(veh_fn f) { veh_fn old = g_unhandled_filter; g_unhandled_filter = f; return (void *)old; }

static const char *exception_name(DWORD code)
{
    switch (code) {
    case STATUS_ACCESS_VIOLATION:        return "access violation";
    case STATUS_ILLEGAL_INSTRUCTION:     return "illegal instruction";
    case STATUS_INTEGER_DIVIDE_BY_ZERO:  return "integer division by zero";
    case STATUS_INTEGER_OVERFLOW:        return "integer overflow";
    case STATUS_FLOAT_DIVIDE_BY_ZERO:    return "floating-point division by zero";
    case STATUS_FLOAT_INVALID_OPERATION: return "invalid floating-point operation";
    case STATUS_STACK_OVERFLOW:          return "stack overflow";
    case STATUS_STACK_BUFFER_OVERRUN:    return "fail-fast / stack buffer overrun";
    case 0xE06D7363:                     return "unhandled C++ exception";
    case 0x20474343:                     return "unhandled C++ exception (GCC)";
    case 0xC0000017:                     return "out of memory";
    case STATUS_NONCONTINUABLE_EXCEPTION:return "noncontinuable exception";
    default:                             return "exception";
    }
}

__attribute__((noreturn))
static void die_unhandled(EXCEPTION_RECORD *rec, CONTEXT *ctx)
{
    struct lsiw_module *m = lsiw_module_from_address(rec->ExceptionAddress);
    char where[300];
    if (m) snprintf(where, sizeof where, "%s+0x%lx", m->name, (unsigned long)((uint8_t *)rec->ExceptionAddress - m->base));
    else snprintf(where, sizeof where, "%p", rec->ExceptionAddress);
    fprintf(stderr, "lsiw: unhandled %s (0x%08X) at %s", exception_name(rec->ExceptionCode), rec->ExceptionCode, where);
    if (rec->ExceptionCode == STATUS_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        fprintf(stderr, ", %s address %p", rec->ExceptionInformation[0] == 1 ? "writing" : rec->ExceptionInformation[0] == 8 ? "executing" : "reading",
                (void *)rec->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    if (lsiw_verbose)
        fprintf(stderr, "lsiw: rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
                        "lsiw: rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx rip=%016llx\n",
                (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx, (unsigned long long)ctx->Rcx,
                (unsigned long long)ctx->Rdx, (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
                (unsigned long long)ctx->Rbp, (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rip);
    fflush(stderr);
    lsiw_exit_process(rec->ExceptionCode);
}

API LONG UnhandledExceptionFilter(struct exc_pointers *p)
{
    if (g_unhandled_filter) {
        LONG r = g_unhandled_filter(p);
        if (r != EXCEPTION_CONTINUE_SEARCH) return r;
    }
    die_unhandled(p->rec, p->ctx);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                             */
/* ------------------------------------------------------------------ */

/* Search for a handler, walking from *ctx. Returns 1 if a handler said
 * "continue execution" (ctx may have been modified), 0 if none was found.
 * Handlers that take the exception call RtlUnwindEx and never return. */
static int search_frames(EXCEPTION_RECORD *rec, CONTEXT *ctx, struct host_region *self)
{
    CONTEXT cur = *ctx, next;
    DISPATCHER_CONTEXT dc;
    uintptr_t nested_frame = 0;
    for (int steps = 0; steps < 100000; steps++) {
        memset(&dc, 0, sizeof dc);
        struct host_region *crossed;
        int r = walk_step(UNW_FLAG_EHANDLER, &cur, &next, &dc, &crossed);
        if (r == 0) return 0;
        if (r == 2) {
            if (crossed == self) return 0;
            /* Nested exception raised from within a handler: continue with
             * the frames the outer operation had not searched yet. */
            if (crossed->kind == REC_UNWIND) cur = *crossed->entry;
            else if (crossed->kind == REC_CONSOLIDATE) { cross_hook(crossed, rec, 0); cur = *crossed->entry; }
            else { cur = *crossed->entry; nested_frame = crossed->top; }
            continue;
        }
        if (lsiw_verbose > 2)
            lsiw_log("  search: pc=%#llx sp=%#llx handler=%p frame=%#llx\n", (unsigned long long)cur.Rip,
                     (unsigned long long)cur.Rsp, dc.LanguageHandler, (unsigned long long)dc.EstablisherFrame);
        if (next.Rsp <= cur.Rsp && next.Rip == cur.Rip) return 0;   /* no progress */
        if (dc.LanguageHandler) {
            dc.ContextRecord = &cur;
            dc.TargetIp = 0;
            dc.HistoryTable = NULL;
            DWORD saved = rec->ExceptionFlags;
            if (nested_frame && dc.EstablisherFrame < nested_frame) rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
            CONTEXT handler_ctx = cur;
            EXCEPTION_DISPOSITION d = ((exc_handler_fn)dc.LanguageHandler)(rec, (void *)dc.EstablisherFrame, &handler_ctx, &dc);
            if (lsiw_verbose > 1) lsiw_log("  handler %p at frame %#llx returned %d\n", dc.LanguageHandler, (unsigned long long)dc.EstablisherFrame, d);
            rec->ExceptionFlags = saved & ~EXCEPTION_NESTED_CALL;
            if (d == ExceptionContinueExecution) {
                if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) {
                    EXCEPTION_RECORD r2;
                    memset(&r2, 0, sizeof r2);
                    r2.ExceptionCode = STATUS_NONCONTINUABLE_EXCEPTION;
                    r2.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
                    r2.ExceptionRecord = rec;
                    r2.ExceptionAddress = rec->ExceptionAddress;
                    die_unhandled(&r2, ctx);
                }
                return 1;
            }
            if (nested_frame && dc.EstablisherFrame >= nested_frame) nested_frame = 0;
        }
        cur = next;
    }
    return 0;
}

__attribute__((noreturn))
static void dispatch(EXCEPTION_RECORD *rec, CONTEXT *ctx)
{
    struct host_region *self = region_push(REC_DISPATCH, ctx);
    LSIW_TRACE("exception 0x%08x at %p\n", rec->ExceptionCode, rec->ExceptionAddress);

    if (call_vectored(g_veh, rec, ctx) || search_frames(rec, ctx, self)) {
        call_vectored(g_vch, rec, ctx);
        region_pop(self);
        lsiw_restore_context(ctx);
    }
    struct exc_pointers p = { rec, ctx };
    if (g_unhandled_filter) {
        LONG r = g_unhandled_filter(&p);
        if (r == EXCEPTION_CONTINUE_EXECUTION) { region_pop(self); lsiw_restore_context(ctx); }
    }
    die_unhandled(rec, ctx);
}

__attribute__((noreturn, used))
void lsiw_raise_entry(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args, CONTEXT *ctx)
{
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
    rec.ExceptionAddress = (void *)ctx->Rip;
    if (nargs > EXCEPTION_MAXIMUM_PARAMETERS) nargs = EXCEPTION_MAXIMUM_PARAMETERS;
    if (args) for (DWORD i = 0; i < nargs; i++) rec.ExceptionInformation[i] = args[i];
    else nargs = 0;
    rec.NumberParameters = nargs;
    dispatch(&rec, ctx);
}

__attribute__((noreturn, used))
void lsiw_rtlraise_entry(EXCEPTION_RECORD *rec, CONTEXT *ctx)
{
    if (!rec->ExceptionAddress) rec->ExceptionAddress = (void *)ctx->Rip;
    dispatch(rec, ctx);
}

/* For LSIW's own code (e.g. HEAP_GENERATE_EXCEPTIONS): there is no Windows
 * caller context to resume, so the exception is reported as unhandled. */
void lsiw_raise(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args)
{
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    memset(&rec, 0, sizeof rec);
    memset(&ctx, 0, sizeof ctx);
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags;
    rec.NumberParameters = nargs > EXCEPTION_MAXIMUM_PARAMETERS ? EXCEPTION_MAXIMUM_PARAMETERS : nargs;
    for (DWORD i = 0; i < rec.NumberParameters; i++) rec.ExceptionInformation[i] = args[i];
    die_unhandled(&rec, &ctx);
}

/* ------------------------------------------------------------------ */
/* Unwind                                                               */
/* ------------------------------------------------------------------ */

typedef void *(WINAPI *consolidate_fn)(EXCEPTION_RECORD *);

__attribute__((noreturn))
static void finish_unwind(CONTEXT *ctx, EXCEPTION_RECORD *rec, struct host_region *self)
{
    if (rec && rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 1) {
        /* Call the consolidation callback (e.g. a C++ catch block) right
         * here, below everything, as Windows does: the frames between the
         * target and the throw point stay intact (the exception object
         * lives there). It returns where execution continues. A walk that
         * reaches this LSIW code resumes in the target frame. */
        self->kind = REC_CONSOLIDATE;
        self->entry = ctx;
        self->top = ctx->Rsp;
        self->hook = NULL;
        consolidate_fn cb = (consolidate_fn)rec->ExceptionInformation[0];
        void *ip = cb(rec);
        ctx->Rip = (uint64_t)(uintptr_t)ip;
    }
    region_pop(self);
    /* Everything recorded at or below the resumed frame's stack pointer
     * belonged to host frames that are now gone (e.g. a catch block that
     * was abandoned for a handler in the same frame). */
    while (t_nregions && t_regions[t_nregions - 1].top <= ctx->Rsp) t_nregions--;
    lsiw_restore_context(ctx);
}

__attribute__((noreturn, used))
void lsiw_unwind_entry(void *target_frame, void *target_ip, EXCEPTION_RECORD *rec, void *retval,
                       CONTEXT *ctx, CONTEXT *start)
{
    EXCEPTION_RECORD local_rec;
    CONTEXT local_ctx;
    if (!rec) {
        memset(&local_rec, 0, sizeof local_rec);
        local_rec.ExceptionCode = STATUS_UNWIND;
        local_rec.ExceptionAddress = (void *)start->Rip;
        rec = &local_rec;
    }
    if (!ctx) ctx = &local_ctx;
    rec->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (!target_frame) rec->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;

    if (lsiw_verbose > 1) lsiw_log("  RtlUnwindEx: target_frame=%p target_ip=%p from Rip=%#llx Rsp=%#llx code=%#x\n", target_frame, target_ip, (unsigned long long)start->Rip, (unsigned long long)start->Rsp, rec->ExceptionCode);
    struct host_region *self = region_push(REC_UNWIND, start);
    DISPATCHER_CONTEXT dc;
    memset(&dc, 0, sizeof dc);
    self->cur = ctx;
    self->dc = &dc;

    *ctx = *start;
    CONTEXT next;
    int collided = 0;
    for (int steps = 0; steps < 100000; steps++) {
        struct host_region *crossed = NULL;
        if (!collided) {
            memset(&dc, 0, sizeof dc);
            int r = walk_step(UNW_FLAG_UHANDLER, ctx, &next, &dc, &crossed);
            if (lsiw_verbose > 2) lsiw_log("  uw-walk: r=%d rsp=%#llx crossed=%p kind=%d self=%p nreg=%d\n", r, (unsigned long long)ctx->Rsp, (void*)crossed, crossed?crossed->kind:-9, (void*)self, t_nregions);
            if (r == 0) break;
            if (r == 2) {
                if (crossed == self && !(crossed = region_outside(self, ctx->Rsp))) break;
                if (crossed->kind == REC_UNWIND && crossed->dc && crossed->cur) {
                    /* Collided unwind: take over the frame the interrupted
                     * unwind was processing and call its handler again. */
                    *ctx = *crossed->cur;
                    dc = *crossed->dc;
                    dc.ContextRecord = ctx;
                    next = *ctx;
                    {
                        ULONG_PTR est;
                        if (dc.FunctionEntry) RtlVirtualUnwind(UNW_FLAG_NHANDLER, dc.ImageBase, dc.ControlPc, dc.FunctionEntry, &next, NULL, &est, NULL);
                        else { next.Rip = *(uint64_t *)ctx->Rsp; next.Rsp = ctx->Rsp + 8; }
                    }
                    rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                    collided = 1;
                    /* Drop the records the collided operation owned. */
                    t_nregions = (int)(crossed - t_regions) + 0;
                    self = region_push(REC_UNWIND, start);
                    self->cur = ctx;
                    self->dc = &dc;
                } else {
                    cross_hook(crossed, rec, 1);
                    *ctx = *crossed->entry;
                    continue;
                }
            }
        } else {
            collided = 0;
        }

        if (target_frame && dc.EstablisherFrame > (ULONG_PTR)target_frame) {
            fprintf(stderr, "lsiw: invalid unwind target %p (frame %p)\n", target_frame, (void *)dc.EstablisherFrame);
            break;
        }
        if (lsiw_verbose > 2)
            lsiw_log("  uw-step: rip=%#llx est=%#llx handler=%p tgt=%p collided=%d\n",
                     (unsigned long long)ctx->Rip, (unsigned long long)dc.EstablisherFrame, dc.LanguageHandler, target_frame, collided);
        if (dc.LanguageHandler) {
            dc.TargetIp = (ULONG_PTR)target_ip;
            dc.ContextRecord = ctx;
            if (dc.EstablisherFrame == (ULONG_PTR)target_frame) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
            EXCEPTION_DISPOSITION d = ((exc_handler_fn)dc.LanguageHandler)(rec, (void *)dc.EstablisherFrame, ctx, &dc);
            rec->ExceptionFlags &= ~(EXCEPTION_TARGET_UNWIND | EXCEPTION_COLLIDED_UNWIND);
            if (d == ExceptionCollidedUnwind) {
                /* The handler copied an older unwind's state into dc. */
                *ctx = *dc.ContextRecord;
                dc.ContextRecord = ctx;
                next = *ctx;
                ULONG_PTR est;
                if (dc.FunctionEntry) RtlVirtualUnwind(UNW_FLAG_NHANDLER, dc.ImageBase, dc.ControlPc, dc.FunctionEntry, &next, NULL, &est, NULL);
                rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                collided = 1;
                continue;
            }
        }
        if (dc.EstablisherFrame == (ULONG_PTR)target_frame && target_frame) break;
        *ctx = next;
    }

    if (!target_frame) {
        /* Exit unwind: the stack is gone; end the thread. */
        void (WINAPI *exit_thread)(DWORD) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "ExitThread");
        exit_thread(0);
    }
    ctx->Rax = (uint64_t)(uintptr_t)retval;
    ctx->Rip = (uint64_t)(uintptr_t)target_ip;
    finish_unwind(ctx, rec, self);
}

/* ------------------------------------------------------------------ */
/* __C_specific_handler: __try / __except / __finally                   */
/* ------------------------------------------------------------------ */

typedef struct { DWORD Begin, End, Handler, Target; } SCOPE_RECORD;
typedef struct { DWORD Count; SCOPE_RECORD Entry[]; } SCOPE_TABLE;
typedef LONG (WINAPI *filter_fn)(struct exc_pointers *, void *frame);
typedef void (WINAPI *termination_fn)(BOOL abnormal, void *frame);

API EXCEPTION_DISPOSITION __C_specific_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    SCOPE_TABLE *st = dc->HandlerData;
    ULONG_PTR base = dc->ImageBase;
    ULONG_PTR pc = dc->ControlPc - base;

    if (!(rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))) {
        for (DWORD i = dc->ScopeIndex; i < st->Count; i++) {
            SCOPE_RECORD *e = &st->Entry[i];
            if (pc < e->Begin || pc >= e->End || !e->Target) continue;   /* not an __except */
            LONG r;
            if (e->Handler == EXCEPTION_EXECUTE_HANDLER) r = EXCEPTION_EXECUTE_HANDLER;
            else {
                struct exc_pointers p = { rec, ctx };
                r = ((filter_fn)(base + e->Handler))(&p, frame);
            }
            if (r < 0) return ExceptionContinueExecution;
            if (r == 0) continue;
            /* Execute this handler: unwind to the __except block. */
            dc->ScopeIndex = i + 1;
            lsiw_RtlUnwindEx(frame, (void *)(base + e->Target), rec, (void *)(uintptr_t)rec->ExceptionCode, dc->ContextRecord, dc->HistoryTable);
        }
        return ExceptionContinueSearch;
    }

    ULONG_PTR target = dc->TargetIp - base;
    for (DWORD i = dc->ScopeIndex; i < st->Count; i++) {
        SCOPE_RECORD *e = &st->Entry[i];
        if (pc < e->Begin || pc >= e->End) continue;
        if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && e->Target) {
            /* Stop at the __except we are unwinding to (or one enclosing it). */
            if (target >= e->Begin && target < e->End) return ExceptionContinueSearch;
            if (target == e->Target) return ExceptionContinueSearch;
        }
        if (!e->Target) {
            /* __finally */
            dc->ScopeIndex = i + 1;
            ((termination_fn)(base + e->Handler))(TRUE, frame);
        }
    }
    return ExceptionContinueSearch;
}

/* /GS: the cookie check is done inline; nothing to do at dispatch time
 * except forward to the C handler when the function also has __try. */
API EXCEPTION_DISPOSITION __GSHandlerCheck(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{ (void)rec; (void)frame; (void)ctx; (void)dc; return ExceptionContinueSearch; }

API EXCEPTION_DISPOSITION __GSHandlerCheck_SEH(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{ return __C_specific_handler(rec, frame, ctx, dc); }

/* ------------------------------------------------------------------ */
/* CPU faults                                                           */
/* ------------------------------------------------------------------ */

static void context_from_uc(CONTEXT *c, const ucontext_t *uc)
{
    memset(c, 0, sizeof *c);
    c->ContextFlags = CONTEXT_FULL;
    const greg_t *g = uc->uc_mcontext.gregs;
    c->Rax = g[REG_RAX]; c->Rcx = g[REG_RCX]; c->Rdx = g[REG_RDX]; c->Rbx = g[REG_RBX];
    c->Rsp = g[REG_RSP]; c->Rbp = g[REG_RBP]; c->Rsi = g[REG_RSI]; c->Rdi = g[REG_RDI];
    c->R8 = g[REG_R8]; c->R9 = g[REG_R9]; c->R10 = g[REG_R10]; c->R11 = g[REG_R11];
    c->R12 = g[REG_R12]; c->R13 = g[REG_R13]; c->R14 = g[REG_R14]; c->R15 = g[REG_R15];
    c->Rip = g[REG_RIP];
    c->EFlags = (uint32_t)g[REG_EFL];
    c->SegCs = 0x33; c->SegSs = 0x2b;
    if (uc->uc_mcontext.fpregs) {
        memcpy(c->FltSave, uc->uc_mcontext.fpregs, 512);
        c->MxCsr = uc->uc_mcontext.fpregs->mxcsr;
    } else c->MxCsr = 0x1f80;
}

struct fault_frame {
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
};

/* Runs on the faulting thread's own stack after the signal handler has
 * returned, so handlers execute in a normal (non-signal) context. */
__attribute__((noreturn, used))
static void fault_dispatch(struct fault_frame *f)
{
    dispatch(&f->rec, &f->ctx);
}

static __thread int t_fault_depth;

static void fatal_write(const char *s) { ssize_t r = write(2, s, strlen(s)); (void)r; }

static void fault_handler(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    uint8_t *teb = lsiw_teb_current();
    if (!teb || ++t_fault_depth > 16) {
        fatal_write("lsiw: fatal fault inside the runtime\n");
        signal(sig, SIG_DFL);
        return;
    }
    greg_t *g = uc->uc_mcontext.gregs;
    uintptr_t rsp = (uintptr_t)g[REG_RSP];
    const uint8_t *ip = (const uint8_t *)g[REG_RIP];
    uintptr_t limit = *(uintptr_t *)(teb + 0x10);

    /* __fastfail (int 0x29) terminates immediately on Windows. */
    if (sig == SIGSEGV && ip && ip[0] == 0xCD && ip[1] == 0x29) {
        fatal_write("lsiw: the program requested immediate termination (__fastfail)\n");
        _exit(0x409 & 0xff);
    }
    /* Stack overflow: no room to run handlers on this stack. */
    if (sig == SIGSEGV && (uintptr_t)si->si_addr < rsp + 0x1000 && (uintptr_t)si->si_addr + 0x20000 > rsp &&
        (!limit || (uintptr_t)si->si_addr < limit + 0x10000)) {
        fatal_write("lsiw: unhandled stack overflow (0xC00000FD)\n");
        _exit(0xFD);
    }

    /* Build the record and context below the faulting frame, leaving room
     * for a System V red zone in case the fault was in LSIW's own code. */
    uintptr_t sp = (rsp - 128 - sizeof(struct fault_frame)) & ~(uintptr_t)63;
    struct fault_frame *f = (struct fault_frame *)sp;
    memset(&f->rec, 0, sizeof f->rec);
    context_from_uc(&f->ctx, uc);
    f->rec.ExceptionAddress = (void *)f->ctx.Rip;
    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        unsigned long err = (unsigned long)g[REG_ERR];
        f->rec.ExceptionCode = STATUS_ACCESS_VIOLATION;
        f->rec.NumberParameters = 2;
        f->rec.ExceptionInformation[0] = (err & 0x10) ? 8 : (err & 2) ? 1 : 0;
        f->rec.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
        if (sig == SIGBUS && si->si_code == BUS_ADRALN) {
            f->rec.ExceptionCode = STATUS_DATATYPE_MISALIGNMENT;
            f->rec.NumberParameters = 0;
        }
        break; }
    case SIGFPE:
        switch (si->si_code) {
        case FPE_INTDIV: f->rec.ExceptionCode = STATUS_INTEGER_DIVIDE_BY_ZERO; break;
        case FPE_INTOVF: f->rec.ExceptionCode = STATUS_INTEGER_OVERFLOW; break;
        case FPE_FLTDIV: f->rec.ExceptionCode = STATUS_FLOAT_DIVIDE_BY_ZERO; break;
        case FPE_FLTOVF: f->rec.ExceptionCode = STATUS_FLOAT_OVERFLOW; break;
        case FPE_FLTUND: f->rec.ExceptionCode = STATUS_FLOAT_UNDERFLOW; break;
        case FPE_FLTRES: f->rec.ExceptionCode = STATUS_FLOAT_INEXACT_RESULT; break;
        default:         f->rec.ExceptionCode = STATUS_FLOAT_INVALID_OPERATION; break;
        }
        break;
    case SIGTRAP:
        /* int3 leaves Rip after the 0xCC; Windows reports the int3 itself. */
        if (si->si_code == SI_KERNEL && ip && ip[-1] == 0xCC) {
            f->ctx.Rip--;
            f->rec.ExceptionAddress = (void *)f->ctx.Rip;
            f->rec.ExceptionCode = 0x80000003;          /* STATUS_BREAKPOINT */
            f->rec.NumberParameters = 1;
        } else if (si->si_code == TRAP_TRACE) {
            f->rec.ExceptionCode = 0x80000004;          /* STATUS_SINGLE_STEP */
        } else {
            f->rec.ExceptionCode = 0x80000003;
            f->rec.NumberParameters = 1;
        }
        break;
    case SIGILL:
    default:
        f->rec.ExceptionCode = STATUS_ILLEGAL_INSTRUCTION;
        break;
    }

    /* Return from the signal into fault_dispatch(f) on the same stack. */
    sp -= 8;
    *(uint64_t *)sp = 0;                  /* fake return address */
    g[REG_RSP] = (greg_t)sp;
    g[REG_RDI] = (greg_t)(uintptr_t)f;
    g[REG_RIP] = (greg_t)(uintptr_t)fault_dispatch;
    t_fault_depth--;
}

void lsiw_seh_thread_init(void)
{
    stack_t ss;
    size_t size = 64 * 1024;
    ss.ss_sp = malloc(size);
    ss.ss_size = size;
    ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}

int lsiw_fault_init(void)
{
    lsiw_seh_thread_init();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    return 0;
}

int lsiw_seh_init(void) { return lsiw_fault_init(); }

/* ------------------------------------------------------------------ */
/* Misc ntdll exception helpers                                         */
/* ------------------------------------------------------------------ */

API WORD RtlCaptureStackBackTrace(ULONG skip, ULONG count, void **frames, ULONG *hash)
{
    CONTEXT ctx, next;
    lsiw_RtlCaptureContext(&ctx);
    /* Start the walk at our Windows caller: its return address and the
     * stack pointer it had before the call (the frame pointer this function
     * keeps gives both, plus the caller's Rbp for frames that use it). */
    uintptr_t *fp = __builtin_frame_address(0);
    ctx.Rip = (uint64_t)(uintptr_t)__builtin_return_address(0);
    ctx.Rsp = (uint64_t)(uintptr_t)(fp + 2);
    ctx.Rbp = (uint64_t)fp[0];
    ULONG n = 0, h = 0;
    DISPATCHER_CONTEXT dc;
    for (ULONG i = 0; n < count && i < 256; i++) {
        struct host_region *crossed;
        memset(&dc, 0, sizeof dc);
        int r = walk_step(UNW_FLAG_NHANDLER, &ctx, &next, &dc, &crossed);
        if (r == 0) break;
        if (r == 2) {
            /* Our own caller chain starts in LSIW code: skip to the first
             * Windows frame by scanning for a plausible return address. */
            if (crossed) { ctx = *crossed->entry; continue; }
            break;
        }
        if (i >= skip) { frames[n++] = (void *)ctx.Rip; h += (ULONG)ctx.Rip; }
        ctx = next;
    }
    if (hash) *hash = h;
    return (WORD)n;
}


/* ------------------------------------------------------------------ */
/* Export table                                                         */
/* ------------------------------------------------------------------ */

const struct lsiw_export lsiw_seh_exports[] = {
    EX("RtlCaptureContext", lsiw_RtlCaptureContext),
    EX("RaiseException", lsiw_RaiseException),
    EX("RtlRaiseException", lsiw_RtlRaiseException),
    EX("RtlUnwindEx", lsiw_RtlUnwindEx),
    EX("RtlUnwind", lsiw_RtlUnwind),
    E(RtlRestoreContext), E(RtlLookupFunctionEntry), E(RtlVirtualUnwind),
    E(RtlAddFunctionTable), E(RtlDeleteFunctionTable), E(RtlInstallFunctionTableCallback),
    E(RtlAddGrowableFunctionTable), E(RtlDeleteGrowableFunctionTable), E(RtlGrowFunctionTable),
    E(RtlPcToFileHeader), E(RtlCaptureStackBackTrace), EX("CaptureStackBackTrace", RtlCaptureStackBackTrace),
    E(__C_specific_handler), E(__GSHandlerCheck), E(__GSHandlerCheck_SEH),
    E(AddVectoredExceptionHandler), E(RemoveVectoredExceptionHandler),
    E(AddVectoredContinueHandler), E(RemoveVectoredContinueHandler),
    E(SetUnhandledExceptionFilter), E(UnhandledExceptionFilter),
    { NULL, NULL }
};
