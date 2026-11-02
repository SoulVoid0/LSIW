/* vcruntime140.dll / vcruntime140_1.dll: the Microsoft C++ runtime support
 * that MSVC-built programs import: C++ exceptions (_CxxThrowException and
 * the __CxxFrameHandler3/__CxxFrameHandler4 language handlers), RTTI
 * (typeid, dynamic_cast), type_info and std::exception helpers.
 *
 * Written from the documented x64 exception model and the published layout
 * of MSVC's exception and RTTI data (ThrowInfo, CatchableType, FuncInfo,
 * the compressed FH4 format, RTTI complete object locators). It sits on
 * LSIW's own unwinder (seh.c): a catch block runs as the consolidation
 * callback of RtlUnwindEx(STATUS_UNWIND_CONSOLIDATE), exactly as on
 * Windows, and a hook on that host region tells this file when a new
 * exception leaves a catch block or an unwind abandons one.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "seh.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define CXX_MAGIC_VC6  0x19930520u
#define CXX_MAGIC_VC7  0x19930521u
#define CXX_MAGIC_VC8  0x19930522u
#define CXX_MAGIC_PURE 0x01994000u

/* ------------------------------------------------------------------ */
/* Exception and RTTI data (image-relative offsets on x64)              */
/* ------------------------------------------------------------------ */

typedef struct { const void *vtable; char *name; char mangled[1]; } type_info;      /* TypeDescriptor */
typedef struct { int this_offset, vbase_descr, vbase_offset; } this_ptr_offsets;   /* PMD */

#define CLASS_IS_SIMPLE_TYPE      1
#define CLASS_BY_REFERENCE_ONLY   2
#define CLASS_HAS_VIRTUAL_BASE    4

typedef struct {                       /* CatchableType */
    UINT flags;
    int  type_info;
    this_ptr_offsets offsets;
    int  size;
    int  copy_ctor;
} cxx_type_info;

typedef struct { int count; int info[]; } cxx_type_info_table;   /* CatchableTypeArray */

#define TYPE_FLAG_CONST     1
#define TYPE_FLAG_VOLATILE  2
#define TYPE_FLAG_UNALIGNED 4
#define TYPE_FLAG_REFERENCE 8
#define TYPE_FLAG_PURE      8          /* in ThrowInfo.flags */

typedef struct {                       /* ThrowInfo */
    UINT flags;
    int  destructor;
    int  custom_handler;
    int  type_info_table;
} cxx_exception_type;

/* FH3 (__CxxFrameHandler3) function descriptor */
typedef struct { UINT ip; int state; } ipmap_info;
typedef struct { UINT flags; int type_info; int offset; int handler; int frame; } catchblock3;
typedef struct { int start, end, catch_high; UINT ncatch; int catchblock; } tryblock3;
typedef struct { int prev; int handler; } unwind3;
typedef struct {
    UINT magic_bbt;                    /* magic:29, bbt:3 */
    UINT unwind_count;
    int  unwind_table;
    UINT tryblock_count;
    int  tryblock;
    UINT ipmap_count;
    int  ipmap;
    int  unwind_help;                  /* frame offset of the UnwindHelp slot */
    int  expect_list;
    UINT flags;
} funcinfo3;
#define FI3_EHS      1                 /* /EHs: synchronous exceptions only */
#define FI3_NOEXCEPT 4

/* RTTI */
typedef struct { UINT signature; int base_class_offset; int cd_offset; int type_descriptor; int type_hierarchy; int object_locator; } rtti_locator;
typedef struct { UINT signature; UINT attributes; UINT array_len; int base_classes; } rtti_hierarchy;
typedef struct { int type_descriptor; UINT num_base_classes; this_ptr_offsets offsets; UINT attributes; int type_hierarchy; } rtti_base;

#define RVA(base, off) ((void *)((uintptr_t)(base) + (intptr_t)(off)))

/* ------------------------------------------------------------------ */
/* Per-thread state                                                     */
/* ------------------------------------------------------------------ */

typedef void (WINAPI *se_translator_fn)(UINT, struct exc_pointers *);
typedef void (WINAPI *terminate_fn)(void);

/* The field order follows vcruntime's per-thread data, so code that finds
 * the translator two slots before __current_exception() still works. */
struct cxx_thread {
    se_translator_fn translator;
    void *purecall;
    EXCEPTION_RECORD *cur_rec;         /* __current_exception          */
    CONTEXT *cur_ctx;                  /* __current_exception_context  */
    int processing_throw;              /* __processing_throw           */
    int fh4_state;                     /* FH4 state handed to the next handler, -2 = none */
    struct { DWORD code, flags, nargs, pad; ULONG_PTR args[EXCEPTION_MAXIMUM_PARAMETERS]; } req;
};

static __thread struct cxx_thread t_cxx = { .fh4_state = -2 };

static terminate_fn g_terminate, g_unexpected;

__attribute__((noreturn)) static void cxx_terminate(void)
{
    terminate_fn f = __atomic_load_n(&g_terminate, __ATOMIC_ACQUIRE);
    if (f) f();
    lsiw_crt_flush_all();
    fprintf(stderr, "lsiw: terminate() was called (an exception was not caught, or left a noexcept function or a destructor)\n");
    lsiw_exit_process(3);
}

static int is_cxx_exception(const EXCEPTION_RECORD *rec)
{
    return rec && rec->ExceptionCode == CXX_EXCEPTION_CODE && rec->NumberParameters >= 3 &&
           ((rec->ExceptionInformation[0] >= CXX_MAGIC_VC6 && rec->ExceptionInformation[0] <= CXX_MAGIC_VC8) ||
            rec->ExceptionInformation[0] == CXX_MAGIC_PURE);
}

static uintptr_t exc_base(const EXCEPTION_RECORD *rec)
{
    return rec->NumberParameters >= 4 ? rec->ExceptionInformation[3] : 0;
}

/* ------------------------------------------------------------------ */
/* Calling Windows code                                                 */
/* ------------------------------------------------------------------ */

typedef void *(WINAPI *funclet_fn)(void *self, ULONG_PTR frame, ULONG notify);
typedef void (WINAPI *dtor_fn)(void *obj);
typedef void (WINAPI *copy_fn)(void *dst, const void *src);
typedef void (WINAPI *copy_vb_fn)(void *dst, const void *src, int most_derived);
typedef void (WINAPI *fh4_dtor_fn)(void *obj, ULONG_PTR frame);

/* Catch and unwind funclets take the parent's establisher frame in RDX. */
static void *call_funclet(void *fn, ULONG_PTR frame)
{
    return ((funclet_fn)fn)(fn, frame, 0x100);
}

static void *adjust_pointer(void *obj, const this_ptr_offsets *off)
{
    if (!obj) return NULL;
    char *p = obj;
    if (off->vbase_descr >= 0) {
        char *vbtable = *(char **)(p + off->vbase_descr);
        p += off->vbase_descr + *(int *)(vbtable + off->vbase_offset);
    }
    return p + off->this_offset;
}

static void destroy_object(EXCEPTION_RECORD *rec)
{
    if (!is_cxx_exception(rec)) return;
    void *obj = (void *)rec->ExceptionInformation[1];
    const cxx_exception_type *ti = (const void *)rec->ExceptionInformation[2];
    if (obj && ti && ti->destructor) ((dtor_fn)RVA(exc_base(rec), ti->destructor))(obj);
}

/* ------------------------------------------------------------------ */
/* Type matching and the catch object                                   */
/* ------------------------------------------------------------------ */

static const cxx_type_info *find_caught_type(const cxx_exception_type *et, uintptr_t base,
                                             const type_info *catch_ti, UINT catch_flags)
{
    const cxx_type_info_table *tab = RVA(base, et->type_info_table);
    for (int i = 0; i < tab->count; i++) {
        const cxx_type_info *t = RVA(base, tab->info[i]);
        const type_info *ti = RVA(base, t->type_info);
        if (!catch_ti || !catch_ti->mangled[0]) return t;          /* catch (...) */
        if (catch_ti != ti && strcmp(catch_ti->mangled, ti->mangled)) continue;
        if ((t->flags & CLASS_BY_REFERENCE_ONLY) && !(catch_flags & TYPE_FLAG_REFERENCE)) continue;
        if ((et->flags & TYPE_FLAG_CONST) && !(catch_flags & TYPE_FLAG_CONST)) continue;
        if ((et->flags & TYPE_FLAG_VOLATILE) && !(catch_flags & TYPE_FLAG_VOLATILE)) continue;
        if ((et->flags & TYPE_FLAG_UNALIGNED) && !(catch_flags & TYPE_FLAG_UNALIGNED)) continue;
        return t;
    }
    return NULL;
}

/* Copy (or reference) the thrown object into the catch block's variable. */
static void build_catch_object(void *obj, void *dest, UINT catch_flags, const cxx_type_info *t, uintptr_t base)
{
    if (catch_flags & TYPE_FLAG_REFERENCE) {
        *(void **)dest = adjust_pointer(obj, &t->offsets);
    } else if (t->flags & CLASS_IS_SIMPLE_TYPE) {
        memmove(dest, obj, (size_t)t->size);
        if (t->size == sizeof(void *) && *(void **)dest)
            *(void **)dest = adjust_pointer(*(void **)dest, &t->offsets);
    } else if (t->copy_ctor) {
        void *src = adjust_pointer(obj, &t->offsets);
        if (t->flags & CLASS_HAS_VIRTUAL_BASE) ((copy_vb_fn)RVA(base, t->copy_ctor))(dest, src, 1);
        else ((copy_fn)RVA(base, t->copy_ctor))(dest, src);
    } else {
        memmove(dest, adjust_pointer(obj, &t->offsets), (size_t)t->size);
    }
}

/* ------------------------------------------------------------------ */
/* Running a catch block                                                */
/* ------------------------------------------------------------------ */

/* The consolidation record passed to RtlUnwindEx. Slots 6 and 7 hold the
 * original record and context, where some programs look for them. */
enum {
    CR_CALLBACK, CR_FRAME, CR_SEARCH_STATE, CR_UNWIND_STATE, CR_UNUSED4, CR_HANDLER,
    CR_PREV_REC, CR_CONTEXT, CR_RET0, CR_RET1, CR_KIND, CR_UNWIND_HELP, CR_COUNT
};
enum { KIND_FH3 = 3, KIND_FH4 = 4 };

struct catch_ctx {
    EXCEPTION_RECORD *prev_rec;        /* the exception being handled */
    EXCEPTION_RECORD *saved_rec;       /* previous "current exception" */
    CONTEXT *saved_ctx;
    int rethrow;                       /* the same object left the catch block */
    int kind;
    int search_state, unwind_state;
    int done;
};

static void catch_cleanup(struct catch_ctx *c)
{
    if (c->done) return;
    c->done = 1;
    if (!c->rethrow) destroy_object(c->prev_rec);
    t_cxx.cur_rec = c->saved_rec;
    t_cxx.cur_ctx = c->saved_ctx;
}

/* Called when a stack walk crosses the catch block's host region. */
static void catch_hook(void *arg, EXCEPTION_RECORD *rec, int unwinding)
{
    struct catch_ctx *c = arg;
    if (!unwinding) {
        /* A new exception is leaving the catch block; the parent frame is
         * searched next, from the catch's state so the same try does not
         * catch again. */
        if (c->kind == KIND_FH4) t_cxx.fh4_state = c->search_state;
        if (is_cxx_exception(rec) && is_cxx_exception(c->prev_rec) &&
            rec->ExceptionInformation[1] == c->prev_rec->ExceptionInformation[1])
            c->rethrow = 1;
    } else {
        /* The catch block is being abandoned by an unwind. */
        if (c->kind == KIND_FH4) t_cxx.fh4_state = c->unwind_state;
        catch_cleanup(c);
    }
}

static void *WINAPI call_catch_block(EXCEPTION_RECORD *rec)
{
    struct catch_ctx c;
    memset(&c, 0, sizeof c);
    c.prev_rec = (EXCEPTION_RECORD *)rec->ExceptionInformation[CR_PREV_REC];
    c.kind = (int)rec->ExceptionInformation[CR_KIND];
    c.search_state = (int)rec->ExceptionInformation[CR_SEARCH_STATE];
    c.unwind_state = (int)rec->ExceptionInformation[CR_UNWIND_STATE];
    c.saved_rec = t_cxx.cur_rec;
    c.saved_ctx = t_cxx.cur_ctx;
    t_cxx.cur_rec = c.prev_rec;
    t_cxx.cur_ctx = (CONTEXT *)rec->ExceptionInformation[CR_CONTEXT];
    t_cxx.processing_throw--;
    lsiw_seh_set_consolidate_hook(catch_hook, &c);

    void *ret = call_funclet((void *)rec->ExceptionInformation[CR_HANDLER], rec->ExceptionInformation[CR_FRAME]);

    catch_cleanup(&c);
    if (c.kind == KIND_FH3) {
        int *uh = (int *)rec->ExceptionInformation[CR_UNWIND_HELP];
        if (uh) { uh[0] = -2; uh[1] = -1; }
    } else {
        t_cxx.fh4_state = -2;
        if (rec->ExceptionInformation[CR_RET1]) {
            /* Two continuations: the funclet returns which one. */
            if ((uintptr_t)ret > 1) cxx_terminate();
            return (void *)rec->ExceptionInformation[CR_RET0 + (uintptr_t)ret];
        }
        if (rec->ExceptionInformation[CR_RET0]) return (void *)rec->ExceptionInformation[CR_RET0];
    }
    return ret;
}

static int is_catch_consolidate(const EXCEPTION_RECORD *rec)
{
    return rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters == CR_COUNT &&
           rec->ExceptionInformation[CR_CALLBACK] == (ULONG_PTR)call_catch_block;
}

__attribute__((noreturn))
static void unwind_to_catch(EXCEPTION_RECORD *rec, CONTEXT *ctx, ULONG_PTR frame, DISPATCHER_CONTEXT *dc,
                            ULONG_PTR orig_frame, int kind, int start, int catch_high, void *handler,
                            ULONG_PTR ret0, ULONG_PTR ret1, int *unwind_help)
{
    EXCEPTION_RECORD cr;
    CONTEXT uctx;
    memset(&cr, 0, sizeof cr);
    cr.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
    cr.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    cr.NumberParameters = CR_COUNT;
    cr.ExceptionInformation[CR_CALLBACK] = (ULONG_PTR)call_catch_block;
    cr.ExceptionInformation[CR_FRAME] = orig_frame;
    cr.ExceptionInformation[CR_SEARCH_STATE] = (ULONG_PTR)(intptr_t)catch_high;
    cr.ExceptionInformation[CR_UNWIND_STATE] = (ULONG_PTR)(intptr_t)start;
    cr.ExceptionInformation[CR_HANDLER] = (ULONG_PTR)handler;
    cr.ExceptionInformation[CR_PREV_REC] = (ULONG_PTR)rec;
    cr.ExceptionInformation[CR_CONTEXT] = (ULONG_PTR)ctx;
    cr.ExceptionInformation[CR_RET0] = ret0;
    cr.ExceptionInformation[CR_RET1] = ret1;
    cr.ExceptionInformation[CR_KIND] = (ULONG_PTR)kind;
    cr.ExceptionInformation[CR_UNWIND_HELP] = (ULONG_PTR)unwind_help;
    lsiw_RtlUnwindEx((void *)frame, (void *)dc->ControlPc, &cr, NULL, &uctx, NULL);
    cxx_terminate();
}

/* ------------------------------------------------------------------ */
/* __CxxFrameHandler3                                                   */
/* ------------------------------------------------------------------ */

static int ip_to_state3(const funcinfo3 *fi, uintptr_t ip, uintptr_t base)
{
    const ipmap_info *map = RVA(base, fi->ipmap);
    UINT i;
    for (i = 0; i < fi->ipmap_count; i++) if (base + map[i].ip > ip) break;
    return i ? map[i - 1].state : -1;
}

static void local_unwind3(ULONG_PTR frame, DISPATCHER_CONTEXT *dc, const funcinfo3 *fi, int last)
{
    const unwind3 *table = RVA(dc->ImageBase, fi->unwind_table);
    int *uh = (int *)(frame + fi->unwind_help);
    int state = uh[0];
    if (state == -2) state = ip_to_state3(fi, dc->ControlPc, dc->ImageBase);
    while (state > last) {
        if (state < 0 || state >= (int)fi->unwind_count) cxx_terminate();
        if (table[state].handler) call_funclet(RVA(dc->ImageBase, table[state].handler), frame);
        state = table[state].prev;
    }
    uh[0] = state;
}

static void find_catch3(EXCEPTION_RECORD *rec, CONTEXT *ctx, ULONG_PTR frame, DISPATCHER_CONTEXT *dc,
                        const funcinfo3 *fi, const cxx_exception_type *et, ULONG_PTR orig_frame)
{
    uintptr_t base = dc->ImageBase;
    const tryblock3 *tbs = RVA(base, fi->tryblock);
    int state = ip_to_state3(fi, dc->ControlPc, base);
    const tryblock3 *in_catch = NULL;

    for (UINT i = fi->tryblock_count; i > 0; i--)
        if (state > tbs[i - 1].end && state <= tbs[i - 1].catch_high) { in_catch = &tbs[i - 1]; break; }

    /* UnwindHelp[1] remembers the highest state searched in this frame, so
     * revisiting the parent from inside its catch block starts there. */
    int *uh = (int *)(orig_frame + fi->unwind_help);
    if (state > uh[1]) uh[0] = uh[1] = state;
    else state = uh[1];

    t_cxx.processing_throw++;
    for (UINT i = 0; i < fi->tryblock_count; i++) {
        const tryblock3 *tb = &tbs[i];
        if (state < tb->start || state > tb->end) continue;
        if (in_catch && (tb->start <= in_catch->end || tb->end > in_catch->catch_high)) continue;
        const catchblock3 *cbs = RVA(base, tb->catchblock);
        for (UINT j = 0; j < tb->ncatch; j++) {
            const catchblock3 *cb = &cbs[j];
            const type_info *cti = cb->type_info ? RVA(base, cb->type_info) : NULL;
            if (et) {
                const cxx_type_info *t = find_caught_type(et, exc_base(rec), cti, cb->flags);
                if (!t) continue;
                if (cti && cti->mangled[0] && cb->offset)
                    build_catch_object((void *)rec->ExceptionInformation[1], (void *)(orig_frame + cb->offset),
                                       cb->flags, t, exc_base(rec));
            } else if (cti && cti->mangled[0]) {
                continue;                   /* SEH exceptions only reach catch (...) */
            }
            unwind_to_catch(rec, ctx, frame, dc, orig_frame, KIND_FH3, tb->start, tb->catch_high,
                            RVA(base, cb->handler), 0, 0, uh);
        }
    }
    t_cxx.processing_throw--;
}

static EXCEPTION_DISPOSITION cxx_frame_handler3(EXCEPTION_RECORD *rec, ULONG_PTR frame, CONTEXT *ctx,
                                                DISPATCHER_CONTEXT *dc, const funcinfo3 *fi)
{
    uintptr_t base = dc->ImageBase;
    UINT magic = fi->magic_bbt & 0x1FFFFFFF;
    if (magic < CXX_MAGIC_VC6 || magic > CXX_MAGIC_VC8) return ExceptionContinueSearch;
    if (magic >= CXX_MAGIC_VC8 && (fi->flags & FI3_EHS) && rec->ExceptionCode != CXX_EXCEPTION_CODE &&
        !is_catch_consolidate(rec) && rec->ExceptionCode != STATUS_LONGJUMP)
        return ExceptionContinueSearch;

    int state = ip_to_state3(fi, dc->ControlPc, base);
    ULONG_PTR orig_frame = frame;
    int unwind_level = -1;

    /* Is this frame one of the function's own catch funclets? Then the
     * objects live in the parent's frame. */
    if (dc->FunctionEntry) {
        uintptr_t start = base + dc->FunctionEntry->BeginAddress;
        const tryblock3 *tbs = RVA(base, fi->tryblock);
        for (UINT i = fi->tryblock_count; i > 0; i--) {
            const tryblock3 *tb = &tbs[i - 1];
            if (state <= tb->end || state > tb->catch_high) continue;
            const catchblock3 *cbs = RVA(base, tb->catchblock);
            for (UINT j = 0; j < tb->ncatch; j++)
                if ((uintptr_t)RVA(base, cbs[j].handler) == start) {
                    unwind_level = tb->end;
                    orig_frame = *(ULONG_PTR *)(frame + cbs[j].frame);
                }
        }
    }

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND)
            local_unwind3(orig_frame, dc, fi, is_catch_consolidate(rec) ? (int)rec->ExceptionInformation[CR_UNWIND_STATE] : state);
        else
            local_unwind3(orig_frame, dc, fi, unwind_level);
        return ExceptionContinueSearch;
    }

    const cxx_exception_type *et = NULL;
    if (fi->tryblock_count) {
        if (rec->ExceptionCode == CXX_EXCEPTION_CODE) {
            if (!rec->ExceptionInformation[1] && !rec->ExceptionInformation[2] && t_cxx.cur_rec)
                *rec = *t_cxx.cur_rec;          /* "throw;" raised by foreign code */
            et = (const cxx_exception_type *)rec->ExceptionInformation[2];
        } else if (t_cxx.translator) {
            struct exc_pointers ep = { rec, ctx };
            t_cxx.translator(rec->ExceptionCode, &ep);   /* usually throws a C++ exception */
        }
        find_catch3(rec, ctx, frame, dc, fi, et, orig_frame);
    }
    if (magic >= CXX_MAGIC_VC8 && (fi->flags & FI3_NOEXCEPT) && orig_frame == frame &&
        rec->ExceptionCode == CXX_EXCEPTION_CODE)
        cxx_terminate();
    return ExceptionContinueSearch;
}

API EXCEPTION_DISPOSITION __CxxFrameHandler3(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    return cxx_frame_handler3(rec, (ULONG_PTR)frame, ctx, dc, RVA(dc->ImageBase, *(const UINT *)dc->HandlerData));
}

/* ------------------------------------------------------------------ */
/* __CxxFrameHandler4 (compressed tables, VS2019+)                      */
/* ------------------------------------------------------------------ */

#define FH4_IS_CATCH     0x01
#define FH4_SEPARATED    0x02
#define FH4_BBT          0x04
#define FH4_UNWIND_MAP   0x08
#define FH4_TRYBLOCK_MAP 0x10
#define FH4_EHS          0x20
#define FH4_NOEXCEPT     0x40

typedef struct {
    BYTE header;
    UINT unwind_count, tryblock_count, ip_count;
    int  unwind_map, tryblock_map, ip_map;
    UINT frame;
} funcinfo4;

#define CB4_FLAGS     0x01
#define CB4_TYPE_INFO 0x02
#define CB4_OFFSET    0x04
#define CB4_SEPARATED 0x08
#define CB4_RET_MASK  0x30
#define CB4_RET1      0x10
#define CB4_RET2      0x20

enum { UW4_NONE, UW4_DTOR_OBJ, UW4_DTOR_PTR, UW4_FRAME };

/* Compressed unsigned integer: the low bits of the first byte say how
 * many bytes follow (x0: 1, x01: 2, x011: 3, x0111: 4, 1111: 4 more). */
static UINT decode_uint(const BYTE **pp)
{
    const BYTE *p = *pp;
    UINT v;
    if (!(p[0] & 1))             { v = p[0] >> 1; p += 1; }
    else if ((p[0] & 3) == 1)    { v = (p[0] >> 2) | ((UINT)p[1] << 6); p += 2; }
    else if ((p[0] & 7) == 3)    { v = (p[0] >> 3) | ((UINT)p[1] << 5) | ((UINT)p[2] << 13); p += 3; }
    else if ((p[0] & 15) == 7)   { v = (p[0] >> 4) | ((UINT)p[1] << 4) | ((UINT)p[2] << 12) | ((UINT)p[3] << 20); p += 4; }
    else                         { memcpy(&v, p + 1, 4); p += 5; }
    *pp = p;
    return v;
}

static int read_rva(const BYTE **pp)
{
    int v;
    memcpy(&v, *pp, 4);
    *pp += 4;
    return v;
}

struct unwind4 { UINT type; const BYTE *prev; int handler; UINT object; };

static void read_unwind4(const BYTE **pp, struct unwind4 *u)
{
    const BYTE *entry = *pp;
    UINT t = decode_uint(pp);
    u->prev = entry - (t >> 2);        /* back-offset to the next entry in the chain */
    u->type = t & 3;
    u->handler = 0;
    u->object = 0;
    if (u->type == UW4_DTOR_OBJ || u->type == UW4_DTOR_PTR) { u->handler = read_rva(pp); u->object = decode_uint(pp); }
    else if (u->type == UW4_FRAME) u->handler = read_rva(pp);
}

struct tryblock4 { int start, end, catch_high; UINT ncatch; int catchblock; };

static void read_tryblock4(const BYTE **pp, struct tryblock4 *t, uintptr_t base)
{
    t->start = (int)decode_uint(pp);
    t->end = (int)decode_uint(pp);
    t->catch_high = (int)decode_uint(pp);
    t->catchblock = read_rva(pp);
    t->ncatch = 0;
    if (t->catchblock) {
        const BYTE *c = RVA(base, t->catchblock), *c0 = c;
        t->ncatch = decode_uint(&c);
        t->catchblock += (int)(c - c0);
    }
}

struct catch4 { BYTE header; UINT flags; int type_info; UINT offset; int handler; UINT ret[2]; };

static int read_catch4(const BYTE **pp, struct catch4 *c, UINT func_rva)
{
    memset(c, 0, sizeof *c);
    c->header = **pp;
    (*pp)++;
    if (c->header & ~(CB4_FLAGS | CB4_TYPE_INFO | CB4_OFFSET | CB4_SEPARATED | CB4_RET_MASK)) return 0;
    UINT rt = c->header & CB4_RET_MASK;
    if (rt == CB4_RET_MASK) return 0;
    if (c->header & CB4_FLAGS) c->flags = decode_uint(pp);
    if (c->header & CB4_TYPE_INFO) c->type_info = read_rva(pp);
    if (c->header & CB4_OFFSET) c->offset = decode_uint(pp);
    c->handler = read_rva(pp);
    if (c->header & CB4_SEPARATED) {
        if (rt) c->ret[0] = (UINT)read_rva(pp);
        if (rt == CB4_RET2) c->ret[1] = (UINT)read_rva(pp);
    } else {
        if (rt) c->ret[0] = decode_uint(pp) + func_rva;
        if (rt == CB4_RET2) c->ret[1] = decode_uint(pp) + func_rva;
    }
    return 1;
}

static int ip_to_state4(const funcinfo4 *fi, DISPATCHER_CONTEXT *dc, uintptr_t ip)
{
    const BYTE *p = RVA(dc->ImageBase, fi->ip_map);
    uintptr_t at = dc->ImageBase + dc->FunctionEntry->BeginAddress;
    int state = -1;
    for (UINT i = 0; i < fi->ip_count; i++) {
        at += decode_uint(&p);
        int s = (int)decode_uint(&p) - 1;
        if (ip < at) break;
        state = s;
    }
    return state;
}

static void local_unwind4(ULONG_PTR frame, DISPATCHER_CONTEXT *dc, const funcinfo4 *fi, int state, int last)
{
    if (state == -2) state = ip_to_state4(fi, dc, dc->ControlPc);
    if (state < -1 || state >= (int)fi->unwind_count) cxx_terminate();
    if (state <= last) return;
    const BYTE *first = RVA(dc->ImageBase, fi->unwind_map), *p = first, *stop = first - 1;
    struct unwind4 u;
    for (int i = 0; i < state; i++) {
        const BYTE *at = p;
        read_unwind4(&p, &u);
        if (i == last) stop = at;
    }
    while (p > stop) {
        read_unwind4(&p, &u);
        p = u.prev;
        if (!u.handler) continue;
        void *obj = (void *)(frame + u.object);
        if (u.type == UW4_DTOR_PTR) obj = *(void **)obj;
        ((fh4_dtor_fn)RVA(dc->ImageBase, u.handler))(obj, frame);
    }
}

static void find_catch4(EXCEPTION_RECORD *rec, CONTEXT *ctx, ULONG_PTR frame, DISPATCHER_CONTEXT *dc,
                        const funcinfo4 *fi, const cxx_exception_type *et, ULONG_PTR orig_frame, int state)
{
    uintptr_t base = dc->ImageBase;
    if (state == -2) state = ip_to_state4(fi, dc, dc->ControlPc);
    t_cxx.processing_throw++;
    const BYTE *p = RVA(base, fi->tryblock_map);
    for (UINT i = 0; i < fi->tryblock_count; i++) {
        struct tryblock4 tb;
        read_tryblock4(&p, &tb, base);
        if (state < tb.start || state > tb.end) continue;
        const BYTE *c = RVA(base, tb.catchblock);
        for (UINT j = 0; j < tb.ncatch; j++) {
            struct catch4 cb;
            if (!read_catch4(&c, &cb, dc->FunctionEntry->BeginAddress)) break;
            const type_info *cti = cb.type_info ? RVA(base, cb.type_info) : NULL;
            if (et) {
                const cxx_type_info *t = find_caught_type(et, exc_base(rec), cti, cb.flags);
                if (!t) continue;
                if (cti && cti->mangled[0] && cb.offset)
                    build_catch_object((void *)rec->ExceptionInformation[1], (void *)(orig_frame + cb.offset),
                                       cb.flags, t, exc_base(rec));
            } else if (cti && cti->mangled[0]) {
                continue;
            }
            unwind_to_catch(rec, ctx, frame, dc, orig_frame, KIND_FH4, tb.start, tb.catch_high,
                            RVA(base, cb.handler),
                            cb.ret[0] ? (ULONG_PTR)RVA(base, cb.ret[0]) : 0,
                            cb.ret[1] ? (ULONG_PTR)RVA(base, cb.ret[1]) : 0, NULL);
        }
    }
    t_cxx.processing_throw--;
}

API EXCEPTION_DISPOSITION __CxxFrameHandler4(EXCEPTION_RECORD *rec, void *establisher, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    ULONG_PTR frame = (ULONG_PTR)establisher;
    uintptr_t base = dc->ImageBase;
    int state = t_cxx.fh4_state;       /* handed over by a catch block we just left */
    t_cxx.fh4_state = -2;

    funcinfo4 fi;
    memset(&fi, 0, sizeof fi);
    const BYTE *p = RVA(base, *(const UINT *)dc->HandlerData);
    fi.header = *p++;
    if ((fi.header & FH4_EHS) && rec->ExceptionCode != CXX_EXCEPTION_CODE &&
        !is_catch_consolidate(rec) && rec->ExceptionCode != STATUS_LONGJUMP)
        return ExceptionContinueSearch;
    if (fi.header & FH4_BBT) (void)decode_uint(&p);
    if (fi.header & FH4_UNWIND_MAP) {
        fi.unwind_map = read_rva(&p);
        const BYTE *q = RVA(base, fi.unwind_map), *q0 = q;
        fi.unwind_count = decode_uint(&q);
        fi.unwind_map += (int)(q - q0);
    }
    if (fi.header & FH4_TRYBLOCK_MAP) {
        fi.tryblock_map = read_rva(&p);
        const BYTE *q = RVA(base, fi.tryblock_map), *q0 = q;
        fi.tryblock_count = decode_uint(&q);
        fi.tryblock_map += (int)(q - q0);
    }
    fi.ip_map = read_rva(&p);
    if (fi.header & FH4_SEPARATED) {
        /* One ip map per funclet: pick the one for this function. */
        const BYTE *q = RVA(base, fi.ip_map);
        UINT n = decode_uint(&q), i;
        for (i = 0; i < n; i++) {
            UINT fn = (UINT)read_rva(&q);
            int map = read_rva(&q);
            if (dc->FunctionEntry && fn == dc->FunctionEntry->BeginAddress) { fi.ip_map = map; break; }
        }
        if (i == n) return ExceptionContinueSearch;
    }
    {
        const BYTE *q = RVA(base, fi.ip_map), *q0 = q;
        fi.ip_count = decode_uint(&q);
        fi.ip_map += (int)(q - q0);
    }
    if (fi.header & FH4_IS_CATCH) fi.frame = decode_uint(&p);
    if (!dc->FunctionEntry) return ExceptionContinueSearch;

    ULONG_PTR orig_frame = (fi.header & FH4_IS_CATCH) ? *(ULONG_PTR *)(frame + fi.frame) : frame;

    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        int last = -1;
        if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && is_catch_consolidate(rec))
            last = (int)rec->ExceptionInformation[CR_UNWIND_STATE];
        else if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && rec->ExceptionCode == STATUS_LONGJUMP)
            last = ip_to_state4(&fi, dc, dc->TargetIp);
        local_unwind4(orig_frame, dc, &fi, state, last);
        return ExceptionContinueSearch;
    }

    if (fi.tryblock_map) {
        const cxx_exception_type *et = NULL;
        if (rec->ExceptionCode == CXX_EXCEPTION_CODE) {
            if (!rec->ExceptionInformation[1] && !rec->ExceptionInformation[2] && t_cxx.cur_rec)
                *rec = *t_cxx.cur_rec;
            et = (const cxx_exception_type *)rec->ExceptionInformation[2];
        } else if (t_cxx.translator) {
            struct exc_pointers ep = { rec, ctx };
            t_cxx.translator(rec->ExceptionCode, &ep);
        }
        find_catch4(rec, ctx, frame, dc, &fi, et, orig_frame, state);
    }
    if (!(fi.header & FH4_IS_CATCH) && (fi.header & FH4_NOEXCEPT) && rec->ExceptionCode == CXX_EXCEPTION_CODE)
        cxx_terminate();
    return ExceptionContinueSearch;
}

/* ------------------------------------------------------------------ */
/* Throwing                                                             */
/* ------------------------------------------------------------------ */

/* The throw entry points are assembly stubs that call a preparation
 * function and then jump to RaiseException with the Windows caller's
 * return address still on top of the stack, so the exception starts in
 * the thrower's frame. The request lives in thread-local storage only
 * until RaiseException has copied it. */
static uintptr_t g_builtin_base;   /* image base of LSIW's own throw data */

__attribute__((used)) void *WINAPI lsiw_cxx_prepare_throw(void *obj, const cxx_exception_type *ti)
{
    struct cxx_thread *t = &t_cxx;
    if (!obj && !ti) {
        /* "throw;": raise the exception being handled again. */
        EXCEPTION_RECORD *cur = t->cur_rec;
        if (!cur) cxx_terminate();
        t->req.code = cur->ExceptionCode;
        t->req.flags = cur->ExceptionFlags & EXCEPTION_NONCONTINUABLE;
        t->req.nargs = cur->NumberParameters;
        memcpy(t->req.args, cur->ExceptionInformation, sizeof t->req.args);
        return &t->req;
    }
    uintptr_t base = 0;
    struct lsiw_module *m = ti ? lsiw_module_from_address(ti) : NULL;
    if (m) base = (uintptr_t)m->base;
    else base = g_builtin_base;
    t->req.code = CXX_EXCEPTION_CODE;
    t->req.flags = EXCEPTION_NONCONTINUABLE;
    t->req.nargs = 4;
    t->req.args[0] = (ti && (ti->flags & TYPE_FLAG_PURE)) ? CXX_MAGIC_PURE : CXX_MAGIC_VC6;
    t->req.args[1] = (ULONG_PTR)obj;
    t->req.args[2] = (ULONG_PTR)ti;
    t->req.args[3] = base;
    return &t->req;
}

/* Exceptions LSIW itself throws on behalf of the program. */
enum { BI_BAD_CAST, BI_BAD_TYPEID, BI_NON_RTTI, BI_COUNT };
__attribute__((used)) void *WINAPI lsiw_cxx_prepare_builtin(int which);

#define THROW_STUB(name, prep)                                   \
    ".globl " name "\n" name ":\n"                               \
    "  subq $40, %rsp\n"                                         \
    "  call " prep "\n"                                          \
    "  addq $40, %rsp\n"                                         \
    "  movl 0(%rax), %ecx\n"                                     \
    "  movl 4(%rax), %edx\n"                                     \
    "  movl 8(%rax), %r8d\n"                                     \
    "  leaq 16(%rax), %r9\n"                                     \
    "  jmp lsiw_RaiseException\n"

__asm__(
    ".text\n"
    THROW_STUB("lsiw__CxxThrowException", "lsiw_cxx_prepare_throw")
    /* ecx = which builtin exception */
    THROW_STUB("lsiw_cxx_throw_builtin", "lsiw_cxx_prepare_builtin")

    /* _local_unwind(frame, target): RtlUnwind(frame, target, NULL, NULL) */
    ".globl lsiw__local_unwind\n"
    "lsiw__local_unwind:\n"
    "  xorl %r8d, %r8d\n"
    "  xorl %r9d, %r9d\n"
    "  jmp lsiw_RtlUnwind\n"

    /* __RTDynamicCast(obj, vfdelta, src, dst, is_ref): the C part reports
     * whether a failed reference cast must throw std::bad_cast. */
    ".globl lsiw___RTDynamicCast\n"
    "lsiw___RTDynamicCast:\n"
    "  subq $56, %rsp\n"
    "  movq 96(%rsp), %rax\n"          /* 5th argument */
    "  movq %rax, 32(%rsp)\n"
    "  leaq 48(%rsp), %rax\n"
    "  movq %rax, 40(%rsp)\n"
    "  movl $0, 48(%rsp)\n"
    "  call lsiw_rtti_dynamic_cast\n"
    "  cmpl $0, 48(%rsp)\n"
    "  jne 1f\n"
    "  addq $56, %rsp\n"
    "  ret\n"
    "1: addq $56, %rsp\n"
    "  movl $0, %ecx\n"                /* BI_BAD_CAST */
    "  jmp lsiw_cxx_throw_builtin\n"

    /* __RTtypeid(obj): throws std::bad_typeid for NULL */
    ".globl lsiw___RTtypeid\n"
    "lsiw___RTtypeid:\n"
    "  testq %rcx, %rcx\n"
    "  jz 2f\n"
    "  jmp lsiw_rtti_typeid\n"
    "2: movl $1, %ecx\n"               /* BI_BAD_TYPEID */
    "  jmp lsiw_cxx_throw_builtin\n"
);

extern void WINAPI lsiw__CxxThrowException(void *, const cxx_exception_type *);
extern void WINAPI lsiw__local_unwind(void *, void *);
extern void *WINAPI lsiw___RTDynamicCast(void *, LONG, void *, void *, BOOL);
extern void *WINAPI lsiw___RTtypeid(void *);

/* ------------------------------------------------------------------ */
/* RTTI                                                                 */
/* ------------------------------------------------------------------ */

static const rtti_locator *get_locator(const void *obj)
{
    return (*(const rtti_locator *const *const *)obj)[-1];
}

static uintptr_t locator_base(const rtti_locator *col)
{
    if (col->signature == 0) {
        struct lsiw_module *m = lsiw_module_from_address(col);
        return m ? (uintptr_t)m->base : 0;
    }
    return (uintptr_t)col - (uintptr_t)(intptr_t)col->object_locator;
}

static char *complete_object(void *obj, const rtti_locator *col)
{
    char *p = (char *)obj - col->base_class_offset;
    if (col->cd_offset) p += *(int *)((char *)obj - col->cd_offset);
    return p;
}

__attribute__((used)) void *WINAPI lsiw_rtti_typeid(void *obj)
{
    const rtti_locator *col = get_locator(obj);
    return RVA(locator_base(col), col->type_descriptor);
}

__attribute__((used)) void *WINAPI lsiw_rtti_dynamic_cast(void *obj, LONG vfdelta, void *src, void *dst, BOOL is_ref, int *must_throw)
{
    (void)src;
    if (!obj) return NULL;
    const rtti_locator *col = get_locator(obj);
    uintptr_t base = locator_base(col);
    const rtti_hierarchy *h = RVA(base, col->type_hierarchy);
    const int *bases = RVA(base, h->base_classes);
    char *complete = complete_object(obj, col);
    (void)vfdelta;
    const type_info *want = dst;
    for (UINT i = 0; i < h->array_len; i++) {
        const rtti_base *b = RVA(base, bases[i]);
        const type_info *ti = RVA(base, b->type_descriptor);
        if (ti == want || !strcmp(ti->mangled, want->mangled))
            return adjust_pointer(complete, &b->offsets);
    }
    if (is_ref) *must_throw = 1;
    return NULL;
}

API void *__RTCastToVoid(void *obj)
{
    if (!obj) return NULL;
    return complete_object(obj, get_locator(obj));
}

/* ------------------------------------------------------------------ */
/* type_info                                                            */
/* ------------------------------------------------------------------ */

/* The undecorated name, e.g. ".?AVwidget@ui@@" -> "class ui::widget". */
struct demangle { const char *p; char *out; size_t len, cap; };

static void dm_put(struct demangle *d, const char *s, size_t n)
{
    if (d->len + n + 1 > d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 64;
        while (cap < d->len + n + 1) cap *= 2;
        char *o = realloc(d->out, cap);
        if (!o) return;
        d->out = o;
        d->cap = cap;
    }
    memcpy(d->out + d->len, s, n);
    d->len += n;
    d->out[d->len] = 0;
}
#define DM_STR(d, s) dm_put(d, s, strlen(s))

static int dm_type(struct demangle *d);

/* name@ns1@ns2@@ -> ns2::ns1::name */
static int dm_qualified(struct demangle *d)
{
    const char *parts[16];
    size_t lens[16];
    int n = 0;
    struct demangle tmpl[16];
    memset(tmpl, 0, sizeof tmpl);
    while (*d->p && *d->p != '@') {
        if (n == 16) return 0;
        if (d->p[0] == '?' && d->p[1] == '$') {
            /* template: ?$name@args@ */
            d->p += 2;
            const char *s = d->p;
            while (*d->p && *d->p != '@') d->p++;
            if (!*d->p) return 0;
            dm_put(&tmpl[n], s, (size_t)(d->p - s));
            d->p++;
            DM_STR(&tmpl[n], "<");
            int first = 1;
            while (*d->p && *d->p != '@') {
                if (!first) DM_STR(&tmpl[n], ",");
                first = 0;
                struct demangle sub = { d->p, NULL, 0, 0 };
                if (*d->p == '$' && d->p[1] == '0') {        /* integer argument */
                    sub.p += 2;
                    const char *v = sub.p;
                    while (*sub.p && *sub.p != '@') sub.p++;
                    dm_put(&tmpl[n], v, (size_t)(sub.p - v));
                    if (*sub.p) sub.p++;
                } else if (!dm_type(&sub)) { free(sub.out); return 0; }
                else if (sub.out) DM_STR(&tmpl[n], sub.out);
                d->p = sub.p;
                free(sub.out);
            }
            if (*d->p) d->p++;
            DM_STR(&tmpl[n], ">");
            parts[n] = tmpl[n].out ? tmpl[n].out : "";
            lens[n] = tmpl[n].len;
            n++;
            continue;
        }
        const char *s = d->p;
        while (*d->p && *d->p != '@') d->p++;
        if (!*d->p) return 0;
        parts[n] = s;
        lens[n] = (size_t)(d->p - s);
        n++;
        d->p++;
    }
    if (*d->p == '@') d->p++;
    for (int i = n - 1; i >= 0; i--) {
        dm_put(d, parts[i], lens[i]);
        if (i) DM_STR(d, "::");
    }
    for (int i = 0; i < 16; i++) free(tmpl[i].out);
    return n > 0;
}

static int dm_type(struct demangle *d)
{
    static const struct { char c; const char *name; } basic[] = {
        {'C', "signed char"}, {'D', "char"}, {'E', "unsigned char"}, {'F', "short"},
        {'G', "unsigned short"}, {'H', "int"}, {'I', "unsigned int"}, {'J', "long"},
        {'K', "unsigned long"}, {'M', "float"}, {'N', "double"}, {'O', "long double"}, {'X', "void"},
    };
    char c = *d->p++;
    for (size_t i = 0; i < sizeof basic / sizeof basic[0]; i++)
        if (basic[i].c == c) { DM_STR(d, basic[i].name); return 1; }
    switch (c) {
    case '_':
        c = *d->p++;
        switch (c) {
        case 'J': DM_STR(d, "__int64"); return 1;
        case 'K': DM_STR(d, "unsigned __int64"); return 1;
        case 'N': DM_STR(d, "bool"); return 1;
        case 'W': DM_STR(d, "wchar_t"); return 1;
        case 'S': DM_STR(d, "char16_t"); return 1;
        case 'U': DM_STR(d, "char32_t"); return 1;
        case 'Q': DM_STR(d, "char8_t"); return 1;
        default: return 0;
        }
    case 'V': DM_STR(d, "class "); return dm_qualified(d);
    case 'U': DM_STR(d, "struct "); return dm_qualified(d);
    case 'T': DM_STR(d, "union "); return dm_qualified(d);
    case 'W': if (*d->p) d->p++; DM_STR(d, "enum "); return dm_qualified(d);
    case 'P': case 'Q': case 'A': case 'B': {
        /* pointer / reference: P|A, then E (__ptr64), then cv: A none, B const, C volatile, D const volatile */
        int ref = (c == 'A' || c == 'B');
        int ptr64 = 0;
        if (*d->p == 'E') { ptr64 = 1; d->p++; }
        char cv = *d->p ? *d->p++ : 'A';
        if (!dm_type(d)) return 0;
        if (cv == 'B' || cv == 'D') DM_STR(d, " const");
        if (cv == 'C' || cv == 'D') DM_STR(d, " volatile");
        DM_STR(d, ref ? " &" : " *");
        if (ptr64) DM_STR(d, " __ptr64");
        return 1;
    }
    default:
        return 0;
    }
}

static char *undecorate_type(const char *mangled)
{
    struct demangle d = { mangled, NULL, 0, 0 };
    if (*d.p == '.') d.p++;
    if (*d.p == '?') {
        /* ".?AVname@@": the type follows '?A' */
        d.p++;
        if (*d.p == 'A') d.p++;
    }
    if (dm_type(&d) && d.out && !*d.p) return d.out;
    free(d.out);
    return strdup(mangled[0] == '.' ? mangled + 1 : mangled);
}

struct std_type_info_data { char *undecorated; char decorated[1]; };

static const char *type_info_name(struct std_type_info_data *data)
{
    char *name = __atomic_load_n(&data->undecorated, __ATOMIC_ACQUIRE);
    if (name) return name;
    char *fresh = undecorate_type(data->decorated);
    char *expected = NULL;
    if (!fresh) return data->decorated;
    if (!__atomic_compare_exchange_n(&data->undecorated, &expected, fresh, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(fresh);
        return expected;
    }
    return fresh;
}

API const char *__std_type_info_name(struct std_type_info_data *data, void *root)
{
    (void)root;
    return type_info_name(data);
}

API int __std_type_info_compare(const struct std_type_info_data *a, const struct std_type_info_data *b)
{
    if (a == b) return 0;
    return strcmp(a->decorated + 1, b->decorated + 1);
}

API size_t __std_type_info_hash(const struct std_type_info_data *data)
{
    uint64_t h = 14695981039346656037ull;          /* FNV-1a, as MSVC does */
    for (const unsigned char *p = (const unsigned char *)data->decorated + 1; *p; p++) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return (size_t)h;
}

API void __std_type_info_destroy_list(void *root) { (void)root; }

/* Older msvcrt.dll exports type_info's members directly (thiscall). */
static WINAPI void *type_info_sdtor(type_info *ti, unsigned flags) { (void)flags; return ti; }
static WINAPI void type_info_dtor(type_info *ti) { (void)ti; }
static WINAPI const char *type_info_name_method(type_info *ti) { return type_info_name((struct std_type_info_data *)&ti->name); }
static WINAPI const char *type_info_raw_name(type_info *ti) { return ti->mangled; }
static WINAPI int type_info_eq(type_info *a, type_info *b) { return a == b || !strcmp(a->mangled, b->mangled); }
static WINAPI int type_info_ne(type_info *a, type_info *b) { return !type_info_eq(a, b); }
static WINAPI int type_info_before(type_info *a, type_info *b) { return strcmp(a->mangled, b->mangled) < 0; }

/* The vtable programs link against as ??_7type_info@@6B@ (a data import). */
static const void *g_type_info_vtable_full[2] = { NULL, (void *)type_info_sdtor };
#define g_type_info_vtable (&g_type_info_vtable_full[1])

/* ------------------------------------------------------------------ */
/* std::exception helpers                                               */
/* ------------------------------------------------------------------ */

struct std_exception_data { const char *what; char do_free; };

API void __std_exception_copy(const struct std_exception_data *src, struct std_exception_data *dst)
{
    if (src->do_free && src->what) {
        size_t n = strlen(src->what) + 1;
        char *copy = lsiw_heap_alloc(n, 0);
        if (copy) {
            memcpy(copy, src->what, n);
            dst->what = copy;
            dst->do_free = 1;
            return;
        }
    }
    dst->what = src->what;
    dst->do_free = 0;
}

API void __std_exception_destroy(struct std_exception_data *data)
{
    if (data->do_free) lsiw_heap_free((void *)data->what);
    data->what = NULL;
    data->do_free = 0;
}

/* ------------------------------------------------------------------ */
/* Builtin exception classes (std::bad_cast, std::bad_typeid, ...)      */
/* ------------------------------------------------------------------ */

/* Their throw/RTTI tables use 32-bit offsets from an image base, so they
 * are laid out in one block together with small trampolines to the member
 * functions, and the block's start serves as the image base. */
struct bi_object { const void *vtable; struct std_exception_data data; };

static WINAPI void bi_dtor(struct bi_object *o) { (void)o; }
static WINAPI void bi_copy(struct bi_object *dst, const struct bi_object *src) { *dst = *src; }
static WINAPI void *bi_sdtor(struct bi_object *o, unsigned flags) { (void)flags; return o; }
static WINAPI const char *bi_what(const struct bi_object *o) { return o->data.what ? o->data.what : "Unknown exception"; }

struct bi_block {
    uint8_t tramp[4][16];
    struct { const void *vtable; void *spare; char name[40]; } td[4];   /* exception, bad_cast, bad_typeid, __non_rtti_object */
    cxx_type_info ct[4];
    struct { int count; int info[3]; } cta[BI_COUNT];
    cxx_exception_type ti[BI_COUNT];
    rtti_locator col[4];
    rtti_hierarchy hier[4];
    rtti_base bases[4];
    int base_arrays[4][3];
    const void *vtables[4][3];         /* COL, scalar deleting dtor, what */
};

static struct bi_block *g_bi;
static pthread_once_t g_bi_once = PTHREAD_ONCE_INIT;
static __thread struct bi_object t_bi_objects[8];
static __thread unsigned t_bi_next;

static void bi_tramp(uint8_t *at, void *target)
{
    at[0] = 0x48; at[1] = 0xB8; memcpy(at + 2, &target, 8);   /* mov rax, imm64 */
    at[10] = 0xFF; at[11] = 0xE0;                            /* jmp rax */
}

static void bi_init(void)
{
    struct bi_block *b = mmap(NULL, (sizeof *b + 4095) & ~4095ul, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED) return;
    uintptr_t base = (uintptr_t)b;
#define OFF(p) ((int)((uintptr_t)(p) - base))
    bi_tramp(b->tramp[0], (void *)bi_dtor);
    bi_tramp(b->tramp[1], (void *)bi_copy);
    bi_tramp(b->tramp[2], (void *)bi_sdtor);
    bi_tramp(b->tramp[3], (void *)bi_what);
    static const char *names[4] = { ".?AVexception@std@@", ".?AVbad_cast@std@@", ".?AVbad_typeid@std@@", ".?AV__non_rtti_object@std@@" };
    for (int i = 0; i < 4; i++) {
        b->td[i].vtable = g_type_info_vtable;
        strcpy(b->td[i].name, names[i]);
        b->ct[i] = (cxx_type_info){ 0, OFF(&b->td[i]), { 0, -1, 0 }, (int)sizeof(struct bi_object), OFF(b->tramp[1]) };
    }
    /* class -> its catchable types, most derived first */
    static const int chain[BI_COUNT][3] = { { 1, 0, -1 }, { 2, 0, -1 }, { 3, 2, 0 } };
    for (int k = 0; k < BI_COUNT; k++) {
        int n = 0;
        for (int j = 0; j < 3 && chain[k][j] >= 0; j++) b->cta[k].info[n++] = OFF(&b->ct[chain[k][j]]);
        b->cta[k].count = n;
        b->ti[k] = (cxx_exception_type){ 0, OFF(b->tramp[0]), 0, OFF(&b->cta[k]) };
    }
    /* RTTI for the objects: class i has the bases of its chain */
    static const int rtti_chain[4][3] = { { 0, -1, -1 }, { 1, 0, -1 }, { 2, 0, -1 }, { 3, 2, 0 } };
    for (int i = 0; i < 4; i++) {
        b->bases[i] = (rtti_base){ OFF(&b->td[i]), 0, { 0, -1, 0 }, 0, OFF(&b->hier[i]) };
        int n = 0;
        for (int j = 0; j < 3 && rtti_chain[i][j] >= 0; j++) b->base_arrays[i][n++] = OFF(&b->bases[rtti_chain[i][j]]);
        b->hier[i] = (rtti_hierarchy){ 0, 0, (UINT)n, OFF(b->base_arrays[i]) };
        b->col[i] = (rtti_locator){ 1, 0, 0, OFF(&b->td[i]), OFF(&b->hier[i]), OFF(&b->col[i]) };
        b->vtables[i][0] = &b->col[i];
        b->vtables[i][1] = b->tramp[2];
        b->vtables[i][2] = b->tramp[3];
    }
#undef OFF
    mprotect(b, (sizeof *b + 4095) & ~4095ul, PROT_READ | PROT_EXEC);
    g_builtin_base = base;
    g_bi = b;
}

__attribute__((used)) void *WINAPI lsiw_cxx_prepare_builtin(int which)
{
    static const char *what[BI_COUNT] = { "Bad dynamic_cast!", "Attempted a typeid of nullptr pointer!", "Access violation - no RTTI data!" };
    static const int cls[BI_COUNT] = { 1, 2, 3 };
    pthread_once(&g_bi_once, bi_init);
    if (!g_bi || which < 0 || which >= BI_COUNT) cxx_terminate();
    struct bi_object *o = &t_bi_objects[t_bi_next++ % 8];
    o->vtable = &g_bi->vtables[cls[which]][1];
    o->data.what = what[which];
    o->data.do_free = 0;
    struct cxx_thread *t = &t_cxx;
    t->req.code = CXX_EXCEPTION_CODE;
    t->req.flags = EXCEPTION_NONCONTINUABLE;
    t->req.nargs = 4;
    t->req.args[0] = CXX_MAGIC_VC6;
    t->req.args[1] = (ULONG_PTR)o;
    t->req.args[2] = (ULONG_PTR)&g_bi->ti[which];
    t->req.args[3] = g_builtin_base;
    return &t->req;
}

/* ------------------------------------------------------------------ */
/* Exported state and helpers                                           */
/* ------------------------------------------------------------------ */

API void **__current_exception(void) { return (void **)&t_cxx.cur_rec; }
API void **__current_exception_context(void) { return (void **)&t_cxx.cur_ctx; }
API int *__processing_throw(void) { return &t_cxx.processing_throw; }
API BOOL __uncaught_exception(void) { return t_cxx.processing_throw != 0; }
API int __uncaught_exceptions(void) { return t_cxx.processing_throw; }

API terminate_fn set_terminate(terminate_fn f) { return __atomic_exchange_n(&g_terminate, f, __ATOMIC_ACQ_REL); }
API terminate_fn _get_terminate(void) { return __atomic_load_n(&g_terminate, __ATOMIC_ACQUIRE); }
API terminate_fn set_unexpected(terminate_fn f) { return __atomic_exchange_n(&g_unexpected, f, __ATOMIC_ACQ_REL); }
API terminate_fn _get_unexpected(void) { return __atomic_load_n(&g_unexpected, __ATOMIC_ACQUIRE); }
API void terminate(void) { cxx_terminate(); }
API void __std_terminate(void) { cxx_terminate(); }
API void unexpected(void)
{
    terminate_fn f = _get_unexpected();
    if (f) f();
    cxx_terminate();
}
API se_translator_fn _set_se_translator(se_translator_fn f)
{
    se_translator_fn old = t_cxx.translator;
    t_cxx.translator = f;
    return old;
}

/* FRAMEINFO-style registration used by /EHa code and C++/CLI. */
API BOOL __CxxRegisterExceptionObject(struct exc_pointers *ep, void **frame_info)
{
    frame_info[0] = ep && ep->rec && is_cxx_exception(ep->rec) ? (void *)ep->rec->ExceptionInformation[1] : NULL;
    frame_info[1] = NULL;
    frame_info[2] = t_cxx.cur_rec;
    frame_info[3] = t_cxx.cur_ctx;
    if (ep) { t_cxx.cur_rec = ep->rec; t_cxx.cur_ctx = ep->ctx; }
    return TRUE;
}

API void __CxxUnregisterExceptionObject(void **frame_info, BOOL rethrow)
{
    if (!rethrow && t_cxx.cur_rec) destroy_object(t_cxx.cur_rec);
    t_cxx.cur_rec = frame_info[2];
    t_cxx.cur_ctx = frame_info[3];
}

API int __CxxQueryExceptionSize(void) { return 4 * (int)sizeof(void *); }

API BOOL __CxxDetectRethrow(struct exc_pointers *ep)
{
    if (!ep || !ep->rec) return FALSE;
    EXCEPTION_RECORD *rec = ep->rec;
    if (rec->ExceptionCode == CXX_EXCEPTION_CODE && rec->NumberParameters >= 3 &&
        !rec->ExceptionInformation[1] && !rec->ExceptionInformation[2] && t_cxx.cur_rec) {
        *rec = *t_cxx.cur_rec;
        return TRUE;
    }
    return t_cxx.cur_rec == rec;
}

API int __CxxExceptionFilter(struct exc_pointers *ep, const type_info *ti, int flags, void **copy)
{
    if (!ep || !is_cxx_exception(ep->rec)) return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD *rec = ep->rec;
    if (!rec->ExceptionInformation[1] && !rec->ExceptionInformation[2] && t_cxx.cur_rec) *rec = *t_cxx.cur_rec;
    const cxx_exception_type *et = (const void *)rec->ExceptionInformation[2];
    if (!et) return EXCEPTION_CONTINUE_SEARCH;
    const cxx_type_info *t = find_caught_type(et, exc_base(rec), ti, (UINT)flags);
    if (!t) return EXCEPTION_CONTINUE_SEARCH;
    if (copy && ti && ti->mangled[0])
        build_catch_object((void *)rec->ExceptionInformation[1], copy, (UINT)flags, t, exc_base(rec));
    return EXCEPTION_EXECUTE_HANDLER;
}

API void __DestructExceptionObject(EXCEPTION_RECORD *rec) { destroy_object(rec); }
API void *__AdjustPointer(void *obj, const this_ptr_offsets *off) { return adjust_pointer(obj, off); }
API void *__GetPlatformExceptionInfo(int *is_winrt) { if (is_winrt) *is_winrt = 0; return NULL; }

API BOOL _is_exception_typeof(const type_info *ti, struct exc_pointers *ep)
{
    if (!ep || !is_cxx_exception(ep->rec)) return FALSE;
    const cxx_exception_type *et = (const void *)ep->rec->ExceptionInformation[2];
    uintptr_t base = exc_base(ep->rec);
    const cxx_type_info_table *tab = RVA(base, et->type_info_table);
    for (int i = 0; i < tab->count; i++) {
        const cxx_type_info *t = RVA(base, tab->info[i]);
        const type_info *cti = RVA(base, t->type_info);
        if (cti == ti || !strcmp(cti->mangled, ti->mangled)) return TRUE;
    }
    return FALSE;
}

API EXCEPTION_DISPOSITION __CxxFrameHandler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    return __CxxFrameHandler3(rec, frame, ctx, dc);
}

API void __NLG_Dispatch2(void) { }
API void __NLG_Return2(void) { }
API void __telemetry_main_invoke_trigger(void *h) { (void)h; }
API void __telemetry_main_return_trigger(void *h) { (void)h; }
API void _SetWinRTOutOfMemoryExceptionCallback(void *cb) { (void)cb; }

API void __report_gsfailure(ULONG_PTR cookie)
{
    (void)cookie;
    lsiw_crt_flush_all();
    fprintf(stderr, "lsiw: the program detected a stack buffer overrun (/GS check failed)\n");
    lsiw_exit_process(0xC0000409);
}


/* __vcrt_* wrappers vcruntime exposes over kernel32. */
static void *k32(const char *name) { return lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), name); }
API DWORD __vcrt_GetModuleFileNameW(HMODULE m, WCHAR *buf, DWORD n)
{ return ((DWORD (WINAPI *)(HMODULE, WCHAR *, DWORD))k32("GetModuleFileNameW"))(m, buf, n); }
API HMODULE __vcrt_GetModuleHandleW(const WCHAR *name)
{ return ((HMODULE (WINAPI *)(const WCHAR *))k32("GetModuleHandleW"))(name); }
API HMODULE __vcrt_LoadLibraryExW(const WCHAR *name, HANDLE f, DWORD flags)
{ return ((HMODULE (WINAPI *)(const WCHAR *, HANDLE, DWORD))k32("LoadLibraryExW"))(name, f, flags); }
API BOOL __vcrt_InitializeCriticalSectionEx(void *cs, DWORD spin, DWORD flags)
{ return ((BOOL (WINAPI *)(void *, DWORD, DWORD))k32("InitializeCriticalSectionEx"))(cs, spin, flags); }

/* Old msvcrt.dll C++ entry points (VC6-era programs). */
API void *msvcrt_operator_new(size_t n)
{
    void *(WINAPI *m)(size_t) = lsiw_builtin_lookup(lsiw_builtin_find("msvcrt.dll"), "malloc");
    return m(n ? n : 1);
}
API void msvcrt_operator_delete(void *p)
{
    void (WINAPI *f)(void *) = lsiw_builtin_lookup(lsiw_builtin_find("msvcrt.dll"), "free");
    f(p);
}

/* ------------------------------------------------------------------ */
/* Export table                                                         */
/* ------------------------------------------------------------------ */

const struct lsiw_export lsiw_vcruntime_exports[] = {
    EX("_CxxThrowException", lsiw__CxxThrowException),
    E(__CxxFrameHandler), EX("__CxxFrameHandler2", __CxxFrameHandler3), E(__CxxFrameHandler3), E(__CxxFrameHandler4),
    EX("_local_unwind", lsiw__local_unwind),
    EX("__RTDynamicCast", lsiw___RTDynamicCast), EX("__RTtypeid", lsiw___RTtypeid), E(__RTCastToVoid),
    E(__std_type_info_name), E(__std_type_info_compare), E(__std_type_info_hash), E(__std_type_info_destroy_list),
    E(__std_exception_copy), E(__std_exception_destroy), E(__std_terminate),
    E(__current_exception), E(__current_exception_context), E(__processing_throw),
    E(__uncaught_exception), E(__uncaught_exceptions),
    E(terminate), E(set_terminate), E(_get_terminate), E(unexpected), E(set_unexpected), E(_get_unexpected),
    E(_set_se_translator),
    E(__CxxRegisterExceptionObject), E(__CxxUnregisterExceptionObject), E(__CxxQueryExceptionSize),
    E(__CxxDetectRethrow), E(__CxxExceptionFilter), E(__DestructExceptionObject), E(__AdjustPointer),
    E(__GetPlatformExceptionInfo), E(_is_exception_typeof),
    E(__NLG_Dispatch2), E(__NLG_Return2), E(__telemetry_main_invoke_trigger), E(__telemetry_main_return_trigger),
    E(_SetWinRTOutOfMemoryExceptionCallback), E(__report_gsfailure),
    E(__vcrt_GetModuleFileNameW), E(__vcrt_GetModuleHandleW), E(__vcrt_LoadLibraryExW), E(__vcrt_InitializeCriticalSectionEx),
    /* decorated names (msvcrt.dll and older runtimes) */
    { "??_7type_info@@6B@", (void *)g_type_info_vtable },
    { "??1type_info@@UEAA@XZ", (void *)type_info_dtor },
    { "??_Gtype_info@@UEAAPEAXI@Z", (void *)type_info_sdtor },
    { "?name@type_info@@QEBAPEBDXZ", (void *)type_info_name_method },
    { "?raw_name@type_info@@QEBAPEBDXZ", (void *)type_info_raw_name },
    { "??8type_info@@QEBAHAEBV0@@Z", (void *)type_info_eq },
    { "??9type_info@@QEBAHAEBV0@@Z", (void *)type_info_ne },
    { "?before@type_info@@QEBAHAEBV0@@Z", (void *)type_info_before },
    { "?terminate@@YAXXZ", (void *)terminate },
    { "?set_terminate@@YAP6AXXZP6AXXZ@Z", (void *)set_terminate },
    { "?unexpected@@YAXXZ", (void *)unexpected },
    { "?set_unexpected@@YAP6AXXZP6AXXZ@Z", (void *)set_unexpected },
    { "?_set_se_translator@@YAP6AXIPEAU_EXCEPTION_POINTERS@@@ZP6AXI0@Z@Z", (void *)_set_se_translator },
    { "??2@YAPEAX_K@Z", (void *)msvcrt_operator_new },
    { "??_U@YAPEAX_K@Z", (void *)msvcrt_operator_new },
    { "??3@YAXPEAX@Z", (void *)msvcrt_operator_delete },
    { "??_V@YAXPEAX@Z", (void *)msvcrt_operator_delete },
    { NULL, NULL }
};
