/* ntdll objects, security, registry and strings, plus the kernel32 /
 * advapi32 / iphlpapi functions that real programs (node.exe, MinGit, MSYS2)
 * still import.
 *
 * The Nt* object routines sit on top of the kernel32/advapi32 implementations
 * that already exist (events/mutexes/semaphores/timers in k32_process.c,
 * sections over CreateFileMapping/MapViewOfFile in k32_file.c, job objects in
 * k32_sys.c, the registry and all the security primitives in advapi32.c); they
 * are reached through lsiw_builtin_lookup so there is one implementation of
 * each.  The Rtl* security and string helpers operate directly on the exact
 * Windows x64 structure layouts.  Behaviour that Linux cannot provide
 * (transactions, cross-process debugging, tape drives) fails the documented
 * Windows way.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native.h"
#include "pe.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

/* ---- NTSTATUS values used throughout ---- */
#define STATUS_SUCCESS                 0x00000000
#define STATUS_TIMEOUT                 0x00000102
#define STATUS_UNSUCCESSFUL            0xC0000001u
#define STATUS_NOT_IMPLEMENTED         0xC0000002u
#define STATUS_INFO_LENGTH_MISMATCH    0xC0000004u
#define STATUS_INVALID_HANDLE          0xC0000008u
#define STATUS_INVALID_PARAMETER       0xC000000Du
#define STATUS_NO_MEMORY               0xC0000017u
#define STATUS_ACCESS_VIOLATION        0xC0000005u
#define STATUS_BUFFER_TOO_SMALL        0xC0000023u
#define STATUS_BUFFER_OVERFLOW         0x80000005u
#define STATUS_OBJECT_NAME_INVALID     0xC0000033u
#define STATUS_OBJECT_NAME_NOT_FOUND   0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION   0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND   0xC000003Au
#define STATUS_OBJECT_PATH_SYNTAX_BAD  0xC000003Bu
#define STATUS_OBJECT_TYPE_MISMATCH    0xC0000024u
#define STATUS_NOT_SUPPORTED           0xC00000BBu
#define STATUS_NO_MORE_ENTRIES         0x8000001Au
#define STATUS_MORE_ENTRIES            0x00000105u
#define ERROR_NOT_ALL_ASSIGNED         1300
#define STATUS_NOT_FOUND               0xC0000225u
#define STATUS_NAME_TOO_LONG           0xC0000106u

/* ---- common Windows types local to this module ---- */
typedef uint8_t  BOOLEAN;
typedef int64_t  LARGE_INTEGER;
typedef uint32_t ACCESS_MASK;
typedef int64_t  LONG_PTR;

typedef struct { WORD Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { WORD Length, MaximumLength; char *Buffer; } ANSI_STRING;

typedef struct {
    ULONG Length;
    HANDLE RootDirectory;
    UNICODE_STRING *ObjectName;
    ULONG Attributes;
    void *SecurityDescriptor;
    void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

typedef struct { union { NTSTATUS Status; void *Pointer; }; ULONG_PTR Information; } IO_STATUS_BLOCK;

/* ---- builtin-DLL function lookup helpers ---- */
static void *k32(const char *sym) { return lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), sym); }
static void *adv(const char *sym) { return lsiw_builtin_lookup(lsiw_builtin_find("advapi32.dll"), sym); }
static void *iph(const char *sym) { return lsiw_builtin_lookup(lsiw_builtin_find("iphlpapi.dll"), sym); }

/* Convert a counted UTF-16 object name to a malloc'd UTF-8 string. */
static char *oa_name(const OBJECT_ATTRIBUTES *oa)
{
    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer) return NULL;
    return lsiw_w2u_n(oa->ObjectName->Buffer, oa->ObjectName->Length / 2);
}

/* The leaf part of an NT object name, as a wide string kernel32 wants:
 * "\BaseNamedObjects\Foo" -> "Foo"; an unnamed object -> NULL. */
static WCHAR *oa_leaf_w(const OBJECT_ATTRIBUTES *oa)
{
    char *n = oa_name(oa);
    if (!n || !*n) { free(n); return NULL; }
    char *leaf = n;
    for (char *c = n; *c; c++) if (*c == '\\') leaf = c + 1;
    WCHAR *w = *leaf ? lsiw_u2w(leaf) : NULL;
    free(n);
    return w;
}

/* ================================================================== */
/* Synchronisation objects: events, mutants, semaphores, timers         */
/* ================================================================== */

/* NtCreateEvent's EventType: NotificationEvent (0) is manual-reset,
 * SynchronizationEvent (1) is auto-reset. */
API NTSTATUS NtCreateEvent(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa,
                           int type, BOOLEAN initial)
{
    (void)access;
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *ce)(void *, BOOL, BOOL, const WCHAR *) = k32("CreateEventW");
    WCHAR *name = oa_leaf_w(oa);
    DWORD prev = lsiw_get_last_error();
    lsiw_set_last_error(0);
    HANDLE h = ce(NULL, type == 0, initial != 0, name);
    NTSTATUS st = STATUS_SUCCESS;
    if (!h) st = STATUS_UNSUCCESSFUL;
    else if (name && lsiw_get_last_error() == ERROR_ALREADY_EXISTS) st = 0x40000000; /* STATUS_OBJECT_NAME_EXISTS */
    lsiw_set_last_error(prev);
    free(name);
    *out = h;
    return st;
}

API NTSTATUS NtOpenEvent(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *oe)(DWORD, BOOL, const WCHAR *) = k32("OpenEventW");
    WCHAR *name = oa_leaf_w(oa);
    HANDLE h = name ? oe(access, FALSE, name) : NULL;
    free(name);
    if (!h) { *out = NULL; return STATUS_OBJECT_NAME_NOT_FOUND; }
    *out = h;
    return STATUS_SUCCESS;
}

/* EVENT_BASIC_INFORMATION: { EVENT_TYPE EventType; LONG EventState; }. */
API NTSTATUS NtQueryEvent(HANDLE h, int cls, void *info, ULONG len, ULONG *ret)
{
    if (cls != 0) return STATUS_INVALID_PARAMETER;
    if (len < 8 || !info) { if (ret) *ret = 8; return STATUS_INFO_LENGTH_MISMATCH; }
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_EVENT);
    if (!o) return STATUS_INVALID_HANDLE;
    ((LONG *)info)[0] = o->manual_reset ? 0 : 1;   /* NotificationEvent / SynchronizationEvent */
    ((LONG *)info)[1] = o->signaled ? 1 : 0;
    if (ret) *ret = 8;
    return STATUS_SUCCESS;
}

/* "Mutant" is the NT name for a mutex. */
API NTSTATUS NtCreateMutant(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa, BOOLEAN owner)
{
    (void)access;
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *cm)(void *, BOOL, const WCHAR *) = k32("CreateMutexW");
    WCHAR *name = oa_leaf_w(oa);
    DWORD prev = lsiw_get_last_error();
    lsiw_set_last_error(0);
    HANDLE h = cm(NULL, owner != 0, name);
    NTSTATUS st = STATUS_SUCCESS;
    if (!h) st = STATUS_UNSUCCESSFUL;
    else if (name && lsiw_get_last_error() == ERROR_ALREADY_EXISTS) st = 0x40000000;
    lsiw_set_last_error(prev);
    free(name);
    *out = h;
    return st;
}

API NTSTATUS NtOpenMutant(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *om)(DWORD, BOOL, const WCHAR *) = k32("OpenMutexW");
    WCHAR *name = oa_leaf_w(oa);
    HANDLE h = name ? om(access, FALSE, name) : NULL;
    free(name);
    if (!h) { *out = NULL; return STATUS_OBJECT_NAME_NOT_FOUND; }
    *out = h;
    return STATUS_SUCCESS;
}

API NTSTATUS NtCreateSemaphore(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa,
                               LONG initial, LONG maximum)
{
    (void)access;
    if (!out) return STATUS_INVALID_PARAMETER;
    if (maximum <= 0 || initial < 0 || initial > maximum) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *cs)(void *, LONG, LONG, const WCHAR *) = k32("CreateSemaphoreW");
    WCHAR *name = oa_leaf_w(oa);
    DWORD prev = lsiw_get_last_error();
    lsiw_set_last_error(0);
    HANDLE h = cs(NULL, initial, maximum, name);
    NTSTATUS st = STATUS_SUCCESS;
    if (!h) st = STATUS_UNSUCCESSFUL;
    else if (name && lsiw_get_last_error() == ERROR_ALREADY_EXISTS) st = 0x40000000;
    lsiw_set_last_error(prev);
    free(name);
    *out = h;
    return st;
}

API NTSTATUS NtOpenSemaphore(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *os)(DWORD, BOOL, const WCHAR *) = k32("OpenSemaphoreW");
    WCHAR *name = oa_leaf_w(oa);
    HANDLE h = name ? os(access, FALSE, name) : NULL;
    free(name);
    if (!h) { *out = NULL; return STATUS_OBJECT_NAME_NOT_FOUND; }
    *out = h;
    return STATUS_SUCCESS;
}

/* SEMAPHORE_BASIC_INFORMATION: { LONG CurrentCount; LONG MaximumCount; }. */
API NTSTATUS NtQuerySemaphore(HANDLE h, int cls, void *info, ULONG len, ULONG *ret)
{
    if (cls != 0) return STATUS_INVALID_PARAMETER;
    if (len < 8 || !info) { if (ret) *ret = 8; return STATUS_INFO_LENGTH_MISMATCH; }
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_SEMAPHORE);
    if (!o) return STATUS_INVALID_HANDLE;
    ((LONG *)info)[0] = (LONG)o->count;
    ((LONG *)info)[1] = (LONG)o->max_count;
    if (ret) *ret = 8;
    return STATUS_SUCCESS;
}

/* Timers.  TimerType: NotificationTimer (0) is manual-reset. */
API NTSTATUS NtCreateTimer(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa, int type)
{
    (void)access;
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *ct)(void *, BOOL, const WCHAR *) = k32("CreateWaitableTimerW");
    WCHAR *name = oa_leaf_w(oa);
    HANDLE h = ct(NULL, type == 0, name);
    free(name);
    if (!h) { *out = NULL; return STATUS_UNSUCCESSFUL; }
    *out = h;
    return STATUS_SUCCESS;
}

API NTSTATUS NtSetTimer(HANDLE h, const LARGE_INTEGER *due, void *apc, void *apc_ctx,
                        BOOLEAN resume, LONG period, BOOLEAN *prev_state)
{
    BOOL (WINAPI *st)(HANDLE, const int64_t *, LONG, void *, void *, BOOL) = k32("SetWaitableTimer");
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_TIMER);
    if (prev_state) *prev_state = o ? (BOOLEAN)o->signaled : 0;
    return st(h, due, period, apc, apc_ctx, resume) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

API NTSTATUS NtCancelTimer(HANDLE h, BOOLEAN *current_state)
{
    BOOL (WINAPI *ct)(HANDLE) = k32("CancelWaitableTimer");
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_TIMER);
    if (current_state) *current_state = o ? (BOOLEAN)o->signaled : 0;
    return ct(h) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* TIMER_BASIC_INFORMATION: { LARGE_INTEGER RemainingTime; BOOLEAN TimerState; }.
 * Remaining time is not tracked precisely here, so a signalled timer reports
 * zero remaining and an unsignalled one reports "unknown" (0). */
API NTSTATUS NtQueryTimer(HANDLE h, int cls, void *info, ULONG len, ULONG *ret)
{
    if (cls != 0) return STATUS_INVALID_PARAMETER;
    if (len < 16 || !info) { if (ret) *ret = 16; return STATUS_INFO_LENGTH_MISMATCH; }
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_TIMER);
    if (!o) return STATUS_INVALID_HANDLE;
    *(LARGE_INTEGER *)info = 0;
    ((BYTE *)info)[8] = (BYTE)(o->signaled ? 1 : 0);
    if (ret) *ret = 16;
    return STATUS_SUCCESS;
}

/* The multimedia timer runs at ~1ms; report that as the min/max/current. */
API NTSTATUS NtQueryTimerResolution(ULONG *maximum, ULONG *minimum, ULONG *current)
{
    if (maximum) *maximum = 156250;   /* 15.625 ms in 100ns units */
    if (minimum) *minimum = 10000;     /* 1 ms */
    if (current) *current = 10000;
    return STATUS_SUCCESS;
}
API NTSTATUS NtSetTimerResolution(ULONG desired, BOOLEAN set, ULONG *current)
{ (void)desired; (void)set; if (current) *current = 10000; return STATUS_SUCCESS; }

/* ================================================================== */
/* Sections (file mappings)                                             */
/* ================================================================== */

/* NtCreateSection builds a CreateFileMapping over an optional backing file.
 * Only the current process can map a section here, matching the single
 * address space LSIW runs programs in. */
API NTSTATUS NtCreateSection(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa,
                             const LARGE_INTEGER *maxsize, ULONG protect, ULONG alloc_attr, HANDLE file)
{
    (void)access; (void)alloc_attr;
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *cfm)(HANDLE, void *, DWORD, DWORD, DWORD, const WCHAR *) = k32("CreateFileMappingW");
    WCHAR *name = oa_leaf_w(oa);
    uint64_t sz = maxsize ? (uint64_t)*maxsize : 0;
    HANDLE f = (file && file != INVALID_HANDLE_VALUE) ? file : INVALID_HANDLE_VALUE;
    HANDLE h = cfm(f, NULL, protect, (DWORD)(sz >> 32), (DWORD)sz, name);
    free(name);
    if (!h) return STATUS_UNSUCCESSFUL;
    *out = h;
    return STATUS_SUCCESS;
}

API NTSTATUS NtOpenSection(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *ofm)(DWORD, BOOL, const WCHAR *) = k32("OpenFileMappingW");
    WCHAR *name = oa_leaf_w(oa);
    HANDLE h = (name && ofm) ? ofm(access, FALSE, name) : NULL;
    free(name);
    if (!h) { *out = NULL; return STATUS_OBJECT_NAME_NOT_FOUND; }
    *out = h;
    return STATUS_SUCCESS;
}

/* protect (section page protection) -> the FILE_MAP_* access MapViewOfFile
 * wants. */
static DWORD section_to_filemap(ULONG protect)
{
    switch (protect & 0xFF) {
    case 0x02: return 0x0004;                 /* PAGE_READONLY -> FILE_MAP_READ */
    case 0x04: return 0x0002;                 /* PAGE_READWRITE -> FILE_MAP_WRITE */
    case 0x08: return 0x0001;                 /* PAGE_WRITECOPY -> FILE_MAP_COPY */
    case 0x20: return 0x0004 | 0x0020;        /* PAGE_EXECUTE_READ */
    case 0x40: return 0x0002 | 0x0020;        /* PAGE_EXECUTE_READWRITE */
    default:   return 0x0002;
    }
}

/* ViewSize of 0 maps the whole section from SectionOffset to the end.
 * InheritDisposition is irrelevant with one process.  process must be the
 * current process (-1 or the process pseudo-handle). */
API NTSTATUS NtMapViewOfSection(HANDLE section, HANDLE process, void **base, ULONG_PTR zero_bits,
                                SIZE_T commit, LARGE_INTEGER *offset, SIZE_T *view_size,
                                int inherit, ULONG alloc_type, ULONG protect)
{
    (void)zero_bits; (void)commit; (void)inherit; (void)alloc_type;
    if (!base || !view_size) return STATUS_INVALID_PARAMETER;
    if (process && process != INVALID_HANDLE_VALUE && process != (HANDLE)(intptr_t)-1)
        return STATUS_NOT_SUPPORTED;    /* mapping into another process is unsupported */
    void *(WINAPI *mv)(HANDLE, DWORD, DWORD, DWORD, SIZE_T, void *) = k32("MapViewOfFileEx");
    uint64_t off = offset ? (uint64_t)*offset : 0;
    void *p = mv(section, section_to_filemap(protect), (DWORD)(off >> 32), (DWORD)off, *view_size, *base);
    if (!p) return STATUS_INVALID_PARAMETER;
    *base = p;
    if (*view_size == 0) {
        struct lsiw_handle_obj *o = lsiw_handle_get(section, H_MAPPING);
        if (o && o->map_size > off) *view_size = (SIZE_T)(o->map_size - off);
    }
    return STATUS_SUCCESS;
}

API NTSTATUS NtUnmapViewOfSection(HANDLE process, void *base)
{
    if (process && process != INVALID_HANDLE_VALUE && process != (HANDLE)(intptr_t)-1)
        return STATUS_NOT_SUPPORTED;
    BOOL (WINAPI *um)(const void *) = k32("UnmapViewOfFile");
    return um(base) ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

/* ================================================================== */
/* The object namespace: directories and symbolic links                */
/* ================================================================== */

/* A small per-process namespace.  Named kernel objects created through
 * kernel32 live under \BaseNamedObjects; the device links under \?? resolve
 * drive letters to device paths.  Entries are tracked for enumeration and
 * for symbolic-link queries; they are not a general object store. */
struct ns_entry {
    char   *name;          /* leaf name within the parent directory */
    int     is_dir;
    char   *target;        /* symbolic-link target (is_dir == 0) */
    struct ns_entry *parent, *child, *sibling;
};

static struct ns_entry *g_ns_root;

static struct ns_entry *ns_new(struct ns_entry *parent, const char *name, int is_dir, const char *target)
{
    struct ns_entry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->name = strdup(name ? name : "");
    e->is_dir = is_dir;
    e->target = target ? strdup(target) : NULL;
    e->parent = parent;
    if (parent) { e->sibling = parent->child; parent->child = e; }
    return e;
}

static struct ns_entry *ns_find_child(struct ns_entry *dir, const char *name)
{
    if (!dir) return NULL;
    for (struct ns_entry *c = dir->child; c; c = c->sibling)
        if (!strcasecmp(c->name, name)) return c;
    return NULL;
}

/* Resolve an absolute "\A\B\C" path to a directory, optionally creating the
 * intermediate directories, returning the parent and leaf. */
static struct ns_entry *ns_resolve(const char *path, int create, char **leaf_out)
{
    if (!g_ns_root) {
        g_ns_root = ns_new(NULL, "", 1, NULL);
        struct ns_entry *bno = ns_new(g_ns_root, "BaseNamedObjects", 1, NULL);
        (void)bno;
        struct ns_entry *sessions = ns_new(g_ns_root, "Sessions", 1, NULL);
        struct ns_entry *s1 = ns_new(sessions, "1", 1, NULL);
        ns_new(s1, "BaseNamedObjects", 1, NULL);
        struct ns_entry *dosdev = ns_new(g_ns_root, "??", 1, NULL);
        struct ns_entry *dev = ns_new(g_ns_root, "Device", 1, NULL);
        ns_new(dev, "HarddiskVolume1", 1, NULL);
        ns_new(dosdev, "C:", 0, "\\Device\\HarddiskVolume1");
    }
    if (!path || path[0] != '\\') return NULL;
    char *dup = strdup(path);
    if (!dup) return NULL;
    struct ns_entry *cur = g_ns_root;
    char *p = dup + 1, *leaf = NULL;
    while (*p) {
        char *slash = strchr(p, '\\');
        if (slash) *slash = 0;
        if (!*p) { p = slash ? slash + 1 : p + strlen(p); continue; }
        if (!slash) { leaf = p; break; }     /* final component */
        struct ns_entry *next = ns_find_child(cur, p);
        if (!next) {
            if (!create) { free(dup); return NULL; }
            next = ns_new(cur, p, 1, NULL);
        }
        if (!next->is_dir) { free(dup); return NULL; }
        cur = next;
        p = slash + 1;
    }
    if (leaf_out) *leaf_out = leaf ? strdup(leaf) : strdup("");
    free(dup);
    return cur;
}

/* Directory-object and symlink handles carry an ns_entry pointer. */
static HANDLE ns_handle(struct ns_entry *e, int type)
{
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(type, &o);
    if (h) o->data = e;
    return h;
}
static struct ns_entry *ns_from_handle(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_GENERIC);
    return o ? (struct ns_entry *)o->data : NULL;
}

API NTSTATUS NtCreateDirectoryObject(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    (void)access;
    if (!out) return STATUS_INVALID_PARAMETER;
    char *name = oa_name(oa);
    if (!name || name[0] != '\\') { free(name); return STATUS_OBJECT_PATH_SYNTAX_BAD; }
    char *leaf;
    struct ns_entry *parent = ns_resolve(name, 1, &leaf);
    free(name);
    if (!parent) return STATUS_OBJECT_PATH_NOT_FOUND;
    struct ns_entry *e = (leaf && *leaf) ? ns_find_child(parent, leaf) : parent;
    if (!e) e = ns_new(parent, leaf, 1, NULL);
    free(leaf);
    if (!e || !e->is_dir) return STATUS_OBJECT_TYPE_MISMATCH;
    *out = ns_handle(e, H_GENERIC);
    return *out ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

API NTSTATUS NtOpenDirectoryObject(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    (void)access;
    if (!out) return STATUS_INVALID_PARAMETER;
    char *name = oa_name(oa);
    if (!name || name[0] != '\\') { free(name); return STATUS_OBJECT_PATH_SYNTAX_BAD; }
    char *leaf;
    struct ns_entry *parent = ns_resolve(name, 0, &leaf);
    free(name);
    if (!parent) return STATUS_OBJECT_NAME_NOT_FOUND;
    struct ns_entry *e = (leaf && *leaf) ? ns_find_child(parent, leaf) : parent;
    free(leaf);
    if (!e || !e->is_dir) return STATUS_OBJECT_NAME_NOT_FOUND;
    *out = ns_handle(e, H_GENERIC);
    return *out ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

/* OBJECT_DIRECTORY_INFORMATION: { UNICODE_STRING Name; UNICODE_STRING TypeName; }.
 * RestartScan resets the per-call index passed through *context. */
API NTSTATUS NtQueryDirectoryObject(HANDLE dir, void *buf, ULONG len, BOOLEAN single,
                                    BOOLEAN restart, ULONG *context, ULONG *ret_len)
{
    struct ns_entry *d = ns_from_handle(dir);
    if (!d || !d->is_dir) return STATUS_INVALID_HANDLE;
    if (!context) return STATUS_INVALID_PARAMETER;
    if (restart) *context = 0;
    /* Walk to the context-th child (insertion order is reversed, which is fine
     * for an unordered directory). */
    ULONG idx = 0;
    struct ns_entry *c = d->child;
    for (; c && idx < *context; c = c->sibling) idx++;
    if (!c) { if (ret_len) *ret_len = 0; return STATUS_NO_MORE_ENTRIES; }

    const size_t rec = 2 * sizeof(UNICODE_STRING);
    BYTE *out = buf;
    size_t used = 0, stroff = rec * (single ? 1 : 0);
    ULONG produced = 0;
    /* Two layouts: one entry, or an array terminated by a zeroed record.
     * We produce entries until the buffer or the directory runs out. */
    size_t array_recs = single ? 1 : 0;
    if (!single) {                        /* count how many fit (plus terminator) */
        size_t avail = len, need = rec;   /* terminator */
        ULONG n = 0;
        for (struct ns_entry *t = c; t; t = t->sibling) {
            int wl = lsiw_utf8_to_utf16(t->name, -1, NULL, 0);
            size_t per = rec + (size_t)(wl + 1) * 2;
            if (need + per > avail) break;
            need += per; n++;
        }
        array_recs = n ? n : 0;
        if (!n) { if (ret_len) *ret_len = (ULONG)(rec + ((lsiw_utf8_to_utf16(c->name, -1, NULL, 0) + 1) * 2)); return STATUS_BUFFER_TOO_SMALL; }
        stroff = rec * (array_recs + 1);
    }
    if (single) {
        int wl = lsiw_utf8_to_utf16(c->name, -1, NULL, 0);
        if (len < rec + (size_t)(wl + 1) * 2) { if (ret_len) *ret_len = (ULONG)(rec + (size_t)(wl + 1) * 2); return STATUS_BUFFER_TOO_SMALL; }
    }
    used = stroff;
    size_t ri = 0;
    for (struct ns_entry *t = c; t && (single ? ri < 1 : ri < array_recs); t = t->sibling, ri++) {
        UNICODE_STRING *us = (UNICODE_STRING *)(out + ri * rec);
        int wl = lsiw_utf8_to_utf16(t->name, -1, NULL, 0);
        WCHAR *dst = (WCHAR *)(out + used);
        lsiw_utf8_to_utf16(t->name, -1, dst, wl + 1);
        us[0].Length = (WORD)(wl * 2);
        us[0].MaximumLength = (WORD)((wl + 1) * 2);
        us[0].Buffer = dst;
        static const WCHAR tn[] = { 'D','i','r','e','c','t','o','r','y',0 };
        static const WCHAR tsym[] = { 'S','y','m','b','o','l','i','c','L','i','n','k',0 };
        const WCHAR *tp = t->is_dir ? tn : tsym;
        us[1].Length = (WORD)(lsiw_wcslen(tp) * 2);
        us[1].MaximumLength = (WORD)(us[1].Length + 2);
        us[1].Buffer = (WCHAR *)tp;
        used += (size_t)(wl + 1) * 2;
        produced++;
        idx++;
    }
    if (!single) memset(out + array_recs * rec, 0, rec);  /* terminating record */
    *context = idx;
    if (ret_len) *ret_len = (ULONG)used;
    return single && c->sibling ? STATUS_MORE_ENTRIES : STATUS_SUCCESS;
}

API NTSTATUS NtCreateSymbolicLinkObject(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa,
                                        UNICODE_STRING *target)
{
    (void)access;
    if (!out || !target) return STATUS_INVALID_PARAMETER;
    char *name = oa_name(oa);
    if (!name || name[0] != '\\') { free(name); return STATUS_OBJECT_PATH_SYNTAX_BAD; }
    char *leaf;
    struct ns_entry *parent = ns_resolve(name, 1, &leaf);
    free(name);
    if (!parent) return STATUS_OBJECT_PATH_NOT_FOUND;
    char *tgt = lsiw_w2u_n(target->Buffer, target->Length / 2);
    struct ns_entry *e = ns_find_child(parent, leaf);
    if (e) { free(e->target); e->target = tgt; e->is_dir = 0; }
    else e = ns_new(parent, leaf, 0, NULL), (e ? (e->target = tgt, e->target) : (free(tgt), (char *)NULL));
    free(leaf);
    if (!e) return STATUS_NO_MEMORY;
    *out = ns_handle(e, H_GENERIC);
    return *out ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

API NTSTATUS NtOpenSymbolicLinkObject(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    (void)access;
    if (!out) return STATUS_INVALID_PARAMETER;
    char *name = oa_name(oa);
    if (!name || name[0] != '\\') { free(name); return STATUS_OBJECT_PATH_SYNTAX_BAD; }
    char *leaf;
    struct ns_entry *parent = ns_resolve(name, 0, &leaf);
    free(name);
    if (!parent) return STATUS_OBJECT_NAME_NOT_FOUND;
    struct ns_entry *e = ns_find_child(parent, leaf);
    free(leaf);
    if (!e || e->is_dir) return STATUS_OBJECT_NAME_NOT_FOUND;
    *out = ns_handle(e, H_GENERIC);
    return *out ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

API NTSTATUS NtQuerySymbolicLinkObject(HANDLE h, UNICODE_STRING *target, ULONG *ret_len)
{
    struct ns_entry *e = ns_from_handle(h);
    if (!e || e->is_dir) return STATUS_INVALID_HANDLE;
    if (!target) return STATUS_INVALID_PARAMETER;
    const char *t = e->target ? e->target : "";
    int wl = lsiw_utf8_to_utf16(t, -1, NULL, 0);
    if (ret_len) *ret_len = (ULONG)(wl * 2);
    if (target->MaximumLength < wl * 2) return STATUS_BUFFER_TOO_SMALL;
    int n = lsiw_utf8_to_utf16(t, -1, target->Buffer, target->MaximumLength / 2);
    target->Length = (WORD)(n * 2);
    return STATUS_SUCCESS;
}

/* ================================================================== */
/* Job objects                                                          */
/* ================================================================== */

API NTSTATUS NtCreateJobObject(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    (void)access;
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *cj)(void *, const WCHAR *) = k32("CreateJobObjectW");
    WCHAR *name = oa_leaf_w(oa);
    HANDLE h = cj(NULL, name);
    free(name);
    if (!h) { *out = NULL; return STATUS_UNSUCCESSFUL; }
    *out = h;
    return STATUS_SUCCESS;
}

API NTSTATUS NtOpenJobObject(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE (WINAPI *oj)(DWORD, BOOL, const WCHAR *) = k32("OpenJobObjectW");
    WCHAR *name = oa_leaf_w(oa);
    HANDLE h = name ? oj(access, FALSE, name) : NULL;
    free(name);
    if (!h) { *out = NULL; return STATUS_OBJECT_NAME_NOT_FOUND; }
    *out = h;
    return STATUS_SUCCESS;
}

API NTSTATUS NtAssignProcessToJobObject(HANDLE job, HANDLE process)
{
    BOOL (WINAPI *ap)(HANDLE, HANDLE) = k32("AssignProcessToJobObject");
    return ap(job, process) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

API NTSTATUS NtQueryInformationJobObject(HANDLE job, int cls, void *info, ULONG len, ULONG *ret)
{
    BOOL (WINAPI *q)(HANDLE, int, void *, DWORD, DWORD *) = k32("QueryInformationJobObject");
    DWORD got = 0;
    if (q(job, cls, info, len, &got)) { if (ret) *ret = got; return STATUS_SUCCESS; }
    DWORD e = lsiw_get_last_error();
    if (e == ERROR_INSUFFICIENT_BUFFER || e == ERROR_MORE_DATA) return STATUS_BUFFER_OVERFLOW;
    return STATUS_INVALID_PARAMETER;
}

API NTSTATUS NtSetInformationJobObject(HANDLE job, int cls, void *info, ULONG len)
{
    BOOL (WINAPI *s)(HANDLE, int, void *, DWORD) = k32("SetInformationJobObject");
    return s(job, cls, info, len) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* ================================================================== */
/* LUIDs and locking virtual memory                                     */
/* ================================================================== */

/* A LUID is a 64-bit value unique for the life of the process. */
API NTSTATUS NtAllocateLocallyUniqueId(void *luid)
{
    static uint64_t counter;
    if (!luid) return STATUS_INVALID_PARAMETER;
    uint64_t v = __atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST) + 999;
    memcpy(luid, &v, 8);
    return STATUS_SUCCESS;
}

API NTSTATUS NtLockVirtualMemory(HANDLE proc, void **base, SIZE_T *size, ULONG flags)
{
    (void)proc; (void)flags;
    if (!base || !size) return STATUS_INVALID_PARAMETER;
    if (mlock(*base, *size) == 0) return STATUS_SUCCESS;
    return errno == ENOMEM || errno == EAGAIN ? (NTSTATUS)0xC000009A /* INSUFFICIENT_RESOURCES */
                                              : (NTSTATUS)STATUS_INVALID_PARAMETER;
}
API NTSTATUS NtUnlockVirtualMemory(HANDLE proc, void **base, SIZE_T *size, ULONG flags)
{
    (void)proc; (void)flags;
    if (!base || !size) return STATUS_INVALID_PARAMETER;
    return munlock(*base, *size) == 0 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/* ================================================================== */
/* Registry (over the advapi32 implementation)                         */
/* ================================================================== */

#define NT_HKLM ((HANDLE)(ULONG_PTR)(LONG)0x80000002)
#define NT_HKU  ((HANDLE)(ULONG_PTR)(LONG)0x80000003)

/* Split an NT key path ("\Registry\Machine\Software\...") into a predefined
 * root handle and the remaining sub-path.  "\Registry\User\<sid>\..." keeps
 * the SID so advapi32 can map it to the current user's hive. */
static HANDLE nt_reg_root(const char *path, const char **sub)
{
    if (!strncasecmp(path, "\\Registry\\Machine", 17)) {
        *sub = path + 17;
        if (**sub == '\\') (*sub)++;
        return NT_HKLM;
    }
    if (!strncasecmp(path, "\\Registry\\User", 14)) {
        *sub = path + 14;
        if (**sub == '\\') (*sub)++;
        return NT_HKU;
    }
    return NULL;
}

/* Resolve OBJECT_ATTRIBUTES to a parent HKEY plus a wide sub-path. */
static NTSTATUS nt_reg_resolve(const OBJECT_ATTRIBUTES *oa, HANDLE *root, WCHAR **sub_w)
{
    *sub_w = NULL;
    char *name = oa_name(oa);
    if (oa && oa->RootDirectory) {
        *root = oa->RootDirectory;                 /* relative to an open key */
        *sub_w = (name && *name) ? lsiw_u2w(name) : NULL;
        free(name);
        return STATUS_SUCCESS;
    }
    if (!name || name[0] != '\\') { free(name); return STATUS_OBJECT_PATH_SYNTAX_BAD; }
    const char *sub;
    HANDLE r = nt_reg_root(name, &sub);
    if (!r) { free(name); return STATUS_OBJECT_PATH_NOT_FOUND; }
    *root = r;
    *sub_w = *sub ? lsiw_u2w(sub) : NULL;
    free(name);
    return STATUS_SUCCESS;
}

API NTSTATUS NtCreateKey(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa, ULONG title,
                         UNICODE_STRING *klass, ULONG options, ULONG *disposition)
{
    (void)title; (void)klass;
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE root; WCHAR *sub;
    NTSTATUS st = nt_reg_resolve(oa, &root, &sub);
    if (st) return st;
    LONG (WINAPI *rc)(HANDLE, const WCHAR *, DWORD, WCHAR *, DWORD, DWORD, void *, HANDLE *, DWORD *) = adv("RegCreateKeyExW");
    HANDLE key = NULL; DWORD disp = 0;
    LONG e = rc(root, sub, 0, NULL, options, access ? access : 0xF003F /* KEY_ALL_ACCESS */, NULL, &key, &disp);
    free(sub);
    if (e) return (NTSTATUS)(0xC0070000u | (e & 0xFFFF));
    *out = key;
    if (disposition) *disposition = disp;   /* 1 CREATED_NEW_KEY, 2 OPENED_EXISTING_KEY */
    return STATUS_SUCCESS;
}

API NTSTATUS NtOpenKey(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    HANDLE root; WCHAR *sub;
    NTSTATUS st = nt_reg_resolve(oa, &root, &sub);
    if (st) return st;
    LONG (WINAPI *ro)(HANDLE, const WCHAR *, DWORD, DWORD, HANDLE *) = adv("RegOpenKeyExW");
    HANDLE key = NULL;
    LONG e = ro(root, sub, 0, access ? access : 0x20019 /* KEY_READ */, &key);
    free(sub);
    if (e == 2 /* ERROR_FILE_NOT_FOUND */) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (e) return (NTSTATUS)(0xC0070000u | (e & 0xFFFF));
    *out = key;
    return STATUS_SUCCESS;
}
API NTSTATUS NtOpenKeyEx(HANDLE *out, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa, ULONG options)
{ (void)options; return NtOpenKey(out, access, oa); }

/* KEY_VALUE_* information classes. */
API NTSTATUS NtQueryValueKey(HANDLE key, UNICODE_STRING *value_name, int cls,
                             void *info, ULONG len, ULONG *ret_len)
{
    LONG (WINAPI *rq)(HANDLE, const WCHAR *, DWORD *, DWORD *, BYTE *, DWORD *) = adv("RegQueryValueExW");
    WCHAR namebuf[512], *namep;
    if (value_name && value_name->Buffer && value_name->Length) {
        WORD n = value_name->Length / 2;
        if (n > 511) n = 511;
        memcpy(namebuf, value_name->Buffer, n * 2);
        namebuf[n] = 0;
        namep = namebuf;
    } else { namebuf[0] = 0; namep = namebuf; }

    DWORD type = 0, size = 0;
    LONG e = rq(key, namep, NULL, &type, NULL, &size);     /* query size */
    if (e == 2) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (e && e != 234 /* MORE_DATA */) return (NTSTATUS)(0xC0070000u | (e & 0xFFFF));
    BYTE *data = size ? malloc(size) : NULL;
    DWORD got = size;
    if (size) { e = rq(key, namep, NULL, &type, data, &got); if (e) { free(data); return (NTSTATUS)(0xC0070000u | (e & 0xFFFF)); } }

    int nl = namep[0] ? (int)lsiw_wcslen(namep) : 0;
    size_t need; NTSTATUS st = STATUS_SUCCESS;
    if (cls == 0) {                 /* KeyValueBasicInformation */
        need = 12 + (size_t)nl * 2;
        if (ret_len) *ret_len = (ULONG)need;
        if (!info || len < need) { st = (len && info) ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL; goto done; }
        DWORD *p = info;
        p[0] = 0; p[1] = type; p[2] = (DWORD)(nl * 2);
        memcpy((BYTE *)info + 12, namep, (size_t)nl * 2);
    } else if (cls == 1) {          /* KeyValueFullInformation */
        size_t name_bytes = (size_t)nl * 2;
        size_t data_off = ((20 + name_bytes) + 7) & ~(size_t)7;
        need = data_off + got;
        if (ret_len) *ret_len = (ULONG)need;
        if (!info || len < need) { st = (len && info) ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL; goto done; }
        DWORD *p = info;
        p[0] = 0; p[1] = type; p[2] = (DWORD)data_off; p[3] = got; p[4] = (DWORD)name_bytes;
        memcpy((BYTE *)info + 20, namep, name_bytes);
        if (got) memcpy((BYTE *)info + data_off, data, got);
    } else if (cls == 2) {          /* KeyValuePartialInformation */
        need = 12 + got;
        if (ret_len) *ret_len = (ULONG)need;
        if (!info || len < need) { st = (len && info) ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL; goto done; }
        DWORD *p = info;
        p[0] = 0; p[1] = type; p[2] = got;
        if (got) memcpy((BYTE *)info + 12, data, got);
    } else { st = STATUS_INVALID_PARAMETER; }
done:
    free(data);
    return st;
}

API NTSTATUS NtSetValueKey(HANDLE key, UNICODE_STRING *value_name, ULONG title, ULONG type,
                           const void *data, ULONG size)
{
    (void)title;
    LONG (WINAPI *rs)(HANDLE, const WCHAR *, DWORD, DWORD, const BYTE *, DWORD) = adv("RegSetValueExW");
    WCHAR namebuf[512];
    if (value_name && value_name->Buffer && value_name->Length) {
        WORD n = value_name->Length / 2;
        if (n > 511) n = 511;
        memcpy(namebuf, value_name->Buffer, n * 2);
        namebuf[n] = 0;
    } else namebuf[0] = 0;
    LONG e = rs(key, namebuf, 0, type, data, size);
    return e ? (NTSTATUS)(0xC0070000u | (e & 0xFFFF)) : STATUS_SUCCESS;
}

/* ---- RtlCheckRegistryKey / RtlQueryRegistryValues ----
 * RelativeTo selects the root: ABSOLUTE (0) uses the path as a full
 * \Registry\... name; others are relative to well-known roots. */
#define RTL_REGISTRY_ABSOLUTE   0
#define RTL_REGISTRY_SERVICES   1   /* \Registry\Machine\System\CurrentControlSet\Services */
#define RTL_REGISTRY_CONTROL    2   /* ...\CurrentControlSet\Control */
#define RTL_REGISTRY_WINDOWS_NT 3   /* ...\Microsoft\Windows NT\CurrentVersion */
#define RTL_REGISTRY_USER       6   /* \Registry\User\<current> */
#define RTL_REGISTRY_HANDLE     0x40000000
#define RTL_REGISTRY_OPTIONAL   0x80000000

#define RTL_QUERY_REGISTRY_DIRECT       0x00000020
#define RTL_QUERY_REGISTRY_REQUIRED     0x00000008
#define RTL_QUERY_REGISTRY_NOEXPAND     0x00000010
#define RTL_QUERY_REGISTRY_TYPECHECK    0x00000100

static HANDLE rtl_reg_open(ULONG rel, const WCHAR *path)
{
    if (rel & RTL_REGISTRY_HANDLE) return (HANDLE)path;    /* path is really an HKEY */
    char *p = path ? lsiw_w2u(path) : NULL;
    char full[1024];
    const char *base;
    switch (rel & 0xFF) {
    case RTL_REGISTRY_SERVICES:   base = "\\Registry\\Machine\\System\\CurrentControlSet\\Services"; break;
    case RTL_REGISTRY_CONTROL:    base = "\\Registry\\Machine\\System\\CurrentControlSet\\Control"; break;
    case RTL_REGISTRY_WINDOWS_NT: base = "\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion"; break;
    case RTL_REGISTRY_USER:       base = "\\Registry\\User\\.Default"; break;
    case RTL_REGISTRY_ABSOLUTE:   base = ""; break;
    default:                      base = "\\Registry\\Machine"; break;
    }
    if (base[0]) snprintf(full, sizeof full, "%s%s%s", base, (p && *p && p[0] != '\\') ? "\\" : "", p ? p : "");
    else snprintf(full, sizeof full, "%s", p ? p : "");
    free(p);
    const char *sub;
    HANDLE root = nt_reg_root(full, &sub);
    if (!root) return NULL;
    LONG (WINAPI *ro)(HANDLE, const WCHAR *, DWORD, DWORD, HANDLE *) = adv("RegOpenKeyExW");
    WCHAR *subw = *sub ? lsiw_u2w(sub) : NULL;
    HANDLE key = NULL;
    LONG e = ro(root, subw, 0, 0x20019, &key);
    free(subw);
    return e ? NULL : key;
}

API NTSTATUS RtlCheckRegistryKey(ULONG rel, const WCHAR *path)
{
    HANDLE key = rtl_reg_open(rel, path);
    if (!key) return (NTSTATUS)STATUS_OBJECT_NAME_NOT_FOUND;
    if (!(rel & RTL_REGISTRY_HANDLE)) { LONG (WINAPI *rclose)(HANDLE) = adv("RegCloseKey"); if (rclose) rclose(key); }
    return STATUS_SUCCESS;
}

/* RTL_QUERY_REGISTRY_TABLE row. */
typedef struct _RTL_QUERY_REGISTRY_TABLE {
    NTSTATUS (WINAPI *QueryRoutine)(WCHAR *, ULONG, void *, ULONG, void *, void *);
    ULONG  Flags;
    WCHAR *Name;
    void  *EntryContext;
    ULONG  DefaultType;
    void  *DefaultData;
    ULONG  DefaultLength;
} RTL_QUERY_REGISTRY_TABLE;

/* Implements the common RTL_QUERY_REGISTRY_DIRECT case: each table row names a
 * value whose data is copied straight into EntryContext.  QueryRoutine-style
 * rows and subkey descent are not supported and are skipped. */
API NTSTATUS RtlQueryRegistryValues(ULONG rel, const WCHAR *path,
                                    RTL_QUERY_REGISTRY_TABLE *table, void *ctx, void *env)
{
    (void)ctx; (void)env;
    HANDLE key = rtl_reg_open(rel, path);
    NTSTATUS result = STATUS_SUCCESS;
    LONG (WINAPI *rq)(HANDLE, const WCHAR *, DWORD *, DWORD *, BYTE *, DWORD *) = adv("RegQueryValueExW");
    LONG (WINAPI *rclose)(HANDLE) = adv("RegCloseKey");
    for (RTL_QUERY_REGISTRY_TABLE *row = table; row && (row->Name || row->QueryRoutine); row++) {
        if (!(row->Flags & RTL_QUERY_REGISTRY_DIRECT) || !row->Name || !row->EntryContext) {
            if (row->Flags & RTL_QUERY_REGISTRY_REQUIRED) { result = STATUS_OBJECT_NAME_NOT_FOUND; break; }
            continue;
        }
        DWORD type = 0, size = 0;
        LONG e = key ? rq(key, row->Name, NULL, &type, NULL, &size) : 2;
        if (e == 0 && size) {
            BYTE *d = malloc(size);
            if (d && rq(key, row->Name, NULL, &type, d, &size) == 0) {
                /* EntryContext points at a UNICODE_STRING for string values, or
                 * a raw buffer otherwise.  The simple DIRECT contract copies the
                 * bytes; string values go into the caller's UNICODE_STRING. */
                if (type == 1 /* REG_SZ */ || type == 2 /* REG_EXPAND_SZ */) {
                    UNICODE_STRING *us = row->EntryContext;
                    WORD cb = (WORD)(size >= 2 ? size - 2 : 0);
                    if (us->Buffer && us->MaximumLength >= cb) { memcpy(us->Buffer, d, cb); us->Length = cb; }
                    else { memcpy(us, d, size < sizeof(void *) ? size : sizeof(void *)); }
                } else {
                    memcpy(row->EntryContext, d, size);
                }
            }
            free(d);
        } else if (row->Flags & RTL_QUERY_REGISTRY_REQUIRED) {
            result = STATUS_OBJECT_NAME_NOT_FOUND;
            break;
        } else if (row->DefaultData && row->DefaultLength) {
            memcpy(row->EntryContext, row->DefaultData, row->DefaultLength);
        }
    }
    if (key && !(rel & RTL_REGISTRY_HANDLE) && rclose) rclose(key);
    return result;
}

/* ================================================================== */
/* Tokens and security descriptors                                      */
/* ================================================================== */

/* The NtXxxToken routines forward to the advapi32 token implementation; the
 * Win32 functions return Win32 errors, converted back to NTSTATUS. */
static NTSTATUS from_win32(BOOL ok)
{
    if (ok) return STATUS_SUCCESS;
    DWORD e = lsiw_get_last_error();
    switch (e) {
    case ERROR_INVALID_HANDLE:   return STATUS_INVALID_HANDLE;
    case ERROR_INVALID_PARAMETER:return STATUS_INVALID_PARAMETER;
    case ERROR_ACCESS_DENIED:    return (NTSTATUS)0xC0000022u;
    case ERROR_INSUFFICIENT_BUFFER: return STATUS_BUFFER_TOO_SMALL;
    case ERROR_NOT_ALL_ASSIGNED: return (NTSTATUS)0x00000106u; /* STATUS_NOT_ALL_ASSIGNED */
    default:                     return (NTSTATUS)(e ? (0xC0070000u | (e & 0xFFFF)) : STATUS_UNSUCCESSFUL);
    }
}

API NTSTATUS NtOpenProcessToken(HANDLE process, ACCESS_MASK access, HANDLE *out)
{
    BOOL (WINAPI *o)(HANDLE, DWORD, HANDLE *) = adv("OpenProcessToken");
    return from_win32(o(process, access, out));
}
API NTSTATUS NtOpenProcessTokenEx(HANDLE process, ACCESS_MASK access, ULONG attrs, HANDLE *out)
{ (void)attrs; return NtOpenProcessToken(process, access, out); }

API NTSTATUS NtOpenThreadToken(HANDLE thread, ACCESS_MASK access, BOOLEAN self, HANDLE *out)
{
    BOOL (WINAPI *o)(HANDLE, DWORD, BOOL, HANDLE *) = adv("OpenThreadToken");
    return from_win32(o(thread, access, self, out));
}
API NTSTATUS NtOpenThreadTokenEx(HANDLE thread, ACCESS_MASK access, BOOLEAN self, ULONG attrs, HANDLE *out)
{ (void)attrs; return NtOpenThreadToken(thread, access, self, out); }

API NTSTATUS NtQueryInformationToken(HANDLE token, int cls, void *info, ULONG len, ULONG *ret)
{
    BOOL (WINAPI *g)(HANDLE, int, void *, DWORD, DWORD *) = adv("GetTokenInformation");
    DWORD got = 0;
    BOOL ok = g(token, cls, info, len, &got);
    if (ret) *ret = got;
    return from_win32(ok);
}

API NTSTATUS NtSetInformationToken(HANDLE token, int cls, void *info, ULONG len)
{
    BOOL (WINAPI *s)(HANDLE, int, void *, DWORD) = adv("SetTokenInformation");
    return from_win32(s(token, cls, info, len));
}

API NTSTATUS NtDuplicateToken(HANDLE token, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa,
                              BOOLEAN effective_only, int type, HANDLE *out)
{
    (void)effective_only;
    BOOL (WINAPI *d)(HANDLE, DWORD, void *, int, int, HANDLE *) = adv("DuplicateTokenEx");
    int level = (oa && oa->SecurityQualityOfService) ? *(int *)oa->SecurityQualityOfService : 2 /* Impersonation */;
    return from_win32(d(token, access, NULL, level, type, out));
}

API NTSTATUS NtAdjustPrivilegesToken(HANDLE token, BOOLEAN disable_all, void *new_state,
                                     ULONG buflen, void *prev, ULONG *ret_len)
{
    BOOL (WINAPI *a)(HANDLE, BOOL, void *, DWORD, void *, DWORD *) = adv("AdjustTokenPrivileges");
    lsiw_set_last_error(0);
    BOOL ok = a(token, disable_all, new_state, buflen, prev, ret_len);
    /* AdjustTokenPrivileges reports partial success with ERROR_NOT_ALL_ASSIGNED
     * while still returning TRUE. */
    if (ok && lsiw_get_last_error() == ERROR_NOT_ALL_ASSIGNED) return (NTSTATUS)0x00000106u;
    return from_win32(ok);
}

API NTSTATUS NtPrivilegeCheck(HANDLE token, void *required, BOOLEAN *result)
{
    BOOL (WINAPI *p)(HANDLE, void *, BOOL *) = adv("PrivilegeCheck");
    BOOL r = 0;
    NTSTATUS st = from_win32(p(token, required, &r));
    if (result) *result = (BOOLEAN)r;
    return st;
}

API NTSTATUS NtAccessCheck(void *sd, HANDLE token, ACCESS_MASK desired, void *mapping,
                           void *priv_set, ULONG *priv_len, ACCESS_MASK *granted, BOOLEAN *status)
{
    BOOL (WINAPI *a)(void *, HANDLE, DWORD, void *, void *, DWORD *, DWORD *, BOOL *) = adv("AccessCheck");
    BOOL ok_status = 0; DWORD g = 0;
    BOOL ok = a(sd, token, desired, mapping, priv_set, priv_len, &g, &ok_status);
    if (granted) *granted = g;
    if (status) *status = (BOOLEAN)ok_status;
    return from_win32(ok);
}

API NTSTATUS NtQuerySecurityObject(HANDLE h, ULONG info, void *sd, ULONG len, ULONG *ret)
{
    BOOL (WINAPI *g)(HANDLE, DWORD, void *, DWORD, DWORD *) = adv("GetKernelObjectSecurity");
    DWORD need = 0;
    BOOL ok = g ? g(h, info, sd, len, &need) : FALSE;
    if (ret) *ret = need;
    if (!ok && lsiw_get_last_error() == ERROR_INSUFFICIENT_BUFFER) return STATUS_BUFFER_TOO_SMALL;
    return from_win32(ok);
}

API NTSTATUS NtSetSecurityObject(HANDLE h, ULONG info, void *sd)
{
    BOOL (WINAPI *s)(HANDLE, DWORD, void *) = adv("SetKernelObjectSecurity");
    return s ? from_win32(s(h, info, sd)) : (NTSTATUS)STATUS_NOT_SUPPORTED;
}

/* ---- Rtl SID / ACL / security-descriptor primitives (exact x64 layouts) ---- */

typedef struct { BYTE Value[6]; } SID_IDENTIFIER_AUTHORITY;
/* SID: Revision, SubAuthorityCount, 6-byte authority, then DWORD sub-authorities. */
#define SID_MAX_SUB_AUTHORITIES 15
static DWORD rtl_sid_len(const void *p) { return 8u + 4u * ((const BYTE *)p)[1]; }

typedef struct { BYTE AclRevision, Sbz1; WORD AclSize, AceCount, Sbz2; } ACL;
typedef struct { BYTE AceType, AceFlags; WORD AceSize; } ACE_HEADER;
typedef struct { BYTE Revision, Sbz1; WORD Control; void *Owner, *Group; ACL *Sacl, *Dacl; } SECURITY_DESCRIPTOR;
typedef struct { BYTE Revision, Sbz1; WORD Control; DWORD Owner, Group, Sacl, Dacl; } SECURITY_DESCRIPTOR_RELATIVE;

#define SE_OWNER_DEFAULTED 0x0001
#define SE_GROUP_DEFAULTED 0x0002
#define SE_DACL_PRESENT    0x0004
#define SE_SACL_PRESENT    0x0010
#define SE_SELF_RELATIVE   0x8000
#define ACL_REVISION       2
#define ACL_REVISION_DS    4

API WCHAR RtlUpcaseUnicodeChar(WCHAR c)
{
    if (c >= 'a' && c <= 'z') return (WCHAR)(c - 32);
    /* Latin-1 supplement lower -> upper (a reasonable subset). */
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (WCHAR)(c - 32);
    return c;
}
API WCHAR RtlDowncaseUnicodeChar(WCHAR c)
{
    if (c >= 'A' && c <= 'Z') return (WCHAR)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (WCHAR)(c + 32);
    return c;
}

API NTSTATUS RtlInitializeSid(void *sid, const SID_IDENTIFIER_AUTHORITY *auth, BYTE n)
{
    if (!sid || n > SID_MAX_SUB_AUTHORITIES) return STATUS_INVALID_PARAMETER;
    BYTE *b = sid;
    b[0] = 1;            /* SID_REVISION */
    b[1] = n;
    if (auth) memcpy(b + 2, auth->Value, 6);
    return STATUS_SUCCESS;
}
API NTSTATUS RtlInitializeSidEx(void *sid, const SID_IDENTIFIER_AUTHORITY *auth, BYTE n)
{ return RtlInitializeSid(sid, auth, n); }

API ULONG RtlLengthSid(void *sid) { return sid ? rtl_sid_len(sid) : 0; }

API NTSTATUS RtlCopySid(ULONG dest_len, void *dst, void *src)
{
    if (!src) return STATUS_INVALID_PARAMETER;
    DWORD need = rtl_sid_len(src);
    if (dest_len < need) return STATUS_BUFFER_TOO_SMALL;
    memcpy(dst, src, need);
    return STATUS_SUCCESS;
}

API BOOLEAN RtlEqualSid(void *a, void *b)
{
    if (!a || !b) return FALSE;
    if (rtl_sid_len(a) != rtl_sid_len(b)) return FALSE;
    return memcmp(a, b, rtl_sid_len(a)) == 0;
}
API BOOLEAN RtlEqualPrefixSid(void *a, void *b)
{
    BYTE *x = a, *y = b;
    if (!x || !y || x[1] != y[1]) return FALSE;
    if (!x[1]) return memcmp(x, y, 8) == 0;
    return memcmp(x, y, 8u + 4u * (x[1] - 1u)) == 0;
}
API SID_IDENTIFIER_AUTHORITY *RtlIdentifierAuthoritySid(void *sid) { return (SID_IDENTIFIER_AUTHORITY *)((BYTE *)sid + 2); }
API DWORD *RtlSubAuthoritySid(void *sid, ULONG n) { return (DWORD *)((BYTE *)sid + 8 + 4 * (size_t)n); }
API BYTE *RtlSubAuthorityCountSid(void *sid) { return (BYTE *)sid + 1; }
API BOOLEAN RtlValidSid(void *sid) { BYTE *b = sid; return b && b[0] == 1 && b[1] <= SID_MAX_SUB_AUTHORITIES; }

/* "S-1-<authority>-<sub>-..." into a caller-allocated or Rtl-allocated string. */
API NTSTATUS RtlConvertSidToUnicodeString(UNICODE_STRING *out, void *sid, BOOLEAN alloc)
{
    if (!out || !RtlValidSid(sid)) return STATUS_INVALID_PARAMETER;
    BYTE *b = sid;
    uint64_t auth = 0;
    for (int i = 0; i < 6; i++) auth = (auth << 8) | b[2 + i];
    char tmp[256];
    int len = snprintf(tmp, sizeof tmp, "S-1-%llu", (unsigned long long)auth);
    for (int i = 0; i < b[1]; i++) {
        DWORD s; memcpy(&s, b + 8 + 4 * i, 4);
        len += snprintf(tmp + len, sizeof tmp - len, "-%u", s);
    }
    WORD need = (WORD)(len * 2);
    if (alloc) {
        out->Buffer = malloc(need + 2);
        if (!out->Buffer) return STATUS_NO_MEMORY;
        out->MaximumLength = (WORD)(need + 2);
    } else if (out->MaximumLength < need + 2) {
        out->Length = need;
        return STATUS_BUFFER_TOO_SMALL;
    }
    int n = lsiw_utf8_to_utf16(tmp, len, out->Buffer, out->MaximumLength / 2);
    out->Buffer[n] = 0;
    out->Length = (WORD)(n * 2);
    return STATUS_SUCCESS;
}

API NTSTATUS RtlCreateSecurityDescriptor(void *sd, ULONG rev)
{
    if (!sd || rev != 1) return rev != 1 ? (NTSTATUS)0xC0000059u /* UNKNOWN_REVISION */ : (NTSTATUS)STATUS_INVALID_PARAMETER;
    SECURITY_DESCRIPTOR *s = sd;
    memset(s, 0, sizeof *s);
    s->Revision = 1;
    return STATUS_SUCCESS;
}

API NTSTATUS RtlGetControlSecurityDescriptor(void *sd, WORD *control, ULONG *rev)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (!sd || !control || !rev) return STATUS_INVALID_PARAMETER;
    *control = s->Control;
    *rev = s->Revision;
    return STATUS_SUCCESS;
}
API NTSTATUS RtlSetControlSecurityDescriptor(void *sd, WORD mask, WORD bits)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (!sd) return STATUS_INVALID_PARAMETER;
    s->Control = (WORD)((s->Control & ~mask) | (bits & mask));
    return STATUS_SUCCESS;
}

API NTSTATUS RtlGetOwnerSecurityDescriptor(void *sd, void **owner, BOOLEAN *defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (!sd || !owner) return STATUS_INVALID_PARAMETER;
    *owner = s->Owner;
    if (defaulted) *defaulted = (s->Control & SE_OWNER_DEFAULTED) != 0;
    return STATUS_SUCCESS;
}
API NTSTATUS RtlSetOwnerSecurityDescriptor(void *sd, void *owner, BOOLEAN defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (!sd) return STATUS_INVALID_PARAMETER;
    s->Owner = owner;
    s->Control = (WORD)((s->Control & ~SE_OWNER_DEFAULTED) | (defaulted ? SE_OWNER_DEFAULTED : 0));
    return STATUS_SUCCESS;
}
API NTSTATUS RtlGetGroupSecurityDescriptor(void *sd, void **group, BOOLEAN *defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (!sd || !group) return STATUS_INVALID_PARAMETER;
    *group = s->Group;
    if (defaulted) *defaulted = (s->Control & SE_GROUP_DEFAULTED) != 0;
    return STATUS_SUCCESS;
}
API NTSTATUS RtlSetGroupSecurityDescriptor(void *sd, void *group, BOOLEAN defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (!sd) return STATUS_INVALID_PARAMETER;
    s->Group = group;
    s->Control = (WORD)((s->Control & ~SE_GROUP_DEFAULTED) | (defaulted ? SE_GROUP_DEFAULTED : 0));
    return STATUS_SUCCESS;
}
API NTSTATUS RtlGetDaclSecurityDescriptor(void *sd, BOOLEAN *present, ACL **dacl, BOOLEAN *defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (!sd || !present || !dacl) return STATUS_INVALID_PARAMETER;
    *present = (s->Control & SE_DACL_PRESENT) != 0;
    *dacl = *present ? s->Dacl : NULL;
    if (defaulted) *defaulted = (s->Control & 0x0008) != 0;
    return STATUS_SUCCESS;
}
API NTSTATUS RtlSetDaclSecurityDescriptor(void *sd, BOOLEAN present, ACL *dacl, BOOLEAN defaulted)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (!sd) return STATUS_INVALID_PARAMETER;
    if (present) { s->Control |= SE_DACL_PRESENT; s->Dacl = dacl; }
    else { s->Control &= ~SE_DACL_PRESENT; s->Dacl = NULL; }
    s->Control = (WORD)((s->Control & ~0x0008) | (defaulted ? 0x0008 : 0));
    return STATUS_SUCCESS;
}

/* Build the self-relative form: header followed by owner, group and DACL. */
API NTSTATUS RtlAbsoluteToSelfRelativeSD(void *abs, void *rel, ULONG *len)
{
    SECURITY_DESCRIPTOR *s = abs;
    if (!abs || !len) return STATUS_INVALID_PARAMETER;
    if (s->Control & SE_SELF_RELATIVE) return (NTSTATUS)0xC0000059u;
    DWORD owner_len = s->Owner ? rtl_sid_len(s->Owner) : 0;
    DWORD group_len = s->Group ? rtl_sid_len(s->Group) : 0;
    DWORD dacl_len = (s->Control & SE_DACL_PRESENT) && s->Dacl ? s->Dacl->AclSize : 0;
    DWORD sacl_len = (s->Control & SE_SACL_PRESENT) && s->Sacl ? s->Sacl->AclSize : 0;
    DWORD total = (DWORD)sizeof(SECURITY_DESCRIPTOR_RELATIVE) + owner_len + group_len + dacl_len + sacl_len;
    if (!rel || *len < total) { *len = total; return STATUS_BUFFER_TOO_SMALL; }
    SECURITY_DESCRIPTOR_RELATIVE *d = rel;
    memset(d, 0, sizeof *d);
    d->Revision = s->Revision;
    d->Control = (WORD)(s->Control | SE_SELF_RELATIVE);
    DWORD off = (DWORD)sizeof *d;
    BYTE *base = rel;
    if (sacl_len) { memcpy(base + off, s->Sacl, sacl_len); d->Sacl = off; off += sacl_len; }
    if (dacl_len) { memcpy(base + off, s->Dacl, dacl_len); d->Dacl = off; off += dacl_len; }
    if (owner_len) { memcpy(base + off, s->Owner, owner_len); d->Owner = off; off += owner_len; }
    if (group_len) { memcpy(base + off, s->Group, group_len); d->Group = off; off += group_len; }
    *len = total;
    return STATUS_SUCCESS;
}

API NTSTATUS RtlCreateAcl(ACL *acl, ULONG len, ULONG rev)
{
    if (!acl) return STATUS_INVALID_PARAMETER;
    if (len < sizeof(ACL)) return STATUS_BUFFER_TOO_SMALL;
    if (len > 0xFFFF || rev < ACL_REVISION || rev > ACL_REVISION_DS) return STATUS_INVALID_PARAMETER;
    memset(acl, 0, sizeof *acl);
    acl->AclRevision = (BYTE)rev;
    acl->AclSize = (WORD)(len & ~3u);
    return STATUS_SUCCESS;
}

static size_t rtl_acl_used(const ACL *a)
{
    size_t off = sizeof(ACL);
    for (WORD i = 0; i < a->AceCount; i++) off += ((const ACE_HEADER *)((const BYTE *)a + off))->AceSize;
    return off;
}
static NTSTATUS rtl_add_ace(ACL *a, ULONG rev, BYTE type, BYTE flags, ACCESS_MASK mask, void *sid)
{
    if (!a || !sid) return STATUS_INVALID_PARAMETER;
    if (rev < ACL_REVISION || rev > ACL_REVISION_DS) return (NTSTATUS)0xC0000059u;
    size_t used = rtl_acl_used(a), sz = 8 + rtl_sid_len(sid);
    if (used + sz > a->AclSize) return (NTSTATUS)0xC0000023u; /* BUFFER_TOO_SMALL */
    BYTE *p = (BYTE *)a + used;
    ACE_HEADER h = { type, flags, (WORD)sz };
    memcpy(p, &h, 4);
    memcpy(p + 4, &mask, 4);
    memcpy(p + 8, sid, rtl_sid_len(sid));
    a->AceCount++;
    if (rev > a->AclRevision) a->AclRevision = (BYTE)rev;
    return STATUS_SUCCESS;
}
API NTSTATUS RtlAddAccessAllowedAce(ACL *a, ULONG rev, ACCESS_MASK mask, void *sid)
{ return rtl_add_ace(a, rev, 0 /* ACCESS_ALLOWED */, 0, mask, sid); }
API NTSTATUS RtlAddAccessAllowedAceEx(ACL *a, ULONG rev, ULONG flags, ACCESS_MASK mask, void *sid)
{ return rtl_add_ace(a, rev, 0, (BYTE)flags, mask, sid); }
API NTSTATUS RtlAddAccessDeniedAce(ACL *a, ULONG rev, ACCESS_MASK mask, void *sid)
{ return rtl_add_ace(a, rev, 1 /* ACCESS_DENIED */, 0, mask, sid); }
API NTSTATUS RtlAddAccessDeniedAceEx(ACL *a, ULONG rev, ULONG flags, ACCESS_MASK mask, void *sid)
{ return rtl_add_ace(a, rev, 1, (BYTE)flags, mask, sid); }

API NTSTATUS RtlGetAce(ACL *a, ULONG index, void **ace)
{
    if (!a || !ace || index >= a->AceCount) return STATUS_INVALID_PARAMETER;
    size_t off = sizeof(ACL);
    for (ULONG i = 0; i < index; i++) off += ((const ACE_HEADER *)((const BYTE *)a + off))->AceSize;
    *ace = (BYTE *)a + off;
    return STATUS_SUCCESS;
}
/* Pointer to the first unused byte, or NULL if the ACL is full. */
API void *RtlFirstFreeAce(ACL *a)
{
    if (!a) return NULL;
    size_t used = rtl_acl_used(a);
    return used <= a->AclSize ? (BYTE *)a + used : NULL;
}

/* ================================================================== */
/* Unicode string helpers                                               */
/* ================================================================== */

API NTSTATUS RtlUpcaseUnicodeString(UNICODE_STRING *dst, const UNICODE_STRING *src, BOOLEAN alloc)
{
    if (!src || !dst) return STATUS_INVALID_PARAMETER;
    WORD n = src->Length;
    if (alloc) {
        dst->Buffer = malloc(n + 2);
        if (!dst->Buffer) return STATUS_NO_MEMORY;
        dst->MaximumLength = (WORD)(n + 2);
    } else if (dst->MaximumLength < n) return STATUS_BUFFER_OVERFLOW;
    for (WORD i = 0; i < n / 2; i++) dst->Buffer[i] = RtlUpcaseUnicodeChar(src->Buffer[i]);
    dst->Length = n;
    if (n + 2 <= dst->MaximumLength) dst->Buffer[n / 2] = 0;
    return STATUS_SUCCESS;
}
API NTSTATUS RtlDowncaseUnicodeString(UNICODE_STRING *dst, const UNICODE_STRING *src, BOOLEAN alloc)
{
    if (!src || !dst) return STATUS_INVALID_PARAMETER;
    WORD n = src->Length;
    if (alloc) {
        dst->Buffer = malloc(n + 2);
        if (!dst->Buffer) return STATUS_NO_MEMORY;
        dst->MaximumLength = (WORD)(n + 2);
    } else if (dst->MaximumLength < n) return STATUS_BUFFER_OVERFLOW;
    for (WORD i = 0; i < n / 2; i++) dst->Buffer[i] = RtlDowncaseUnicodeChar(src->Buffer[i]);
    dst->Length = n;
    if (n + 2 <= dst->MaximumLength) dst->Buffer[n / 2] = 0;
    return STATUS_SUCCESS;
}

API NTSTATUS RtlAppendUnicodeToString(UNICODE_STRING *dst, const WCHAR *src)
{
    if (!dst) return STATUS_INVALID_PARAMETER;
    if (!src) return STATUS_SUCCESS;
    WORD add = (WORD)(lsiw_wcslen(src) * 2);
    if (dst->Length + add > dst->MaximumLength) return STATUS_BUFFER_TOO_SMALL;
    memcpy((BYTE *)dst->Buffer + dst->Length, src, add);
    dst->Length = (WORD)(dst->Length + add);
    if (dst->Length + 2 <= dst->MaximumLength) dst->Buffer[dst->Length / 2] = 0;
    return STATUS_SUCCESS;
}
API NTSTATUS RtlAppendUnicodeStringToString(UNICODE_STRING *dst, const UNICODE_STRING *src)
{
    if (!dst || !src) return STATUS_INVALID_PARAMETER;
    if (!src->Length) return STATUS_SUCCESS;
    if (dst->Length + src->Length > dst->MaximumLength) return STATUS_BUFFER_TOO_SMALL;
    memcpy((BYTE *)dst->Buffer + dst->Length, src->Buffer, src->Length);
    dst->Length = (WORD)(dst->Length + src->Length);
    if (dst->Length + 2 <= dst->MaximumLength) dst->Buffer[dst->Length / 2] = 0;
    return STATUS_SUCCESS;
}

/* TRUE if `prefix` is a leading substring of `str`. */
API BOOLEAN RtlPrefixUnicodeString(const UNICODE_STRING *prefix, const UNICODE_STRING *str, BOOLEAN ci)
{
    if (!prefix || !str) return FALSE;
    if (prefix->Length > str->Length) return FALSE;
    for (WORD i = 0; i < prefix->Length / 2; i++) {
        WCHAR a = prefix->Buffer[i], b = str->Buffer[i];
        if (ci) { a = RtlUpcaseUnicodeChar(a); b = RtlUpcaseUnicodeChar(b); }
        if (a != b) return FALSE;
    }
    return TRUE;
}

API NTSTATUS RtlSetCurrentDirectory_U(const UNICODE_STRING *path)
{
    if (!path || !path->Buffer) return STATUS_INVALID_PARAMETER;
    BOOL (WINAPI *scd)(const WCHAR *) = k32("SetCurrentDirectoryW");
    WCHAR *p = malloc(path->Length + 2);
    if (!p) return STATUS_NO_MEMORY;
    memcpy(p, path->Buffer, path->Length);
    p[path->Length / 2] = 0;
    BOOL ok = scd(p);
    free(p);
    if (ok) return STATUS_SUCCESS;
    return lsiw_get_last_error() == ERROR_PATH_NOT_FOUND ? (NTSTATUS)STATUS_OBJECT_NAME_NOT_FOUND
                                                         : (NTSTATUS)STATUS_UNSUCCESSFUL;
}

/* ================================================================== */
/* Transactions (KTM) - not provided                                    */
/* ================================================================== */

/* LSIW has no transaction manager, so there is never a current transaction. */
API HANDLE RtlGetCurrentTransaction(void) { return NULL; }
API BOOLEAN RtlSetCurrentTransaction(HANDLE t) { return t == NULL; }

/* Creating or completing a transaction is reported as unimplemented so that
 * callers fall back to non-transacted operations (the common pattern). */
API NTSTATUS NtCreateTransaction(HANDLE *out, ACCESS_MASK a, OBJECT_ATTRIBUTES *oa, void *uow,
                                 void *tm, ULONG opt, ULONG iso, ULONG isoflags, LARGE_INTEGER *timeout,
                                 UNICODE_STRING *desc)
{ (void)a; (void)oa; (void)uow; (void)tm; (void)opt; (void)iso; (void)isoflags; (void)timeout; (void)desc;
  if (out) *out = NULL;
  return (NTSTATUS)STATUS_NOT_IMPLEMENTED; }
API NTSTATUS NtCommitTransaction(HANDLE t, BOOLEAN wait) { (void)t; (void)wait; return (NTSTATUS)STATUS_NOT_IMPLEMENTED; }
API NTSTATUS NtRollbackTransaction(HANDLE t, BOOLEAN wait) { (void)t; (void)wait; return (NTSTATUS)STATUS_NOT_IMPLEMENTED; }

/* ================================================================== */
/* RtlCreateQueryDebugBuffer / RtlQueryProcessDebugInformation          */
/* ================================================================== */

typedef struct {
    HANDLE SectionHandleClient;
    void *ViewBaseClient, *ViewBaseTarget;
    ULONG_PTR ViewBaseDelta;
    HANDLE EventPairClient, EventPairTarget, TargetProcessId, TargetThreadHandle;
    ULONG Flags;
    SIZE_T OffsetFree, CommitSize, ViewSize;
    void *Modules, *BackTraces, *Heaps, *Locks, *SpecificHeap;
    HANDLE TargetProcessHandle;
    void *Reserved[6];
} RTL_DEBUG_INFORMATION;

typedef struct {
    void *Section, *MappedBase, *ImageBase;
    ULONG ImageSize, Flags;
    WORD LoadOrderIndex, InitOrderIndex, LoadCount, OffsetToFileName;
    char FullPathName[256];
} RTL_PROCESS_MODULE_INFORMATION;
typedef struct { ULONG NumberOfModules; RTL_PROCESS_MODULE_INFORMATION Modules[1]; } RTL_PROCESS_MODULES;


typedef struct {
    void  *Base;
    ULONG  Flags, Signature, HeapId;
    SIZE_T NumberOfEntries, NumberOfPseudoTags, CounterOffset, BytesCommitted;
    SIZE_T BytesAllocated;
    SIZE_T NumberOfTags, NumberOfTagEntries, NumberOfPseudoTagEntries;
    SIZE_T MaximumTagIndex;
    void  *TagEntries, *Entries;
} RTL_HEAP_INFORMATION;
typedef struct { ULONG NumberOfHeaps; RTL_HEAP_INFORMATION Heaps[1]; } RTL_PROCESS_HEAPS;

#define PDI_MODULES 0x01
#define PDI_HEAPS   0x04

/* A simple bump-allocated buffer, with the RTL_DEBUG_INFORMATION header at the
 * start and OffsetFree tracking the next free byte. */
API void *RtlCreateQueryDebugBuffer(ULONG size, BOOLEAN event_pair)
{
    (void)event_pair;
    size_t total = size ? size : 0x10000;
    if (total < sizeof(RTL_DEBUG_INFORMATION) + 0x4000) total = sizeof(RTL_DEBUG_INFORMATION) + 0x4000;
    RTL_DEBUG_INFORMATION *b = calloc(1, total);
    if (!b) return NULL;
    b->ViewBaseClient = b;
    b->OffsetFree = sizeof *b;
    b->CommitSize = total;
    b->ViewSize = total;
    return b;
}

static void *debug_alloc(RTL_DEBUG_INFORMATION *b, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (b->OffsetFree + n > b->ViewSize) return NULL;
    void *p = (BYTE *)b + b->OffsetFree;
    b->OffsetFree += n;
    return p;
}

API NTSTATUS RtlQueryProcessDebugInformation(HANDLE pid, ULONG mask, void *buffer)
{
    RTL_DEBUG_INFORMATION *b = buffer;
    if (!b) return STATUS_INVALID_PARAMETER;
    b->TargetProcessId = pid;

    if (mask & PDI_MODULES) {
        ULONG count = 0;
        for (struct lsiw_module *m = lsiw_modules; m; m = m->next) count++;
        RTL_PROCESS_MODULES *pm = debug_alloc(b, sizeof(ULONG) + (size_t)(count ? count : 1) * sizeof(RTL_PROCESS_MODULE_INFORMATION));
        if (!pm) return STATUS_NO_MEMORY;
        pm->NumberOfModules = count;
        ULONG i = 0;
        for (struct lsiw_module *m = lsiw_modules; m; m = m->next, i++) {
            RTL_PROCESS_MODULE_INFORMATION *mi = &pm->Modules[i];
            memset(mi, 0, sizeof *mi);
            mi->ImageBase = m->base;
            mi->MappedBase = m->base;
            mi->ImageSize = (ULONG)m->size;
            mi->LoadOrderIndex = (WORD)i;
            mi->LoadCount = 1;
            const char *path = m->win_path ? m->win_path : m->name;
            size_t pl = strlen(path);
            if (pl > 255) { path += pl - 255; pl = 255; }
            memcpy(mi->FullPathName, path, pl);
            const char *base = mi->FullPathName;
            for (const char *c = mi->FullPathName; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
            mi->OffsetToFileName = (WORD)(base - mi->FullPathName);
        }
        b->Modules = pm;
    }
    if (mask & PDI_HEAPS) {
        RTL_PROCESS_HEAPS *ph = debug_alloc(b, sizeof(ULONG) + sizeof(RTL_HEAP_INFORMATION));
        if (!ph) return STATUS_NO_MEMORY;
        ph->NumberOfHeaps = 1;
        memset(&ph->Heaps[0], 0, sizeof ph->Heaps[0]);
        ph->Heaps[0].Base = lsiw_process_heap();
        ph->Heaps[0].Flags = 2;   /* HEAP_GROWABLE */
        b->Heaps = ph;
    }
    return STATUS_SUCCESS;
}

API NTSTATUS RtlDestroyQueryDebugBuffer(void *buffer) { free(buffer); return STATUS_SUCCESS; }

/* ================================================================== */
/* kernel32 / advapi32 / iphlpapi additions (lsiw_k32more_exports)      */
/* ================================================================== */

/* ---- resource enumeration (walks the loaded PE .rsrc tree) ---- */

static struct lsiw_module *mod_of(HMODULE h)
{
    struct lsiw_module *m = h ? NULL : lsiw_main_module;
    for (struct lsiw_module *x = lsiw_modules; x && !m; x = x->next) if (x->base == h) m = x;
    return m;
}

static uint8_t *res_root(HMODULE h, struct lsiw_module **mod)
{
    struct lsiw_module *m = mod_of(h);
    if (!m || m->is_builtin) { lsiw_set_last_error(1813 /* RESOURCE_TYPE_NOT_FOUND */); return NULL; }
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(m->base + ((IMAGE_DOS_HEADER *)m->base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    if (!dd.VirtualAddress) { lsiw_set_last_error(1812 /* RESOURCE_DATA_NOT_FOUND */); return NULL; }
    if (mod) *mod = m;
    return m->base + dd.VirtualAddress;
}

/* Turn a directory entry's Name field into either an integer id (returned) or
 * a malloc'd wide string (*name_w, NULL for an id). */
static WCHAR *res_entry_id(uint8_t *root, const IMAGE_RESOURCE_DIRECTORY_ENTRY *e, uintptr_t *id)
{
    if (e->Name & 0x80000000) {
        uint16_t *s = (uint16_t *)(root + (e->Name & 0x7fffffff));
        WORD n = s[0];
        WCHAR *w = malloc((n + 1) * 2);
        if (w) { memcpy(w, s + 1, n * 2); w[n] = 0; }
        *id = 0;
        return w;
    }
    *id = e->Name & 0xFFFF;
    return NULL;
}

/* A resource identifier for a callback: either a MAKEINTRESOURCE integer or a
 * pointer to a wide (or, for ANSI callbacks, narrow) string. */
typedef BOOL (WINAPI *ENUMTYPE_W)(HMODULE, WCHAR *, LONG_PTR);
typedef BOOL (WINAPI *ENUMNAME_W)(HMODULE, const WCHAR *, WCHAR *, LONG_PTR);
typedef BOOL (WINAPI *ENUMLANG_W)(HMODULE, const WCHAR *, const WCHAR *, WORD, LONG_PTR);
typedef BOOL (WINAPI *ENUMTYPE_A)(HMODULE, char *, LONG_PTR);
typedef BOOL (WINAPI *ENUMNAME_A)(HMODULE, const char *, char *, LONG_PTR);
typedef BOOL (WINAPI *ENUMLANG_A)(HMODULE, const char *, const char *, WORD, LONG_PTR);


static const IMAGE_RESOURCE_DIRECTORY_ENTRY *res_entries(uint8_t *root, uint32_t dir_rva, int *count)
{
    IMAGE_RESOURCE_DIRECTORY *d = (IMAGE_RESOURCE_DIRECTORY *)(root + dir_rva);
    *count = d->NumberOfNamedEntries + d->NumberOfIdEntries;
    return (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(d + 1);
}

/* case-insensitive wide compare. */
static int res_wcscmp_ci(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR x = RtlUpcaseUnicodeChar(*a), y = RtlUpcaseUnicodeChar(*b);
        if (x != y) return (int)x - (int)y;
        if (!x) return 0;
    }
}

/* Find the type-level entry matching `type` (integer or wide string). */
static const IMAGE_RESOURCE_DIRECTORY_ENTRY *res_find(uint8_t *root, uint32_t dir_rva, const WCHAR *want, uintptr_t want_id)
{
    int n;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *e = res_entries(root, dir_rva, &n);
    for (int i = 0; i < n; i++) {
        uintptr_t id;
        WCHAR *nm = res_entry_id(root, &e[i], &id);
        int match;
        if (want) match = nm && !res_wcscmp_ci(nm, want);
        else match = !nm && id == want_id;
        free(nm);
        if (match) return &e[i];
    }
    return NULL;
}

/* Interpret a type/name argument: MAKEINTRESOURCE integer or wide string. */
static const WCHAR *res_arg(const WCHAR *p, uintptr_t *id)
{
    if ((uintptr_t)p < 0x10000) { *id = (uintptr_t)p; return NULL; }
    *id = 0;
    return p;
}

#define ERROR_RESOURCE_ENUM_USER_STOP 15106

static BOOL enum_types_w(HMODULE h, ENUMTYPE_W cb, LONG_PTR param)
{
    struct lsiw_module *m;
    uint8_t *root = res_root(h, &m);
    if (!root) return FALSE;
    int n;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *e = res_entries(root, 0, &n);
    for (int i = 0; i < n; i++) {
        uintptr_t id;
        WCHAR *nm = res_entry_id(root, &e[i], &id);
        WCHAR *arg = nm ? nm : (WCHAR *)id;
        BOOL go = cb(h, arg, param);
        free(nm);
        if (!go) { lsiw_set_last_error(ERROR_RESOURCE_ENUM_USER_STOP); return FALSE; }
    }
    return TRUE;
}

static BOOL enum_names_w(HMODULE h, const WCHAR *type, ENUMNAME_W cb, LONG_PTR param)
{
    struct lsiw_module *m;
    uint8_t *root = res_root(h, &m);
    if (!root) return FALSE;
    uintptr_t tid;
    const WCHAR *tw = res_arg(type, &tid);
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *te = res_find(root, 0, tw, tid);
    if (!te || !(te->OffsetToData & 0x80000000)) { lsiw_set_last_error(1813); return FALSE; }
    int n;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *e = res_entries(root, te->OffsetToData & 0x7fffffff, &n);
    for (int i = 0; i < n; i++) {
        uintptr_t id;
        WCHAR *nm = res_entry_id(root, &e[i], &id);
        WCHAR *arg = nm ? nm : (WCHAR *)id;
        BOOL go = cb(h, type, arg, param);
        free(nm);
        if (!go) { lsiw_set_last_error(ERROR_RESOURCE_ENUM_USER_STOP); return FALSE; }
    }
    return TRUE;
}

static BOOL enum_langs_w(HMODULE h, const WCHAR *type, const WCHAR *name, ENUMLANG_W cb, LONG_PTR param)
{
    struct lsiw_module *m;
    uint8_t *root = res_root(h, &m);
    if (!root) return FALSE;
    uintptr_t tid, nid;
    const WCHAR *tw = res_arg(type, &tid), *nw = res_arg(name, &nid);
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *te = res_find(root, 0, tw, tid);
    if (!te || !(te->OffsetToData & 0x80000000)) { lsiw_set_last_error(1813); return FALSE; }
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *ne = res_find(root, te->OffsetToData & 0x7fffffff, nw, nid);
    if (!ne || !(ne->OffsetToData & 0x80000000)) { lsiw_set_last_error(1814); return FALSE; }
    int n;
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *e = res_entries(root, ne->OffsetToData & 0x7fffffff, &n);
    for (int i = 0; i < n; i++) {
        WORD lang = (WORD)(e[i].Name & 0xFFFF);
        if (!cb(h, type, name, lang, param)) { lsiw_set_last_error(ERROR_RESOURCE_ENUM_USER_STOP); return FALSE; }
    }
    return TRUE;
}

API BOOL EnumResourceTypesW(HMODULE h, ENUMTYPE_W cb, LONG_PTR param) { return enum_types_w(h, cb, param); }
API BOOL EnumResourceNamesW(HMODULE h, const WCHAR *type, ENUMNAME_W cb, LONG_PTR param) { return enum_names_w(h, type, cb, param); }
API BOOL EnumResourceLanguagesW(HMODULE h, const WCHAR *type, const WCHAR *name, ENUMLANG_W cb, LONG_PTR param)
{ return enum_langs_w(h, type, name, cb, param); }
API BOOL EnumResourceTypesExW(HMODULE h, ENUMTYPE_W cb, LONG_PTR param, DWORD flags, DWORD lang)
{ (void)flags; (void)lang; return enum_types_w(h, cb, param); }
API BOOL EnumResourceNamesExW(HMODULE h, const WCHAR *type, ENUMNAME_W cb, LONG_PTR param, DWORD flags, DWORD lang)
{ (void)flags; (void)lang; return enum_names_w(h, type, cb, param); }
API BOOL EnumResourceLanguagesExW(HMODULE h, const WCHAR *type, const WCHAR *name, ENUMLANG_W cb, LONG_PTR param, DWORD flags, DWORD lang)
{ (void)flags; (void)lang; return enum_langs_w(h, type, name, cb, param); }

/* ANSI variants: the callback receives an integer id as a pointer, or a
 * freshly converted ANSI string.  The A caller passes ANSI type/name. */
struct a_ctx { void *cb; LONG_PTR param; };
static char *res_a_from_w(const WCHAR *p)
{
    if ((uintptr_t)p < 0x10000) return (char *)p;   /* MAKEINTRESOURCE id */
    return lsiw_w2u(p);
}
static WCHAR *res_w_from_a(const char *p)
{
    if ((uintptr_t)p < 0x10000) return (WCHAR *)p;
    return lsiw_u2w(p);
}
static void res_a_free(char *p) { if ((uintptr_t)p >= 0x10000) free(p); }
static void res_w_free(WCHAR *p) { if ((uintptr_t)p >= 0x10000) free(p); }

/* The A wrappers pass their context through the enum param, so no global
 * state is needed and concurrent enumeration is safe. */
static BOOL WINAPI type_w2a(HMODULE h, WCHAR *type, LONG_PTR p)
{
    struct a_ctx *c = (struct a_ctx *)(ULONG_PTR)p;
    char *t = res_a_from_w(type);
    BOOL r = ((ENUMTYPE_A)c->cb)(h, t, c->param);
    res_a_free(t);
    return r;
}
static BOOL WINAPI name_w2a(HMODULE h, const WCHAR *type, WCHAR *name, LONG_PTR p)
{
    struct a_ctx *c = (struct a_ctx *)(ULONG_PTR)p;
    char *t = res_a_from_w(type), *nm = res_a_from_w(name);
    BOOL r = ((ENUMNAME_A)c->cb)(h, t, nm, c->param);
    res_a_free(t); res_a_free(nm);
    return r;
}
static BOOL WINAPI lang_w2a(HMODULE h, const WCHAR *type, const WCHAR *name, WORD lang, LONG_PTR p)
{
    struct a_ctx *c = (struct a_ctx *)(ULONG_PTR)p;
    char *t = res_a_from_w(type), *nm = res_a_from_w(name);
    BOOL r = ((ENUMLANG_A)c->cb)(h, t, nm, lang, c->param);
    res_a_free(t); res_a_free(nm);
    return r;
}

API BOOL EnumResourceTypesA(HMODULE h, ENUMTYPE_A cb, LONG_PTR param)
{
    struct a_ctx c = { (void *)cb, param };
    return enum_types_w(h, type_w2a, (LONG_PTR)(ULONG_PTR)&c);
}
API BOOL EnumResourceNamesA(HMODULE h, const char *type, ENUMNAME_A cb, LONG_PTR param)
{
    struct a_ctx c = { (void *)cb, param };
    WCHAR *tw = res_w_from_a(type);
    BOOL r = enum_names_w(h, tw, name_w2a, (LONG_PTR)(ULONG_PTR)&c);
    res_w_free(tw);
    return r;
}
API BOOL EnumResourceLanguagesA(HMODULE h, const char *type, const char *name, ENUMLANG_A cb, LONG_PTR param)
{
    struct a_ctx c = { (void *)cb, param };
    WCHAR *tw = res_w_from_a(type), *nw = res_w_from_a(name);
    BOOL r = enum_langs_w(h, tw, nw, lang_w2a, (LONG_PTR)(ULONG_PTR)&c);
    res_w_free(tw); res_w_free(nw);
    return r;
}
API BOOL EnumResourceTypesExA(HMODULE h, ENUMTYPE_A cb, LONG_PTR param, DWORD flags, DWORD lang)
{ (void)flags; (void)lang; return EnumResourceTypesA(h, cb, param); }
API BOOL EnumResourceNamesExA(HMODULE h, const char *type, ENUMNAME_A cb, LONG_PTR param, DWORD flags, DWORD lang)
{ (void)flags; (void)lang; return EnumResourceNamesA(h, type, cb, param); }
API BOOL EnumResourceLanguagesExA(HMODULE h, const char *type, const char *name, ENUMLANG_A cb, LONG_PTR param, DWORD flags, DWORD lang)
{ (void)flags; (void)lang; return EnumResourceLanguagesA(h, type, name, cb, param); }

/* ---- CreateDirectoryExA (the W form lives in k32_file.c) ---- */
API BOOL CreateDirectoryExA(const char *template_dir, const char *path, void *sa)
{
    (void)template_dir;   /* no template attributes are copied */
    BOOL (WINAPI *cd)(const char *, void *) = k32("CreateDirectoryA");
    return cd(path, sa);
}

/* ---- console output reads ----
 * LSIW keeps no off-screen buffer, so there is nothing real to read back.
 * These return blanks painted with the console's current attribute, which is
 * what a just-cleared screen would contain, and clamp the region to the
 * window size reported by k32_file's console code. */
#pragma pack(push, 2)
typedef struct { short X, Y; } COORD;
typedef struct { short Left, Top, Right, Bottom; } SMALL_RECT;
typedef struct { union { WCHAR UnicodeChar; char AsciiChar; } Char; WORD Attributes; } CHAR_INFO;
struct cons_csbi { COORD size; COORD cursor; WORD attr; SMALL_RECT window; COORD max; };
#pragma pack(pop)

static WORD console_attr(HANDLE h, int *ok)
{
    BOOL (WINAPI *g)(HANDLE, struct cons_csbi *) = k32("GetConsoleScreenBufferInfo");
    struct cons_csbi info;
    if (g && g(h, &info)) { if (ok) *ok = 1; return info.attr; }
    if (ok) *ok = 0;
    return 0x07;
}

static BOOL read_output(HANDLE h, CHAR_INFO *buf, COORD bufsize, COORD bufcoord, SMALL_RECT *region, int wide)
{
    int ok;
    WORD attr = console_attr(h, &ok);
    if (!ok) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!buf || !region) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    (void)wide;
    for (short y = region->Top; y <= region->Bottom; y++) {
        for (short x = region->Left; x <= region->Right; x++) {
            short bx = (short)(x - region->Left + bufcoord.X);
            short by = (short)(y - region->Top + bufcoord.Y);
            if (bx < 0 || by < 0 || bx >= bufsize.X || by >= bufsize.Y) continue;
            CHAR_INFO *ci = &buf[by * bufsize.X + bx];
            ci->Char.UnicodeChar = ' ';
            ci->Attributes = attr;
        }
    }
    return TRUE;
}

API BOOL ReadConsoleOutputW(HANDLE h, CHAR_INFO *buf, COORD bufsize, COORD bufcoord, SMALL_RECT *region)
{ return read_output(h, buf, bufsize, bufcoord, region, 1); }
API BOOL ReadConsoleOutputA(HANDLE h, CHAR_INFO *buf, COORD bufsize, COORD bufcoord, SMALL_RECT *region)
{ return read_output(h, buf, bufsize, bufcoord, region, 0); }

API BOOL ReadConsoleOutputCharacterW(HANDLE h, WCHAR *buf, DWORD len, COORD pos, DWORD *nread)
{
    int ok; console_attr(h, &ok);
    (void)pos;
    if (!ok) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    for (DWORD i = 0; i < len; i++) buf[i] = ' ';
    if (nread) *nread = len;
    return TRUE;
}
API BOOL ReadConsoleOutputCharacterA(HANDLE h, char *buf, DWORD len, COORD pos, DWORD *nread)
{
    int ok; console_attr(h, &ok);
    (void)pos;
    if (!ok) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    memset(buf, ' ', len);
    if (nread) *nread = len;
    return TRUE;
}
API BOOL ReadConsoleOutputAttribute(HANDLE h, WORD *attr, DWORD len, COORD pos, DWORD *nread)
{
    int ok;
    WORD a = console_attr(h, &ok);
    (void)pos;
    if (!ok) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    for (DWORD i = 0; i < len; i++) attr[i] = a;
    if (nread) *nread = len;
    return TRUE;
}

/* ---- serial ports (termios on tty handles) ----
 * COMn maps to /dev/ttyS(n-1) only if device_of in k32_file.c is taught the
 * mapping (see the report); until then these act on any tty handle (e.g. a
 * pseudo-terminal passed in) and fail ERROR_INVALID_FUNCTION on non-ttys. */
#pragma pack(push, 1)
typedef struct {
    DWORD DCBlength;
    DWORD BaudRate;
    DWORD fBinary:1, fParity:1, fOutxCtsFlow:1, fOutxDsrFlow:1, fDtrControl:2,
          fDsrSensitivity:1, fTXContinueOnXoff:1, fOutX:1, fInX:1, fErrorChar:1,
          fNull:1, fRtsControl:2, fAbortOnError:1, fDummy2:17;
    WORD  wReserved;
    WORD  XonLim, XoffLim;
    BYTE  ByteSize, Parity, StopBits;
    char  XonChar, XoffChar, ErrorChar, EofChar, EvtChar;
    WORD  wReserved1;
} DCB;
typedef struct { DWORD ReadIntervalTimeout, ReadTotalTimeoutMultiplier, ReadTotalTimeoutConstant,
                       WriteTotalTimeoutMultiplier, WriteTotalTimeoutConstant; } COMMTIMEOUTS;
typedef struct { DWORD fCtsHold:1, fDsrHold:1, fRlsdHold:1, fXoffHold:1, fXoffSent:1,
                       fEof:1, fTxim:1, fReserved:25; DWORD cbInQue, cbOutQue; } COMSTAT;
#pragma pack(pop)

static int comm_fd(HANDLE h, DWORD *err)
{
    int fd = lsiw_fd_from_handle(h);
    if (fd < 0) { *err = ERROR_INVALID_HANDLE; return -1; }
    if (!isatty(fd)) { *err = ERROR_INVALID_FUNCTION; return -1; }
    *err = 0;
    return fd;
}

static DWORD baud_to_win(speed_t s)
{
    switch (s) {
    case B110: return 110; case B300: return 300; case B600: return 600; case B1200: return 1200;
    case B2400: return 2400; case B4800: return 4800; case B9600: return 9600; case B19200: return 19200;
    case B38400: return 38400; case B57600: return 57600; case B115200: return 115200; case B230400: return 230400;
    default: return 9600;
    }
}
static speed_t win_to_baud(DWORD b)
{
    switch (b) {
    case 110: return B110; case 300: return B300; case 600: return B600; case 1200: return B1200;
    case 2400: return B2400; case 4800: return B4800; case 9600: return B9600; case 19200: return B19200;
    case 38400: return B38400; case 57600: return B57600; case 115200: return B115200; case 230400: return B230400;
    default: return B9600;
    }
}

static BOOL comm_fail(DWORD e) { lsiw_set_last_error(e); return FALSE; }

API BOOL GetCommState(HANDLE h, DCB *dcb)
{
    DWORD err; int fd = comm_fd(h, &err);
    if (fd < 0) return comm_fail(err);
    if (!dcb) return comm_fail(ERROR_INVALID_PARAMETER);
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return comm_fail(ERROR_INVALID_FUNCTION);
    memset(dcb, 0, sizeof *dcb);
    dcb->DCBlength = sizeof *dcb;
    dcb->BaudRate = baud_to_win(cfgetospeed(&t));
    dcb->fBinary = 1;
    switch (t.c_cflag & CSIZE) { case CS5: dcb->ByteSize = 5; break; case CS6: dcb->ByteSize = 6; break;
                                 case CS7: dcb->ByteSize = 7; break; default: dcb->ByteSize = 8; break; }
    if (t.c_cflag & PARENB) { dcb->fParity = 1; dcb->Parity = (t.c_cflag & PARODD) ? 1 : 2; }
    else dcb->Parity = 0;
    dcb->StopBits = (t.c_cflag & CSTOPB) ? 2 : 0;   /* TWOSTOPBITS / ONESTOPBIT */
    dcb->XonChar = 17; dcb->XoffChar = 19;
    return TRUE;
}

API BOOL SetCommState(HANDLE h, DCB *dcb)
{
    DWORD err; int fd = comm_fd(h, &err);
    if (fd < 0) return comm_fail(err);
    if (!dcb) return comm_fail(ERROR_INVALID_PARAMETER);
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return comm_fail(ERROR_INVALID_FUNCTION);
    cfmakeraw(&t);
    speed_t sp = win_to_baud(dcb->BaudRate);
    cfsetispeed(&t, sp); cfsetospeed(&t, sp);
    t.c_cflag &= ~(unsigned)CSIZE;
    switch (dcb->ByteSize) { case 5: t.c_cflag |= CS5; break; case 6: t.c_cflag |= CS6; break;
                             case 7: t.c_cflag |= CS7; break; default: t.c_cflag |= CS8; break; }
    if (dcb->Parity == 0) t.c_cflag &= ~(unsigned)PARENB;
    else { t.c_cflag |= PARENB; if (dcb->Parity == 1) t.c_cflag |= PARODD; else t.c_cflag &= ~(unsigned)PARODD; }
    if (dcb->StopBits == 2) t.c_cflag |= CSTOPB; else t.c_cflag &= ~(unsigned)CSTOPB;
    t.c_cflag |= CLOCAL | CREAD;
    return tcsetattr(fd, TCSANOW, &t) == 0 ? TRUE : comm_fail(ERROR_INVALID_FUNCTION);
}

/* Per-fd timeout and event-mask shadow state (small fixed table). */
static struct { int fd; COMMTIMEOUTS to; DWORD mask; int used; } g_comm[16];
static int comm_slot(int fd)
{
    int free_i = -1;
    for (int i = 0; i < 16; i++) { if (g_comm[i].used && g_comm[i].fd == fd) return i; if (free_i < 0 && !g_comm[i].used) free_i = i; }
    if (free_i >= 0) { g_comm[free_i].used = 1; g_comm[free_i].fd = fd; memset(&g_comm[free_i].to, 0, sizeof g_comm[free_i].to); g_comm[free_i].mask = 0; }
    return free_i;
}

API BOOL SetCommTimeouts(HANDLE h, COMMTIMEOUTS *ct)
{
    DWORD err; int fd = comm_fd(h, &err);
    if (fd < 0) return comm_fail(err);
    if (!ct) return comm_fail(ERROR_INVALID_PARAMETER);
    int i = comm_slot(fd);
    if (i >= 0) g_comm[i].to = *ct;
    /* Reflect the read total-timeout into VTIME (tenths of a second). */
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        DWORD ms = ct->ReadTotalTimeoutConstant;
        t.c_cc[VMIN] = ms ? 0 : 1;
        t.c_cc[VTIME] = (cc_t)((ms + 99) / 100);
        tcsetattr(fd, TCSANOW, &t);
    }
    return TRUE;
}
API BOOL GetCommTimeouts(HANDLE h, COMMTIMEOUTS *ct)
{
    DWORD err; int fd = comm_fd(h, &err);
    if (fd < 0) return comm_fail(err);
    if (!ct) return comm_fail(ERROR_INVALID_PARAMETER);
    int i = comm_slot(fd);
    *ct = i >= 0 ? g_comm[i].to : (COMMTIMEOUTS){ 0, 0, 0, 0, 0 };
    return TRUE;
}
API BOOL SetCommMask(HANDLE h, DWORD mask)
{
    DWORD err; int fd = comm_fd(h, &err);
    if (fd < 0) return comm_fail(err);
    int i = comm_slot(fd);
    if (i >= 0) g_comm[i].mask = mask;
    return TRUE;
}
API BOOL GetCommMask(HANDLE h, DWORD *mask)
{
    DWORD err; int fd = comm_fd(h, &err);
    if (fd < 0) return comm_fail(err);
    int i = comm_slot(fd);
    if (mask) *mask = i >= 0 ? g_comm[i].mask : 0;
    return TRUE;
}

#define EV_RXCHAR 0x0001
API BOOL WaitCommEvent(HANDLE h, DWORD *evt, void *overlapped)
{
    (void)overlapped;
    DWORD err; int fd = comm_fd(h, &err);
    if (fd < 0) return comm_fail(err);
    int i = comm_slot(fd);
    DWORD mask = i >= 0 ? g_comm[i].mask : EV_RXCHAR;
    if (evt) *evt = 0;
    if (mask & EV_RXCHAR) {
        char c; ssize_t r;
        struct termios t, saved;
        if (tcgetattr(fd, &t) == 0) { saved = t; t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0; tcsetattr(fd, TCSANOW, &t); }
        do r = recv(fd, &c, 1, MSG_PEEK); while (r < 0 && errno == EINTR);
        if (r < 0) { do r = read(fd, &c, 0); while (r < 0 && errno == EINTR); }
        tcsetattr(fd, TCSANOW, &saved);
        if (evt) *evt = EV_RXCHAR;
    }
    return TRUE;
}

API BOOL ClearCommError(HANDLE h, DWORD *errors, COMSTAT *stat)
{
    DWORD err; int fd = comm_fd(h, &err);
    if (fd < 0) return comm_fail(err);
    if (errors) *errors = 0;
    if (stat) {
        memset(stat, 0, sizeof *stat);
        int n = 0;
        if (ioctl(fd, FIONREAD, &n) == 0) stat->cbInQue = (DWORD)n;
    }
    return TRUE;
}
API BOOL SetCommBreak(HANDLE h)
{ DWORD e; int fd = comm_fd(h, &e); if (fd < 0) return comm_fail(e); (void)ioctl(fd, TIOCSBRK); return TRUE; }
API BOOL ClearCommBreak(HANDLE h)
{ DWORD e; int fd = comm_fd(h, &e); if (fd < 0) return comm_fail(e); (void)ioctl(fd, TIOCCBRK); return TRUE; }

#define SETXOFF 1
#define SETXON  2
#define SETBREAK 8
#define CLRBREAK 9
API BOOL EscapeCommFunction(HANDLE h, DWORD func)
{
    DWORD e; int fd = comm_fd(h, &e);
    if (fd < 0) return comm_fail(e);
    if (func == SETBREAK) (void)ioctl(fd, TIOCSBRK);
    else if (func == CLRBREAK) (void)ioctl(fd, TIOCCBRK);
    return TRUE;
}

API BOOL GetCommModemStatus(HANDLE h, DWORD *status)
{
    DWORD e; int fd = comm_fd(h, &e);
    if (fd < 0) return comm_fail(e);
    int bits = 0;
    if (ioctl(fd, TIOCMGET, &bits) < 0) { if (status) *status = 0; return TRUE; }
    DWORD s = 0;
    if (bits & TIOCM_CTS) s |= 0x0010;   /* MS_CTS_ON */
    if (bits & TIOCM_DSR) s |= 0x0020;   /* MS_DSR_ON */
    if (bits & TIOCM_RNG) s |= 0x0040;   /* MS_RING_ON */
    if (bits & TIOCM_CAR) s |= 0x0080;   /* MS_RLSD_ON */
    if (status) *status = s;
    return TRUE;
}

#define PURGE_TXCLEAR 0x0004
#define PURGE_RXCLEAR 0x0008
API BOOL PurgeComm(HANDLE h, DWORD flags)
{
    DWORD e; int fd = comm_fd(h, &e);
    if (fd < 0) return comm_fail(e);
    int q = ((flags & PURGE_RXCLEAR) && (flags & PURGE_TXCLEAR)) ? TCIOFLUSH
          : (flags & PURGE_RXCLEAR) ? TCIFLUSH : (flags & PURGE_TXCLEAR) ? TCOFLUSH : -1;
    if (q >= 0) tcflush(fd, q);
    return TRUE;
}
API BOOL TransmitCommChar(HANDLE h, char c)
{
    DWORD e; int fd = comm_fd(h, &e);
    if (fd < 0) return comm_fail(e);
    ssize_t r; do r = write(fd, &c, 1); while (r < 0 && errno == EINTR);
    return r == 1 ? TRUE : comm_fail(ERROR_INVALID_FUNCTION);
}
API BOOL SetupComm(HANDLE h, DWORD in_q, DWORD out_q)
{ (void)in_q; (void)out_q; DWORD e; int fd = comm_fd(h, &e); return fd < 0 ? comm_fail(e) : TRUE; }

/* COMMPROP: a generic RS-232 provider description. */
typedef struct {
    WORD wPacketLength, wPacketVersion;
    DWORD dwServiceMask, dwReserved1, dwMaxTxQueue, dwMaxRxQueue, dwMaxBaud, dwProvSubType,
          dwProvCapabilities, dwSettableParams, dwSettableBaud;
    WORD wSettableData, wSettableStopParity;
    DWORD dwCurrentTxQueue, dwCurrentRxQueue, dwProvSpec1, dwProvSpec2;
    WCHAR wcProvChar[1];
} COMMPROP;
API BOOL GetCommProperties(HANDLE h, COMMPROP *p)
{
    DWORD e; int fd = comm_fd(h, &e);
    if (fd < 0) return comm_fail(e);
    if (!p) return comm_fail(ERROR_INVALID_PARAMETER);
    memset(p, 0, sizeof *p);
    p->wPacketLength = sizeof *p;
    p->dwServiceMask = 1;                 /* SP_SERIALCOMM */
    p->dwMaxBaud = 0x10000000;            /* BAUD_USER */
    p->dwProvSubType = 1;                 /* PST_RS232 */
    p->dwSettableBaud = 0x1FFFF;
    p->wSettableData = 0x0F;              /* 5..8 data bits */
    p->wSettableStopParity = 0x1F07;
    p->dwMaxTxQueue = p->dwMaxRxQueue = 0;
    return TRUE;
}

/* BuildCommDCB: parse "baud=9600 parity=N data=8 stop=1" (and legacy mode
 * strings) into a DCB. */
static BOOL build_dcb(const char *s, DCB *dcb)
{
    if (!s || !dcb) return comm_fail(ERROR_INVALID_PARAMETER);
    memset(dcb, 0, sizeof *dcb);
    dcb->DCBlength = sizeof *dcb;
    dcb->BaudRate = 1200; dcb->ByteSize = 7; dcb->Parity = 2; dcb->StopBits = 0; dcb->fBinary = 1;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
    char *tok, *save = NULL;
    for (tok = strtok_r(buf, " ,\t", &save); tok; tok = strtok_r(NULL, " ,\t", &save)) {
        if (!strncmp(tok, "BAUD=", 5)) dcb->BaudRate = (DWORD)strtoul(tok + 5, NULL, 10);
        else if (!strncmp(tok, "PARITY=", 7)) {
            char c = tok[7];
            dcb->Parity = c == 'O' ? 1 : c == 'E' ? 2 : c == 'M' ? 3 : c == 'S' ? 4 : 0;
            dcb->fParity = dcb->Parity != 0;
        } else if (!strncmp(tok, "DATA=", 5)) dcb->ByteSize = (BYTE)strtoul(tok + 5, NULL, 10);
        else if (!strncmp(tok, "STOP=", 5)) {
            if (!strcmp(tok + 5, "1")) dcb->StopBits = 0;
            else if (!strcmp(tok + 5, "1.5")) dcb->StopBits = 1;
            else if (!strcmp(tok + 5, "2")) dcb->StopBits = 2;
        }
    }
    return TRUE;
}
API BOOL BuildCommDCBA(const char *s, DCB *dcb) { return build_dcb(s, dcb); }
API BOOL BuildCommDCBW(const WCHAR *s, DCB *dcb)
{ char *a = s ? lsiw_w2u(s) : NULL; BOOL r = build_dcb(a, dcb); free(a); return r; }

/* ---- tape drives: none exist, so every call reports no function ----
 * Tape functions return a Win32 error code directly (not BOOL). */
API DWORD CreateTapePartition(HANDLE h, DWORD type, DWORD count, DWORD size)
{ (void)h; (void)type; (void)count; (void)size; return ERROR_INVALID_FUNCTION; }
API DWORD EraseTape(HANDLE h, DWORD type, BOOL immediate)
{ (void)h; (void)type; (void)immediate; return ERROR_INVALID_FUNCTION; }
API DWORD GetTapeParameters(HANDLE h, DWORD op, DWORD *size, void *info)
{ (void)h; (void)op; (void)size; (void)info; return ERROR_INVALID_FUNCTION; }
API DWORD GetTapePosition(HANDLE h, DWORD type, DWORD *part, DWORD *off_low, DWORD *off_high)
{ (void)h; (void)type; (void)part; (void)off_low; (void)off_high; return ERROR_INVALID_FUNCTION; }
API DWORD GetTapeStatus(HANDLE h) { (void)h; return ERROR_INVALID_FUNCTION; }
API DWORD PrepareTape(HANDLE h, DWORD op, BOOL immediate)
{ (void)h; (void)op; (void)immediate; return ERROR_INVALID_FUNCTION; }
API DWORD SetTapeParameters(HANDLE h, DWORD op, void *info)
{ (void)h; (void)op; (void)info; return ERROR_INVALID_FUNCTION; }
API DWORD SetTapePosition(HANDLE h, DWORD method, DWORD part, DWORD off_low, DWORD off_high, BOOL immediate)
{ (void)h; (void)method; (void)part; (void)off_low; (void)off_high; (void)immediate; return ERROR_INVALID_FUNCTION; }
API DWORD WriteTapemark(HANDLE h, DWORD type, DWORD count, BOOL immediate)
{ (void)h; (void)type; (void)count; (void)immediate; return ERROR_INVALID_FUNCTION; }

/* ---- user-mode debugging of other processes: not supported ----
 * LSIW does not run a debug port, so a process cannot attach to and single-
 * step another.  Programs that probe for this capability get a clean failure. */
API BOOL DebugActiveProcess(DWORD pid) { (void)pid; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API BOOL DebugActiveProcessStop(DWORD pid) { (void)pid; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API BOOL WaitForDebugEvent(void *event, DWORD ms) { (void)event; (void)ms; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API BOOL WaitForDebugEventEx(void *event, DWORD ms) { (void)event; (void)ms; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API BOOL ContinueDebugEvent(DWORD pid, DWORD tid, DWORD status)
{ (void)pid; (void)tid; (void)status; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }

/* ---- IDN (IDNA2003 ToASCII / ToUnicode with Punycode, RFC 3492) ---- */
#define IDN_ALLOW_UNASSIGNED     0x01
#define IDN_USE_STD3_ASCII_RULES 0x02
#define IDN_EMAIL_ADDRESS        0x04
#define IDN_RAW_PUNYCODE         0x08

#define PUNY_BASE 36
#define PUNY_TMIN 1
#define PUNY_TMAX 26
#define PUNY_SKEW 38
#define PUNY_DAMP 700
#define PUNY_INITIAL_BIAS 72
#define PUNY_INITIAL_N 128

static uint32_t puny_adapt(uint32_t delta, uint32_t numpoints, int first)
{
    delta = first ? delta / PUNY_DAMP : delta / 2;
    delta += delta / numpoints;
    uint32_t k = 0;
    while (delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2) { delta /= PUNY_BASE - PUNY_TMIN; k += PUNY_BASE; }
    return k + (PUNY_BASE - PUNY_TMIN + 1) * delta / (delta + PUNY_SKEW);
}
static char puny_encode_digit(uint32_t d) { return (char)(d < 26 ? d + 'a' : d - 26 + '0'); }
static int puny_decode_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0' + 26;
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    return -1;
}

/* UTF-16 (with surrogates) to code points. Returns count, or -1 on bad input. */
static int utf16_to_cp(const WCHAR *s, int n, uint32_t *out, int outcap)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= n) return -1;
            uint32_t lo = s[++i];
            if (lo < 0xDC00 || lo > 0xDFFF) return -1;
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        } else if (c >= 0xDC00 && c <= 0xDFFF) return -1;
        if (k >= outcap) return -1;
        out[k++] = c;
    }
    return k;
}
static int cp_to_utf16(uint32_t c, WCHAR *out)
{
    if (c <= 0xFFFF) { out[0] = (WCHAR)c; return 1; }
    c -= 0x10000;
    out[0] = (WCHAR)(0xD800 + (c >> 10));
    out[1] = (WCHAR)(0xDC00 + (c & 0x3FF));
    return 2;
}

/* Punycode-encode code points into ASCII bytes (no "xn--" prefix). */
static int puny_encode(const uint32_t *in, int n, char *out, int outcap)
{
    uint32_t delta = 0, bias = PUNY_INITIAL_BIAS, nmin = PUNY_INITIAL_N;
    int outlen = 0, basic = 0;
    for (int i = 0; i < n; i++) if (in[i] < 0x80) { if (outlen >= outcap) return -1; out[outlen++] = (char)in[i]; basic++; }
    int handled = basic;
    if (basic && outlen < outcap) out[outlen++] = '-';
    while (handled < n) {
        uint32_t m = 0xFFFFFFFF;
        for (int i = 0; i < n; i++) if (in[i] >= nmin && in[i] < m) m = in[i];
        if (m - nmin > (0xFFFFFFFF - delta) / (uint32_t)(handled + 1)) return -1;
        delta += (m - nmin) * (uint32_t)(handled + 1);
        nmin = m;
        for (int i = 0; i < n; i++) {
            if (in[i] < nmin) { if (++delta == 0) return -1; }
            else if (in[i] == nmin) {
                uint32_t q = delta;
                for (uint32_t k = PUNY_BASE;; k += PUNY_BASE) {
                    uint32_t t = k <= bias ? PUNY_TMIN : k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias;
                    if (q < t) break;
                    if (outlen >= outcap) return -1;
                    out[outlen++] = puny_encode_digit(t + (q - t) % (PUNY_BASE - t));
                    q = (q - t) / (PUNY_BASE - t);
                }
                if (outlen >= outcap) return -1;
                out[outlen++] = puny_encode_digit(q);
                bias = puny_adapt(delta, (uint32_t)(handled + 1), handled == basic);
                delta = 0;
                handled++;
            }
        }
        delta++;
        nmin++;
    }
    return outlen;
}

/* Punycode-decode ASCII bytes (no "xn--" prefix) into code points. */
static int puny_decode(const char *in, int n, uint32_t *out, int outcap)
{
    uint32_t nmax = PUNY_INITIAL_N, bias = PUNY_INITIAL_BIAS, i = 0;
    int outlen = 0, b = -1;
    for (int j = 0; j < n; j++) if (in[j] == '-') b = j;
    for (int j = 0; j < (b < 0 ? 0 : b); j++) { if ((unsigned char)in[j] >= 0x80) return -1; if (outlen >= outcap) return -1; out[outlen++] = (unsigned char)in[j]; }
    int pos = b < 0 ? 0 : b + 1;
    while (pos < n) {
        uint32_t oldi = i, w = 1;
        for (uint32_t k = PUNY_BASE;; k += PUNY_BASE) {
            if (pos >= n) return -1;
            int digit = puny_decode_digit((unsigned char)in[pos++]);
            if (digit < 0) return -1;
            if ((uint32_t)digit > (0xFFFFFFFF - i) / w) return -1;
            i += (uint32_t)digit * w;
            uint32_t t = k <= bias ? PUNY_TMIN : k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias;
            if ((uint32_t)digit < t) break;
            if (w > 0xFFFFFFFF / (PUNY_BASE - t)) return -1;
            w *= PUNY_BASE - t;
        }
        bias = puny_adapt(i - oldi, (uint32_t)(outlen + 1), oldi == 0);
        if (i / (uint32_t)(outlen + 1) > 0xFFFFFFFF - nmax) return -1;
        nmax += i / (uint32_t)(outlen + 1);
        i %= (uint32_t)(outlen + 1);
        if (outlen >= outcap) return -1;
        memmove(out + i + 1, out + i, (size_t)(outlen - i) * sizeof *out);
        out[i++] = nmax;
        outlen++;
    }
    return outlen;
}

static int idn_is_sep(uint32_t c) { return c == '.' || c == 0x3002 || c == 0xFF0E || c == 0xFF61; }

API int IdnToAscii(DWORD flags, const WCHAR *in, int inlen, WCHAR *out, int outlen)
{
    if (!in) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (inlen < 0) inlen = (int)lsiw_wcslen(in);
    uint32_t cps[512];
    int ncp = utf16_to_cp(in, inlen, cps, 512);
    if (ncp < 0) { lsiw_set_last_error(ERROR_INVALID_NAME); return 0; }
    WCHAR result[1024];
    int rlen = 0, start = 0;
    for (int i = 0; i <= ncp; i++) {
        if (i == ncp || idn_is_sep(cps[i])) {
            int llen = i - start;
            if (llen == 0 && i != ncp) { lsiw_set_last_error(ERROR_INVALID_NAME); return 0; }
            int ascii = 1;
            for (int j = start; j < i; j++) if (cps[j] >= 0x80) { ascii = 0; break; }
            if (ascii || (flags & IDN_RAW_PUNYCODE)) {
                for (int j = start; j < i; j++) { if (rlen >= 1023) return 0; result[rlen++] = (WCHAR)cps[j]; }
            } else {
                char enc[512];
                int el = puny_encode(cps + start, llen, enc, sizeof enc);
                if (el < 0) { lsiw_set_last_error(ERROR_INVALID_NAME); return 0; }
                const char *pfx = "xn--";
                for (int k = 0; k < 4; k++) { if (rlen >= 1023) return 0; result[rlen++] = pfx[k]; }
                for (int k = 0; k < el; k++) { if (rlen >= 1023) return 0; result[rlen++] = (WCHAR)enc[k]; }
            }
            if (i < ncp) { if (rlen >= 1023) return 0; result[rlen++] = '.'; }
            start = i + 1;
        }
    }
    if (outlen == 0) return rlen;
    if (rlen > outlen) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, result, (size_t)rlen * 2);
    return rlen;
}

API int IdnToUnicode(DWORD flags, const WCHAR *in, int inlen, WCHAR *out, int outlen)
{
    (void)flags;
    if (!in) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (inlen < 0) inlen = (int)lsiw_wcslen(in);
    WCHAR result[1024];
    int rlen = 0, start = 0;
    for (int i = 0; i <= inlen; i++) {
        if (i == inlen || in[i] == '.') {
            int llen = i - start;
            int is_xn = llen >= 4 && (in[start] == 'x' || in[start] == 'X') && (in[start+1] == 'n' || in[start+1] == 'N')
                        && in[start+2] == '-' && in[start+3] == '-';
            if (is_xn) {
                char enc[512];
                int el = 0;
                for (int j = start + 4; j < i && el < (int)sizeof enc; j++) enc[el++] = (char)in[j];
                uint32_t cps[512];
                int ncp = puny_decode(enc, el, cps, 512);
                if (ncp < 0) { lsiw_set_last_error(ERROR_INVALID_NAME); return 0; }
                for (int k = 0; k < ncp; k++) { WCHAR w[2]; int wl = cp_to_utf16(cps[k], w); for (int t = 0; t < wl; t++) { if (rlen >= 1023) return 0; result[rlen++] = w[t]; } }
            } else {
                for (int j = start; j < i; j++) { if (rlen >= 1023) return 0; result[rlen++] = in[j]; }
            }
            if (i < inlen) { if (rlen >= 1023) return 0; result[rlen++] = '.'; }
            start = i + 1;
        }
    }
    if (outlen == 0) return rlen;
    if (rlen > outlen) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, result, (size_t)rlen * 2);
    return rlen;
}

/* ================================================================== */
/* advapi32 CryptoAPI: provider enumeration and signing                 */
/* ================================================================== */

#define NTE_BAD_UID    0x80090001u
#define NTE_BAD_KEY    0x80090003u
#define NTE_BAD_FLAGS  0x80090009u
#define NTE_NO_KEY     0x8009000Du

/* The providers advapi32's CryptAcquireContext actually serves (it accepts any
 * of these names and reports two of them by PP_NAME).  Enumerating the standard
 * Microsoft set keeps CAPI probing - all Node needs here - consistent. */
static const struct { const char *name; DWORD type; } g_providers[] = {
    { "Microsoft Base Cryptographic Provider v1.0", 1 },        /* PROV_RSA_FULL */
    { "Microsoft Enhanced Cryptographic Provider v1.0", 1 },
    { "Microsoft Strong Cryptographic Provider", 1 },
    { "Microsoft RSA SChannel Cryptographic Provider", 12 },    /* PROV_RSA_SCHANNEL */
    { "Microsoft Base DSS and Diffie-Hellman Cryptographic Provider", 13 },
    { "Microsoft Enhanced RSA and AES Cryptographic Provider", 24 }, /* PROV_RSA_AES */
};
#define NPROV (int)(sizeof g_providers / sizeof g_providers[0])

/* Return a string the Win32 way to either a wide or an ANSI caller buffer:
 * pcb is a byte count; a NULL buffer asks for the needed size. */
static BOOL crypt_ret_str(const char *s, void *buf, DWORD *pcb, int wide)
{
    if (!pcb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD need = wide ? (DWORD)(strlen(s) + 1) * 2 : (DWORD)(strlen(s) + 1);
    if (!buf) { *pcb = need; return TRUE; }
    if (*pcb < need) { *pcb = need; lsiw_set_last_error(ERROR_MORE_DATA); return FALSE; }
    if (wide) lsiw_utf8_to_utf16(s, -1, buf, (int)(need / 2));
    else memcpy(buf, s, need);
    *pcb = need;
    return TRUE;
}

static BOOL enum_providers(DWORD idx, DWORD *type, void *name, DWORD *cb, int wide)
{
    if (idx >= (DWORD)NPROV) { lsiw_set_last_error(ERROR_NO_MORE_ITEMS); return FALSE; }
    if (type) *type = g_providers[idx].type;
    return crypt_ret_str(g_providers[idx].name, name, cb, wide);
}
API BOOL CryptEnumProvidersW(DWORD idx, DWORD *res, DWORD flags, DWORD *type, WCHAR *name, DWORD *cb)
{ (void)res; (void)flags; return enum_providers(idx, type, name, cb, 1); }
API BOOL CryptEnumProvidersA(DWORD idx, DWORD *res, DWORD flags, DWORD *type, char *name, DWORD *cb)
{ (void)res; (void)flags; return enum_providers(idx, type, name, cb, 0); }

/* Distinct provider types, each with a representative type name. */
static const struct { DWORD type; const char *name; } g_provtypes[] = {
    { 1,  "Microsoft Base Cryptographic Provider v1.0" },
    { 12, "Microsoft RSA SChannel Cryptographic Provider" },
    { 13, "Microsoft Base DSS and Diffie-Hellman Cryptographic Provider" },
    { 24, "Microsoft Enhanced RSA and AES Cryptographic Provider" },
};
#define NPROVTYPE (int)(sizeof g_provtypes / sizeof g_provtypes[0])

static BOOL enum_provtypes(DWORD idx, DWORD *type, void *name, DWORD *cb, int wide)
{
    if (idx >= (DWORD)NPROVTYPE) { lsiw_set_last_error(ERROR_NO_MORE_ITEMS); return FALSE; }
    if (type) *type = g_provtypes[idx].type;
    return crypt_ret_str(g_provtypes[idx].name, name, cb, wide);
}
API BOOL CryptEnumProviderTypesW(DWORD idx, DWORD *res, DWORD flags, DWORD *type, WCHAR *name, DWORD *cb)
{ (void)res; (void)flags; return enum_provtypes(idx, type, name, cb, 1); }
API BOOL CryptEnumProviderTypesA(DWORD idx, DWORD *res, DWORD flags, DWORD *type, char *name, DWORD *cb)
{ (void)res; (void)flags; return enum_provtypes(idx, type, name, cb, 0); }

static const char *default_provider(DWORD type)
{
    switch (type) {
    case 1:  return "Microsoft Enhanced Cryptographic Provider v1.0";
    case 12: return "Microsoft RSA SChannel Cryptographic Provider";
    case 13: return "Microsoft Base DSS and Diffie-Hellman Cryptographic Provider";
    case 24: return "Microsoft Enhanced RSA and AES Cryptographic Provider";
    default: return NULL;
    }
}
API BOOL CryptGetDefaultProviderW(DWORD type, DWORD *res, DWORD flags, WCHAR *name, DWORD *cb)
{
    (void)res; (void)flags;
    const char *p = default_provider(type);
    if (!p) { lsiw_set_last_error(NTE_BAD_UID); return FALSE; }
    return crypt_ret_str(p, name, cb, 1);
}
API BOOL CryptGetDefaultProviderA(DWORD type, DWORD *res, DWORD flags, char *name, DWORD *cb)
{
    (void)res; (void)flags;
    const char *p = default_provider(type);
    if (!p) { lsiw_set_last_error(NTE_BAD_UID); return FALSE; }
    return crypt_ret_str(p, name, cb, 0);
}

/* advapi32 keeps no persistent key containers, so a provider never holds a
 * user key pair. */
API BOOL CryptGetUserKey(ULONG_PTR prov, DWORD keyspec, ULONG_PTR *key)
{
    (void)prov; (void)keyspec;
    if (key) *key = 0;
    lsiw_set_last_error(NTE_NO_KEY);
    return FALSE;
}

/* Signing needs a provider RSA/DSS key pair, which advapi32 cannot create or
 * import (CryptGenKey/CryptImportKey only handle symmetric plaintext blobs),
 * so there is no key to sign with. */
API BOOL CryptSignHashW(ULONG_PTR hash, DWORD keyspec, const WCHAR *desc, DWORD flags, BYTE *sig, DWORD *siglen)
{
    (void)hash; (void)keyspec; (void)desc; (void)flags; (void)sig; (void)siglen;
    lsiw_set_last_error(NTE_BAD_KEY);
    return FALSE;
}
API BOOL CryptSignHashA(ULONG_PTR hash, DWORD keyspec, const char *desc, DWORD flags, BYTE *sig, DWORD *siglen)
{
    (void)hash; (void)keyspec; (void)desc; (void)flags; (void)sig; (void)siglen;
    lsiw_set_last_error(NTE_BAD_KEY);
    return FALSE;
}

/* ================================================================== */
/* iphlpapi: GetBestRoute2                                              */
/* ================================================================== */

#define WIN_AF_INET   2
#define WIN_AF_INET6  23
#define NO_ERROR                  0
#define ERROR_NETWORK_UNREACHABLE 1231

#pragma pack(push, 1)
typedef struct { uint16_t sin_family, sin_port; uint32_t sin_addr; uint8_t sin_zero[8]; } WIN_SOCKADDR_IN;
typedef struct { uint16_t sin6_family, sin6_port; uint32_t sin6_flowinfo; uint8_t sin6_addr[16]; uint32_t sin6_scope_id; } WIN_SOCKADDR_IN6;
typedef union { WIN_SOCKADDR_IN Ipv4; WIN_SOCKADDR_IN6 Ipv6; uint16_t si_family; } SOCKADDR_INET;
#pragma pack(pop)

typedef uint64_t NET_LUID;
typedef struct { SOCKADDR_INET Prefix; uint8_t PrefixLength; } IP_ADDRESS_PREFIX;
typedef struct {
    NET_LUID InterfaceLuid;
    ULONG    InterfaceIndex;
    IP_ADDRESS_PREFIX DestinationPrefix;
    SOCKADDR_INET NextHop;
    uint8_t  SitePrefixLength;
    ULONG    ValidLifetime, PreferredLifetime, Metric, Protocol;
    uint8_t  Loopback, AutoconfigureAddress, Publish, Immortal;
    ULONG    Age, Origin;
} MIB_IPFORWARD_ROW2;

/* Windows SOCKADDR_INET -> a Linux sockaddr for the routing socket. */
static int win_sa_to_linux(const SOCKADDR_INET *w, struct sockaddr_storage *ss, socklen_t *len)
{
    memset(ss, 0, sizeof *ss);
    if (w->si_family == WIN_AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)ss;
        s->sin_family = AF_INET;
        s->sin_port = w->Ipv4.sin_port;
        memcpy(&s->sin_addr, &w->Ipv4.sin_addr, 4);
        *len = sizeof *s;
        return AF_INET;
    }
    if (w->si_family == WIN_AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)ss;
        s->sin6_family = AF_INET6;
        s->sin6_port = w->Ipv6.sin6_port;
        s->sin6_flowinfo = w->Ipv6.sin6_flowinfo;
        memcpy(&s->sin6_addr, w->Ipv6.sin6_addr, 16);
        s->sin6_scope_id = w->Ipv6.sin6_scope_id;
        *len = sizeof *s;
        return AF_INET6;
    }
    return -1;
}
static void linux_sa_to_win(const struct sockaddr *s, SOCKADDR_INET *w)
{
    memset(w, 0, sizeof *w);
    if (s->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)s;
        w->Ipv4.sin_family = WIN_AF_INET;
        memcpy(&w->Ipv4.sin_addr, &in->sin_addr, 4);
    } else if (s->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in = (const struct sockaddr_in6 *)s;
        w->Ipv6.sin6_family = WIN_AF_INET6;
        memcpy(w->Ipv6.sin6_addr, &in->sin6_addr, 16);
        w->Ipv6.sin6_scope_id = in->sin6_scope_id;
    }
}

/* Find the best route to a destination.  The source address and outgoing
 * interface are discovered with the kernel's own route selection: a UDP
 * socket is connected (no packets are sent) and getsockname reports the
 * address the kernel would use.  Works for IPv4 and IPv6. */
API DWORD GetBestRoute2(NET_LUID *luid, ULONG ifindex, const SOCKADDR_INET *src,
                        const SOCKADDR_INET *dest, ULONG sort, MIB_IPFORWARD_ROW2 *best, SOCKADDR_INET *best_src)
{
    (void)sort;
    if (!dest || !best) return ERROR_INVALID_PARAMETER;
    struct sockaddr_storage dss, sss;
    socklen_t dlen = 0, slen = 0;
    int fam = win_sa_to_linux(dest, &dss, &dlen);
    if (fam < 0) return ERROR_INVALID_PARAMETER;

    int fd = socket(fam, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return ERROR_NETWORK_UNREACHABLE;
    if (src && src->si_family == dest->si_family) {
        if (win_sa_to_linux(src, &sss, &slen) >= 0) (void)bind(fd, (struct sockaddr *)&sss, slen);
    }
    /* A port must be set for connect(); 53 is arbitrary and nothing is sent. */
    if (fam == AF_INET) ((struct sockaddr_in *)&dss)->sin_port = htons(53);
    else ((struct sockaddr_in6 *)&dss)->sin6_port = htons(53);
    if (connect(fd, (struct sockaddr *)&dss, dlen) < 0) { close(fd); return ERROR_NETWORK_UNREACHABLE; }

    struct sockaddr_storage chosen;
    socklen_t clen = sizeof chosen;
    if (getsockname(fd, (struct sockaddr *)&chosen, &clen) < 0) { close(fd); return ERROR_NETWORK_UNREACHABLE; }
    close(fd);

    memset(best, 0, sizeof *best);
    SOCKADDR_INET bs;
    linux_sa_to_win((struct sockaddr *)&chosen, &bs);
    if (best_src) *best_src = bs;

    /* Interface index: prefer the caller's, else resolve via GetBestInterfaceEx. */
    ULONG idx = ifindex;
    if (!idx) {
        DWORD (WINAPI *gbi)(void *, DWORD *) = iph("GetBestInterfaceEx");
        DWORD got = 0;
        struct sockaddr_storage dss2; socklen_t dl2;
        win_sa_to_linux(dest, &dss2, &dl2);
        /* GetBestInterfaceEx wants a Windows sockaddr; pass the original. */
        if (gbi && gbi((void *)dest, &got) == NO_ERROR) idx = got;
    }
    best->InterfaceIndex = idx;
    if (luid) best->InterfaceLuid = *luid;
    else if (idx) {
        DWORD (WINAPI *cil)(ULONG, NET_LUID *) = iph("ConvertInterfaceIndexToLuid");
        if (cil) cil(idx, &best->InterfaceLuid);
    }

    best->DestinationPrefix.Prefix = *dest;
    best->DestinationPrefix.PrefixLength = 0;
    best->NextHop.si_family = dest->si_family;   /* on-link: next hop unspecified */
    best->Protocol = 3;                          /* MIB_IPPROTO_NETMGMT */
    best->Origin = 0;                            /* NlroManual */
    best->Metric = 0;

    /* For IPv4, refine NextHop/Metric from the kernel route table. */
    if (fam == AF_INET) {
        typedef struct { DWORD dest, idx, mask, policy, nexthop, type, proto, age, nexthopas, m1, m2, m3, m4, m5; } FWDROW;
        DWORD (WINAPI *gbr)(DWORD, DWORD, FWDROW *) = iph("GetBestRoute");
        FWDROW row;
        if (gbr && gbr(((struct sockaddr_in *)&dss)->sin_addr.s_addr, 0, &row) == NO_ERROR) {
            best->NextHop.Ipv4.sin_family = WIN_AF_INET;
            memcpy(&best->NextHop.Ipv4.sin_addr, &row.nexthop, 4);
            best->Metric = row.m1;
        }
    }
    return NO_ERROR;
}

/* ================================================================== */
/* Export tables                                                        */
/* ================================================================== */

const struct lsiw_export lsiw_ntobj_exports[] = {
    /* events / mutants / semaphores / timers */
    E(NtCreateEvent), E(NtOpenEvent), E(NtQueryEvent),
    E(NtCreateMutant), E(NtOpenMutant),
    E(NtCreateSemaphore), E(NtOpenSemaphore), E(NtQuerySemaphore),
    E(NtCreateTimer), E(NtSetTimer), E(NtCancelTimer), E(NtQueryTimer),
    E(NtQueryTimerResolution), E(NtSetTimerResolution),
    EX("ZwCreateEvent", NtCreateEvent), EX("ZwOpenEvent", NtOpenEvent),
    EX("ZwCreateMutant", NtCreateMutant), EX("ZwOpenMutant", NtOpenMutant),
    EX("ZwCreateSemaphore", NtCreateSemaphore), EX("ZwOpenSemaphore", NtOpenSemaphore),
    EX("ZwCreateTimer", NtCreateTimer), EX("ZwSetTimer", NtSetTimer), EX("ZwCancelTimer", NtCancelTimer),
    /* sections */
    E(NtCreateSection), E(NtOpenSection), E(NtMapViewOfSection), E(NtUnmapViewOfSection),
    EX("ZwCreateSection", NtCreateSection), EX("ZwOpenSection", NtOpenSection),
    EX("ZwMapViewOfSection", NtMapViewOfSection), EX("ZwUnmapViewOfSection", NtUnmapViewOfSection),
    /* object namespace */
    E(NtCreateDirectoryObject), E(NtOpenDirectoryObject), E(NtQueryDirectoryObject),
    E(NtCreateSymbolicLinkObject), E(NtOpenSymbolicLinkObject), E(NtQuerySymbolicLinkObject),
    EX("ZwCreateDirectoryObject", NtCreateDirectoryObject), EX("ZwOpenDirectoryObject", NtOpenDirectoryObject),
    EX("ZwOpenSymbolicLinkObject", NtOpenSymbolicLinkObject), EX("ZwQuerySymbolicLinkObject", NtQuerySymbolicLinkObject),
    /* job objects */
    E(NtCreateJobObject), E(NtOpenJobObject), E(NtAssignProcessToJobObject),
    E(NtQueryInformationJobObject), E(NtSetInformationJobObject),
    EX("ZwCreateJobObject", NtCreateJobObject), EX("ZwAssignProcessToJobObject", NtAssignProcessToJobObject),
    EX("ZwQueryInformationJobObject", NtQueryInformationJobObject), EX("ZwSetInformationJobObject", NtSetInformationJobObject),
    /* LUID / memory locking */
    E(NtAllocateLocallyUniqueId), E(NtLockVirtualMemory), E(NtUnlockVirtualMemory),
    EX("ZwAllocateLocallyUniqueId", NtAllocateLocallyUniqueId),
    EX("ZwLockVirtualMemory", NtLockVirtualMemory), EX("ZwUnlockVirtualMemory", NtUnlockVirtualMemory),
    /* registry */
    E(NtCreateKey), E(NtOpenKey), E(NtOpenKeyEx), E(NtQueryValueKey), E(NtSetValueKey),
    EX("ZwCreateKey", NtCreateKey), EX("ZwOpenKey", NtOpenKey), EX("ZwOpenKeyEx", NtOpenKeyEx),
    EX("ZwQueryValueKey", NtQueryValueKey), EX("ZwSetValueKey", NtSetValueKey),
    E(RtlCheckRegistryKey), E(RtlQueryRegistryValues),
    /* tokens & security objects */
    E(NtOpenProcessToken), E(NtOpenProcessTokenEx), E(NtOpenThreadToken), E(NtOpenThreadTokenEx),
    E(NtQueryInformationToken), E(NtSetInformationToken), E(NtDuplicateToken), E(NtAdjustPrivilegesToken),
    E(NtPrivilegeCheck), E(NtAccessCheck), E(NtQuerySecurityObject), E(NtSetSecurityObject),
    EX("ZwOpenProcessToken", NtOpenProcessToken), EX("ZwOpenProcessTokenEx", NtOpenProcessTokenEx),
    EX("ZwOpenThreadToken", NtOpenThreadToken), EX("ZwQueryInformationToken", NtQueryInformationToken),
    EX("ZwAdjustPrivilegesToken", NtAdjustPrivilegesToken), EX("ZwAccessCheck", NtAccessCheck),
    EX("ZwQuerySecurityObject", NtQuerySecurityObject), EX("ZwSetSecurityObject", NtSetSecurityObject),
    EX("ZwDuplicateToken", NtDuplicateToken),
    /* Rtl security descriptors / SIDs / ACLs */
    E(RtlCreateSecurityDescriptor), E(RtlAbsoluteToSelfRelativeSD),
    E(RtlGetOwnerSecurityDescriptor), E(RtlSetOwnerSecurityDescriptor),
    E(RtlGetGroupSecurityDescriptor), E(RtlSetGroupSecurityDescriptor),
    E(RtlGetDaclSecurityDescriptor), E(RtlSetDaclSecurityDescriptor),
    E(RtlGetControlSecurityDescriptor), E(RtlSetControlSecurityDescriptor),
    E(RtlCreateAcl), E(RtlAddAccessAllowedAce), E(RtlAddAccessAllowedAceEx),
    E(RtlAddAccessDeniedAce), E(RtlAddAccessDeniedAceEx), E(RtlGetAce), E(RtlFirstFreeAce),
    E(RtlInitializeSid), E(RtlInitializeSidEx), E(RtlLengthSid), E(RtlCopySid),
    E(RtlEqualSid), E(RtlEqualPrefixSid), E(RtlValidSid), E(RtlIdentifierAuthoritySid),
    E(RtlSubAuthoritySid), E(RtlSubAuthorityCountSid), E(RtlConvertSidToUnicodeString),
    /* Rtl strings */
    E(RtlUpcaseUnicodeChar), E(RtlDowncaseUnicodeChar),
    E(RtlUpcaseUnicodeString), E(RtlDowncaseUnicodeString),
    E(RtlAppendUnicodeToString), E(RtlAppendUnicodeStringToString),
    E(RtlPrefixUnicodeString), E(RtlSetCurrentDirectory_U),
    /* transactions (stubs) */
    E(RtlGetCurrentTransaction), E(RtlSetCurrentTransaction),
    E(NtCreateTransaction), E(NtCommitTransaction), E(NtRollbackTransaction),
    EX("ZwCreateTransaction", NtCreateTransaction), EX("ZwCommitTransaction", NtCommitTransaction),
    EX("ZwRollbackTransaction", NtRollbackTransaction),
    /* debug buffers */
    E(RtlCreateQueryDebugBuffer), E(RtlQueryProcessDebugInformation), E(RtlDestroyQueryDebugBuffer),
    { NULL, NULL }
};

const struct lsiw_export lsiw_k32more_exports[] = {
    /* resource enumeration */
    E(EnumResourceTypesW), E(EnumResourceTypesA), E(EnumResourceNamesW), E(EnumResourceNamesA),
    E(EnumResourceLanguagesW), E(EnumResourceLanguagesA),
    E(EnumResourceTypesExW), E(EnumResourceTypesExA), E(EnumResourceNamesExW), E(EnumResourceNamesExA),
    E(EnumResourceLanguagesExW), E(EnumResourceLanguagesExA),
    /* directory creation (W form lives in k32_file.c) */
    E(CreateDirectoryExA),
    /* console output reads */
    E(ReadConsoleOutputW), E(ReadConsoleOutputA),
    E(ReadConsoleOutputCharacterW), E(ReadConsoleOutputCharacterA), E(ReadConsoleOutputAttribute),
    /* serial ports */
    E(GetCommState), E(SetCommState), E(SetCommTimeouts), E(GetCommTimeouts),
    E(SetCommMask), E(GetCommMask), E(WaitCommEvent), E(ClearCommError),
    E(ClearCommBreak), E(SetCommBreak), E(EscapeCommFunction), E(GetCommModemStatus),
    E(PurgeComm), E(TransmitCommChar), E(SetupComm), E(GetCommProperties),
    E(BuildCommDCBA), E(BuildCommDCBW),
    /* tape (unsupported) */
    E(CreateTapePartition), E(EraseTape), E(GetTapeParameters), E(GetTapePosition),
    E(GetTapeStatus), E(PrepareTape), E(SetTapeParameters), E(SetTapePosition), E(WriteTapemark),
    /* cross-process debugging (unsupported) */
    E(DebugActiveProcess), E(DebugActiveProcessStop), E(WaitForDebugEvent),
    E(WaitForDebugEventEx), E(ContinueDebugEvent),
    /* IDN / Punycode */
    E(IdnToAscii), E(IdnToUnicode),
    /* advapi32 CryptoAPI */
    E(CryptEnumProvidersA), E(CryptEnumProvidersW), E(CryptEnumProviderTypesA), E(CryptEnumProviderTypesW),
    E(CryptGetDefaultProviderA), E(CryptGetDefaultProviderW), E(CryptGetUserKey),
    E(CryptSignHashA), E(CryptSignHashW),
    /* iphlpapi */
    E(GetBestRoute2),
    { NULL, NULL }
};
