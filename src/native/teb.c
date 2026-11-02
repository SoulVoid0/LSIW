/* Thread Environment Block / Process Environment Block for x86-64.
 *
 * Windows x64 code reaches its TEB through the GS segment: GS:[0x30] is the
 * TEB's own address, GS:[0x58] the implicit-TLS array, GS:[0x60] the PEB,
 * GS:[0x68] the last-error value and GS:[0x1480] the TlsAlloc slots. glibc
 * keeps its own thread pointer in FS, so GS is free and each thread that
 * runs Windows code gets a TEB installed with arch_prctl(ARCH_SET_GS).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include <asm/prctl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static uint8_t *g_peb;
static __thread uint8_t *t_teb;     /* host-side copy, valid in host code too */

void *lsiw_peb(void)
{
    if (!g_peb) {
        g_peb = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_peb == MAP_FAILED) abort();
        memset(g_peb, 0, 0x1000);
        /* 0x30 ProcessHeap */
        *(void **)(g_peb + 0x30) = lsiw_process_heap();
        /* NumberOfProcessors, OSMajorVersion/Minor/BuildNumber/PlatformId (Windows 10). */
        *(uint32_t *)(g_peb + 0xb8) = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
        *(uint32_t *)(g_peb + 0x118) = 10;
        *(uint32_t *)(g_peb + 0x11c) = 0;
        *(uint16_t *)(g_peb + 0x120) = 19045;
        *(uint32_t *)(g_peb + 0x124) = 2;
    }
    return g_peb;
}

void *lsiw_teb_create(void)
{
    uint8_t *teb = mmap(NULL, TEB_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (teb == MAP_FAILED) return NULL;
    memset(teb, 0, TEB_SIZE);

    /* Stack bounds (NT_TIB.StackBase / StackLimit). */
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void *lo; size_t sz;
        if (pthread_attr_getstack(&attr, &lo, &sz) == 0) {
            *(void **)(teb + 0x08) = (uint8_t *)lo + sz;
            *(void **)(teb + 0x10) = lo;
        }
        pthread_attr_destroy(&attr);
    }
    *(void **)(teb + 0x30) = teb;                              /* Self */
    *(uint64_t *)(teb + 0x40) = (uint64_t)getpid();            /* ClientId.UniqueProcess */
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    *(uint64_t *)(teb + 0x48) = tid;                           /* ClientId.UniqueThread */
    *(void **)(teb + 0x60) = lsiw_peb();
    struct lsiw_thread_priv *priv = (struct lsiw_thread_priv *)(teb + TEB_PRIV);
    priv->tid = tid;

    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)teb) != 0) {
        munmap(teb, TEB_SIZE);
        return NULL;
    }
    t_teb = teb;
    /* Implicit TLS for every loaded module (ThreadLocalStoragePointer). */
    *(void ***)(teb + 0x58) = lsiw_tls_build_array();
    return teb;
}

void *lsiw_teb_current(void) { return t_teb; }

struct lsiw_thread_priv *lsiw_thread(void)
{
    static struct lsiw_thread_priv fallback;
    return t_teb ? (struct lsiw_thread_priv *)(t_teb + TEB_PRIV) : &fallback;
}

uint32_t lsiw_get_last_error(void)
{
    return t_teb ? *(uint32_t *)(t_teb + 0x68) : 0;
}

void lsiw_set_last_error(uint32_t v)
{
    if (t_teb) *(uint32_t *)(t_teb + 0x68) = v;
}

uint32_t lsiw_errno_to_win(int err)
{
    switch (err) {
    case 0:            return ERROR_SUCCESS;
    case ENOENT:       return ERROR_FILE_NOT_FOUND;
    case ENOTDIR:      return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EISDIR:       return ERROR_ACCESS_DENIED;
    case EEXIST:       return ERROR_FILE_EXISTS;
    case EBADF:        return ERROR_INVALID_HANDLE;
    case ENOMEM:       return ERROR_NOT_ENOUGH_MEMORY;
    case EINVAL:       return ERROR_INVALID_PARAMETER;
    case EMFILE:
    case ENFILE:       return ERROR_TOO_MANY_OPEN_FILES;
    case ENOSPC:       return ERROR_DISK_FULL;
    case ENOTEMPTY:    return ERROR_DIR_NOT_EMPTY;
    case EXDEV:        return ERROR_NOT_SAME_DEVICE;
    case EROFS:        return ERROR_WRITE_PROTECT;
    case EBUSY:
    case ETXTBSY:      return ERROR_SHARING_VIOLATION;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case EPIPE:        return ERROR_BROKEN_PIPE;
    case ENOSYS:       return ERROR_CALL_NOT_IMPLEMENTED;
    case ETIMEDOUT:    return ERROR_TIMEOUT;
    default:           return ERROR_INVALID_FUNCTION;
    }
}

void lsiw_set_errno_error(int err)
{
    lsiw_set_last_error(lsiw_errno_to_win(err));
}

/* ------------------------------------------------------------------ */
/* PEB.ProcessParameters and PEB.Ldr                                    */
/* ------------------------------------------------------------------ */

/* Some programs and runtimes read the process parameters (command line,
 * standard handles, current directory) and the loader's module lists
 * straight from the PEB instead of calling kernel32. Both are filled in
 * once the executable and its static imports are loaded; modules loaded
 * later are appended to the lists. */

struct ustr { uint16_t len, max; uint32_t pad; WCHAR *buf; };
struct list_entry { struct list_entry *flink, *blink; };

static void set_ustr(struct ustr *u, const WCHAR *s)
{
    size_t n = s ? lsiw_wcslen(s) : 0;
    WCHAR *copy = calloc(n + 1, sizeof(WCHAR));
    if (n) memcpy(copy, s, n * sizeof(WCHAR));
    u->len = (uint16_t)(n * 2 > 0xfffc ? 0xfffc : n * 2);
    u->max = (uint16_t)(u->len + 2);
    u->buf = copy;
}

static void set_ustr_utf8(struct ustr *u, const char *s)
{
    WCHAR *w = lsiw_u2w(s ? s : "");
    set_ustr(u, w);
    free(w);
}

static void list_append(struct list_entry *head, struct list_entry *e)
{
    e->flink = head;
    e->blink = head->blink;
    head->blink->flink = e;
    head->blink = e;
}

/* PEB_LDR_DATA: Length, Initialized, SsHandle, then the three lists. */
static uint8_t *g_ldr;
static pthread_mutex_t g_ldr_lock = PTHREAD_MUTEX_INITIALIZER;

void lsiw_peb_add_module(struct lsiw_module *m)
{
    if (!g_ldr) return;
    /* LDR_DATA_TABLE_ENTRY (x64): the three links, DllBase@0x30,
     * EntryPoint@0x38, SizeOfImage@0x40, FullDllName@0x48,
     * BaseDllName@0x58, Flags@0x68, LoadCount@0x6c, TlsIndex@0x6e,
     * HashLinks@0x70, TimeDateStamp@0x80. */
    uint8_t *e = calloc(1, 0x120);
    *(void **)(e + 0x30) = m->base;
    *(void **)(e + 0x38) = m->entry;
    *(uint32_t *)(e + 0x40) = (uint32_t)m->size;
    set_ustr_utf8((struct ustr *)(e + 0x48), m->win_path ? m->win_path : m->name);
    set_ustr_utf8((struct ustr *)(e + 0x58), m->name);
    *(uint32_t *)(e + 0x68) = m->is_exe ? 0x4000 : 0x80004;   /* ENTRY_PROCESSED (+ IMAGE_DLL) */
    *(uint16_t *)(e + 0x6c) = 0xffff;
    struct list_entry *hl = (struct list_entry *)(e + 0x70);
    hl->flink = hl->blink = hl;
    pthread_mutex_lock(&g_ldr_lock);
    list_append((struct list_entry *)(g_ldr + 0x10), (struct list_entry *)(e + 0x00));
    list_append((struct list_entry *)(g_ldr + 0x20), (struct list_entry *)(e + 0x10));
    if (!m->is_exe) list_append((struct list_entry *)(g_ldr + 0x30), (struct list_entry *)(e + 0x20));
    else ((struct list_entry *)(e + 0x20))->flink = ((struct list_entry *)(e + 0x20))->blink = (struct list_entry *)(e + 0x20);
    pthread_mutex_unlock(&g_ldr_lock);
}

typedef HANDLE (WINAPI *get_std_handle_fn)(DWORD);
typedef DWORD (WINAPI *get_cur_dir_fn)(DWORD, WCHAR *);
typedef WCHAR *(WINAPI *get_env_fn)(void);

void lsiw_peb_init_process(struct lsiw_module *exe)
{
    uint8_t *peb = lsiw_peb();
    const struct lsiw_builtin_dll *k32 = lsiw_builtin_find("kernel32.dll");
    lsiw_kuser_init();

    /* RTL_USER_PROCESS_PARAMETERS (x64 layout). */
    uint8_t *pp = calloc(1, 0x440);
    *(uint32_t *)(pp + 0x00) = 0x440;                         /* MaximumLength */
    *(uint32_t *)(pp + 0x04) = 0x440;                         /* Length */
    *(uint32_t *)(pp + 0x08) = 1;                             /* PARAMS_NORMALIZED */
    get_std_handle_fn gsh = k32 ? (get_std_handle_fn)lsiw_builtin_lookup(k32, "GetStdHandle") : NULL;
    if (gsh) {
        *(HANDLE *)(pp + 0x20) = gsh((DWORD)-10);
        *(HANDLE *)(pp + 0x28) = gsh((DWORD)-11);
        *(HANDLE *)(pp + 0x30) = gsh((DWORD)-12);
    }
    get_cur_dir_fn gcd = k32 ? (get_cur_dir_fn)lsiw_builtin_lookup(k32, "GetCurrentDirectoryW") : NULL;
    WCHAR cwd[1024] = { 0 };
    if (gcd) {
        DWORD n = gcd(1023, cwd);
        if (n && n < 1022 && cwd[n - 1] != '\\') { cwd[n] = '\\'; cwd[n + 1] = 0; }
    }
    set_ustr((struct ustr *)(pp + 0x38), cwd);                /* CurrentDirectory.DosPath */
    set_ustr((struct ustr *)(pp + 0x50), NULL);               /* DllPath */
    set_ustr_utf8((struct ustr *)(pp + 0x60), exe->win_path); /* ImagePathName */
    set_ustr((struct ustr *)(pp + 0x70), lsiw_cmdline_w());   /* CommandLine */
    get_env_fn gev = k32 ? (get_env_fn)lsiw_builtin_lookup(k32, "GetEnvironmentStringsW") : NULL;
    WCHAR *env = gev ? gev() : NULL;
    *(WCHAR **)(pp + 0x80) = env;                             /* Environment */
    if (env) {
        const WCHAR *p = env;
        while (*p) p += lsiw_wcslen(p) + 1;
        *(uint64_t *)(pp + 0x3f0) = (uint64_t)((p + 1 - env) * sizeof(WCHAR));   /* EnvironmentSize */
    }
    *(uint32_t *)(pp + 0xa8) = 1;                             /* ShowWindowFlags = SW_SHOWNORMAL */
    set_ustr_utf8((struct ustr *)(pp + 0xb0), exe->win_path); /* WindowTitle */
    set_ustr((struct ustr *)(pp + 0xc0), NULL);               /* DesktopInfo */
    set_ustr((struct ustr *)(pp + 0xd0), NULL);               /* ShellInfo */
    set_ustr((struct ustr *)(pp + 0xe0), NULL);               /* RuntimeData */
    *(void **)(peb + 0x20) = pp;
    *(uint32_t *)(peb + 0x2c0) = 1;                           /* SessionId */

    /* PEB_LDR_DATA with the executable first, then the other modules in
     * the order they were loaded. */
    g_ldr = calloc(1, 0x58);
    *(uint32_t *)g_ldr = 0x58;
    g_ldr[4] = 1;                                             /* Initialized */
    for (int off = 0x10; off <= 0x30; off += 0x10) {
        struct list_entry *h = (struct list_entry *)(g_ldr + off);
        h->flink = h->blink = h;
    }
    *(void **)(peb + 0x18) = g_ldr;
    struct lsiw_module *order[512];
    int n = 0;
    for (struct lsiw_module *m = lsiw_modules; m && n < 512; m = m->next) order[n++] = m;
    lsiw_peb_add_module(exe);
    for (int i = n - 1; i >= 0; i--) if (order[i] != exe) lsiw_peb_add_module(order[i]);
}
