/* kernel32: heaps, virtual memory, Global/Local memory, interlocked
 * singly-linked lists, fibers, and memory probing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define HEAP_NO_SERIALIZE          0x00000001
#define HEAP_GENERATE_EXCEPTIONS   0x00000004
#define HEAP_ZERO_MEMORY           0x00000008
#define HEAP_REALLOC_IN_PLACE_ONLY 0x00000010

#define MEM_COMMIT      0x00001000
#define MEM_RESERVE     0x00002000
#define MEM_DECOMMIT    0x00004000
#define MEM_RELEASE     0x00008000
#define MEM_FREE        0x00010000
#define MEM_PRIVATE     0x00020000
#define MEM_MAPPED      0x00040000
#define MEM_RESET       0x00080000
#define MEM_TOP_DOWN    0x00100000
#define MEM_RESET_UNDO  0x01000000
#define MEM_LARGE_PAGES 0x20000000
#define MEM_IMAGE       0x01000000

#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD             0x100

#define GRANULARITY 0x10000
#define PAGE 0x1000

/* ================================================================== */
/* Heaps                                                                */
/* ================================================================== */

/* Every block carries a 32-byte header (keeps 16-byte alignment). Blocks
 * of private heaps are linked so HeapDestroy can free them all. */
#define HEAP_MAGIC 0x4c534957u   /* "LSIW" */

struct heap {
    uint32_t magic;
    uint32_t flags;
    pthread_mutex_t lock;
    struct block *head;
    int is_process;
};

struct block {
    struct block *prev, *next;
    uint64_t size;
    struct heap *heap;
};

static struct heap g_process_heap = { HEAP_MAGIC, 0, PTHREAD_MUTEX_INITIALIZER, NULL, 1 };
static struct heap *g_heaps[256] = { &g_process_heap };
static int g_nheaps = 1;
static pthread_mutex_t g_heaps_lock = PTHREAD_MUTEX_INITIALIZER;

void *lsiw_process_heap(void) { return &g_process_heap; }

static struct heap *heap_of(HANDLE h)
{
    struct heap *hp = h;
    if (!hp || hp->magic != HEAP_MAGIC) return &g_process_heap;
    return hp;
}

static struct block *block_of(const void *p)
{
    if (!p) return NULL;
    struct block *b = (struct block *)p - 1;
    struct heap *hp = b->heap;
    if (!hp) return NULL;
    /* Only dereference a heap pointer that names a live heap: a foreign
     * pointer handed to HeapFree/LocalFree must fail, not crash. */
    if (hp != &g_process_heap) {
        int known = 0;
        for (int i = 0; i < 256 && !known; i++) known = __atomic_load_n(&g_heaps[i], __ATOMIC_ACQUIRE) == hp;
        if (!known) return NULL;
    }
    if (hp->magic != HEAP_MAGIC) return NULL;
    return b;
}

static void link_block(struct heap *hp, struct block *b)
{
    b->heap = hp;
    b->prev = b->next = NULL;
    if (hp->is_process) return;
    pthread_mutex_lock(&hp->lock);
    b->next = hp->head;
    if (hp->head) hp->head->prev = b;
    hp->head = b;
    pthread_mutex_unlock(&hp->lock);
}

static void unlink_block(struct block *b)
{
    struct heap *hp = b->heap;
    if (hp->is_process) return;
    pthread_mutex_lock(&hp->lock);
    if (b->prev) b->prev->next = b->next; else hp->head = b->next;
    if (b->next) b->next->prev = b->prev;
    pthread_mutex_unlock(&hp->lock);
}

static void *heap_alloc(struct heap *hp, size_t n, int zero)
{
    if (n > (size_t)1 << 47) { errno = ENOMEM; return NULL; }
    struct block *b = zero ? calloc(1, sizeof *b + n) : malloc(sizeof *b + n);
    if (!b) return NULL;
    b->size = n;
    link_block(hp, b);
    return b + 1;
}

/* Allocator shared with the C runtime, so memory from malloc() can be
 * passed to HeapFree()/HeapSize() and the other way round. */
void *lsiw_heap_alloc(size_t n, int zero) { return heap_alloc(&g_process_heap, n, zero); }
size_t lsiw_heap_size(const void *p) { struct block *b = block_of(p); return b ? b->size : 0; }

void lsiw_heap_free(void *p)
{
    struct block *b = block_of(p);
    if (!b) return;
    unlink_block(b);
    b->heap = NULL;
    free(b);
}

void *lsiw_heap_realloc(void *p, size_t n)
{
    if (!p) return lsiw_heap_alloc(n, 0);
    struct block *b = block_of(p);
    if (!b) return NULL;
    struct heap *hp = b->heap;
    unlink_block(b);
    struct block *nb = realloc(b, sizeof *nb + n);
    if (!nb) { link_block(hp, b); return NULL; }
    nb->size = n;
    link_block(hp, nb);
    return nb + 1;
}

int lsiw_heap_owns(const void *p) { return block_of(p) != NULL; }

API HANDLE GetProcessHeap(void) { return &g_process_heap; }

API DWORD GetProcessHeaps(DWORD n, HANDLE *out)
{
    pthread_mutex_lock(&g_heaps_lock);
    DWORD c = 0;
    for (int i = 0; i < 256; i++) if (g_heaps[i]) { if (c < n) out[c] = g_heaps[i]; c++; }
    pthread_mutex_unlock(&g_heaps_lock);
    return c;
}

API HANDLE HeapCreate(DWORD flags, SIZE_T init, SIZE_T max)
{
    (void)init; (void)max;
    struct heap *hp = calloc(1, sizeof *hp);
    hp->magic = HEAP_MAGIC;
    hp->flags = flags;
    pthread_mutex_init(&hp->lock, NULL);
    pthread_mutex_lock(&g_heaps_lock);
    for (int i = 1; i < 256; i++) if (!g_heaps[i]) { g_heaps[i] = hp; break; }
    g_nheaps++;
    pthread_mutex_unlock(&g_heaps_lock);
    return hp;
}

API BOOL HeapDestroy(HANDLE h)
{
    struct heap *hp = h;
    if (!hp || hp->magic != HEAP_MAGIC || hp->is_process) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&g_heaps_lock);
    for (int i = 1; i < 256; i++) if (g_heaps[i] == hp) g_heaps[i] = NULL;
    g_nheaps--;
    pthread_mutex_unlock(&g_heaps_lock);
    pthread_mutex_lock(&hp->lock);
    struct block *b = hp->head;
    while (b) { struct block *n = b->next; b->heap = NULL; free(b); b = n; }
    hp->head = NULL;
    hp->magic = 0;
    pthread_mutex_unlock(&hp->lock);
    pthread_mutex_destroy(&hp->lock);
    free(hp);
    return TRUE;
}

static void heap_failure(DWORD flags, struct heap *hp)
{
    lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    if ((flags | hp->flags) & HEAP_GENERATE_EXCEPTIONS) {

        lsiw_raise(0xC0000017 /* STATUS_NO_MEMORY */, 0, 0, NULL);
    }
}

API void *HeapAlloc(HANDLE h, DWORD flags, SIZE_T n)
{
    struct heap *hp = heap_of(h);
    void *p = heap_alloc(hp, n, (flags & HEAP_ZERO_MEMORY) != 0);
    if (!p) heap_failure(flags, hp);
    return p;
}

API BOOL HeapFree(HANDLE h, DWORD flags, void *p)
{
    (void)h; (void)flags;
    if (!p) return TRUE;
    if (!block_of(p)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    lsiw_heap_free(p);
    return TRUE;
}

API void *HeapReAlloc(HANDLE h, DWORD flags, void *p, SIZE_T n)
{
    struct heap *hp = heap_of(h);
    if (!p) return HeapAlloc(h, flags, n);
    struct block *b = block_of(p);
    if (!b) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    size_t old = b->size;
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        if (n > old) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        b->size = n;
        return p;
    }
    void *np = lsiw_heap_realloc(p, n);
    if (!np) { heap_failure(flags, hp); return NULL; }
    if ((flags & HEAP_ZERO_MEMORY) && n > old) memset((char *)np + old, 0, n - old);
    return np;
}

API SIZE_T HeapSize(HANDLE h, DWORD flags, const void *p)
{
    (void)h; (void)flags;
    struct block *b = block_of(p);
    if (!b) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return (SIZE_T)-1; }
    return b->size;
}

API BOOL HeapValidate(HANDLE h, DWORD f, const void *p) { (void)h; (void)f; return !p || block_of(p) != NULL; }
API SIZE_T HeapCompact(HANDLE h, DWORD f) { (void)h; (void)f; return 0x100000; }
API BOOL HeapLock(HANDLE h) { pthread_mutex_lock(&heap_of(h)->lock); return TRUE; }
API BOOL HeapUnlock(HANDLE h) { pthread_mutex_unlock(&heap_of(h)->lock); return TRUE; }
API BOOL HeapWalk(HANDLE h, void *e) { (void)h; (void)e; lsiw_set_last_error(ERROR_NO_MORE_ITEMS); return FALSE; }
API BOOL HeapSetInformation(HANDLE h, int cls, void *info, SIZE_T len) { (void)h; (void)cls; (void)info; (void)len; return TRUE; }
API BOOL HeapQueryInformation(HANDLE h, int cls, void *info, SIZE_T len, SIZE_T *ret)
{
    (void)h;
    if (cls == 0 /* HeapCompatibilityInformation */) {
        if (ret) *ret = 4;
        if (len < 4) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        *(ULONG *)info = 2;   /* low-fragmentation heap */
        return TRUE;
    }
    lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}
struct heap_summary { DWORD cb; SIZE_T allocated, committed, reserved, max_reserve; };
API BOOL HeapSummary(HANDLE h, DWORD f, struct heap_summary *s) { (void)h; (void)f; s->allocated = s->committed = s->reserved = s->max_reserve = 0; return TRUE; }

/* ntdll heap functions (same implementation, different calling order). */
API void *RtlAllocateHeap(HANDLE h, ULONG flags, SIZE_T n) { return HeapAlloc(h, flags, n); }
API BOOL RtlFreeHeap(HANDLE h, ULONG flags, void *p) { return HeapFree(h, flags, p); }
API void *RtlReAllocateHeap(HANDLE h, ULONG flags, void *p, SIZE_T n) { return HeapReAlloc(h, flags, p, n); }
API SIZE_T RtlSizeHeap(HANDLE h, ULONG flags, const void *p) { return HeapSize(h, flags, p); }
API HANDLE RtlCreateHeap(ULONG flags, void *base, SIZE_T res, SIZE_T com, void *lock, void *params)
{ (void)base; (void)lock; (void)params; return HeapCreate(flags, com, res); }
API HANDLE RtlDestroyHeap(HANDLE h) { return HeapDestroy(h) ? NULL : h; }
API ULONG RtlGetProcessHeaps(ULONG n, HANDLE *out) { return GetProcessHeaps(n, out); }

/* ---- Global / Local: handles are the block pointers themselves ---- */

#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GMEM_MODIFY   0x0080

API HANDLE GlobalAlloc(UINT flags, SIZE_T n) { void *p = lsiw_heap_alloc(n ? n : 1, (flags & GMEM_ZEROINIT) != 0); if (!p) lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return p; }
API HANDLE GlobalFree(HANDLE h) { if (h && !block_of(h)) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return h; } lsiw_heap_free(h); return NULL; }
API HANDLE GlobalReAlloc(HANDLE h, SIZE_T n, UINT flags)
{
    if (flags & GMEM_MODIFY) return h;
    struct block *b = block_of(h);
    if (!b) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    size_t old = b->size;
    void *p = lsiw_heap_realloc(h, n ? n : 1);
    if (p && (flags & GMEM_ZEROINIT) && n > old) memset((char *)p + old, 0, n - old);
    return p;
}
API void *GlobalLock(HANDLE h) { return h; }
API BOOL GlobalUnlock(HANDLE h) { (void)h; lsiw_set_last_error(0); return FALSE; }
API SIZE_T GlobalSize(HANDLE h) { return lsiw_heap_size(h); }
API HANDLE GlobalHandle(const void *p) { return (HANDLE)p; }
API UINT GlobalFlags(HANDLE h) { (void)h; return 0; }
API SIZE_T GlobalCompact(DWORD n) { (void)n; return 0x100000; }
API HANDLE LocalAlloc(UINT flags, SIZE_T n) { return GlobalAlloc(flags, n); }
API HANDLE LocalFree(HANDLE h) { return GlobalFree(h); }
API HANDLE LocalReAlloc(HANDLE h, SIZE_T n, UINT flags) { return GlobalReAlloc(h, n, flags); }
API void *LocalLock(HANDLE h) { return h; }
API BOOL LocalUnlock(HANDLE h) { (void)h; lsiw_set_last_error(0); return FALSE; }
API SIZE_T LocalSize(HANDLE h) { return lsiw_heap_size(h); }
API HANDLE LocalHandle(const void *p) { return (HANDLE)p; }
API UINT LocalFlags(HANDLE h) { (void)h; return 0; }

/* ================================================================== */
/* Virtual memory                                                       */
/* ================================================================== */

struct vregion {
    uintptr_t base;
    size_t size;
    DWORD alloc_protect;
    uint16_t *pages;          /* Windows protection per page, 0 = reserved only */
    struct vregion *next;
};
static struct vregion *g_regions;
static pthread_mutex_t g_vlock = PTHREAD_MUTEX_INITIALIZER;

static int posix_prot(DWORD p)
{
    switch (p & 0xff) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_EXEC | PROT_READ;
    case PAGE_EXECUTE_READ:      return PROT_EXEC | PROT_READ;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return PROT_EXEC | PROT_READ | PROT_WRITE;
    default:                     return -1;
    }
}

static DWORD win_prot(int prot)
{
    int r = prot & PROT_READ, w = prot & PROT_WRITE, x = prot & PROT_EXEC;
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

static struct vregion *region_find(uintptr_t a)
{
    for (struct vregion *r = g_regions; r; r = r->next)
        if (a >= r->base && a < r->base + r->size) return r;
    return NULL;
}

/* One line of /proc/self/maps covering `a`, or the gap containing it. */

int lsiw_query_maps(uintptr_t a, struct map_info *out)
{
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return -1;
    char line[512];
    uintptr_t prev_end = 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e, off, ino;
        char perm[8], dev[16];
        if (sscanf(line, "%lx-%lx %7s %lx %15s %lu", &s, &e, perm, &off, dev, &ino) < 6) continue;
        if (a < s) {
            out->start = prev_end; out->end = s; out->prot = 0; out->mapped = 0; out->file = 0;
            found = 1;
            break;
        }
        if (a < e) {
            out->start = s; out->end = e; out->mapped = 1; out->file = ino != 0;
            out->prot = (perm[0] == 'r' ? PROT_READ : 0) | (perm[1] == 'w' ? PROT_WRITE : 0) | (perm[2] == 'x' ? PROT_EXEC : 0);
            found = 1;
            break;
        }
        prev_end = e;
    }
    fclose(f);
    if (!found) { out->start = prev_end; out->end = 0x7fffffff0000ULL; out->prot = 0; out->mapped = 0; out->file = 0; }
    return 0;
}

/* Reserve `size` bytes aligned to 64 KiB, as Windows does. */
static void *reserve_aligned(size_t size, int top_down)
{
    (void)top_down;
    size_t span = size + GRANULARITY;
    uint8_t *p = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    uintptr_t a = ((uintptr_t)p + GRANULARITY - 1) & ~(uintptr_t)(GRANULARITY - 1);
    if (a > (uintptr_t)p) munmap(p, a - (uintptr_t)p);
    uintptr_t end = (uintptr_t)p + span;
    if (end > a + size) munmap((void *)(a + size), end - (a + size));
    return (void *)a;
}

static void *virtual_alloc(void *addr, SIZE_T size, DWORD type, DWORD protect)
{
    if (!size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    int prot = posix_prot(protect);
    if (prot < 0 && (type & MEM_COMMIT)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    if (type & (MEM_RESET | MEM_RESET_UNDO)) {
        uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(PAGE - 1);
        if (type & MEM_RESET) madvise((void *)a, size + ((uintptr_t)addr - a), MADV_FREE);
        return addr;
    }
    pthread_mutex_lock(&g_vlock);
    struct vregion *r = addr ? region_find((uintptr_t)addr) : NULL;
    void *result = NULL;

    if (!r) {
        if (!(type & (MEM_RESERVE | MEM_COMMIT))) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); goto out; }
        uintptr_t base;
        size_t len;
        if (addr) {
            base = (uintptr_t)addr & ~(uintptr_t)(GRANULARITY - 1);
            len = (((uintptr_t)addr + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1)) - base;
            void *p = mmap((void *)base, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED || (uintptr_t)p != base) {
                if (p != MAP_FAILED) munmap(p, len);
                lsiw_set_last_error(487 /* ERROR_INVALID_ADDRESS */);
                goto out;
            }
        } else {
            len = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
            void *p = reserve_aligned(len, (type & MEM_TOP_DOWN) != 0);
            if (!p) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); goto out; }
            base = (uintptr_t)p;
        }
        r = calloc(1, sizeof *r);
        r->base = base;
        r->size = len;
        r->alloc_protect = protect;
        r->pages = calloc(len / PAGE, sizeof *r->pages);
        if (!r->pages) { munmap((void *)base, len); free(r); lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); goto out; }
        r->next = g_regions;
        g_regions = r;
        if (!(type & MEM_COMMIT)) { result = (void *)base; goto out; }
        addr = addr ? addr : (void *)base;
    } else if (!(type & MEM_COMMIT)) {
        /* Reserving inside an existing reservation is an error on Windows. */
        lsiw_set_last_error(487);
        goto out;
    }

    /* Commit [addr, addr+size) inside r. */
    {
        uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(PAGE - 1);
        uintptr_t e = ((uintptr_t)addr + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
        if (e > r->base + r->size) { lsiw_set_last_error(487); goto out; }
        if (mprotect((void *)a, e - a, prot) != 0) { lsiw_set_errno_error(errno); goto out; }
        for (uintptr_t p = a; p < e; p += PAGE) r->pages[(p - r->base) / PAGE] = (uint16_t)protect;
        result = (void *)a;
    }
out:
    pthread_mutex_unlock(&g_vlock);
    return result;
}

API void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect) { return virtual_alloc(addr, size, type, protect); }
API void *VirtualAllocEx(HANDLE proc, void *addr, SIZE_T size, DWORD type, DWORD protect) { (void)proc; return virtual_alloc(addr, size, type, protect); }
API void *VirtualAllocExNuma(HANDLE proc, void *addr, SIZE_T size, DWORD type, DWORD protect, DWORD node) { (void)proc; (void)node; return virtual_alloc(addr, size, type, protect); }
API void *VirtualAlloc2(HANDLE proc, void *addr, SIZE_T size, ULONG type, ULONG protect, void *params, ULONG n) { (void)proc; (void)params; (void)n; return virtual_alloc(addr, size, type, protect); }
API void *VirtualAllocFromApp(void *addr, SIZE_T size, ULONG type, ULONG protect) { return virtual_alloc(addr, size, type, protect); }

static BOOL virtual_free(void *addr, SIZE_T size, DWORD type)
{
    pthread_mutex_lock(&g_vlock);
    BOOL ok = FALSE;
    struct vregion **pp = &g_regions, *r = NULL;
    for (; *pp; pp = &(*pp)->next) if ((uintptr_t)addr >= (*pp)->base && (uintptr_t)addr < (*pp)->base + (*pp)->size) { r = *pp; break; }
    if (!r) {
        /* Not ours (e.g. a mapped view); unmap if asked to release. */
        if ((type & MEM_RELEASE) && size == 0) lsiw_set_last_error(487);
        else lsiw_set_last_error(487);
        goto out;
    }
    if (type & MEM_RELEASE) {
        if (size != 0 || (uintptr_t)addr != r->base) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); goto out; }
        munmap((void *)r->base, r->size);
        *pp = r->next;
        free(r->pages);
        free(r);
        ok = TRUE;
    } else if (type & MEM_DECOMMIT) {
        uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(PAGE - 1);
        uintptr_t e = size ? (((uintptr_t)addr + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1)) : r->base + r->size;
        if (!size && a != r->base) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); goto out; }
        if (e > r->base + r->size) e = r->base + r->size;
        mmap((void *)a, e - a, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        for (uintptr_t p = a; p < e; p += PAGE) r->pages[(p - r->base) / PAGE] = 0;
        ok = TRUE;
    } else lsiw_set_last_error(ERROR_INVALID_PARAMETER);
out:
    pthread_mutex_unlock(&g_vlock);
    return ok;
}

API BOOL VirtualFree(void *addr, SIZE_T size, DWORD type) { return virtual_free(addr, size, type); }
API BOOL VirtualFreeEx(HANDLE proc, void *addr, SIZE_T size, DWORD type) { (void)proc; return virtual_free(addr, size, type); }

static BOOL virtual_protect(void *addr, SIZE_T size, DWORD protect, DWORD *old)
{
    int prot = posix_prot(protect);
    if (prot < 0) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(PAGE - 1);
    uintptr_t e = ((uintptr_t)addr + size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    if (e == a) e = a + PAGE;
    pthread_mutex_lock(&g_vlock);
    struct vregion *r = region_find(a);
    DWORD prev;
    if (r) {
        prev = r->pages[(a - r->base) / PAGE];
        if (!prev) { pthread_mutex_unlock(&g_vlock); lsiw_set_last_error(487); return FALSE; }
    } else {
        struct map_info mi;
        lsiw_query_maps(a, &mi);
        prev = mi.mapped ? win_prot(mi.prot) : PAGE_NOACCESS;
        /* Image sections that were writable are copy-on-write on Windows. */
        if (mi.mapped && prev == PAGE_READWRITE && lsiw_module_from_address(addr)) prev = PAGE_READWRITE;
    }
    if (mprotect((void *)a, e - a, prot) != 0) {
        pthread_mutex_unlock(&g_vlock);
        lsiw_set_last_error(errno == ENOMEM ? 487 : lsiw_errno_to_win(errno));
        return FALSE;
    }
    if (r) for (uintptr_t p = a; p < e && p < r->base + r->size; p += PAGE) r->pages[(p - r->base) / PAGE] = (uint16_t)protect;
    pthread_mutex_unlock(&g_vlock);
    if (old) *old = prev;
    return TRUE;
}

API BOOL VirtualProtect(void *addr, SIZE_T size, DWORD protect, DWORD *old) { return virtual_protect(addr, size, protect, old); }
API BOOL VirtualProtectEx(HANDLE proc, void *addr, SIZE_T size, DWORD protect, DWORD *old) { (void)proc; return virtual_protect(addr, size, protect, old); }
API BOOL VirtualProtectFromApp(void *addr, SIZE_T size, ULONG protect, ULONG *old) { return virtual_protect(addr, size, protect, (DWORD *)old); }

struct mbi {
    void *base, *alloc_base;
    DWORD alloc_protect;
    WORD partition, pad;
    SIZE_T region_size;
    DWORD state, protect, type, pad2;
};

static SIZE_T virtual_query(const void *addr, struct mbi *m, SIZE_T len)
{
    if (len < sizeof *m) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(PAGE - 1);
    if (a >= 0x7ffffffeffffULL) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    memset(m, 0, sizeof *m);
    pthread_mutex_lock(&g_vlock);
    struct vregion *r = region_find(a);
    if (r) {
        size_t i = (a - r->base) / PAGE, n = r->size / PAGE, j = i;
        uint16_t p = r->pages[i];
        while (j < n && r->pages[j] == p) j++;
        m->base = (void *)a;
        m->alloc_base = (void *)r->base;
        m->alloc_protect = r->alloc_protect;
        m->region_size = (j - i) * PAGE;
        m->state = p ? MEM_COMMIT : MEM_RESERVE;
        m->protect = p;
        m->type = MEM_PRIVATE;
        pthread_mutex_unlock(&g_vlock);
        return sizeof *m;
    }
    pthread_mutex_unlock(&g_vlock);
    struct map_info mi;
    lsiw_query_maps(a, &mi);
    m->base = (void *)a;
    if (!mi.mapped) {
        /* Free memory: stop at the next LSIW reservation, too. */
        uintptr_t end = mi.end;
        pthread_mutex_lock(&g_vlock);
        for (struct vregion *v = g_regions; v; v = v->next) if (v->base > a && v->base < end) end = v->base;
        pthread_mutex_unlock(&g_vlock);
        m->region_size = end - a;
        m->state = MEM_FREE;
        m->protect = PAGE_NOACCESS;
        return sizeof *m;
    }
    struct lsiw_module *mod = lsiw_module_from_address(addr);
    m->alloc_base = mod ? (void *)mod->base : (void *)mi.start;
    m->alloc_protect = mod ? PAGE_EXECUTE_WRITECOPY : win_prot(mi.prot);
    m->region_size = mi.end - a;
    m->state = MEM_COMMIT;
    m->protect = win_prot(mi.prot);
    m->type = mod ? MEM_IMAGE : mi.file ? MEM_MAPPED : MEM_PRIVATE;
    return sizeof *m;
}

API SIZE_T VirtualQuery(const void *addr, struct mbi *m, SIZE_T len) { return virtual_query(addr, m, len); }
API SIZE_T VirtualQueryEx(HANDLE proc, const void *addr, struct mbi *m, SIZE_T len) { (void)proc; return virtual_query(addr, m, len); }
API BOOL VirtualLock(void *a, SIZE_T n) { mlock(a, n); return TRUE; }
API BOOL VirtualUnlock(void *a, SIZE_T n) { munlock(a, n); return TRUE; }
API BOOL FlushInstructionCache(HANDLE p, const void *a, SIZE_T n) { (void)p; (void)a; (void)n; return TRUE; }
API void FlushProcessWriteBuffers(void) { __sync_synchronize(); }
API SIZE_T GetLargePageMinimum(void) { return 0x200000; }
API UINT GetWriteWatch(DWORD f, void *a, SIZE_T n, void **addrs, ULONG_PTR *count, ULONG *gran)
{ (void)f; (void)a; (void)n; (void)addrs; (void)count; (void)gran; return ERROR_NOT_SUPPORTED; }
API UINT ResetWriteWatch(void *a, SIZE_T n) { (void)a; (void)n; return 0; }
API BOOL PrefetchVirtualMemory(HANDLE p, ULONG_PTR n, void *e, ULONG f) { (void)p; (void)n; (void)e; (void)f; return TRUE; }
API BOOL DiscardVirtualMemory(void *a, SIZE_T n) { madvise(a, n, MADV_DONTNEED); return 0; }
API BOOL OfferVirtualMemory(void *a, SIZE_T n, int p) { (void)p; madvise(a, n, MADV_FREE); return 0; }
API BOOL ReclaimVirtualMemory(const void *a, SIZE_T n) { (void)a; (void)n; return 0; }
API BOOL SetProcessWorkingSetSize(HANDLE p, SIZE_T a, SIZE_T b) { (void)p; (void)a; (void)b; return TRUE; }
API BOOL SetProcessWorkingSetSizeEx(HANDLE p, SIZE_T a, SIZE_T b, DWORD f) { (void)p; (void)a; (void)b; (void)f; return TRUE; }
API BOOL GetProcessWorkingSetSize(HANDLE p, SIZE_T *mn, SIZE_T *mx) { (void)p; *mn = 0x32000; *mx = 0x159000; return TRUE; }
API BOOL GetProcessWorkingSetSizeEx(HANDLE p, SIZE_T *mn, SIZE_T *mx, DWORD *f) { (void)p; *mn = 0x32000; *mx = 0x159000; if (f) *f = 0; return TRUE; }
API BOOL SetProcessValidCallTargets(HANDLE p, void *v, SIZE_T s, ULONG n, void *t) { (void)p; (void)v; (void)s; (void)n; (void)t; return TRUE; }

/* Own-process memory access. */
static int is_self(HANDLE h) { return h == (HANDLE)(intptr_t)-1 || !h || lsiw_handle_get(h, H_PROCESS) == NULL; }
API BOOL ReadProcessMemory(HANDLE p, const void *src, void *dst, SIZE_T n, SIZE_T *done)
{
    if (!is_self(p)) {
        struct lsiw_handle_obj *o = lsiw_handle_get(p, H_PROCESS);
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/mem", o->pid);
        FILE *f = fopen(path, "rb");
        if (!f) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
        fseeko(f, (off_t)(uintptr_t)src, SEEK_SET);
        size_t r = fread(dst, 1, n, f);
        fclose(f);
        if (done) *done = r;
        return r == n;
    }
    memcpy(dst, src, n);
    if (done) *done = n;
    return TRUE;
}
API BOOL WriteProcessMemory(HANDLE p, void *dst, const void *src, SIZE_T n, SIZE_T *done)
{
    if (!is_self(p)) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    memcpy(dst, src, n);
    if (done) *done = n;
    return TRUE;
}

/* psapi memory counters, from /proc/self/statm. */
struct pmc { DWORD cb, page_faults; SIZE_T peak_ws, ws, peak_paged, paged, peak_nonpaged, nonpaged, pagefile, peak_pagefile, private_usage; };
API BOOL K32GetProcessMemoryInfo(HANDLE p, struct pmc *c, DWORD cb)
{
    (void)p;
    if (cb < 72) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    unsigned long size = 0, res = 0;
    FILE *f = fopen("/proc/self/statm", "re");
    if (f) { if (fscanf(f, "%lu %lu", &size, &res) != 2) size = res = 0; fclose(f); }
    long pg = sysconf(_SC_PAGESIZE);
    memset(c, 0, cb);
    c->cb = cb;
    c->ws = c->peak_ws = res * (SIZE_T)pg;
    c->pagefile = c->peak_pagefile = size * (SIZE_T)pg;
    if (cb >= 80) c->private_usage = c->pagefile;
    return TRUE;
}

/* ================================================================== */
/* Probing                                                              */
/* ================================================================== */

static BOOL range_ok(const void *p, SIZE_T n, int need)
{
    if (!n) return FALSE;
    if (!p) return TRUE;
    uintptr_t a = (uintptr_t)p, e = a + n;
    if (e < a) return TRUE;
    while (a < e) {
        struct map_info mi;
        struct vregion *r;
        pthread_mutex_lock(&g_vlock);
        r = region_find(a);
        int ok = 1;
        if (r) {
            int pr = posix_prot(r->pages[(a - r->base) / PAGE]);
            ok = pr > 0 && (pr & need) == need;
            pthread_mutex_unlock(&g_vlock);
            if (!ok) return TRUE;
            a = (a & ~(uintptr_t)(PAGE - 1)) + PAGE;
            continue;
        }
        pthread_mutex_unlock(&g_vlock);
        lsiw_query_maps(a, &mi);
        if (!mi.mapped || (mi.prot & need) != need) return TRUE;
        a = mi.end;
    }
    return FALSE;
}

API BOOL IsBadReadPtr(const void *p, ULONG_PTR n) { return range_ok(p, n, PROT_READ); }
API BOOL IsBadWritePtr(void *p, ULONG_PTR n) { return range_ok(p, n, PROT_READ | PROT_WRITE); }
API BOOL IsBadHugeReadPtr(const void *p, ULONG_PTR n) { return range_ok(p, n, PROT_READ); }
API BOOL IsBadHugeWritePtr(void *p, ULONG_PTR n) { return range_ok(p, n, PROT_READ | PROT_WRITE); }
API BOOL IsBadCodePtr(void *p) { return range_ok(p, 1, PROT_READ); }
API BOOL IsBadStringPtrA(const char *s, ULONG_PTR max)
{
    if (!s) return TRUE;
    for (ULONG_PTR i = 0; i < max; i++) {
        if (((uintptr_t)(s + i) & (PAGE - 1)) == 0 || i == 0) if (range_ok(s + i, 1, PROT_READ)) return TRUE;
        if (!s[i]) break;
    }
    return FALSE;
}
API BOOL IsBadStringPtrW(const WCHAR *s, ULONG_PTR max)
{
    if (!s) return TRUE;
    for (ULONG_PTR i = 0; i < max; i++) {
        if (((uintptr_t)(s + i) & (PAGE - 1)) == 0 || i == 0) if (range_ok(s + i, 2, PROT_READ)) return TRUE;
        if (!s[i]) break;
    }
    return FALSE;
}

/* ================================================================== */
/* Rtl memory helpers exported by kernel32 and ntdll                    */
/* ================================================================== */

API void RtlMoveMemory(void *d, const void *s, SIZE_T n) { memmove(d, s, n); }
API void RtlCopyMemory(void *d, const void *s, SIZE_T n) { memmove(d, s, n); }
API void RtlZeroMemory(void *d, SIZE_T n) { memset(d, 0, n); }
API void RtlFillMemory(void *d, SIZE_T n, BYTE v) { memset(d, v, n); }
API SIZE_T RtlCompareMemory(const void *a, const void *b, SIZE_T n)
{
    SIZE_T i = 0;
    while (i < n && ((const BYTE *)a)[i] == ((const BYTE *)b)[i]) i++;
    return i;
}
API void *RtlSecureZeroMemory(void *d, SIZE_T n) { volatile BYTE *p = d; while (n--) *p++ = 0; return d; }

/* ================================================================== */
/* Interlocked singly-linked lists (16-byte header)                     */
/* ================================================================== */

/* Layout of the x64 SLIST_HEADER: the first quadword holds Depth (16
 * bits) and Sequence (48 bits); the second holds HeaderType in bit 0 and
 * the first entry's address (16-byte aligned) in bits 4..63. A process-wide
 * spinlock makes every operation atomic. */
typedef struct slist_entry { struct slist_entry *next; } SLIST_ENTRY;
typedef struct { uint64_t lo, hi; } __attribute__((aligned(16))) SLIST_HEADER;

static volatile int g_slist_lock;
static inline void sl_lock(void) { while (__sync_lock_test_and_set(&g_slist_lock, 1)) while (g_slist_lock) __builtin_ia32_pause(); }
static inline void sl_unlock(void) { __sync_lock_release(&g_slist_lock); }
static inline SLIST_ENTRY *sl_first(SLIST_HEADER *h) { return (SLIST_ENTRY *)(uintptr_t)(h->hi & ~(uint64_t)15); }
static inline void sl_set(SLIST_HEADER *h, SLIST_ENTRY *first, uint64_t depth)
{
    uint64_t seq = (h->lo >> 16) + 1;
    h->lo = (depth & 0xffff) | (seq << 16);
    h->hi = (uint64_t)(uintptr_t)first | 1;
}

API void InitializeSListHead(SLIST_HEADER *h) { h->lo = 0; h->hi = 1; }
API SLIST_ENTRY *InterlockedPushEntrySList(SLIST_HEADER *h, SLIST_ENTRY *e)
{
    sl_lock();
    SLIST_ENTRY *old = sl_first(h);
    e->next = old;
    sl_set(h, e, (h->lo & 0xffff) + 1);
    sl_unlock();
    return old;
}
API SLIST_ENTRY *InterlockedPopEntrySList(SLIST_HEADER *h)
{
    sl_lock();
    SLIST_ENTRY *e = sl_first(h);
    if (e) sl_set(h, e->next, (h->lo & 0xffff) - 1);
    sl_unlock();
    return e;
}
API SLIST_ENTRY *InterlockedFlushSList(SLIST_HEADER *h)
{
    sl_lock();
    SLIST_ENTRY *e = sl_first(h);
    if (e) sl_set(h, NULL, 0);
    sl_unlock();
    return e;
}
API SLIST_ENTRY *InterlockedPushListSListEx(SLIST_HEADER *h, SLIST_ENTRY *first, SLIST_ENTRY *last, ULONG count)
{
    sl_lock();
    SLIST_ENTRY *old = sl_first(h);
    last->next = old;
    sl_set(h, first, (h->lo & 0xffff) + count);
    sl_unlock();
    return old;
}
API SLIST_ENTRY *InterlockedPushListSList(SLIST_HEADER *h, SLIST_ENTRY *first, SLIST_ENTRY *last, ULONG count)
{ return InterlockedPushListSListEx(h, first, last, count); }
API WORD QueryDepthSList(SLIST_HEADER *h) { return (WORD)(h->lo & 0xffff); }
API SLIST_ENTRY *RtlFirstEntrySList(const SLIST_HEADER *h) { return sl_first((SLIST_HEADER *)h); }

/* ================================================================== */
/* Fibers                                                               */
/* ================================================================== */

typedef void (WINAPI *fiber_start_fn)(void *);

struct fiber {
    void *param;                 /* must be first: GetFiberData() reads it */
    void *stack_base, *stack_limit;
    void *alloc;
    size_t alloc_size;
    fiber_start_fn start;
    int is_thread;
    ucontext_t ctx;
};

static inline uint8_t *teb(void) { return lsiw_teb_current(); }

static void fiber_entry(void)
{
    struct fiber *f = *(struct fiber **)(teb() + 0x20);
    f->start(f->param);
    /* Returning from a fiber's start routine ends the thread. */
    void (WINAPI *exit_thread)(DWORD) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "ExitThread");
    exit_thread(0);
}

API void *ConvertThreadToFiberEx(void *param, DWORD flags)
{
    (void)flags;
    uint8_t *t = teb();
    if (*(void **)(t + 0x20)) { lsiw_set_last_error(1280 /* ERROR_ALREADY_FIBER */); return NULL; }
    struct fiber *f = calloc(1, sizeof *f);
    f->param = param;
    f->is_thread = 1;
    f->stack_base = *(void **)(t + 0x08);
    f->stack_limit = *(void **)(t + 0x10);
    *(void **)(t + 0x20) = f;
    return f;
}
API void *ConvertThreadToFiber(void *param) { return ConvertThreadToFiberEx(param, 0); }

API BOOL ConvertFiberToThread(void)
{
    uint8_t *t = teb();
    struct fiber *f = *(struct fiber **)(t + 0x20);
    if (!f) { lsiw_set_last_error(1281 /* ERROR_ALREADY_THREAD */); return FALSE; }
    *(void **)(t + 0x20) = NULL;
    if (f->is_thread) free(f);
    return TRUE;
}

API void *CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, fiber_start_fn start, void *param)
{
    (void)commit; (void)flags;
    size_t size = reserve ? reserve : 0x100000;
    if (size < 0x10000) size = 0x10000;
    size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
    uint8_t *stack = mmap(NULL, size + PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    mprotect(stack, PAGE, PROT_NONE);                 /* guard page */
    struct fiber *f = calloc(1, sizeof *f);
    f->param = param;
    f->start = start;
    f->alloc = stack;
    f->alloc_size = size + PAGE;
    f->stack_limit = stack + PAGE;
    f->stack_base = stack + size + PAGE;
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = stack + PAGE;
    f->ctx.uc_stack.ss_size = size;
    f->ctx.uc_link = NULL;
    makecontext(&f->ctx, fiber_entry, 0);
    return f;
}
API void *CreateFiber(SIZE_T stack, fiber_start_fn start, void *param) { return CreateFiberEx(0, stack, 0, start, param); }

API void SwitchToFiber(void *to)
{
    uint8_t *t = teb();
    struct fiber *cur = *(struct fiber **)(t + 0x20), *next = to;
    if (!next || cur == next) return;
    if (cur) {
        cur->stack_base = *(void **)(t + 0x08);
        cur->stack_limit = *(void **)(t + 0x10);
    }
    *(void **)(t + 0x20) = next;
    *(void **)(t + 0x08) = next->stack_base;
    *(void **)(t + 0x10) = next->stack_limit;
    if (cur) swapcontext(&cur->ctx, &next->ctx);
    else setcontext(&next->ctx);
}

API void DeleteFiber(void *fiber)
{
    struct fiber *f = fiber;
    if (!f) return;
    if (*(void **)(teb() + 0x20) == f) {
        void (WINAPI *exit_thread)(DWORD) = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), "ExitThread");
        exit_thread(0);
    }
    if (f->alloc) munmap(f->alloc, f->alloc_size);
    free(f);
}

/* ================================================================== */
/* Export table                                                         */
/* ================================================================== */

const struct lsiw_export lsiw_k32mem_exports[] = {
    E(GetProcessHeap), E(GetProcessHeaps), E(HeapCreate), E(HeapDestroy), E(HeapAlloc), E(HeapFree), E(HeapReAlloc),
    E(HeapSize), E(HeapValidate), E(HeapCompact), E(HeapLock), E(HeapUnlock), E(HeapWalk), E(HeapSetInformation),
    E(HeapQueryInformation), E(HeapSummary),
    E(RtlAllocateHeap), E(RtlFreeHeap), E(RtlReAllocateHeap), E(RtlSizeHeap), E(RtlCreateHeap), E(RtlDestroyHeap), E(RtlGetProcessHeaps),
    E(GlobalAlloc), E(GlobalFree), E(GlobalReAlloc), E(GlobalLock), E(GlobalUnlock), E(GlobalSize), E(GlobalHandle),
    E(GlobalFlags), E(GlobalCompact), E(LocalAlloc), E(LocalFree), E(LocalReAlloc), E(LocalLock), E(LocalUnlock),
    E(LocalSize), E(LocalHandle), E(LocalFlags),
    E(VirtualAlloc), E(VirtualAllocEx), E(VirtualAllocExNuma), E(VirtualAlloc2), E(VirtualAllocFromApp),
    E(VirtualFree), E(VirtualFreeEx), E(VirtualProtect), E(VirtualProtectEx), E(VirtualProtectFromApp),
    E(VirtualQuery), E(VirtualQueryEx), E(VirtualLock), E(VirtualUnlock), E(FlushInstructionCache), E(FlushProcessWriteBuffers),
    E(GetLargePageMinimum), E(GetWriteWatch), E(ResetWriteWatch), E(PrefetchVirtualMemory), E(DiscardVirtualMemory),
    E(OfferVirtualMemory), E(ReclaimVirtualMemory), E(SetProcessWorkingSetSize), E(SetProcessWorkingSetSizeEx),
    E(GetProcessWorkingSetSize), E(GetProcessWorkingSetSizeEx), E(SetProcessValidCallTargets),
    E(ReadProcessMemory), E(WriteProcessMemory), E(K32GetProcessMemoryInfo), EX("GetProcessMemoryInfo", K32GetProcessMemoryInfo),
    E(IsBadReadPtr), E(IsBadWritePtr), E(IsBadHugeReadPtr), E(IsBadHugeWritePtr), E(IsBadCodePtr), E(IsBadStringPtrA), E(IsBadStringPtrW),
    E(RtlMoveMemory), E(RtlCopyMemory), E(RtlZeroMemory), E(RtlFillMemory), E(RtlCompareMemory), E(RtlSecureZeroMemory),
    E(InitializeSListHead), E(InterlockedPushEntrySList), E(InterlockedPopEntrySList), E(InterlockedFlushSList),
    E(InterlockedPushListSListEx), E(InterlockedPushListSList), E(QueryDepthSList), E(RtlFirstEntrySList),
    EX("RtlInitializeSListHead", InitializeSListHead), EX("RtlInterlockedPushEntrySList", InterlockedPushEntrySList),
    EX("RtlInterlockedPopEntrySList", InterlockedPopEntrySList), EX("RtlInterlockedFlushSList", InterlockedFlushSList),
    EX("RtlQueryDepthSList", QueryDepthSList), EX("RtlInterlockedPushListSListEx", InterlockedPushListSListEx),
    E(ConvertThreadToFiber), E(ConvertThreadToFiberEx), E(ConvertFiberToThread), E(CreateFiber), E(CreateFiberEx),
    E(SwitchToFiber), E(DeleteFiber),
    { NULL, NULL }
};
