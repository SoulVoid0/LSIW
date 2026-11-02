/* LSIW native PE32+ (x86-64) loader.
 *
 * Maps Windows executables and DLLs into memory, applies base relocations,
 * resolves imports against LSIW's own Win32 implementations (or against
 * other PE DLLs shipped with the program), sets up implicit TLS, runs TLS
 * callbacks and DllMain, and finally jumps to the program's entry point.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include "pe.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3

struct lsiw_module *lsiw_modules;
struct lsiw_module *lsiw_main_module;
static int g_tls_count;

typedef BOOL (WINAPI *dllmain_fn)(HMODULE, DWORD, LPVOID);
typedef void (WINAPI *tls_cb_fn)(HMODULE, DWORD, LPVOID);
typedef int  (WINAPI *exe_entry_fn)(void *peb);

/* DLLs LSIW always provides itself, even if a copy sits next to the .exe. */
static int is_core_dll(const char *lname)
{
    static const char *core[] = {
        "kernel32.dll", "kernelbase.dll", "ntdll.dll", "msvcrt.dll", "ucrtbase.dll",
        "user32.dll", "advapi32.dll", "shell32.dll", NULL
    };
    if (!strncmp(lname, "api-ms-win-", 11) || !strncmp(lname, "ext-ms-win-", 11)) return 1;
    for (int i = 0; core[i]; i++) if (!strcmp(lname, core[i])) return 1;
    return 0;
}

static void lower_name(const char *in, char *out, size_t n)
{
    const char *b = in;
    for (const char *p = in; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    size_t i = 0;
    for (; b[i] && i + 1 < n; i++) out[i] = (char)tolower((unsigned char)b[i]);
    out[i] = 0;
    if (!strchr(out, '.') && i + 4 < n) strcat(out, ".dll");
}

/* ------------------------------------------------------------------ */
/* Unimplemented-function stubs                                         */
/* ------------------------------------------------------------------ */

static void WINAPI unimplemented_called(const char *what)
{
    fprintf(stderr, "lsiw: the program called %s, which the LSIW native engine does not implement.\n"
                    "lsiw: run it with `lsiw --engine=wine` to use the Wine fallback.\n", what);
    fflush(stderr);
    _exit(127);
}

void lsiw_unimplemented(const char *what) { unimplemented_called(what); }

/* A tiny per-symbol trampoline: mov rcx, <name>; mov rax, <reporter>; jmp rax */
static void *make_stub(const char *dll, const char *sym)
{
    static uint8_t *page; static size_t used = 4096;
    if (used + 32 > 4096) {
        page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return (void *)unimplemented_called;
        used = 0;
    }
    char *what = lsiw_strdup_printf("%s!%s", dll, sym);
    uint8_t *c = page + used;
    c[0] = 0x48; c[1] = 0xB9; memcpy(c + 2, &what, 8);                 /* mov rcx, imm64 */
    void *fn = (void *)unimplemented_called;
    c[10] = 0x48; c[11] = 0xB8; memcpy(c + 12, &fn, 8);                /* mov rax, imm64 */
    c[20] = 0xFF; c[21] = 0xE0;                                       /* jmp rax */
    used += 32;
    return c;
}

/* LSIW_APITRACE=FILE (or 1 for stderr) logs every call a program makes into
 * LSIW's built-in DLLs: each imported function goes through a small thunk
 * that saves the argument registers, logs the name and jumps on. */
static int g_trace_fd = -2;
static void WINAPI trace_call(const char *what)
{
    char line[160];
    int n = snprintf(line, sizeof line, "%d %s\n", (int)syscall(SYS_gettid), what);
    if (n > 0 && write(g_trace_fd, line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1)) < 0) {}
}

static int trace_on(void)
{
    if (g_trace_fd == -2) {
        const char *t = getenv("LSIW_APITRACE");
        if (!t || !*t) g_trace_fd = -1;
        else if (!strcmp(t, "1")) g_trace_fd = 2;
        else g_trace_fd = open(t, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    return g_trace_fd >= 0;
}

extern char __executable_start[], etext[];

static void *trace_thunk(const char *dll, const char *sym, void *fn)
{
    /* Only functions: data exports (_pgmptr, _iob...) live outside .text. */
    if ((char *)fn < __executable_start || (char *)fn >= etext) return fn;
    static uint8_t *page; static size_t used = 4096;
    if (used + 128 > 4096) {
        page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) { page = NULL; used = 4096; return fn; }
        used = 0;
    }
    char *what = lsiw_strdup_printf("%s!%s", dll, sym);
    void *log = (void *)trace_call;
    static const uint8_t pre[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51,                 /* push rcx, rdx, r8, r9 */
        0x48, 0x83, 0xEC, 0x68,                             /* sub rsp, 0x68 */
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x28,                 /* movdqu [rsp+0x28], xmm0 */
        0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x38,                 /* movdqu [rsp+0x38], xmm1 */
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x48,                 /* movdqu [rsp+0x48], xmm2 */
        0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x58 };               /* movdqu [rsp+0x58], xmm3 */
    static const uint8_t post[] = {
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x28,                 /* movdqu xmm0, [rsp+0x28] */
        0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x38,
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x48,
        0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x58,
        0x48, 0x83, 0xC4, 0x68,                             /* add rsp, 0x68 */
        0x41, 0x59, 0x41, 0x58, 0x5A, 0x59 };               /* pop r9, r8, rdx, rcx */
    uint8_t *c = page + used, *o = c;
    memcpy(o, pre, sizeof pre); o += sizeof pre;
    *o++ = 0x48; *o++ = 0xB9; memcpy(o, &what, 8); o += 8;  /* mov rcx, what */
    *o++ = 0x48; *o++ = 0xB8; memcpy(o, &log, 8); o += 8;   /* mov rax, trace_call */
    *o++ = 0xFF; *o++ = 0xD0;                               /* call rax */
    memcpy(o, post, sizeof post); o += sizeof post;
    *o++ = 0x48; *o++ = 0xB8; memcpy(o, &fn, 8); o += 8;    /* mov rax, fn */
    *o++ = 0xFF; *o++ = 0xE0;                               /* jmp rax */
    used += 128;
    return c;
}

/* GetProcAddress results from built-in DLLs, traced the same way. */
void *lsiw_trace_wrap(const char *dll, const char *sym, void *fn)
{
    if (!trace_on()) return fn;
    if (!fn) {
        char what[200];
        snprintf(what, sizeof what, "%s!%s -> NOT FOUND", dll, sym);
        trace_call(what);
        return fn;
    }
    return trace_thunk(dll, sym, fn);
}

/* ------------------------------------------------------------------ */
/* Lookups                                                              */
/* ------------------------------------------------------------------ */

struct lsiw_module *lsiw_find_module(const char *name)
{
    char lname[260];
    lower_name(name, lname, sizeof lname);
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next)
        if (!strcmp(m->name, lname)) return m;
    return NULL;
}

struct lsiw_module *lsiw_module_from_handle(HMODULE h)
{
    if (!h) return lsiw_main_module;
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next)
        if ((void *)m->base == h) return m;
    return NULL;
}

struct lsiw_module *lsiw_module_from_address(const void *addr)
{
    const uint8_t *a = addr;
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next)
        if (a >= m->base && a < m->base + m->size) return m;
    return NULL;
}

static IMAGE_NT_HEADERS64 *nt_of(uint8_t *base)
{
    return (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
}

void *lsiw_get_export(struct lsiw_module *m, const char *name, int ordinal)
{
    if (!m) return NULL;
    if (m->is_builtin) {
        if (!name) return NULL;
        return lsiw_builtin_lookup(m->builtin, name);
    }
    IMAGE_NT_HEADERS64 *nt = nt_of(m->base);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress) return NULL;
    IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)(m->base + dd.VirtualAddress);
    uint32_t *funcs = (uint32_t *)(m->base + ed->AddressOfFunctions);
    uint32_t *names = (uint32_t *)(m->base + ed->AddressOfNames);
    uint16_t *ords  = (uint16_t *)(m->base + ed->AddressOfNameOrdinals);
    long idx = -1;
    if (name) {
        long lo = 0, hi = (long)ed->NumberOfNames - 1;
        while (lo <= hi) {                         /* names are sorted */
            long mid = (lo + hi) / 2;
            int c = strcmp(name, (const char *)(m->base + names[mid]));
            if (!c) { idx = ords[mid]; break; }
            if (c < 0) hi = mid - 1; else lo = mid + 1;
        }
        if (idx < 0)                               /* tolerate unsorted tables */
            for (uint32_t i = 0; i < ed->NumberOfNames; i++)
                if (!strcmp(name, (const char *)(m->base + names[i]))) { idx = ords[i]; break; }
    } else {
        idx = ordinal - (long)ed->Base;
    }
    if (idx < 0 || (uint32_t)idx >= ed->NumberOfFunctions || !funcs[idx]) return NULL;
    uint32_t rva = funcs[idx];
    if (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size) {
        /* Forwarder: "OTHERDLL.Function" or "OTHERDLL.#12" */
        const char *fwd = (const char *)(m->base + rva);
        const char *dot = strrchr(fwd, '.');
        if (!dot) return NULL;
        char dll[260];
        snprintf(dll, sizeof dll, "%.*s", (int)(dot - fwd), fwd);
        struct lsiw_module *t = lsiw_load_library(dll, NULL);
        if (!t) return NULL;
        if (dot[1] == '#') return lsiw_get_export(t, NULL, atoi(dot + 2));
        return lsiw_get_export(t, dot + 1, 0);
    }
    return m->base + rva;
}

/* Wine fills its prefix's system32 (which is also C:\windows\system32
 * here) with its own PE DLLs. They only work on top of Wine's ntdll, so the
 * native engine must not load them: they carry "Wine builtin DLL" or
 * "Wine placeholder DLL" in the DOS stub at offset 0x40. */
static int is_wine_dll(const char *path)
{
    char sig[20] = { 0 };
    FILE *f = fopen(path, "rbe");
    if (!f) return 0;
    int ok = fseek(f, 0x40, SEEK_SET) == 0 && fread(sig, 1, sizeof sig - 1, f) == sizeof sig - 1;
    fclose(f);
    return ok && !strncmp(sig, "Wine ", 5) && (strstr(sig, "builtin") || strstr(sig, "placeholder"));
}

/* Case-insensitive search for `name` inside `dir`. Caller frees. */
static char *find_in_dir(const char *dir, const char *name)
{
    if (!dir) return NULL;
    char *p = lsiw_strdup_printf("%s/%s", dir, name);
    struct stat st;
    if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
        if (!is_wine_dll(p)) return p;
        free(p);
        return NULL;
    }
    free(p);
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *de;
    char *res = NULL;
    while ((de = readdir(d)))
        if (!strcasecmp(de->d_name, name)) { res = lsiw_strdup_printf("%s/%s", dir, de->d_name); break; }
    closedir(d);
    if (res && is_wine_dll(res)) { free(res); res = NULL; }
    return res;
}

static char *dirname_of(const char *path)
{
    char *d = strdup(path);
    char *s = strrchr(d, '/');
    if (s && s != d) *s = 0; else strcpy(d, "/");
    return d;
}

/* Search order for a PE DLL: the program's folder, the current folder,
 * C:\windows\system32, then each folder in the Windows PATH. */
static char *search_dll(const char *lname, const char *from_dir)
{
    char *r = NULL;
    if (from_dir && (r = find_in_dir(from_dir, lname))) return r;
    if (lsiw_main_module && lsiw_main_module->linux_path) {
        char *d = dirname_of(lsiw_main_module->linux_path);
        r = find_in_dir(d, lname);
        free(d);
        if (r) return r;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) && (r = find_in_dir(cwd, lname))) return r;
    if ((r = find_in_dir(lsiw_var_linux("SYSTEM32"), lname))) return r;
    const char *path = lsiw_env_get("PATH");
    if (path) {
        char *copy = strdup(path), *save = NULL;
        for (char *e = strtok_r(copy, ";", &save); e && !r; e = strtok_r(NULL, ";", &save)) {
            if (!*e) continue;
            char *lin = lsiw_win_to_linux(e);
            r = find_in_dir(lin, lname);
            free(lin);
        }
        free(copy);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Builtin modules                                                      */
/* ------------------------------------------------------------------ */

static struct lsiw_module *builtin_module(const struct lsiw_builtin_dll *b, const char *lname)
{
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next)
        if (m->is_builtin && m->builtin == b && !strcmp(m->name, lname)) return m;
    struct lsiw_module *m = calloc(1, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", lname);
    m->is_builtin = 1;
    m->builtin = b;
    m->base = lsiw_builtin_handle(b);
    m->size = 0x1000;
    m->tls_index = -1;
    const char *sys = lsiw_var_windows("SYSTEM32");
    m->win_path = lsiw_strdup_printf("%s\\%s", sys ? sys : "C:\\windows\\system32", lname);
    m->next = lsiw_modules;
    lsiw_modules = m;
    lsiw_peb_add_module(m);
    return m;
}

/* ------------------------------------------------------------------ */
/* TLS                                                                  */
/* ------------------------------------------------------------------ */

void **lsiw_tls_build_array(void)
{
    int n = g_tls_count > 64 ? g_tls_count : 64;
    void **arr = calloc((size_t)n, sizeof(void *));
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next) {
        if (m->tls_index < 0) continue;
        uint8_t *blk = calloc(1, m->tls_template_size + m->tls_zero_fill + 16);
        memcpy(blk, m->tls_template, m->tls_template_size);
        arr[m->tls_index] = blk;
    }
    return arr;
}

static void tls_attach_current(struct lsiw_module *m)
{
    uint8_t *teb = lsiw_teb_current();
    if (!teb) return;
    void **arr = *(void ***)(teb + 0x58);
    if (!arr || m->tls_index >= 64) {
        *(void ***)(teb + 0x58) = lsiw_tls_build_array();
        return;
    }
    uint8_t *blk = calloc(1, m->tls_template_size + m->tls_zero_fill + 16);
    memcpy(blk, m->tls_template, m->tls_template_size);
    arr[m->tls_index] = blk;
}

static void call_tls_callbacks_of(struct lsiw_module *m, uint32_t reason)
{
    if (!m->tls_callbacks) return;
    for (uint64_t *cb = m->tls_callbacks; *cb; cb++)
        ((tls_cb_fn)(uintptr_t)*cb)((HMODULE)m->base, reason, NULL);
}

void lsiw_call_tls_callbacks(uint32_t reason)
{
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next)
        if (!m->is_builtin) call_tls_callbacks_of(m, reason);
}

static int g_no_thread_calls[256];
static void *g_no_thread_mods[256];
static int g_no_thread_n;

void lsiw_disable_thread_calls(HMODULE h)
{
    if (g_no_thread_n < 256) { g_no_thread_mods[g_no_thread_n] = h; g_no_thread_calls[g_no_thread_n++] = 1; }
}

void lsiw_dll_notify_threads(uint32_t reason)
{
    /* Initialisation order (dependencies first) is the reverse of the list. */
    struct lsiw_module *order[512]; int n = 0;
    for (struct lsiw_module *m = lsiw_modules; m && n < 512; m = m->next) order[n++] = m;
    for (int i = n - 1; i >= 0; i--) {
        struct lsiw_module *m = order[i];
        if (m->is_builtin) continue;
        call_tls_callbacks_of(m, reason);
        if (m->is_exe || !m->entry) continue;
        int skip = 0;
        for (int k = 0; k < g_no_thread_n; k++) if (g_no_thread_mods[k] == m->base) skip = 1;
        if (!skip) ((dllmain_fn)m->entry)((HMODULE)m->base, reason, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Mapping                                                              */
/* ------------------------------------------------------------------ */

static int section_prot(uint32_t ch)
{
    int p = 0;
    if (ch & IMAGE_SCN_MEM_READ)    p |= PROT_READ;
    if (ch & IMAGE_SCN_MEM_WRITE)   p |= PROT_WRITE;
    if (ch & IMAGE_SCN_MEM_EXECUTE) p |= PROT_EXEC | PROT_READ;
    return p ? p : PROT_READ;
}

static int apply_relocations(uint8_t *base, IMAGE_NT_HEADERS64 *nt, int64_t delta)
{
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!delta) return 0;
    if (!d.VirtualAddress || !d.Size) return -1;
    uint8_t *p = base + d.VirtualAddress, *end = p + d.Size;
    while (p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
        IMAGE_BASE_RELOCATION *blk = (IMAGE_BASE_RELOCATION *)p;
        if (blk->SizeOfBlock < sizeof *blk) break;
        uint32_t cnt = (blk->SizeOfBlock - sizeof *blk) / 2;
        uint16_t *rel = (uint16_t *)(p + sizeof *blk);
        for (uint32_t i = 0; i < cnt; i++) {
            int type = rel[i] >> 12, off = rel[i] & 0xfff;
            uint8_t *slot = base + blk->VirtualAddress + off;
            if (type == IMAGE_REL_BASED_DIR64) *(uint64_t *)slot += (uint64_t)delta;
            else if (type == IMAGE_REL_BASED_HIGHLOW) *(uint32_t *)slot += (uint32_t)delta;
        }
        p += blk->SizeOfBlock;
    }
    return 0;
}

static int resolve_imports(struct lsiw_module *m, IMAGE_NT_HEADERS64 *nt, char **why)
{
    uint8_t *base = m->base;
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d.VirtualAddress) return 0;
    char *dir = m->linux_path ? dirname_of(m->linux_path) : NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + d.VirtualAddress);
         imp->Name; imp++) {
        const char *dllname = (const char *)(base + imp->Name);
        struct lsiw_module *dep = lsiw_load_library(dllname, dir);
        char lname[260];
        lower_name(dllname, lname, sizeof lname);
        if (!dep) {
            LSIW_TRACE("%s: cannot load dependency %s\n", m->name, dllname);
            if (why && !*why) *why = lsiw_strdup_printf("missing DLL %s", lname);
        }
        uint64_t *oft = (uint64_t *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        uint64_t *iat = (uint64_t *)(base + imp->FirstThunk);
        for (; *oft; oft++, iat++) {
            void *fn = NULL;
            char symbuf[64];
            const char *sym;
            if (*oft & IMAGE_ORDINAL_FLAG64) {
                int ord = (int)(*oft & 0xffff);
                snprintf(symbuf, sizeof symbuf, "#%d", ord);
                sym = symbuf;
                if (dep) fn = dep->is_builtin ? lsiw_builtin_lookup(dep->builtin, sym)
                                              : lsiw_get_export(dep, NULL, ord);
            } else {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + (uint32_t)*oft);
                sym = (const char *)ibn->Name;
                if (dep) fn = lsiw_get_export(dep, sym, 0);
            }
            if (!fn) {
                LSIW_TRACE("%s: unresolved import %s!%s\n", m->name, lname, sym);
                fn = make_stub(lname, sym);
            }
            else if (dep && dep->is_builtin && trace_on()) fn = trace_thunk(lname, sym, fn);
            *iat = (uint64_t)(uintptr_t)fn;
        }
    }
    free(dir);
    return 0;
}

struct lsiw_module *lsiw_load_pe(const char *path, int is_exe, char **why)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (why) *why = lsiw_strdup_printf("%s: %s", path, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(IMAGE_DOS_HEADER)) {
        close(fd);
        if (why) *why = lsiw_strdup_printf("%s: not a Windows program", path);
        return NULL;
    }
    uint8_t *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) { if (why) *why = lsiw_strdup_printf("%s: %s", path, strerror(errno)); return NULL; }

#define FAIL(...) do { if (why) *why = lsiw_strdup_printf(__VA_ARGS__); munmap(file, (size_t)st.st_size); return NULL; } while (0)
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)file;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        (off_t)dos->e_lfanew + (off_t)sizeof(IMAGE_NT_HEADERS64) > st.st_size)
        FAIL("%s: not a Windows program", path);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(file + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) FAIL("%s: MS-DOS program (not supported)", path);
    if (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386)
        FAIL("%s: 32-bit program (the native engine runs 64-bit programs)", path);
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        FAIL("%s: unsupported CPU type 0x%x", path, nt->FileHeader.Machine);

    size_t size = nt->OptionalHeader.SizeOfImage;
    uint64_t pref = nt->OptionalHeader.ImageBase;
    uint8_t *base = mmap((void *)(uintptr_t)pref, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (base == MAP_FAILED || base != (uint8_t *)(uintptr_t)pref) {
        if (base != MAP_FAILED) munmap(base, size);
        base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) FAIL("%s: out of memory", path);
    }
    size_t hdr = nt->OptionalHeader.SizeOfHeaders;
    memcpy(base, file, hdr < (size_t)st.st_size ? hdr : (size_t)st.st_size);
    IMAGE_SECTION_HEADER *sec = (IMAGE_SECTION_HEADER *)((uint8_t *)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        size_t raw = sec[i].SizeOfRawData;
        if (sec[i].VirtualSize && raw > sec[i].VirtualSize) raw = sec[i].VirtualSize;
        if (raw && sec[i].PointerToRawData + raw <= (size_t)st.st_size &&
            sec[i].VirtualAddress + raw <= size)
            memcpy(base + sec[i].VirtualAddress, file + sec[i].PointerToRawData, raw);
    }
    munmap(file, (size_t)st.st_size);
#undef FAIL

    nt = nt_of(base);
    sec = (IMAGE_SECTION_HEADER *)((uint8_t *)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
    int64_t delta = (int64_t)((uint64_t)(uintptr_t)base - pref);
    if (apply_relocations(base, nt, delta) < 0) {
        munmap(base, size);
        if (why) *why = lsiw_strdup_printf("%s: cannot be relocated and its preferred address is taken", path);
        return NULL;
    }
    nt->OptionalHeader.ImageBase = (uint64_t)(uintptr_t)base;

    struct lsiw_module *m = calloc(1, sizeof *m);
    lower_name(path, m->name, sizeof m->name);
    m->linux_path = realpath(path, NULL);
    if (!m->linux_path) m->linux_path = strdup(path);
    m->win_path = lsiw_linux_to_win(m->linux_path);
    m->base = base;
    m->size = size;
    m->is_exe = is_exe;
    m->tls_index = -1;
    if (nt->OptionalHeader.AddressOfEntryPoint)
        m->entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    /* Register before resolving imports so circular imports terminate. */
    m->next = lsiw_modules;
    lsiw_modules = m;
    lsiw_peb_add_module(m);
    if (is_exe) {
        lsiw_main_module = m;
        lsiw_crt_set_program(m->win_path);
        *(void **)((uint8_t *)lsiw_peb() + 0x10) = base;    /* PEB.ImageBaseAddress */
    }

    resolve_imports(m, nt, NULL);

    /* Implicit TLS. */
    IMAGE_DATA_DIRECTORY td = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (td.VirtualAddress) {
        IMAGE_TLS_DIRECTORY64 *tls = (IMAGE_TLS_DIRECTORY64 *)(base + td.VirtualAddress);
        m->tls_index = g_tls_count++;
        m->tls_template = (void *)(uintptr_t)tls->StartAddressOfRawData;
        m->tls_template_size = (size_t)(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
        m->tls_zero_fill = tls->SizeOfZeroFill;
        if (tls->AddressOfIndex) *(uint32_t *)(uintptr_t)tls->AddressOfIndex = (uint32_t)m->tls_index;
        if (tls->AddressOfCallBacks) m->tls_callbacks = (uint64_t *)(uintptr_t)tls->AddressOfCallBacks;
        tls_attach_current(m);
    }

    /* Final page protections. */
    mprotect(base, (hdr + 0xfff) & ~(size_t)0xfff, PROT_READ);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uintptr_t a = (uintptr_t)(base + sec[i].VirtualAddress);
        size_t len = sec[i].VirtualSize ? sec[i].VirtualSize : sec[i].SizeOfRawData;
        if (!len) continue;
        uintptr_t pg = a & ~(uintptr_t)0xfff;
        mprotect((void *)pg, ((a + len + 0xfff) & ~(uintptr_t)0xfff) - pg, section_prot(sec[i].Characteristics));
    }

    LSIW_TRACE("loaded %s at %p (preferred 0x%llx)\n", m->name, (void *)base, (unsigned long long)pref);

    if (!is_exe) {
        call_tls_callbacks_of(m, DLL_PROCESS_ATTACH);
        if (m->entry && !((dllmain_fn)m->entry)((HMODULE)base, DLL_PROCESS_ATTACH, NULL)) {
            LSIW_TRACE("%s: DllMain failed\n", m->name);
            if (why) *why = lsiw_strdup_printf("%s: DLL initialisation failed", m->name);
            return NULL;
        }
    }
    return m;
}

struct lsiw_module *lsiw_load_library(const char *name, const char *from_dir)
{
    char lname[260];
    lower_name(name, lname, sizeof lname);
    struct lsiw_module *m = lsiw_find_module(lname);
    if (m) return m;

    const struct lsiw_builtin_dll *b = lsiw_builtin_find(lname);
    if (b && is_core_dll(lname)) return builtin_module(b, lname);

    /* A path given explicitly (LoadLibrary("C:\\x\\y.dll")). */
    char *path = NULL;
    if (strchr(name, '\\') || strchr(name, '/')) {
        path = (name[0] == '/') ? strdup(name) : lsiw_win_to_linux(name);
        struct stat st;
        if (stat(path, &st) != 0) { free(path); path = NULL; }
    }
    if (!path) path = search_dll(lname, from_dir);
    if (path) {
        char *why = NULL;
        m = lsiw_load_pe(path, 0, &why);
        if (!m) { LSIW_TRACE("%s\n", why ? why : path); free(why); }
        free(path);
        if (m) return m;
    }
    if (b) return builtin_module(b, lname);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Running                                                              */
/* ------------------------------------------------------------------ */

int lsiw_run_exe(struct lsiw_module *exe)
{
    lsiw_peb_init_process(exe);
    call_tls_callbacks_of(exe, DLL_PROCESS_ATTACH);
    exe_entry_fn ep = (exe_entry_fn)exe->entry;
    LSIW_TRACE("entering %s at %p\n", exe->name, exe->entry);
    int r = ep(lsiw_peb());
    lsiw_exit_process((uint32_t)r);
}

void lsiw_process_detach(void)
{
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next) {
        if (m->is_builtin || m->is_exe) continue;
        call_tls_callbacks_of(m, DLL_PROCESS_DETACH);
        if (m->entry) ((dllmain_fn)m->entry)((HMODULE)m->base, DLL_PROCESS_DETACH, (void *)1);
    }
}

/* ------------------------------------------------------------------ */
/* Static check: can this program run on the native engine?             */
/* ------------------------------------------------------------------ */

struct check_ctx {
    char  *report;
    size_t len;
    int    missing;
    char  *visited[256];
    int    nvisited;
};

static void report_add(struct check_ctx *c, const char *fmt, ...)
{
    va_list ap;
    char *s = NULL;
    va_start(ap, fmt);
    if (vasprintf(&s, fmt, ap) < 0) s = NULL;
    va_end(ap);
    if (!s) return;
    size_t l = strlen(s);
    c->report = realloc(c->report, c->len + l + 2);
    memcpy(c->report + c->len, s, l);
    c->len += l;
    c->report[c->len++] = '\n';
    c->report[c->len] = 0;
    free(s);
}

static uint8_t *rva_ptr(uint8_t *file, size_t fsize, IMAGE_NT_HEADERS64 *nt, uint32_t rva)
{
    IMAGE_SECTION_HEADER *sec = (IMAGE_SECTION_HEADER *)((uint8_t *)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress;
        uint32_t vs = sec[i].VirtualSize > sec[i].SizeOfRawData ? sec[i].VirtualSize : sec[i].SizeOfRawData;
        if (rva >= va && rva < va + vs) {
            size_t off = sec[i].PointerToRawData + (rva - va);
            if (rva - va >= sec[i].SizeOfRawData || off >= fsize) return NULL;
            return file + off;
        }
    }
    return rva < fsize ? file + rva : NULL;
}

static int has_export_file(uint8_t *file, size_t fsize, IMAGE_NT_HEADERS64 *nt, const char *sym, int ord)
{
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress) return 0;
    IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)rva_ptr(file, fsize, nt, dd.VirtualAddress);
    if (!ed) return 0;
    if (!sym) return ord - (int)ed->Base >= 0 && (uint32_t)(ord - (int)ed->Base) < ed->NumberOfFunctions;
    uint32_t *names = (uint32_t *)rva_ptr(file, fsize, nt, ed->AddressOfNames);
    if (!names) return 0;
    for (uint32_t i = 0; i < ed->NumberOfNames; i++) {
        const char *n = (const char *)rva_ptr(file, fsize, nt, names[i]);
        if (n && !strcmp(n, sym)) return 1;
    }
    return 0;
}

static int check_file(struct check_ctx *c, const char *path, int top);

static void check_imports(struct check_ctx *c, uint8_t *file, size_t fsize, IMAGE_NT_HEADERS64 *nt,
                          const char *self_dir, const char *self_name)
{
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)rva_ptr(file, fsize, nt, d.VirtualAddress);
         imp && imp->Name; imp++) {
        const char *dllname = (const char *)rva_ptr(file, fsize, nt, imp->Name);
        if (!dllname) break;
        char lname[260];
        lower_name(dllname, lname, sizeof lname);
        const struct lsiw_builtin_dll *b = lsiw_builtin_find(lname);
        char *dllpath = NULL;
        if (!b || !is_core_dll(lname)) dllpath = search_dll(lname, self_dir);

        /* Load the dependency file (if it is a PE DLL) for export checks. */
        uint8_t *dfile = NULL; size_t dsize = 0; IMAGE_NT_HEADERS64 *dnt = NULL;
        if (dllpath) {
            if (check_file(c, dllpath, 0) == 0) {
                int fd = open(dllpath, O_RDONLY);
                struct stat st;
                if (fd >= 0 && fstat(fd, &st) == 0) {
                    dsize = (size_t)st.st_size;
                    dfile = mmap(NULL, dsize, PROT_READ, MAP_PRIVATE, fd, 0);
                    if (dfile == MAP_FAILED) dfile = NULL;
                    else dnt = (IMAGE_NT_HEADERS64 *)(dfile + ((IMAGE_DOS_HEADER *)dfile)->e_lfanew);
                }
                if (fd >= 0) close(fd);
            } else {
                free(dllpath);
                dllpath = NULL;
            }
        }
        if (!b && !dfile) {
            report_add(c, "%s needs %s, which is not available", self_name, lname);
            c->missing++;
            continue;
        }
        uint32_t t = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        uint64_t *oft = (uint64_t *)rva_ptr(file, fsize, nt, t);
        for (; oft && *oft; oft++) {
            const char *sym = NULL; int ord = 0;
            char symbuf[32];
            if (*oft & IMAGE_ORDINAL_FLAG64) { ord = (int)(*oft & 0xffff); snprintf(symbuf, sizeof symbuf, "#%d", ord); }
            else {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)rva_ptr(file, fsize, nt, (uint32_t)*oft);
                if (!ibn) break;
                sym = (const char *)ibn->Name;
            }
            int ok = dfile ? has_export_file(dfile, dsize, dnt, sym, ord)
                           : lsiw_builtin_lookup(b, sym ? sym : symbuf) != NULL;
            if (!ok) {
                report_add(c, "%s needs %s!%s (not implemented)", self_name, lname, sym ? sym : symbuf);
                c->missing++;
            }
        }
        if (dfile) munmap(dfile, dsize);
        free(dllpath);
    }
}

static int check_file(struct check_ctx *c, const char *path, int top)
{
    for (int i = 0; i < c->nvisited; i++) if (!strcmp(c->visited[i], path)) return 0;
    if (c->nvisited < 256) c->visited[c->nvisited++] = strdup(path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) { report_add(c, "%s: %s", path, strerror(errno)); c->missing++; return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0x200) { close(fd); report_add(c, "%s: not a Windows program", path); c->missing++; return -1; }
    size_t fsize = (size_t)st.st_size;
    uint8_t *file = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) { c->missing++; return -1; }

    int rc = 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)file;
    IMAGE_NT_HEADERS64 *nt = NULL;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > fsize ||
        (nt = (IMAGE_NT_HEADERS64 *)(file + dos->e_lfanew))->Signature != IMAGE_NT_SIGNATURE) {
        report_add(c, "%s: not a Windows PE program", path);
        c->missing++; rc = -1;
    } else if (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386) {
        report_add(c, "%s: 32-bit program (the native engine runs 64-bit programs only)", path);
        c->missing++; rc = -1;
    } else if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
               nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        report_add(c, "%s: unsupported CPU type 0x%x", path, nt->FileHeader.Machine);
        c->missing++; rc = -1;
    } else {
        if (top && nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress)
            report_add(c, "%s: .NET program (needs the .NET runtime)", path), c->missing++;
        char *dir = dirname_of(path);
        char lname[260];
        lower_name(path, lname, sizeof lname);
        check_imports(c, file, fsize, nt, dir, lname);
        free(dir);
    }
    munmap(file, fsize);
    return rc;
}

int lsiw_native_check(const char *path, char **report)
{
    struct check_ctx c = {0};
    int rc = check_file(&c, path, 1);
    for (int i = 0; i < c.nvisited; i++) free(c.visited[i]);
    if (report) *report = c.report; else free(c.report);
    if (rc < 0) return -1;              /* not a usable 64-bit PE */
    return c.missing ? 1 : 0;
}
