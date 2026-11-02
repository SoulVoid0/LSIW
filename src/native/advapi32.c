/* advapi32 for the LSIW native engine: the registry, users and security
 * (SIDs, access tokens, ACLs, security descriptors), CryptoAPI hashing and
 * random numbers, event logging and ETW, the service control manager, the
 * credential store and a few related odds and ends.
 *
 * builtins.c also routes kernelbase.dll, sechost.dll and the
 * api-ms-win-core-registry / -security / -eventing API sets here.
 *
 * The registry
 * ------------
 * HKEY_LOCAL_MACHINE and HKEY_CURRENT_USER are two hive files shared by
 * every LSIW process of the user:
 *     <lsiw_data_dir()>/registry/system.reg   HKEY_LOCAL_MACHINE
 *     <lsiw_data_dir()>/registry/user.reg     HKEY_CURRENT_USER
 * HKEY_USERS lists ".DEFAULT", the user's SID and "<SID>_Classes", which
 * are views of user.reg. HKEY_CLASSES_ROOT is the merged view Windows
 * gives: HKCU\Software\Classes over HKLM\Software\Classes. HKEY_CURRENT_CONFIG
 * is an empty in-memory hive. The performance keys are empty.
 *
 * Each hive is loaded lazily into an in-memory tree. Key and value names
 * compare case-insensitively (ASCII plus the common Latin, Greek and
 * Cyrillic letters). Value data is kept as the exact bytes the program
 * wrote; strings written by the W functions are UTF-16LE, the A functions
 * convert between the ANSI code page (UTF-8 in LSIW) and UTF-16 the way
 * Windows does. Before any access the hive file's identity (inode, size,
 * mtime) is compared with the version in memory and the file is re-read if
 * another process replaced it. Every modification takes an flock() on
 * "<hive>.lock", re-reads the file if needed, applies the change and
 * writes the whole hive to a temporary file that is rename()d over the old
 * one, so readers never see a half-written file. Keys created with
 * REG_OPTION_VOLATILE live only in the memory of the process that created
 * them (Windows keeps them system-wide until reboot).
 *
 * Some values are generated at load time (Windows version, folders from
 * the LSIW variable table, CPU, computer name...). They are not written to
 * the file unless a program changes them; a value in the file always wins.
 *
 * Hive file format (UTF-8 text, one item per line):
 *     LSIW-REGISTRY 1                      first line, identifies the format
 *     ; comment                            ignored, like blank lines
 *     ["Software\\Vendor\\App"] 01d9f3c2a7b4e000 class="..."
 *                                          a key: full path from the hive
 *                                          root ("" is the root itself), its
 *                                          last-write FILETIME in hex, and
 *                                          an optional class string
 *     @=sz:"text"                          the key's default (unnamed) value
 *     "Name"=sz:"text"                     REG_SZ
 *     "Name"=expand:"%SystemRoot%\\x"      REG_EXPAND_SZ
 *     "Name"=multi:"one\0two\0"            REG_MULTI_SZ
 *     "Name"=dword:0000002a                REG_DWORD (exactly 4 bytes)
 *     "Name"=qword:00000000deadbeef        REG_QWORD (exactly 8 bytes)
 *     "Name"=hex(3):00,ff,10               any type (hex), raw bytes
 * The string forms are used only when the data is an even number of bytes
 * ending in a UTF-16 NUL; the quoted text is the data without that final
 * NUL. Anything else is written as hex(type). Quoted strings escape \\ \"
 * \n \r \t \0, other control characters as \xHH and unpaired UTF-16
 * surrogates as \uXXXX; everything else is UTF-8. The round trip is exact.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native.h"
#include "hash.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ================================================================== */
/* Windows types and constants used in this file                        */
/* ================================================================== */

typedef void    *HKEY;
typedef LONG     LSTATUS;
typedef DWORD    REGSAM;
typedef void    *PSID;
typedef BYTE     BOOLEAN;
typedef uint16_t USHORT;
typedef uint8_t  UCHAR;
typedef DWORD    ACCESS_MASK;
typedef DWORD    SECURITY_INFORMATION;
typedef unsigned int ALG_ID;
typedef ULONG_PTR HCRYPTPROV, HCRYPTHASH, HCRYPTKEY;
typedef uint64_t REGHANDLE, TRACEHANDLE;

typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { DWORD LowPart; LONG HighPart; } LUID;
typedef struct { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID;

#define EPOCH_DIFF 116444736000000000ULL

#define ERROR_BAD_NETPATH                 53
#define ERROR_INVALID_LEVEL               124
#define ERROR_BUFFER_OVERFLOW             111
#define ERROR_MR_MID_NOT_FOUND            317
#define ERROR_NOACCESS                    998
#define ERROR_INVALID_FLAGS               1004
#define ERROR_NO_TOKEN                    1008
#define ERROR_BADKEY                      1010
#define ERROR_KEY_DELETED                 1018
#define ERROR_CHILD_MUST_BE_VOLATILE      1021
#define ERROR_INVALID_SERVICE_CONTROL     1052
#define ERROR_SERVICE_DOES_NOT_EXIST      1060
#define ERROR_FAILED_SERVICE_CONTROLLER_CONNECT 1063
#define ERROR_DATABASE_DOES_NOT_EXIST     1065
#define ERROR_SERVICE_NOT_IN_EXE          1083
#define ERROR_NO_SHUTDOWN_IN_PROGRESS     1116
#define ERROR_NOT_FOUND                   1168
#define ERROR_NOT_ALL_ASSIGNED            1300
#define ERROR_SOME_NOT_MAPPED             1301
#define ERROR_UNKNOWN_REVISION            1305
#define ERROR_REVISION_MISMATCH           1306
#define ERROR_NO_IMPERSONATION_TOKEN      1309
#define ERROR_NO_SUCH_LOGON_SESSION       1312
#define ERROR_NO_SUCH_PRIVILEGE           1313
#define ERROR_PRIVILEGE_NOT_HELD          1314
#define ERROR_LOGON_FAILURE               1326
#define ERROR_NONE_MAPPED                 1332
#define ERROR_INVALID_ACL                 1336
#define ERROR_INVALID_SID                 1337
#define ERROR_INVALID_SECURITY_DESCR      1338
#define ERROR_ALLOTTED_SPACE_EXCEEDED     1344
#define ERROR_BAD_IMPERSONATION_LEVEL     1346
#define ERROR_BAD_TOKEN_TYPE              1349
#define ERROR_BAD_DESCRIPTOR_FORMAT       1361
#define ERROR_INVALID_SERVICE_ACCOUNT     1057
#define ERROR_DATATYPE_MISMATCH           1629
#define ERROR_UNSUPPORTED_TYPE            1630
#define RPC_S_SERVER_UNAVAILABLE          1722

#define STATUS_SUCCESS                    0x00000000
#define STATUS_INVALID_HANDLE             ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER          ((NTSTATUS)0xC000000D)
#define STATUS_NO_MEMORY                  ((NTSTATUS)0xC0000017)
#define STATUS_ACCESS_DENIED              ((NTSTATUS)0xC0000022)
#define STATUS_BUFFER_TOO_SMALL           ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_NAME_NOT_FOUND      ((NTSTATUS)0xC0000034)
#define STATUS_NO_SUCH_PRIVILEGE          ((NTSTATUS)0xC0000060)
#define STATUS_PRIVILEGE_NOT_HELD         ((NTSTATUS)0xC0000061)
#define STATUS_NO_SUCH_USER               ((NTSTATUS)0xC0000064)
#define STATUS_WRONG_PASSWORD             ((NTSTATUS)0xC000006A)
#define STATUS_LOGON_FAILURE              ((NTSTATUS)0xC000006D)
#define STATUS_NONE_MAPPED                ((NTSTATUS)0xC0000073)
#define STATUS_INVALID_SID                ((NTSTATUS)0xC0000078)
#define STATUS_INSUFFICIENT_RESOURCES     ((NTSTATUS)0xC000009A)
#define STATUS_SOME_NOT_MAPPED            ((NTSTATUS)0x00000107)

#define GENERIC_READ     0x80000000u
#define GENERIC_WRITE    0x40000000u
#define GENERIC_EXECUTE  0x20000000u
#define GENERIC_ALL      0x10000000u
#define MAXIMUM_ALLOWED  0x02000000u
#define ACCESS_SYSTEM_SECURITY 0x01000000u
#define DELETE           0x00010000u
#define READ_CONTROL     0x00020000u
#define WRITE_DAC        0x00040000u
#define WRITE_OWNER      0x00080000u
#define SYNCHRONIZE      0x00100000u

/* ================================================================== */
/* Small helpers                                                        */
/* ================================================================== */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { lsiw_log("advapi32: out of memory\n"); abort(); }
    return p;
}

static void *xcalloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) { lsiw_log("advapi32: out of memory\n"); abort(); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { lsiw_log("advapi32: out of memory\n"); abort(); }
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(xmalloc(n), s, n);
}

static uint64_t now_filetime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100 + EPOCH_DIFF;
}

static void put_filetime(FILETIME *ft, uint64_t v)
{
    if (!ft) return;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

static WCHAR *wdup_n(const WCHAR *s, size_t n)
{
    WCHAR *d = xmalloc((n + 1) * sizeof(WCHAR));
    if (n) memcpy(d, s, n * sizeof(WCHAR));
    d[n] = 0;
    return d;
}

static WCHAR *wdup(const WCHAR *s) { return s ? wdup_n(s, lsiw_wcslen(s)) : NULL; }

/* ANSI (UTF-8 in LSIW) <-> UTF-16, NULL-safe, malloc'd. */
static WCHAR *a2w(const char *s) { return s ? lsiw_u2w(s) : NULL; }
static char  *w2a(const WCHAR *s) { return s ? lsiw_w2u(s) : NULL; }

/* UTF-16 from UTF-8 with an explicit byte count: the A registry functions
 * convert exactly cbData bytes, embedded NULs included. */
static WCHAR *a2w_n(const char *s, size_t n, size_t *outn)
{
    int need = n ? lsiw_utf8_to_utf16(s, (int)n, NULL, 0) : 0;
    WCHAR *w = xmalloc(((size_t)need + 1) * sizeof(WCHAR));
    if (need) lsiw_utf8_to_utf16(s, (int)n, w, need);
    w[need] = 0;
    if (outn) *outn = (size_t)need;
    return w;
}

static char *w2a_n(const WCHAR *s, size_t n, size_t *outn)
{
    int need = n ? lsiw_utf16_to_utf8(s, (int)n, NULL, 0) : 0;
    char *a = xmalloc((size_t)need + 1);
    if (need) lsiw_utf16_to_utf8(s, (int)n, a, need);
    a[need] = 0;
    if (outn) *outn = (size_t)need;
    return a;
}

/* Simple case folding to upper case: ASCII, Latin-1, Latin Extended-A,
 * Greek and Cyrillic. Registry names compare equal under this folding. */
static WCHAR fold_wc(WCHAR c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (WCHAR)(c - 32);
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x137) return (WCHAR)(c & ~1u);
    if (c >= 0x139 && c <= 0x148) return (c & 1) ? c : (WCHAR)(c - 1);
    if (c >= 0x14A && c <= 0x177) return (WCHAR)(c & ~1u);
    if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c : (WCHAR)(c - 1);
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return (WCHAR)(c - 32);
    if (c >= 0x430 && c <= 0x44F) return (WCHAR)(c - 32);
    if (c >= 0x450 && c <= 0x45F) return (WCHAR)(c - 80);
    return c;
}

static int wcmp_fold(const WCHAR *a, size_t an, const WCHAR *b, size_t bn)
{
    size_t n = an < bn ? an : bn;
    for (size_t i = 0; i < n; i++) {
        WCHAR x = fold_wc(a[i]), y = fold_wc(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return an < bn ? -1 : an > bn ? 1 : 0;
}

/* Case-insensitive comparison of a counted UTF-16 string with ASCII. */
static int weq_ascii(const WCHAR *w, size_t n, const char *a)
{
    size_t i;
    for (i = 0; i < n && a[i]; i++)
        if (fold_wc(w[i]) != fold_wc((WCHAR)(unsigned char)a[i])) return 0;
    return i == n && !a[i];
}

/* Growable UTF-16 buffer. */
struct wbuf { WCHAR *p; size_t n, cap; };

static void wb_reserve(struct wbuf *b, size_t extra)
{
    if (b->n + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (b->n + extra + 1 > cap) cap *= 2;
    b->p = xrealloc(b->p, cap * sizeof(WCHAR));
    b->cap = cap;
}

static void wb_putc(struct wbuf *b, WCHAR c) { wb_reserve(b, 1); b->p[b->n++] = c; b->p[b->n] = 0; }

static void wb_put(struct wbuf *b, const WCHAR *s, size_t n)
{
    wb_reserve(b, n);
    if (n) memcpy(b->p + b->n, s, n * sizeof(WCHAR));
    b->n += n;
    b->p[b->n] = 0;
}

static WCHAR *wb_take(struct wbuf *b)
{
    if (!b->p) return wdup_n(NULL, 0);
    WCHAR *p = b->p;
    b->p = NULL; b->n = b->cap = 0;
    return p;
}

/* Registry of live objects that programs hold as opaque pointers (crypto
 * contexts, event sources, service handles...), so a bad handle is
 * rejected instead of dereferenced. */
struct objent { void *p; uint32_t type; };
static struct objent  *g_objs;
static size_t          g_nobjs, g_capobjs;
static pthread_mutex_t g_obj_lock = PTHREAD_MUTEX_INITIALIZER;

enum { OBJ_PROV = 1, OBJ_HASH, OBJ_KEY, OBJ_EVSRC, OBJ_EVLOG, OBJ_SCM, OBJ_LSA, OBJ_REGKEY, OBJ_TOKEN, OBJ_SVCSTATUS };

static void obj_add(void *p, uint32_t type)
{
    pthread_mutex_lock(&g_obj_lock);
    if (g_nobjs == g_capobjs) {
        g_capobjs = g_capobjs ? g_capobjs * 2 : 32;
        g_objs = xrealloc(g_objs, g_capobjs * sizeof *g_objs);
    }
    g_objs[g_nobjs].p = p;
    g_objs[g_nobjs].type = type;
    g_nobjs++;
    pthread_mutex_unlock(&g_obj_lock);
}

static int obj_check(const void *p, uint32_t type)
{
    int ok = 0;
    if (!p) return 0;
    pthread_mutex_lock(&g_obj_lock);
    for (size_t i = 0; i < g_nobjs; i++)
        if (g_objs[i].p == p && g_objs[i].type == type) { ok = 1; break; }
    pthread_mutex_unlock(&g_obj_lock);
    return ok;
}

static int obj_remove(const void *p, uint32_t type)
{
    int ok = 0;
    pthread_mutex_lock(&g_obj_lock);
    for (size_t i = 0; i < g_nobjs; i++)
        if (g_objs[i].p == p && g_objs[i].type == type) {
            g_objs[i] = g_objs[--g_nobjs];
            ok = 1;
            break;
        }
    pthread_mutex_unlock(&g_obj_lock);
    return ok;
}

/* Read a small text file (sysfs, /etc), trimmed; NULL if unreadable. */
static char *read_small_file(const char *path)
{
    char buf[512];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    char *s = buf;
    while (*s == ' ' || *s == '\t') s++;
    return *s ? xstrdup(s) : NULL;
}

/* 16 bytes identifying this machine: /etc/machine-id (systemd), else the
 * D-Bus copy, else a hash of the host name. */
static const uint8_t *machine_id(void)
{
    static uint8_t id[16];
    static int done;
    if (done) return id;
    char *s = read_small_file("/etc/machine-id");
    if (!s) s = read_small_file("/var/lib/dbus/machine-id");
    int ok = 0;
    if (s && strlen(s) >= 32) {
        ok = 1;
        for (int i = 0; i < 16 && ok; i++) {
            unsigned v;
            if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) ||
                sscanf(s + 2 * i, "%2x", &v) != 1) ok = 0;
            else id[i] = (uint8_t)v;
        }
    }
    if (!ok) {
        char host[256] = "lsiw";
        gethostname(host, sizeof host);
        host[sizeof host - 1] = 0;
        uint64_t h1 = 1469598103934665603ULL, h2 = 1099511628211ULL;
        for (const char *p = host; *p; p++) {
            h1 = (h1 ^ (unsigned char)*p) * 1099511628211ULL;
            h2 = (h2 ^ (unsigned char)*p) * 1469598103934665603ULL + 7;
        }
        memcpy(id, &h1, 8);
        memcpy(id + 8, &h2, 8);
    }
    free(s);
    done = 1;
    return id;
}

static const char *computer_name(void)
{
    const char *c = lsiw_var_windows("COMPUTERNAME");
    return (c && *c) ? c : "LSIW";
}

static int is_local_machine_a(const char *name)
{
    if (!name || !*name) return 1;
    while (*name == '\\') name++;
    return !*name || !strcasecmp(name, computer_name()) || !strcasecmp(name, "localhost") ||
           !strcmp(name, ".") || !strcmp(name, "127.0.0.1");
}

static int is_local_machine_w(const WCHAR *name)
{
    char *a = w2a(name);
    int r = is_local_machine_a(a);
    free(a);
    return r;
}

/* ExpandEnvironmentStringsW from kernel32 (the process environment); the
 * LSIW variable table is the fallback. Returns a malloc'd string. */
typedef DWORD (WINAPI *expand_env_fn)(const WCHAR *, WCHAR *, DWORD);

static WCHAR *expand_env_w(const WCHAR *s)
{
    static expand_env_fn fn;
    static int looked;
    if (!looked) {
        const struct lsiw_builtin_dll *k = lsiw_builtin_find("kernel32.dll");
        fn = k ? (expand_env_fn)lsiw_builtin_lookup(k, "ExpandEnvironmentStringsW") : NULL;
        looked = 1;
    }
    if (fn) {
        DWORD n = fn(s, NULL, 0);
        if (n) {
            WCHAR *out = xcalloc((size_t)n + 1, sizeof(WCHAR));
            DWORD m = fn(s, out, n + 1);
            if (m && m <= n + 1) return out;
            free(out);
        }
    }
    char *u = w2a(s);
    char *e = lsiw_expand(u ? u : "");
    WCHAR *w = a2w(e);
    free(u); free(e);
    return w;
}

/* ================================================================== */
/* Registry: in-memory tree                                             */
/* ================================================================== */

#define REG_NONE                    0
#define REG_SZ                      1
#define REG_EXPAND_SZ               2
#define REG_BINARY                  3
#define REG_DWORD                   4
#define REG_DWORD_BIG_ENDIAN        5
#define REG_LINK                    6
#define REG_MULTI_SZ                7
#define REG_QWORD                   11

#define KEY_QUERY_VALUE         0x0001u
#define KEY_SET_VALUE           0x0002u
#define KEY_CREATE_SUB_KEY      0x0004u
#define KEY_ENUMERATE_SUB_KEYS  0x0008u
#define KEY_NOTIFY              0x0010u
#define KEY_CREATE_LINK         0x0020u
#define KEY_WOW64_64KEY         0x0100u
#define KEY_WOW64_32KEY         0x0200u
#define KEY_READ                0x20019u
#define KEY_WRITE               0x20006u
#define KEY_ALL_ACCESS          0xF003Fu

#define REG_OPTION_VOLATILE     0x0001u
#define REG_CREATED_NEW_KEY     1
#define REG_OPENED_EXISTING_KEY 2

struct reg_value {
    WCHAR   *name;          /* "" is the default value                      */
    size_t   nlen;
    DWORD    type;
    DWORD    size;
    uint8_t *data;
    int      generated;     /* from the load-time defaults: not saved       */
};

struct reg_key {
    WCHAR   *name;
    size_t   nlen;
    WCHAR   *klass;         /* class string given to RegCreateKeyEx         */
    struct reg_key   *parent;
    struct reg_key  **sub;  /* sorted by folded name, as Windows enumerates */
    size_t   nsub, capsub;
    struct reg_value *val;  /* in creation order                            */
    size_t   nval, capval;
    uint64_t mtime;         /* last write time (FILETIME)                   */
    int      volatile_key;  /* REG_OPTION_VOLATILE: never saved             */
    int      generated;     /* created only to hold generated defaults      */
};

static struct reg_key *key_new(const WCHAR *name, size_t nlen, struct reg_key *parent)
{
    struct reg_key *k = xcalloc(1, sizeof *k);
    k->name = wdup_n(name, nlen);
    k->nlen = nlen;
    k->parent = parent;
    k->mtime = now_filetime();
    return k;
}

static void key_free(struct reg_key *k)
{
    if (!k) return;
    for (size_t i = 0; i < k->nsub; i++) key_free(k->sub[i]);
    for (size_t i = 0; i < k->nval; i++) { free(k->val[i].name); free(k->val[i].data); }
    free(k->sub);
    free(k->val);
    free(k->name);
    free(k->klass);
    free(k);
}

/* Binary search among the subkeys; returns the insertion position. */
static size_t key_sub_pos(const struct reg_key *k, const WCHAR *name, size_t nlen, int *found)
{
    size_t lo = 0, hi = k->nsub;
    *found = 0;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = wcmp_fold(k->sub[mid]->name, k->sub[mid]->nlen, name, nlen);
        if (c == 0) { *found = 1; return mid; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static struct reg_key *key_child(const struct reg_key *k, const WCHAR *name, size_t nlen)
{
    int found;
    size_t pos = key_sub_pos(k, name, nlen, &found);
    return found ? k->sub[pos] : NULL;
}

static void key_insert_child(struct reg_key *k, struct reg_key *c)
{
    int found;
    size_t pos = key_sub_pos(k, c->name, c->nlen, &found);
    if (k->nsub == k->capsub) {
        k->capsub = k->capsub ? k->capsub * 2 : 4;
        k->sub = xrealloc(k->sub, k->capsub * sizeof *k->sub);
    }
    memmove(k->sub + pos + 1, k->sub + pos, (k->nsub - pos) * sizeof *k->sub);
    k->sub[pos] = c;
    k->nsub++;
    c->parent = k;
}

static struct reg_key *key_add_child(struct reg_key *k, const WCHAR *name, size_t nlen)
{
    struct reg_key *c = key_new(name, nlen, k);
    key_insert_child(k, c);
    return c;
}

static void key_detach(struct reg_key *c)
{
    struct reg_key *p = c->parent;
    if (!p) return;
    for (size_t i = 0; i < p->nsub; i++)
        if (p->sub[i] == c) {
            memmove(p->sub + i, p->sub + i + 1, (p->nsub - i - 1) * sizeof *p->sub);
            p->nsub--;
            break;
        }
    c->parent = NULL;
}

static struct reg_value *key_value(const struct reg_key *k, const WCHAR *name, size_t nlen)
{
    for (size_t i = 0; i < k->nval; i++)
        if (!wcmp_fold(k->val[i].name, k->val[i].nlen, name, nlen)) return &k->val[i];
    return NULL;
}

static struct reg_value *key_set_value(struct reg_key *k, const WCHAR *name, size_t nlen,
                                       DWORD type, const void *data, DWORD size)
{
    struct reg_value *v = key_value(k, name, nlen);
    if (!v) {
        if (k->nval == k->capval) {
            k->capval = k->capval ? k->capval * 2 : 4;
            k->val = xrealloc(k->val, k->capval * sizeof *k->val);
        }
        v = &k->val[k->nval++];
        memset(v, 0, sizeof *v);
        v->name = wdup_n(name, nlen);
        v->nlen = nlen;
    } else {
        free(v->data);
    }
    v->type = type;
    v->size = size;
    v->data = xmalloc(size);
    if (size) memcpy(v->data, data, size);
    v->generated = 0;
    return v;
}

static int key_delete_value(struct reg_key *k, const WCHAR *name, size_t nlen)
{
    for (size_t i = 0; i < k->nval; i++)
        if (!wcmp_fold(k->val[i].name, k->val[i].nlen, name, nlen)) {
            free(k->val[i].name);
            free(k->val[i].data);
            memmove(k->val + i, k->val + i + 1, (k->nval - i - 1) * sizeof *k->val);
            k->nval--;
            return 1;
        }
    return 0;
}

static void key_clear(struct reg_key *k)
{
    for (size_t i = 0; i < k->nsub; i++) key_free(k->sub[i]);
    for (size_t i = 0; i < k->nval; i++) { free(k->val[i].name); free(k->val[i].data); }
    k->nsub = k->nval = 0;
}

/* Iterate over the '\'-separated components of a path. Empty components
 * (doubled or trailing separators) are skipped. */
static int path_next(const WCHAR **pp, const WCHAR **comp, size_t *len)
{
    const WCHAR *p = *pp;
    if (!p) return 0;
    while (*p == '\\') p++;
    if (!*p) { *pp = p; return 0; }
    const WCHAR *s = p;
    while (*p && *p != '\\') p++;
    *comp = s;
    *len = (size_t)(p - s);
    *pp = p;
    return 1;
}

static struct reg_key *key_walk(struct reg_key *k, const WCHAR *path)
{
    const WCHAR *c;
    size_t n;
    while (k && path_next(&path, &c, &n)) k = key_child(k, c, n);
    return k;
}

/* ================================================================== */
/* Registry: hives, files and the cross-process protocol                */
/* ================================================================== */

struct hive {
    const char *file;       /* file name in the registry folder; NULL = memory only */
    const char *label;
    int         id;         /* 0 = machine, 1 = user, 2 = current config */
    char       *path;
    struct reg_key *root;
    int         loaded;
    int         have_file;  /* identity below is valid */
    dev_t       dev;
    ino_t       ino;
    off_t       size;
    struct timespec mtim;
};

static struct hive g_hklm = { "system.reg", "HKEY_LOCAL_MACHINE", 0, NULL, NULL, 0, 0, 0, 0, 0, { 0, 0 } };
static struct hive g_hkcu = { "user.reg",   "HKEY_CURRENT_USER",  1, NULL, NULL, 0, 0, 0, 0, 0, { 0, 0 } };
static struct hive g_hkcc = { NULL,         "HKEY_CURRENT_CONFIG", 2, NULL, NULL, 0, 0, 0, 0, 0, { 0, 0 } };

static pthread_mutex_t g_reg_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static unsigned long   g_reg_serial = 1;    /* bumped on every change and reload */

static void reg_lock(void)   { pthread_mutex_lock(&g_reg_lock); }
static void reg_unlock(void) { pthread_mutex_unlock(&g_reg_lock); }

static const char *reg_dir(void)
{
    static char *dir;
    if (!dir) dir = lsiw_strdup_printf("%s/registry", lsiw_data_dir());
    return dir;
}

static const char *hive_path(struct hive *h)
{
    if (!h->path) h->path = lsiw_strdup_printf("%s/%s", reg_dir(), h->file);
    return h->path;
}

/* ---- text format: writing ---- */

static void put_utf8(FILE *f, uint32_t c)
{
    if (c < 0x80) putc((int)c, f);
    else if (c < 0x800) { putc((int)(0xC0 | (c >> 6)), f); putc((int)(0x80 | (c & 0x3F)), f); }
    else if (c < 0x10000) {
        putc((int)(0xE0 | (c >> 12)), f); putc((int)(0x80 | ((c >> 6) & 0x3F)), f); putc((int)(0x80 | (c & 0x3F)), f);
    } else {
        putc((int)(0xF0 | (c >> 18)), f); putc((int)(0x80 | ((c >> 12) & 0x3F)), f);
        putc((int)(0x80 | ((c >> 6) & 0x3F)), f); putc((int)(0x80 | (c & 0x3F)), f);
    }
}

static void put_quoted(FILE *f, const WCHAR *s, size_t n)
{
    putc('"', f);
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            put_utf8(f, 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00u));
            i++;
            continue;
        }
        if (c >= 0xD800 && c <= 0xDFFF) { fprintf(f, "\\u%04x", (unsigned)c); continue; }
        switch (c) {
        case '\\': fputs("\\\\", f); break;
        case '"':  fputs("\\\"", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        case 0:    fputs("\\0", f); break;
        default:
            if (c < 0x20 || c == 0x7F) fprintf(f, "\\x%02x", (unsigned)c);
            else put_utf8(f, c);
        }
    }
    putc('"', f);
}

static void put_hex_bytes(FILE *f, const uint8_t *d, size_t n, int commas)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        if (commas && i) putc(',', f);
        putc(hx[d[i] >> 4], f);
        putc(hx[d[i] & 15], f);
    }
}

static int is_string_type(DWORD t) { return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ; }

static void put_value(FILE *f, const struct reg_value *v)
{
    if (!v->nlen) putc('@', f);
    else put_quoted(f, v->name, v->nlen);
    putc('=', f);
    if (is_string_type(v->type) && v->size >= 2 && !(v->size & 1) &&
        !v->data[v->size - 1] && !v->data[v->size - 2]) {
        size_t n = v->size / 2 - 1;
        WCHAR *w = xmalloc((n + 1) * sizeof(WCHAR));
        memcpy(w, v->data, n * sizeof(WCHAR));
        fputs(v->type == REG_SZ ? "sz:" : v->type == REG_EXPAND_SZ ? "expand:" : "multi:", f);
        put_quoted(f, w, n);
        free(w);
    } else if (v->type == REG_DWORD && v->size == 4) {
        uint32_t d;
        memcpy(&d, v->data, 4);
        fprintf(f, "dword:%08x", d);
    } else if (v->type == REG_QWORD && v->size == 8) {
        uint64_t q;
        memcpy(&q, v->data, 8);
        fprintf(f, "qword:%016llx", (unsigned long long)q);
    } else {
        fprintf(f, "hex(%x):", (unsigned)v->type);
        put_hex_bytes(f, v->data, v->size, 1);
    }
    putc('\n', f);
}

static void save_key(FILE *f, const struct reg_key *k, struct wbuf *path)
{
    if (k->volatile_key) return;
    int saved_values = 0;
    for (size_t i = 0; i < k->nval; i++) if (!k->val[i].generated) saved_values++;
    if (saved_values || (k->parent && !k->generated) || (k->klass && !k->generated)) {
        putc('[', f);
        put_quoted(f, path->p ? path->p : (const WCHAR *)u"", path->n);
        fprintf(f, "] %016llx", (unsigned long long)k->mtime);
        if (k->klass) { fputs(" class=", f); put_quoted(f, k->klass, lsiw_wcslen(k->klass)); }
        putc('\n', f);
        for (size_t i = 0; i < k->nval; i++)
            if (!k->val[i].generated) put_value(f, &k->val[i]);
    }
    for (size_t i = 0; i < k->nsub; i++) {
        size_t keep = path->n;
        if (path->n) wb_putc(path, '\\');
        wb_put(path, k->sub[i]->name, k->sub[i]->nlen);
        save_key(f, k->sub[i], path);
        path->n = keep;
        if (path->p) path->p[keep] = 0;
    }
}

/* ---- text format: reading ---- */

static uint32_t utf8_decode(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    uint32_t c = *p++;
    int extra;
    if (c < 0x80) extra = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else { *pp = p; return 0xFFFD; }
    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) { *pp = p; return 0xFFFD; }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *pp = p;
    return c;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a quoted string at *pp into out (cleared first). Returns 0 on
 * success with *pp just past the closing quote. */
static int get_quoted(const char **pp, struct wbuf *out)
{
    const unsigned char *p = (const unsigned char *)*pp;
    out->n = 0;
    if (out->p) out->p[0] = 0;
    if (*p != '"') return -1;
    p++;
    for (;;) {
        if (!*p) return -1;
        if (*p == '"') { p++; break; }
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': wb_putc(out, '\n'); p++; break;
            case 'r': wb_putc(out, '\r'); p++; break;
            case 't': wb_putc(out, '\t'); p++; break;
            case '0': wb_putc(out, 0); p++; break;
            case 'x': case 'u': {
                int digits = *p == 'x' ? 2 : 4;
                unsigned v = 0;
                p++;
                for (int i = 0; i < digits; i++) {
                    int h = hexval(*p);
                    if (h < 0) return -1;
                    v = v * 16 + (unsigned)h;
                    p++;
                }
                wb_putc(out, (WCHAR)v);
                break;
            }
            case 0: return -1;
            default: wb_putc(out, *p); p++; break;
            }
            continue;
        }
        uint32_t c = utf8_decode(&p);
        if (c >= 0x10000) {
            c -= 0x10000;
            wb_putc(out, (WCHAR)(0xD800 + (c >> 10)));
            wb_putc(out, (WCHAR)(0xDC00 + (c & 0x3FF)));
        } else wb_putc(out, (WCHAR)c);
    }
    *pp = (const char *)p;
    return 0;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Create every missing component of path below root. */
static struct reg_key *key_make_path(struct reg_key *root, const WCHAR *path, int generated, int *created)
{
    struct reg_key *k = root;
    const WCHAR *c;
    size_t n;
    if (created) *created = 0;
    while (path_next(&path, &c, &n)) {
        struct reg_key *s = key_child(k, c, n);
        if (!s) {
            s = key_add_child(k, c, n);
            s->generated = generated;
            if (created) *created = 1;
        }
        k = s;
    }
    return k;
}

static int parse_value_line(const char *p, struct reg_key *k, struct wbuf *name, struct wbuf *tmp)
{
    if (*p == '@') { name->n = 0; p++; }
    else if (get_quoted(&p, name)) return -1;
    p = skip_ws(p);
    if (*p++ != '=') return -1;
    p = skip_ws(p);
    if (!strncmp(p, "sz:", 3) || !strncmp(p, "expand:", 7) || !strncmp(p, "multi:", 6)) {
        DWORD type = p[0] == 's' ? REG_SZ : p[0] == 'e' ? REG_EXPAND_SZ : REG_MULTI_SZ;
        p = strchr(p, ':') + 1;
        if (get_quoted(&p, tmp)) return -1;
        wb_putc(tmp, 0);
        key_set_value(k, name->p ? name->p : (const WCHAR *)u"", name->n, type, tmp->p,
                      (DWORD)(tmp->n * sizeof(WCHAR)));
        return 0;
    }
    if (!strncmp(p, "dword:", 6)) {
        uint32_t d = (uint32_t)strtoul(p + 6, NULL, 16);
        key_set_value(k, name->p ? name->p : (const WCHAR *)u"", name->n, REG_DWORD, &d, 4);
        return 0;
    }
    if (!strncmp(p, "qword:", 6)) {
        uint64_t q = strtoull(p + 6, NULL, 16);
        key_set_value(k, name->p ? name->p : (const WCHAR *)u"", name->n, REG_QWORD, &q, 8);
        return 0;
    }
    if (!strncmp(p, "hex(", 4)) {
        char *end;
        unsigned long type = strtoul(p + 4, &end, 16);
        if (*end != ')' || end[1] != ':') return -1;
        p = end + 2;
        size_t cap = strlen(p) / 2 + 1, n = 0;
        uint8_t *d = xmalloc(cap);
        while (*p) {
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            int h = hexval(p[0]), l = h >= 0 ? hexval(p[1]) : -1;
            if (h < 0 || l < 0) break;
            if (n < cap) d[n++] = (uint8_t)(h * 16 + l);
            p += 2;
        }
        key_set_value(k, name->p ? name->p : (const WCHAR *)u"", name->n, (DWORD)type, d, (DWORD)n);
        free(d);
        return 0;
    }
    return -1;
}

static void parse_hive(FILE *f, struct reg_key *root, const char *fname)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    struct reg_key *cur = root;
    struct wbuf a = { 0 }, b = { 0 };
    unsigned long lineno = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        const char *p = skip_ws(line);
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            p++;
            if (get_quoted(&p, &a) || *p != ']') { LSIW_TRACE("%s:%lu: bad key line\n", fname, lineno); cur = NULL; continue; }
            p++;
            cur = key_make_path(root, a.p ? a.p : (const WCHAR *)u"", 0, NULL);
            p = skip_ws(p);
            if (isxdigit((unsigned char)*p)) {
                char *end;
                cur->mtime = strtoull(p, &end, 16);
                p = skip_ws(end);
            }
            if (!strncmp(p, "class=", 6)) {
                p += 6;
                if (!get_quoted(&p, &b)) { free(cur->klass); cur->klass = wdup_n(b.p, b.n); }
            }
            continue;
        }
        if (!cur || parse_value_line(p, cur, &a, &b))
            LSIW_TRACE("%s:%lu: ignored malformed line\n", fname, lineno);
    }
    free(line);
    free(a.p);
    free(b.p);
}

/* Move this process's volatile keys from the old tree into the new one. */
static void graft_volatile(struct reg_key *from, struct reg_key *to)
{
    for (size_t i = 0; i < from->nsub; ) {
        struct reg_key *c = from->sub[i];
        struct reg_key *same = key_child(to, c->name, c->nlen);
        if (c->volatile_key) {
            if (!same) {
                key_detach(c);
                key_insert_child(to, c);
                continue;           /* from->sub shifted down */
            }
        } else if (same) graft_volatile(c, same);
        i++;
    }
}

static void defaults_apply(struct hive *h, struct reg_key *root);

static int same_identity(const struct hive *h, const struct stat *st)
{
    return h->have_file && h->dev == st->st_dev && h->ino == st->st_ino && h->size == st->st_size &&
           h->mtim.tv_sec == st->st_mtim.tv_sec && h->mtim.tv_nsec == st->st_mtim.tv_nsec;
}

static void remember_identity(struct hive *h, const struct stat *st)
{
    h->have_file = 1;
    h->dev = st->st_dev;
    h->ino = st->st_ino;
    h->size = st->st_size;
    h->mtim = st->st_mtim;
}

static void hive_load(struct hive *h)
{
    struct reg_key *root = key_new(NULL, 0, NULL);
    h->have_file = 0;
    if (h->file) {
        int fd = open(hive_path(h), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat st;
            FILE *f = fdopen(fd, "r");
            if (f && fstat(fd, &st) == 0) {
                char *line = NULL;
                size_t cap = 0;
                ssize_t n = getline(&line, &cap, f);
                if (n > 0 && !strncmp(line, "LSIW-REGISTRY 1", 15)) {
                    remember_identity(h, &st);
                    parse_hive(f, root, h->file);
                } else if (n > 0) {
                    /* Not ours: keep it aside rather than overwrite it. */
                    char *aside = lsiw_strdup_printf("%s.unreadable-%ld", hive_path(h), (long)time(NULL));
                    lsiw_log("registry: %s has an unknown format; moved to %s\n", hive_path(h), aside);
                    rename(hive_path(h), aside);
                    free(aside);
                }
                free(line);
            }
            if (f) fclose(f); else close(fd);
        }
    }
    if (h->root) {
        graft_volatile(h->root, root);
        key_free(h->root);
    }
    h->root = root;
    h->loaded = 1;
    defaults_apply(h, root);
    g_reg_serial++;
}

/* Bring the in-memory hive up to date with its file. */
static void hive_sync(struct hive *h)
{
    if (!h->loaded) { hive_load(h); return; }
    if (!h->file) return;
    struct stat st;
    if (stat(hive_path(h), &st) != 0) {
        if (h->have_file) hive_load(h);     /* someone removed it */
        return;
    }
    if (!same_identity(h, &st)) hive_load(h);
}

static int hive_lock_file(struct hive *h)
{
    if (!h->file) return -1;
    lsiw_mkdirs(reg_dir(), 0700);
    char *lp = lsiw_strdup_printf("%s.lock", hive_path(h));
    int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(lp);
    if (fd < 0) return -1;
    while (flock(fd, LOCK_EX) < 0 && errno == EINTR) {}
    return fd;
}

static void hive_unlock_file(int fd)
{
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

static void hive_save(struct hive *h)
{
    static int warned;
    if (!h->file) return;
    lsiw_mkdirs(reg_dir(), 0700);
    char *tmp = lsiw_strdup_printf("%s.tmp.%d", hive_path(h), (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0) close(fd);
        if (!warned++) lsiw_log("registry: cannot write %s: %s\n", tmp, strerror(errno));
        free(tmp);
        return;
    }
    static char iobuf[1 << 16];
    setvbuf(f, iobuf, _IOFBF, sizeof iobuf);
    fprintf(f, "LSIW-REGISTRY 1\n; %s - written by LSIW (advapi32); the format is described in src/native/advapi32.c\n\n",
            h->label);
    struct wbuf path = { 0 };
    save_key(f, h->root, &path);
    free(path.p);
    int bad = fflush(f) != 0 || ferror(f);
    struct stat st;
    /* Make the modification time strictly newer than the version being
     * replaced so other processes always notice the change. */
    if (!bad && h->have_file && fstat(fd, &st) == 0 &&
        (st.st_mtim.tv_sec < h->mtim.tv_sec ||
         (st.st_mtim.tv_sec == h->mtim.tv_sec && st.st_mtim.tv_nsec <= h->mtim.tv_nsec))) {
        struct timespec ts[2] = { { 0, UTIME_OMIT }, h->mtim };
        if (++ts[1].tv_nsec >= 1000000000L) { ts[1].tv_sec++; ts[1].tv_nsec = 0; }
        futimens(fd, ts);
    }
    if (fclose(f) != 0) bad = 1;
    if (bad || rename(tmp, hive_path(h)) != 0) {
        if (!warned++) lsiw_log("registry: cannot save %s: %s\n", hive_path(h), strerror(errno));
        unlink(tmp);
    } else if (stat(hive_path(h), &st) == 0) {
        remember_identity(h, &st);
    }
    free(tmp);
}

/* A modification: lock the hive file, catch up with other processes,
 * change the tree, write it back. */
struct reg_tx { struct hive *h; int fd; };

static void tx_begin(struct reg_tx *t, struct hive *h)
{
    t->h = h;
    t->fd = hive_lock_file(h);
    hive_sync(h);
}

static void reg_watchers_poke(void);

static void tx_end(struct reg_tx *t, int save)
{
    if (save) hive_save(t->h);
    hive_unlock_file(t->fd);
    t->fd = -1;
    g_reg_serial++;
    reg_watchers_poke();
}

/* ================================================================== */
/* Registry: values generated at load time                              */
/* ================================================================== */

struct reg_default {
    int      hive;          /* 0 = HKLM, 1 = HKCU */
    WCHAR   *path;
    WCHAR   *name;          /* NULL: only make sure the key exists */
    DWORD    type, size;
    uint8_t *data;
};

static struct reg_default *g_defs;
static size_t g_ndefs, g_capdefs;
static int    g_defs_built;

static void def_add(int hive, const char *path, const char *name, DWORD type, const void *data, DWORD size)
{
    if (g_ndefs == g_capdefs) {
        g_capdefs = g_capdefs ? g_capdefs * 2 : 128;
        g_defs = xrealloc(g_defs, g_capdefs * sizeof *g_defs);
    }
    struct reg_default *d = &g_defs[g_ndefs++];
    d->hive = hive;
    d->path = a2w(path);
    d->name = name ? a2w(name) : NULL;
    d->type = type;
    d->size = size;
    d->data = xmalloc(size);
    if (size) memcpy(d->data, data, size);
}

static void def_key(int hive, const char *path) { def_add(hive, path, NULL, REG_NONE, NULL, 0); }

static void def_str(int hive, const char *path, const char *name, DWORD type, const char *val)
{
    if (!val) return;
    size_t n;
    WCHAR *w = a2w_n(val, strlen(val), &n);
    def_add(hive, path, name, type, w, (DWORD)((n + 1) * sizeof(WCHAR)));
    free(w);
}

static void def_sz(int hive, const char *path, const char *name, const char *val) { def_str(hive, path, name, REG_SZ, val); }
static void def_expand(int hive, const char *path, const char *name, const char *val) { def_str(hive, path, name, REG_EXPAND_SZ, val); }
static void def_dword(int hive, const char *path, const char *name, DWORD v) { def_add(hive, path, name, REG_DWORD, &v, 4); }

/* Rewrite a Windows path with %USERPROFILE%-style variables, the form
 * Windows keeps in REG_EXPAND_SZ folder values. */
static char *unexpand_path(const char *p)
{
    static const struct { const char *var, *token; } t[] = {
        { "USERPROFILE", "%USERPROFILE%" }, { "PUBLIC", "%PUBLIC%" },
        { "CSIDL_COMMON_APPDATA", "%ProgramData%" }, { "WINDIR", "%SystemRoot%" },
        { "CSIDL_PROGRAM_FILES", "%ProgramFiles%" },
    };
    size_t best = 0;
    int bi = -1;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *v = lsiw_var_windows(t[i].var);
        if (!v) continue;
        size_t l = strlen(v);
        if (l > best && !strncasecmp(p, v, l) && (p[l] == 0 || p[l] == '\\')) { best = l; bi = (int)i; }
    }
    if (bi < 0) return xstrdup(p);
    return lsiw_strdup_printf("%s%s", t[bi].token, p + best);
}

struct cpu_info { char *name, *vendor; int family, model, stepping; DWORD mhz; };

static int read_cpus(struct cpu_info **out)
{
    FILE *f = fopen("/proc/cpuinfo", "re");
    struct cpu_info *v = NULL;
    int n = 0;
    if (f) {
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, f) > 0) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            char *key = line, *val = colon + 1;
            char *e = colon;
            while (e > key && (e[-1] == ' ' || e[-1] == '\t')) e--;
            *e = 0;
            while (*val == ' ' || *val == '\t') val++;
            size_t vl = strlen(val);
            while (vl && (val[vl - 1] == '\n' || val[vl - 1] == ' ')) val[--vl] = 0;
            if (!strcmp(key, "processor")) {
                v = xrealloc(v, (size_t)(n + 1) * sizeof *v);
                memset(&v[n], 0, sizeof v[n]);
                n++;
            } else if (n) {
                struct cpu_info *c = &v[n - 1];
                if (!strcmp(key, "model name") && !c->name) c->name = xstrdup(val);
                else if (!strcmp(key, "vendor_id") && !c->vendor) c->vendor = xstrdup(val);
                else if (!strcmp(key, "cpu family")) c->family = atoi(val);
                else if (!strcmp(key, "model")) c->model = atoi(val);
                else if (!strcmp(key, "stepping")) c->stepping = atoi(val);
                else if (!strcmp(key, "cpu MHz")) c->mhz = (DWORD)(atof(val) + 0.5);
            }
        }
        free(line);
        fclose(f);
    }
    if (!n) {
        long cnt = sysconf(_SC_NPROCESSORS_ONLN);
        n = cnt > 0 ? (int)cnt : 1;
        v = xcalloc((size_t)n, sizeof *v);
    }
    char *maxf = read_small_file("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    DWORD max_mhz = maxf ? (DWORD)(strtoul(maxf, NULL, 10) / 1000) : 0;
    free(maxf);
    for (int i = 0; i < n; i++) if (max_mhz) v[i].mhz = max_mhz;
    *out = v;
    return n;
}

static const char *user_sid_string(void);

static void defaults_build(void)
{
    const char *windir = lsiw_var_windows("WINDIR");
    if (!windir) windir = "C:\\windows";
    char buf[512];

    /* ---- HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion ---- */
    static const char *const ntcv[] = { "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                        "SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion" };
    const uint8_t *mid = machine_id();
    struct stat st;
    DWORD install = (DWORD)time(NULL);
    if (stat("/etc/machine-id", &st) == 0) install = (DWORD)st.st_mtime;
    for (int i = 0; i < 2; i++) {
        const char *k = ntcv[i];
        def_sz(0, k, "ProductName", "Windows 10 Pro");
        def_sz(0, k, "EditionID", "Professional");
        def_sz(0, k, "CurrentVersion", "6.3");
        def_sz(0, k, "CurrentBuild", "19045");
        def_sz(0, k, "CurrentBuildNumber", "19045");
        def_dword(0, k, "CurrentMajorVersionNumber", 10);
        def_dword(0, k, "CurrentMinorVersionNumber", 0);
        def_sz(0, k, "ReleaseId", "2009");
        def_sz(0, k, "DisplayVersion", "22H2");
        def_sz(0, k, "InstallationType", "Client");
        def_sz(0, k, "RegisteredOwner", lsiw_user_name());
        def_sz(0, k, "RegisteredOrganization", "");
        def_sz(0, k, "SystemRoot", windir);
        def_sz(0, k, "PathName", windir);
        def_dword(0, k, "UBR", 3803);
        def_sz(0, k, "BuildLab", "19041.vb_release.191206-1406");
        def_sz(0, k, "BuildLabEx", "19041.1.amd64fre.vb_release.191206-1406");
        def_sz(0, k, "BuildBranch", "vb_release");
        def_sz(0, k, "CurrentType", "Multiprocessor Free");
        def_sz(0, k, "SoftwareType", "System");
        snprintf(buf, sizeof buf, "00330-%05u-%05u-AA%03u", (mid[0] << 8 | mid[1]) % 100000u,
                 (mid[2] << 8 | mid[3]) % 100000u, mid[4] % 1000u);
        def_sz(0, k, "ProductId", buf);
        def_dword(0, k, "InstallDate", install);
    }

    /* ---- profile list ---- */
    const char *pl = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
    def_expand(0, pl, "ProfilesDirectory", lsiw_var_windows("PROFILESFOLDER"));
    def_expand(0, pl, "Default", lsiw_var_windows("DEFAULTUSERPROFILE"));
    def_expand(0, pl, "Public", lsiw_var_windows("PUBLIC"));
    def_expand(0, pl, "ProgramData", lsiw_var_windows("CSIDL_COMMON_APPDATA"));
    snprintf(buf, sizeof buf, "%s\\%s", pl, user_sid_string());
    def_expand(0, buf, "ProfileImagePath", lsiw_var_windows("USERPROFILE"));
    def_dword(0, buf, "Flags", 0);
    def_dword(0, buf, "State", 0);
    snprintf(buf, sizeof buf, "%s\\S-1-5-18", pl);
    def_expand(0, buf, "ProfileImagePath", "%systemroot%\\system32\\config\\systemprofile");

    /* ---- program folders ---- */
    static const char *const cv[] = { "SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
                                      "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion" };
    for (int i = 0; i < 2; i++) {
        const char *k = cv[i];
        def_sz(0, k, "ProgramFilesDir", lsiw_var_windows(i ? "CSIDL_PROGRAM_FILESX86" : "CSIDL_PROGRAM_FILES"));
        def_sz(0, k, "ProgramFilesDir (x86)", lsiw_var_windows("CSIDL_PROGRAM_FILESX86"));
        def_sz(0, k, "ProgramW6432Dir", lsiw_var_windows("PROGRAMW6432"));
        def_sz(0, k, "CommonFilesDir", lsiw_var_windows(i ? "CSIDL_PROGRAM_FILES_COMMONX86" : "CSIDL_PROGRAM_FILES_COMMON"));
        def_sz(0, k, "CommonFilesDir (x86)", lsiw_var_windows("CSIDL_PROGRAM_FILES_COMMONX86"));
        def_sz(0, k, "CommonW6432Dir", lsiw_var_windows("COMMONPROGRAMW6432"));
        def_expand(0, k, "ProgramFilesPath", "%ProgramFiles%");
        def_expand(0, k, "DevicePath", "%SystemRoot%\\inf");
        def_expand(0, k, "MediaPathUnexpanded", "%SystemRoot%\\Media");
    }
    def_key(0, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run");
    def_key(0, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce");
    def_key(0, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
    def_key(0, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths");
    def_key(0, "SOFTWARE\\Classes");
    def_key(0, "SOFTWARE\\Policies");
    def_key(0, "SYSTEM\\CurrentControlSet\\Services");

    /* ---- shell folders ---- */
    size_t nv;
    const struct lsiw_var *vars = lsiw_vars(&nv);
    const char *usf = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
    const char *sf = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders";
    for (size_t i = 0; i < nv; i++) {
        if (!vars[i].shell_value) continue;
        const char *w = lsiw_var_windows(vars[i].name);
        if (!w) continue;
        char *ux = unexpand_path(w);
        def_sz(1, sf, vars[i].shell_value, w);
        def_expand(1, usf, vars[i].shell_value, ux);
        if (vars[i].scope == LSIW_SCOPE_SYSTEM && vars[i].kind == LSIW_VAR_FOLDER && !strstr(vars[i].shell_value, "Dir")) {
            def_sz(0, sf, vars[i].shell_value, w);
            def_expand(0, usf, vars[i].shell_value, ux);
        }
        free(ux);
    }

    /* ---- environment ---- */
    const char *uenv = "Environment";
    const char *t = lsiw_var_windows("TEMP");
    if (t) {
        char *ux = unexpand_path(t);
        def_expand(1, uenv, "TEMP", ux);
        def_expand(1, uenv, "TMP", ux);
        free(ux);
    }
    const char *venv = "Volatile Environment";
    def_sz(1, venv, "APPDATA", lsiw_var_windows("APPDATA"));
    def_sz(1, venv, "LOCALAPPDATA", lsiw_var_windows("LOCALAPPDATA"));
    def_sz(1, venv, "HOMEDRIVE", "C:");
    def_sz(1, venv, "HOMEPATH", lsiw_var_windows("HOMEPATH"));
    def_sz(1, venv, "USERPROFILE", lsiw_var_windows("USERPROFILE"));
    def_sz(1, venv, "USERNAME", lsiw_user_name());
    def_sz(1, venv, "USERDOMAIN", computer_name());
    def_sz(1, venv, "USERDOMAIN_ROAMINGPROFILE", computer_name());
    snprintf(buf, sizeof buf, "\\\\%s", computer_name());
    def_sz(1, venv, "LOGONSERVER", buf);
    def_key(1, "Software\\Classes");
    def_key(1, "Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    def_key(1, "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce");
    def_sz(1, "Control Panel\\International", "LocaleName", "en-US");
    def_sz(1, "Control Panel\\International", "Locale", "00000409");

    struct cpu_info *cpus;
    int ncpu = read_cpus(&cpus);
    const char *menv = "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    def_expand(0, menv, "ComSpec", "%SystemRoot%\\system32\\cmd.exe");
    def_sz(0, menv, "OS", "Windows_NT");
    def_expand(0, menv, "Path", "%SystemRoot%\\system32;%SystemRoot%;%SystemRoot%\\System32\\Wbem;"
                                "%SYSTEMROOT%\\System32\\WindowsPowerShell\\v1.0\\");
    def_sz(0, menv, "PATHEXT", ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");
    def_sz(0, menv, "PROCESSOR_ARCHITECTURE", "AMD64");
    snprintf(buf, sizeof buf, "%d", ncpu);
    def_sz(0, menv, "NUMBER_OF_PROCESSORS", buf);
    def_expand(0, menv, "TEMP", "%SystemRoot%\\TEMP");
    def_expand(0, menv, "TMP", "%SystemRoot%\\TEMP");
    def_expand(0, menv, "windir", "%SystemRoot%");
    def_sz(0, menv, "USERNAME", "SYSTEM");

    /* ---- processors ---- */
    for (int i = 0; i < ncpu; i++) {
        struct cpu_info *c = &cpus[i];
        const char *vendor = c->vendor ? c->vendor : "GenuineIntel";
        int amd = !strcmp(vendor, "AuthenticAMD") || !strcmp(vendor, "HygonGenuine");
        char ident[128];
        snprintf(ident, sizeof ident, "%s Family %d Model %d Stepping %d", amd ? "AMD64" : "Intel64",
                 c->family, c->model, c->stepping);
        snprintf(buf, sizeof buf, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%d", i);
        def_sz(0, buf, "ProcessorNameString", c->name ? c->name : "Unknown processor");
        def_sz(0, buf, "VendorIdentifier", vendor);
        def_sz(0, buf, "Identifier", ident);
        def_dword(0, buf, "~MHz", c->mhz);
        if (i == 0) {
            char pid[192];
            snprintf(pid, sizeof pid, "%s, %s", ident, vendor);
            def_sz(0, menv, "PROCESSOR_IDENTIFIER", pid);
            snprintf(pid, sizeof pid, "%d", c->family);
            def_sz(0, menv, "PROCESSOR_LEVEL", pid);
            snprintf(pid, sizeof pid, "%02x%02x", c->model & 0xff, c->stepping & 0xff);
            def_sz(0, menv, "PROCESSOR_REVISION", pid);
        }
        free(c->name);
        free(c->vendor);
    }
    free(cpus);

    /* ---- firmware (DMI) ---- */
    static const struct { const char *file, *value; } dmi[] = {
        { "bios_vendor", "BIOSVendor" }, { "bios_version", "BIOSVersion" }, { "bios_date", "BIOSReleaseDate" },
        { "sys_vendor", "SystemManufacturer" }, { "product_name", "SystemProductName" },
        { "product_family", "SystemFamily" }, { "product_version", "SystemVersion" },
        { "product_sku", "SystemSKU" }, { "board_vendor", "BaseBoardManufacturer" },
        { "board_name", "BaseBoardProduct" }, { "board_version", "BaseBoardVersion" },
    };
    def_key(0, "HARDWARE\\DESCRIPTION\\System");
    for (size_t i = 0; i < sizeof dmi / sizeof dmi[0]; i++) {
        snprintf(buf, sizeof buf, "/sys/class/dmi/id/%s", dmi[i].file);
        char *v = read_small_file(buf);
        if (v) def_sz(0, "HARDWARE\\DESCRIPTION\\System\\BIOS", dmi[i].value, v);
        free(v);
    }

    /* ---- identity of the machine ---- */
    def_sz(0, "SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ComputerName", "ComputerName", computer_name());
    def_sz(0, "SYSTEM\\CurrentControlSet\\Control\\ComputerName\\ActiveComputerName", "ComputerName", computer_name());
    char host[256] = "lsiw";
    gethostname(host, sizeof host);
    host[sizeof host - 1] = 0;
    char *dot = strchr(host, '.');
    const char *domain = dot ? dot + 1 : "";
    if (dot) *dot = 0;
    def_sz(0, "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", "Hostname", host);
    def_sz(0, "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", "NV Hostname", host);
    def_sz(0, "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", "Domain", domain);
    snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             mid[0], mid[1], mid[2], mid[3], mid[4], mid[5], mid[6], mid[7],
             mid[8], mid[9], mid[10], mid[11], mid[12], mid[13], mid[14], mid[15]);
    def_sz(0, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", buf);
    def_sz(0, "SYSTEM\\CurrentControlSet\\Control\\ProductOptions", "ProductType", "WinNT");
    def_sz(0, "SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage", "ACP", "65001");
    def_sz(0, "SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage", "OEMCP", "65001");
    def_sz(0, "SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage", "MACCP", "10000");
    def_sz(0, "SYSTEM\\CurrentControlSet\\Control\\Nls\\Language", "Default", "0409");
    def_sz(0, "SYSTEM\\CurrentControlSet\\Control\\Nls\\Language", "InstallLanguage", "0409");
    /* HKLM\SOFTWARE\Microsoft\NET Framework Setup is deliberately absent:
     * programs that look for an installed .NET Framework see none. */
    g_defs_built = 1;
}

static void defaults_apply(struct hive *h, struct reg_key *root)
{
    if (h->id > 1) return;
    if (!g_defs_built) defaults_build();
    for (size_t i = 0; i < g_ndefs; i++) {
        struct reg_default *d = &g_defs[i];
        if (d->hive != h->id) continue;
        struct reg_key *k = key_make_path(root, d->path, 1, NULL);
        if (!d->name) continue;
        size_t nl = lsiw_wcslen(d->name);
        if (key_value(k, d->name, nl)) continue;
        struct reg_value *v = key_set_value(k, d->name, nl, d->type, d->data, d->size);
        v->generated = 1;
    }
}

/* ================================================================== */
/* Registry: handles and the logical roots                              */
/* ================================================================== */

/* Logical roots, numbered like the low byte of the predefined handles. */
enum { R_HKCR, R_HKCU, R_HKLM, R_HKU, R_PERF, R_HKCC, R_DYN, R_COUNT };

#define HKEY_CLASSES_ROOT     ((HKEY)(ULONG_PTR)(LONG)0x80000000)
#define HKEY_CURRENT_USER     ((HKEY)(ULONG_PTR)(LONG)0x80000001)
#define HKEY_LOCAL_MACHINE    ((HKEY)(ULONG_PTR)(LONG)0x80000002)
#define HKEY_USERS            ((HKEY)(ULONG_PTR)(LONG)0x80000003)

#define REGKEY_MAGIC 0x4b474552u            /* "REGK" */

/* An open key handle: a logical root and a canonical path below it
 * (components separated by one '\', no leading or trailing separator). The
 * key itself is looked up again on every call, so a handle to a key that
 * another process deleted reports ERROR_KEY_DELETED like Windows. */
struct open_key {
    uint32_t magic;
    int      root;
    WCHAR   *path;
    REGSAM   sam;
};

/* RegOverridePredefKey: the key a predefined handle currently stands for. */
static struct { int set; int root; WCHAR *path; } g_override[R_COUNT];

/* Predefined handles are accepted sign-extended (the Windows headers) and
 * zero-extended (code that truncated them to 32 bits). */
static int predef_root(HKEY h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if ((v >> 32) != 0xFFFFFFFFu && (v >> 32) != 0) return -1;
    switch ((DWORD)v) {
    case 0x80000000u: return R_HKCR;
    case 0x80000001u: return R_HKCU;
    case 0x80000002u: return R_HKLM;
    case 0x80000003u: return R_HKU;
    case 0x80000004u: case 0x80000050u: case 0x80000060u: return R_PERF;
    case 0x80000005u: return R_HKCC;
    case 0x80000006u: return R_DYN;
    }
    return -1;
}

/* Join two '\'-separated paths into canonical form. */
static WCHAR *path_join(const WCHAR *a, const WCHAR *b)
{
    struct wbuf out = { 0 };
    const WCHAR *parts[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        const WCHAR *p = parts[i], *c;
        size_t n;
        while (p && path_next(&p, &c, &n)) {
            if (out.n) wb_putc(&out, '\\');
            wb_put(&out, c, n);
        }
    }
    return wb_take(&out);
}

/* Access rights a handle grants, with the generic rights mapped the way
 * the registry maps them. */
static int sam_allows(REGSAM sam, REGSAM want)
{
    REGSAM eff = sam;
    if (sam & (MAXIMUM_ALLOWED | GENERIC_ALL)) eff |= KEY_ALL_ACCESS;
    if (sam & GENERIC_READ) eff |= KEY_READ;
    if (sam & GENERIC_WRITE) eff |= KEY_WRITE;
    if (sam & GENERIC_EXECUTE) eff |= KEY_READ;
    return (eff & want) == want;
}

struct keyref { int root; WCHAR *path; REGSAM sam; };

/* Resolve a handle plus an optional relative path. Call with the
 * registry lock held (so the handle cannot be closed meanwhile). */
static LSTATUS keyref_get(HKEY h, const WCHAR *sub, struct keyref *out)
{
    const WCHAR *base;
    int r = predef_root(h);
    if (r >= 0) {
        if (g_override[r].set) { out->root = g_override[r].root; base = g_override[r].path; }
        else { out->root = r; base = NULL; }
        out->sam = KEY_ALL_ACCESS;
    } else {
        struct open_key *k = (struct open_key *)h;
        if (!k || !obj_check(k, OBJ_REGKEY) || k->magic != REGKEY_MAGIC) return ERROR_INVALID_HANDLE;
        out->root = k->root;
        base = k->path;
        out->sam = k->sam;
    }
    out->path = path_join(base, sub);
    return ERROR_SUCCESS;
}

static HKEY open_key_new(int root, WCHAR *path, REGSAM sam)
{
    struct open_key *k = xcalloc(1, sizeof *k);
    k->magic = REGKEY_MAGIC;
    k->root = root;
    k->path = path;
    k->sam = sam;
    obj_add(k, OBJ_REGKEY);
    return (HKEY)k;
}

/* ---- the current user's SID, as HKEY_USERS names it ---- */

static const WCHAR *sid_w(int classes)
{
    static WCHAR *w[2];
    if (!w[classes]) {
        char *s = lsiw_strdup_printf("%s%s", user_sid_string(), classes ? "_Classes" : "");
        w[classes] = a2w(s);
        free(s);
    }
    return w[classes];
}

/* A concrete location: a hive and a path from its root. */
struct cand { struct hive *h; WCHAR *path; };

struct regop {
    struct keyref ref;
    struct cand   c[2];     /* c[1]: HKEY_CLASSES_ROOT's HKLM half        */
    int           nc;
    int           virt;     /* exists without backing storage (a root)   */
    int           hku_root; /* the synthetic top of HKEY_USERS            */
};

static const WCHAR CLASSES_W[] = { 'S','o','f','t','w','a','r','e','\\','C','l','a','s','s','e','s',0 };

/* Map the logical key onto hive locations. HKEY_USERS\<SID> and .DEFAULT
 * are HKEY_CURRENT_USER, HKEY_USERS\<SID>_Classes is its Software\Classes.
 * HKEY_CLASSES_ROOT is HKCU\Software\Classes over HKLM\Software\Classes. */
static void regop_resolve(struct regop *op)
{
    const WCHAR *path = op->ref.path;
    switch (op->ref.root) {
    case R_HKLM: op->c[0].h = &g_hklm; op->c[0].path = wdup(path); op->nc = 1; break;
    case R_HKCU: op->c[0].h = &g_hkcu; op->c[0].path = wdup(path); op->nc = 1; break;
    case R_HKCC: op->c[0].h = &g_hkcc; op->c[0].path = wdup(path); op->nc = 1; break;
    case R_HKCR:
        op->c[0].h = &g_hkcu; op->c[0].path = path_join(CLASSES_W, path);
        op->c[1].h = &g_hklm; op->c[1].path = path_join(CLASSES_W, path);
        op->nc = 2;
        break;
    case R_HKU: {
        const WCHAR *p = path, *c;
        size_t n;
        if (!path_next(&p, &c, &n)) { op->virt = op->hku_root = 1; break; }
        if (weq_ascii(c, n, ".DEFAULT") || !wcmp_fold(c, n, sid_w(0), lsiw_wcslen(sid_w(0)))) {
            op->c[0].h = &g_hkcu; op->c[0].path = path_join(NULL, p); op->nc = 1;
        } else if (!wcmp_fold(c, n, sid_w(1), lsiw_wcslen(sid_w(1)))) {
            op->c[0].h = &g_hkcu; op->c[0].path = path_join(CLASSES_W, p); op->nc = 1;
        }
        break;
    }
    default:    /* performance data, dynamic data: empty keys */
        if (!*path) op->virt = 1;
        break;
    }
}

/* Start an operation on hKey\sub. Call with the registry lock held. */
static LSTATUS regop_begin(struct regop *op, HKEY h, const WCHAR *sub)
{
    memset(op, 0, sizeof *op);
    LSTATUS e = keyref_get(h, sub, &op->ref);
    if (e) return e;
    regop_resolve(op);
    for (int i = 0; i < op->nc; i++) hive_sync(op->c[i].h);
    return ERROR_SUCCESS;
}

static void regop_end(struct regop *op)
{
    free(op->ref.path);
    for (int i = 0; i < 2; i++) free(op->c[i].path);
}

static struct reg_key *cand_key(const struct cand *c)
{
    return c->h ? key_walk(c->h->root, c->path) : NULL;
}

static int regop_exists(const struct regop *op)
{
    if (op->virt) return 1;
    for (int i = 0; i < op->nc; i++) if (cand_key(&op->c[i])) return 1;
    return 0;
}

/* Number of leading components of path that exist below root. */
static size_t depth_existing(struct reg_key *root, const WCHAR *path)
{
    const WCHAR *c;
    size_t n, d = 0;
    struct reg_key *k = root;
    while (path_next(&path, &c, &n)) {
        k = key_child(k, c, n);
        if (!k) break;
        d++;
    }
    return d;
}

/* Which half of HKEY_CLASSES_ROOT a modification goes to: the one that
 * holds the key (or its deepest existing ancestor), the per-user half when
 * both do. A key and its subkeys therefore stay in the same hive. */
static int regop_target(const struct regop *op)
{
    if (op->nc < 2) return 0;
    size_t d0 = depth_existing(op->c[0].h->root, op->c[0].path);
    size_t d1 = depth_existing(op->c[1].h->root, op->c[1].path);
    return d1 > d0 ? 1 : 0;
}

static int key_in_volatile(const struct reg_key *k)
{
    for (; k; k = k->parent) if (k->volatile_key) return 1;
    return 0;
}

static void key_touch(struct reg_key *k) { if (k) k->mtime = now_filetime(); }

/* ---- collecting subkeys and values across both halves ---- */

struct sub_ent { const WCHAR *name; size_t nlen; const struct reg_key *k; };

static size_t merged_subkeys(const struct regop *op, struct sub_ent **out)
{
    if (op->hku_root) {
        static const WCHAR def[] = { '.','D','E','F','A','U','L','T',0 };
        struct sub_ent *v = xcalloc(3, sizeof *v);
        v[0].name = def; v[0].nlen = 8;
        v[1].name = sid_w(0); v[1].nlen = lsiw_wcslen(sid_w(0));
        v[2].name = sid_w(1); v[2].nlen = lsiw_wcslen(sid_w(1));
        *out = v;
        return 3;
    }
    const struct reg_key *a = op->nc > 0 ? cand_key(&op->c[0]) : NULL;
    const struct reg_key *b = op->nc > 1 ? cand_key(&op->c[1]) : NULL;
    size_t na = a ? a->nsub : 0, nb = b ? b->nsub : 0, i = 0, j = 0, n = 0;
    struct sub_ent *v = xcalloc(na + nb + 1, sizeof *v);
    while (i < na || j < nb) {
        const struct reg_key *x;
        if (i < na && j < nb) {
            int c = wcmp_fold(a->sub[i]->name, a->sub[i]->nlen, b->sub[j]->name, b->sub[j]->nlen);
            if (c == 0) { x = a->sub[i++]; j++; }
            else if (c < 0) x = a->sub[i++];
            else x = b->sub[j++];
        } else if (i < na) x = a->sub[i++];
        else x = b->sub[j++];
        v[n].name = x->name; v[n].nlen = x->nlen; v[n].k = x;
        n++;
    }
    *out = v;
    return n;
}

static size_t merged_values(const struct regop *op, const struct reg_value ***out)
{
    const struct reg_key *a = op->nc > 0 ? cand_key(&op->c[0]) : NULL;
    const struct reg_key *b = op->nc > 1 ? cand_key(&op->c[1]) : NULL;
    size_t na = a ? a->nval : 0, nb = b ? b->nval : 0, n = 0;
    const struct reg_value **v = xcalloc(na + nb + 1, sizeof *v);
    for (size_t i = 0; i < na; i++) v[n++] = &a->val[i];
    for (size_t i = 0; i < nb; i++)
        if (!a || !key_value(a, b->val[i].name, b->val[i].nlen)) v[n++] = &b->val[i];
    *out = v;
    return n;
}

/* ---- string data conversion for the A functions ---- */

static uint8_t *strdata_w2a(const uint8_t *d, DWORD size, DWORD *outsize)
{
    size_t nw = size / 2, na;
    WCHAR *w = xmalloc((nw + 1) * sizeof(WCHAR));
    if (nw) memcpy(w, d, nw * sizeof(WCHAR));
    char *a = w2a_n(w, nw, &na);
    free(w);
    *outsize = (DWORD)na;
    return (uint8_t *)a;
}

static uint8_t *strdata_a2w(const uint8_t *d, DWORD size, DWORD *outsize)
{
    size_t nw;
    WCHAR *w = a2w_n((const char *)d, size, &nw);
    *outsize = (DWORD)(nw * sizeof(WCHAR));
    return (uint8_t *)w;
}

/* Copy data out with the registry's size protocol. */
static LSTATUS copy_out(const void *src, DWORD size, void *dst, DWORD *cb)
{
    if (dst && !cb) return ERROR_INVALID_PARAMETER;
    if (!cb) return ERROR_SUCCESS;
    if (dst) {
        if (*cb < size) { *cb = size; return ERROR_MORE_DATA; }
        if (size) memcpy(dst, src, size);
    }
    *cb = size;
    return ERROR_SUCCESS;
}

/* Copy a counted name into a character buffer: *cch is the buffer size
 * in characters on input and the length without the NUL on output. */
static LSTATUS name_out_w(const WCHAR *s, size_t n, WCHAR *buf, DWORD *cch)
{
    if (!buf || !cch) return ERROR_INVALID_PARAMETER;
    if (n + 1 > *cch) return ERROR_MORE_DATA;
    if (n) memcpy(buf, s, n * sizeof(WCHAR));
    buf[n] = 0;
    *cch = (DWORD)n;
    return ERROR_SUCCESS;
}

static LSTATUS name_out_a(const WCHAR *s, size_t n, char *buf, DWORD *cch)
{
    if (!buf || !cch) return ERROR_INVALID_PARAMETER;
    size_t na;
    char *a = w2a_n(s, n, &na);
    LSTATUS e = ERROR_SUCCESS;
    if (na + 1 > *cch) e = ERROR_MORE_DATA;
    else { memcpy(buf, a, na + 1); *cch = (DWORD)na; }
    free(a);
    return e;
}

/* ================================================================== */
/* Registry API: keys                                                   */
/* ================================================================== */

static LSTATUS open_key_w(HKEY h, const WCHAR *sub, REGSAM sam, HKEY *out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    struct regop op;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, sub);
    if (!e) {
        if (!regop_exists(&op)) e = ERROR_FILE_NOT_FOUND;
        else {
            *out = open_key_new(op.ref.root, op.ref.path, sam);
            op.ref.path = NULL;
        }
        regop_end(&op);
    }
    reg_unlock();
    return e;
}

API LSTATUS RegOpenKeyExW(HKEY h, const WCHAR *sub, DWORD options, REGSAM sam, HKEY *out)
{
    (void)options;
    return open_key_w(h, sub, sam, out);
}

API LSTATUS RegOpenKeyExA(HKEY h, const char *sub, DWORD options, REGSAM sam, HKEY *out)
{
    WCHAR *w = a2w(sub);
    LSTATUS e = RegOpenKeyExW(h, w, options, sam, out);
    free(w);
    return e;
}

/* RegOpenKey: an empty or missing subkey returns the same handle. */
API LSTATUS RegOpenKeyW(HKEY h, const WCHAR *sub, HKEY *out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    if (!sub || !*sub) {
        struct keyref r;
        reg_lock();
        LSTATUS e = keyref_get(h, NULL, &r);
        reg_unlock();
        if (e) return e;
        free(r.path);
        *out = h;
        return ERROR_SUCCESS;
    }
    return open_key_w(h, sub, MAXIMUM_ALLOWED, out);
}

API LSTATUS RegOpenKeyA(HKEY h, const char *sub, HKEY *out)
{
    WCHAR *w = a2w(sub);
    LSTATUS e = RegOpenKeyW(h, w, out);
    free(w);
    return e;
}

API LSTATUS RegOpenKeyTransactedW(HKEY h, const WCHAR *sub, DWORD options, REGSAM sam, HKEY *out,
                                  HANDLE tx, void *reserved)
{
    (void)tx; (void)reserved;       /* no transactions: applied at once */
    return RegOpenKeyExW(h, sub, options, sam, out);
}

API LSTATUS RegOpenKeyTransactedA(HKEY h, const char *sub, DWORD options, REGSAM sam, HKEY *out,
                                  HANDLE tx, void *reserved)
{
    (void)tx; (void)reserved;
    return RegOpenKeyExA(h, sub, options, sam, out);
}

API LSTATUS RegCreateKeyExW(HKEY h, const WCHAR *sub, DWORD reserved, WCHAR *klass, DWORD options,
                            REGSAM sam, void *sa, HKEY *out, DWORD *disp)
{
    (void)reserved; (void)sa;
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    /* Windows rejects a subkey that starts with a separator. */
    if (sub && *sub == '\\') return ERROR_BAD_PATHNAME;
    struct regop op;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, sub);
    if (e) { reg_unlock(); return e; }
    if (regop_exists(&op)) {
        if (disp) *disp = REG_OPENED_EXISTING_KEY;
    } else if (!op.nc) {
        e = ERROR_ACCESS_DENIED;        /* nothing can be created there */
    } else if (!sam_allows(op.ref.sam, KEY_CREATE_SUB_KEY)) {
        e = ERROR_ACCESS_DENIED;
    } else {
        int t = regop_target(&op);
        struct reg_tx tx;
        tx_begin(&tx, op.c[t].h);
        /* The hive may have been re-read: walk again from its root. */
        struct reg_key *k = op.c[t].h->root;
        const WCHAR *p = op.c[t].path, *c;
        size_t n;
        int created = 0, vol = 0;
        while (!e && path_next(&p, &c, &n)) {
            struct reg_key *s = key_child(k, c, n);
            if (!s) {
                if (key_in_volatile(k) && !(options & REG_OPTION_VOLATILE)) { e = ERROR_CHILD_MUST_BE_VOLATILE; break; }
                s = key_add_child(k, c, n);
                s->volatile_key = (options & REG_OPTION_VOLATILE) != 0;
                key_touch(k);
                created = 1;
            }
            k = s;
        }
        if (!e && created && klass && *klass) { free(k->klass); k->klass = wdup(klass); }
        if (!e) vol = key_in_volatile(k);
        tx_end(&tx, !e && created && !vol);
        if (!e && disp) *disp = created ? REG_CREATED_NEW_KEY : REG_OPENED_EXISTING_KEY;
    }
    if (!e) {
        *out = open_key_new(op.ref.root, op.ref.path, sam);
        op.ref.path = NULL;
    }
    regop_end(&op);
    reg_unlock();
    return e;
}

API LSTATUS RegCreateKeyExA(HKEY h, const char *sub, DWORD reserved, char *klass, DWORD options,
                            REGSAM sam, void *sa, HKEY *out, DWORD *disp)
{
    WCHAR *ws = a2w(sub), *wc = a2w(klass);
    LSTATUS e = RegCreateKeyExW(h, ws, reserved, wc, options, sam, sa, out, disp);
    free(ws); free(wc);
    return e;
}

API LSTATUS RegCreateKeyW(HKEY h, const WCHAR *sub, HKEY *out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    if (!sub || !*sub) return RegOpenKeyW(h, sub, out);
    return RegCreateKeyExW(h, sub, 0, NULL, 0, MAXIMUM_ALLOWED, NULL, out, NULL);
}

API LSTATUS RegCreateKeyA(HKEY h, const char *sub, HKEY *out)
{
    WCHAR *w = a2w(sub);
    LSTATUS e = RegCreateKeyW(h, w, out);
    free(w);
    return e;
}

API LSTATUS RegCreateKeyTransactedW(HKEY h, const WCHAR *sub, DWORD reserved, WCHAR *klass, DWORD options,
                                    REGSAM sam, void *sa, HKEY *out, DWORD *disp, HANDLE tx, void *ext)
{
    (void)tx; (void)ext;
    return RegCreateKeyExW(h, sub, reserved, klass, options, sam, sa, out, disp);
}

API LSTATUS RegCreateKeyTransactedA(HKEY h, const char *sub, DWORD reserved, char *klass, DWORD options,
                                    REGSAM sam, void *sa, HKEY *out, DWORD *disp, HANDLE tx, void *ext)
{
    (void)tx; (void)ext;
    return RegCreateKeyExA(h, sub, reserved, klass, options, sam, sa, out, disp);
}

API LSTATUS RegCloseKey(HKEY h)
{
    if (predef_root(h) >= 0) return ERROR_SUCCESS;
    reg_lock();
    struct open_key *k = (struct open_key *)h;
    int ok = k && obj_remove(k, OBJ_REGKEY);
    if (ok) { k->magic = 0; free(k->path); free(k); }
    reg_unlock();
    return ok ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

/* Delete one key that has no subkeys. */
static LSTATUS delete_key_w(HKEY h, const WCHAR *sub)
{
    if (!sub) return ERROR_INVALID_PARAMETER;
    struct regop op;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, sub);
    if (e) { reg_unlock(); return e; }
    if (!op.nc || !*op.ref.path) e = op.virt || !*op.ref.path ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND;
    else if (!regop_exists(&op)) e = ERROR_FILE_NOT_FOUND;
    else {
        int t = regop_target(&op);
        struct reg_tx tx;
        tx_begin(&tx, op.c[t].h);
        struct reg_key *k = cand_key(&op.c[t]);
        int save = 0;
        if (!k) e = ERROR_FILE_NOT_FOUND;
        else if (!k->parent) e = ERROR_ACCESS_DENIED;
        else if (k->nsub) e = ERROR_ACCESS_DENIED;     /* Windows refuses keys with subkeys */
        else {
            struct reg_key *parent = k->parent;
            save = !key_in_volatile(k);
            key_detach(k);
            key_free(k);
            key_touch(parent);
        }
        tx_end(&tx, save);
    }
    regop_end(&op);
    reg_unlock();
    return e;
}

API LSTATUS RegDeleteKeyW(HKEY h, const WCHAR *sub) { return delete_key_w(h, sub); }

API LSTATUS RegDeleteKeyA(HKEY h, const char *sub)
{
    if (!sub) return ERROR_INVALID_PARAMETER;
    WCHAR *w = a2w(sub);
    LSTATUS e = delete_key_w(h, w);
    free(w);
    return e;
}

API LSTATUS RegDeleteKeyExW(HKEY h, const WCHAR *sub, REGSAM sam, DWORD reserved)
{
    (void)sam; (void)reserved;      /* KEY_WOW64_*: one registry view */
    return delete_key_w(h, sub);
}

API LSTATUS RegDeleteKeyExA(HKEY h, const char *sub, REGSAM sam, DWORD reserved)
{
    (void)sam; (void)reserved;
    return RegDeleteKeyA(h, sub);
}

API LSTATUS RegDeleteKeyTransactedW(HKEY h, const WCHAR *sub, REGSAM sam, DWORD reserved, HANDLE tx, void *ext)
{
    (void)tx; (void)ext;
    return RegDeleteKeyExW(h, sub, sam, reserved);
}

API LSTATUS RegDeleteKeyTransactedA(HKEY h, const char *sub, REGSAM sam, DWORD reserved, HANDLE tx, void *ext)
{
    (void)tx; (void)ext;
    return RegDeleteKeyExA(h, sub, sam, reserved);
}

/* RegDeleteTree: with a subkey, delete it and everything below; without,
 * delete the values and subkeys of hKey itself. */
API LSTATUS RegDeleteTreeW(HKEY h, const WCHAR *sub)
{
    struct regop op;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, sub);
    if (e) { reg_unlock(); return e; }
    int whole = sub && *sub;
    if (!regop_exists(&op)) e = ERROR_FILE_NOT_FOUND;
    else if (whole && (!op.nc || !*op.ref.path)) e = ERROR_ACCESS_DENIED;
    else {
        for (int i = 0; i < op.nc; i++) {
            struct reg_tx tx;
            tx_begin(&tx, op.c[i].h);
            struct reg_key *k = cand_key(&op.c[i]);
            int save = 0;
            if (k) {
                save = !key_in_volatile(k);
                if (whole && k->parent) {
                    struct reg_key *parent = k->parent;
                    key_detach(k);
                    key_free(k);
                    key_touch(parent);
                } else {
                    key_clear(k);
                    key_touch(k);
                }
            }
            tx_end(&tx, save);
        }
    }
    regop_end(&op);
    reg_unlock();
    return e;
}

API LSTATUS RegDeleteTreeA(HKEY h, const char *sub)
{
    WCHAR *w = a2w(sub);
    LSTATUS e = RegDeleteTreeW(h, w);
    free(w);
    return e;
}

/* ================================================================== */
/* Registry API: values                                                 */
/* ================================================================== */

/* Fetch a value: its type and a malloc'd copy of its data. */
static LSTATUS reg_fetch(HKEY h, const WCHAR *sub, const WCHAR *name, DWORD *type, uint8_t **data, DWORD *size)
{
    struct regop op;
    *data = NULL;
    *size = 0;
    *type = REG_NONE;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, sub);
    if (e) { reg_unlock(); return e; }
    if (!regop_exists(&op)) e = (sub && *sub) ? ERROR_FILE_NOT_FOUND : ERROR_KEY_DELETED;
    else {
        const struct reg_value *v = NULL;
        size_t nl = name ? lsiw_wcslen(name) : 0;
        for (int i = 0; i < op.nc && !v; i++) {
            struct reg_key *k = cand_key(&op.c[i]);
            if (k) v = key_value(k, name ? name : (const WCHAR *)u"", nl);
        }
        if (!v) e = ERROR_FILE_NOT_FOUND;
        else {
            *type = v->type;
            *size = v->size;
            *data = xmalloc(v->size);
            if (v->size) memcpy(*data, v->data, v->size);
        }
    }
    regop_end(&op);
    reg_unlock();
    return e;
}

/* Store a value in hKey\sub, which must exist. */
static LSTATUS reg_store(HKEY h, const WCHAR *sub, const WCHAR *name, DWORD type,
                         const void *data, DWORD size)
{
    if (name && lsiw_wcslen(name) > 16383) return ERROR_INVALID_PARAMETER;
    struct regop op;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, sub);
    if (e) { reg_unlock(); return e; }
    if (!sam_allows(op.ref.sam, KEY_SET_VALUE)) e = ERROR_ACCESS_DENIED;
    else if (!op.nc) e = op.virt ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND;
    else if (!regop_exists(&op)) e = ERROR_KEY_DELETED;
    else {
        int t = regop_target(&op);
        struct reg_tx tx;
        tx_begin(&tx, op.c[t].h);
        int made = 0;
        struct reg_key *k = key_make_path(op.c[t].h->root, op.c[t].path, 0, &made);
        if (made && key_in_volatile(k->parent) && !k->volatile_key) k->volatile_key = 1;
        key_set_value(k, name ? name : (const WCHAR *)u"", name ? lsiw_wcslen(name) : 0, type, data, size);
        key_touch(k);
        tx_end(&tx, !key_in_volatile(k));
    }
    regop_end(&op);
    reg_unlock();
    return e;
}

API LSTATUS RegQueryValueExW(HKEY h, const WCHAR *name, DWORD *reserved, DWORD *type, BYTE *data, DWORD *cb)
{
    (void)reserved;
    if (data && !cb) return ERROR_INVALID_PARAMETER;
    DWORD t, size;
    uint8_t *d;
    LSTATUS e = reg_fetch(h, NULL, name, &t, &d, &size);
    if (!e) {
        if (type) *type = t;
        e = copy_out(d, size, data, cb);
    }
    free(d);
    return e;
}

API LSTATUS RegQueryValueExA(HKEY h, const char *name, DWORD *reserved, DWORD *type, BYTE *data, DWORD *cb)
{
    (void)reserved;
    if (data && !cb) return ERROR_INVALID_PARAMETER;
    WCHAR *wn = a2w(name);
    DWORD t, size;
    uint8_t *d;
    LSTATUS e = reg_fetch(h, NULL, wn, &t, &d, &size);
    free(wn);
    if (!e) {
        if (type) *type = t;
        if (is_string_type(t)) {
            DWORD asz;
            uint8_t *a = strdata_w2a(d, size, &asz);
            free(d);
            d = a;
            size = asz;
        }
        e = copy_out(d, size, data, cb);
    }
    free(d);
    return e;
}

API LSTATUS RegSetValueExW(HKEY h, const WCHAR *name, DWORD reserved, DWORD type, const BYTE *data, DWORD cb)
{
    (void)reserved;
    if (!data && cb) return ERROR_NOACCESS;
    return reg_store(h, NULL, name, type, data, cb);
}

API LSTATUS RegSetValueExA(HKEY h, const char *name, DWORD reserved, DWORD type, const BYTE *data, DWORD cb)
{
    (void)reserved;
    if (!data && cb) return ERROR_NOACCESS;
    WCHAR *wn = a2w(name);
    LSTATUS e;
    if (is_string_type(type) && data) {
        DWORD wsz;
        uint8_t *w = strdata_a2w(data, cb, &wsz);
        e = reg_store(h, NULL, wn, type, w, wsz);
        free(w);
    } else {
        e = reg_store(h, NULL, wn, type, data, cb);
    }
    free(wn);
    return e;
}

/* RegSetKeyValue creates the subkey when it does not exist. */
API LSTATUS RegSetKeyValueW(HKEY h, const WCHAR *sub, const WCHAR *name, DWORD type, const void *data, DWORD cb)
{
    if (!data && cb) return ERROR_NOACCESS;
    if (!sub || !*sub) return reg_store(h, NULL, name, type, data, cb);
    HKEY k;
    LSTATUS e = RegCreateKeyExW(h, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    if (e) return e;
    e = reg_store(k, NULL, name, type, data, cb);
    RegCloseKey(k);
    return e;
}

API LSTATUS RegSetKeyValueA(HKEY h, const char *sub, const char *name, DWORD type, const void *data, DWORD cb)
{
    if (!data && cb) return ERROR_NOACCESS;
    HKEY k = h;
    LSTATUS e = ERROR_SUCCESS;
    if (sub && *sub) e = RegCreateKeyExA(h, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    if (e) return e;
    e = RegSetValueExA(k, name, 0, type, data, cb);
    if (k != h) RegCloseKey(k);
    return e;
}

/* RegSetValue (16-bit era): sets the default value of a subkey, which is
 * created if needed; only REG_SZ, and the length comes from the string. */
API LSTATUS RegSetValueW(HKEY h, const WCHAR *sub, DWORD type, const WCHAR *data, DWORD cb)
{
    (void)cb;
    if (type != REG_SZ || !data) return ERROR_INVALID_PARAMETER;
    DWORD size = (DWORD)((lsiw_wcslen(data) + 1) * sizeof(WCHAR));
    return RegSetKeyValueW(h, sub, NULL, REG_SZ, data, size);
}

API LSTATUS RegSetValueA(HKEY h, const char *sub, DWORD type, const char *data, DWORD cb)
{
    (void)cb;
    if (type != REG_SZ || !data) return ERROR_INVALID_PARAMETER;
    WCHAR *ws = a2w(sub), *wd = a2w(data);
    LSTATUS e = RegSetValueW(h, ws, type, wd, 0);
    free(ws); free(wd);
    return e;
}

/* RegQueryValue (16-bit era): the default value of a subkey as a string;
 * a key without one yields an empty string. */
static LSTATUS query_value_old(HKEY h, const WCHAR *sub, int ansi, void *data, LONG *cb)
{
    if (data && !cb) return ERROR_INVALID_PARAMETER;
    DWORD t, size;
    uint8_t *d;
    LSTATUS e = reg_fetch(h, sub, NULL, &t, &d, &size);
    if (e == ERROR_FILE_NOT_FOUND && sub && *sub) {
        HKEY k;
        if (RegOpenKeyExW(h, sub, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) { RegCloseKey(k); e = ERROR_SUCCESS; }
    } else if (e == ERROR_FILE_NOT_FOUND && (!sub || !*sub)) {
        e = ERROR_SUCCESS;
    }
    if (e) { free(d); return e; }
    if (!d) { t = REG_SZ; size = 2; d = xcalloc(1, 2); }
    if (t != REG_SZ && t != REG_EXPAND_SZ) { free(d); return ERROR_INVALID_DATA; }
    if (ansi) {
        DWORD asz;
        uint8_t *a = strdata_w2a(d, size, &asz);
        free(d);
        d = a;
        size = asz;
    }
    DWORD c = cb ? (DWORD)*cb : 0;
    e = copy_out(d, size, data, cb ? &c : NULL);
    if (cb) *cb = (LONG)c;
    free(d);
    return e;
}

API LSTATUS RegQueryValueW(HKEY h, const WCHAR *sub, WCHAR *data, LONG *cb) { return query_value_old(h, sub, 0, data, cb); }

API LSTATUS RegQueryValueA(HKEY h, const char *sub, char *data, LONG *cb)
{
    WCHAR *w = a2w(sub);
    LSTATUS e = query_value_old(h, w, 1, data, cb);
    free(w);
    return e;
}

static LSTATUS delete_value_w(HKEY h, const WCHAR *sub, const WCHAR *name, int check_access)
{
    struct regop op;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, sub);
    if (e) { reg_unlock(); return e; }
    if (check_access && !sam_allows(op.ref.sam, KEY_SET_VALUE)) e = ERROR_ACCESS_DENIED;
    else if (!regop_exists(&op)) e = (sub && *sub) ? ERROR_FILE_NOT_FOUND : ERROR_KEY_DELETED;
    else {
        size_t nl = name ? lsiw_wcslen(name) : 0;
        const WCHAR *nm = name ? name : (const WCHAR *)u"";
        int t = -1;
        for (int i = 0; i < op.nc && t < 0; i++) {
            struct reg_key *k = cand_key(&op.c[i]);
            if (k && key_value(k, nm, nl)) t = i;
        }
        if (t < 0) e = ERROR_FILE_NOT_FOUND;
        else {
            struct reg_tx tx;
            tx_begin(&tx, op.c[t].h);
            struct reg_key *k = cand_key(&op.c[t]);
            int save = 0;
            if (k && key_delete_value(k, nm, nl)) { key_touch(k); save = !key_in_volatile(k); }
            else e = ERROR_FILE_NOT_FOUND;
            tx_end(&tx, save);
        }
    }
    regop_end(&op);
    reg_unlock();
    return e;
}

API LSTATUS RegDeleteValueW(HKEY h, const WCHAR *name) { return delete_value_w(h, NULL, name, 1); }

API LSTATUS RegDeleteValueA(HKEY h, const char *name)
{
    WCHAR *w = a2w(name);
    LSTATUS e = delete_value_w(h, NULL, w, 1);
    free(w);
    return e;
}

API LSTATUS RegDeleteKeyValueW(HKEY h, const WCHAR *sub, const WCHAR *name) { return delete_value_w(h, sub, name, 0); }

API LSTATUS RegDeleteKeyValueA(HKEY h, const char *sub, const char *name)
{
    WCHAR *ws = a2w(sub), *wn = a2w(name);
    LSTATUS e = delete_value_w(h, ws, wn, 0);
    free(ws); free(wn);
    return e;
}

/* ---- RegGetValue ---- */

#define RRF_RT_REG_NONE      0x00000001u
#define RRF_RT_REG_SZ        0x00000002u
#define RRF_RT_REG_EXPAND_SZ 0x00000004u
#define RRF_RT_REG_BINARY    0x00000008u
#define RRF_RT_REG_DWORD     0x00000010u
#define RRF_RT_REG_MULTI_SZ  0x00000020u
#define RRF_RT_REG_QWORD     0x00000040u
#define RRF_RT_DWORD         0x00000018u
#define RRF_RT_QWORD         0x00000048u
#define RRF_RT_ANY           0x0000ffffu
#define RRF_NOEXPAND         0x10000000u
#define RRF_ZEROONFAILURE    0x20000000u

/* Does a value of this type and size pass the RRF_RT_* filter? A
 * REG_BINARY value passes RRF_RT_DWORD / RRF_RT_QWORD only when it is 4 /
 * 8 bytes long. */
static int rrf_type_ok(DWORD t, DWORD size, DWORD flags)
{
    DWORD rt = flags & RRF_RT_ANY;
    if (rt == RRF_RT_ANY) return 1;
    switch (t) {
    case REG_NONE:      return (rt & RRF_RT_REG_NONE) != 0;
    case REG_SZ:        return (rt & RRF_RT_REG_SZ) != 0;
    case REG_EXPAND_SZ: return (rt & RRF_RT_REG_EXPAND_SZ) != 0;
    case REG_MULTI_SZ:  return (rt & RRF_RT_REG_MULTI_SZ) != 0;
    case REG_DWORD:     return (rt & RRF_RT_REG_DWORD) != 0;
    case REG_QWORD:     return (rt & RRF_RT_REG_QWORD) != 0;
    case REG_BINARY: {
        if (!(rt & RRF_RT_REG_BINARY)) return 0;
        int dw = (rt & RRF_RT_DWORD) == RRF_RT_DWORD, qw = (rt & RRF_RT_QWORD) == RRF_RT_QWORD;
        if (!dw && !qw) return 1;
        return (dw && size == 4) || (qw && size == 8);
    }
    }
    return 0;
}

static LSTATUS get_value(HKEY h, const WCHAR *sub, const WCHAR *name, DWORD flags, DWORD *type,
                         void *data, DWORD *cb, int ansi)
{
    DWORD inbuf = cb ? *cb : 0;
    LSTATUS e;
    if (!(flags & RRF_RT_ANY) || (data && !cb)) e = ERROR_INVALID_PARAMETER;
    /* Asking for unexpanded strings only makes sense with RRF_NOEXPAND. */
    else if ((flags & RRF_RT_ANY) == RRF_RT_REG_EXPAND_SZ && !(flags & RRF_NOEXPAND)) e = ERROR_INVALID_PARAMETER;
    else {
        DWORD t, size;
        uint8_t *d;
        e = reg_fetch(h, sub, name, &t, &d, &size);
        if (!e && t == REG_EXPAND_SZ && !(flags & RRF_NOEXPAND)) {
            WCHAR *w = wdup_n((const WCHAR *)d, size / 2);
            WCHAR *x = expand_env_w(w);
            free(w);
            free(d);
            size = (DWORD)((lsiw_wcslen(x) + 1) * sizeof(WCHAR));
            d = (uint8_t *)x;
            t = REG_SZ;
        }
        if (!e && !rrf_type_ok(t, size, flags)) {
            e = ERROR_UNSUPPORTED_TYPE;
            if (type) *type = t;
            if (cb) *cb = size;
        }
        if (!e && ansi && is_string_type(t)) {
            DWORD asz;
            uint8_t *a = strdata_w2a(d, size, &asz);
            free(d);
            d = a;
            size = asz;
        }
        if (!e) {
            if (type) *type = t;
            e = copy_out(d, size, data, cb);
        }
        free(d);
    }
    if (e && (flags & RRF_ZEROONFAILURE) && data && inbuf) memset(data, 0, inbuf);
    return e;
}

API LSTATUS RegGetValueW(HKEY h, const WCHAR *sub, const WCHAR *name, DWORD flags, DWORD *type, void *data, DWORD *cb)
{
    return get_value(h, sub, name, flags, type, data, cb, 0);
}

API LSTATUS RegGetValueA(HKEY h, const char *sub, const char *name, DWORD flags, DWORD *type, void *data, DWORD *cb)
{
    WCHAR *ws = a2w(sub), *wn = a2w(name);
    LSTATUS e = get_value(h, ws, wn, flags, type, data, cb, 1);
    free(ws); free(wn);
    return e;
}

/* ---- enumeration ---- */

static LSTATUS enum_key(HKEY h, DWORD index, void *name, DWORD *cch, void *klass, DWORD *cchClass,
                        FILETIME *ft, int ansi)
{
    if (!name || !cch) return ERROR_INVALID_PARAMETER;
    struct regop op;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, NULL);
    if (e) { reg_unlock(); return e; }
    if (!regop_exists(&op)) e = ERROR_KEY_DELETED;
    else {
        struct sub_ent *v;
        size_t n = merged_subkeys(&op, &v);
        if (index >= n) e = ERROR_NO_MORE_ITEMS;
        else {
            const struct sub_ent *s = &v[index];
            e = ansi ? name_out_a(s->name, s->nlen, name, cch) : name_out_w(s->name, s->nlen, name, cch);
            if (!e && cchClass) {
                const WCHAR *kc = s->k && s->k->klass ? s->k->klass : (const WCHAR *)u"";
                size_t kn = lsiw_wcslen(kc);
                if (klass) e = ansi ? name_out_a(kc, kn, klass, cchClass) : name_out_w(kc, kn, klass, cchClass);
                else if (ansi) { size_t na; free(w2a_n(kc, kn, &na)); *cchClass = (DWORD)na; }
                else *cchClass = (DWORD)kn;
            }
            if (!e && ft) put_filetime(ft, s->k ? s->k->mtime : now_filetime());
        }
        free(v);
    }
    regop_end(&op);
    reg_unlock();
    return e;
}

API LSTATUS RegEnumKeyExW(HKEY h, DWORD index, WCHAR *name, DWORD *cch, DWORD *reserved, WCHAR *klass,
                          DWORD *cchClass, FILETIME *ft)
{
    (void)reserved;
    return enum_key(h, index, name, cch, klass, cchClass, ft, 0);
}

API LSTATUS RegEnumKeyExA(HKEY h, DWORD index, char *name, DWORD *cch, DWORD *reserved, char *klass,
                          DWORD *cchClass, FILETIME *ft)
{
    (void)reserved;
    return enum_key(h, index, name, cch, klass, cchClass, ft, 1);
}

API LSTATUS RegEnumKeyW(HKEY h, DWORD index, WCHAR *name, DWORD cch)
{
    return enum_key(h, index, name, &cch, NULL, NULL, NULL, 0);
}

API LSTATUS RegEnumKeyA(HKEY h, DWORD index, char *name, DWORD cch)
{
    return enum_key(h, index, name, &cch, NULL, NULL, NULL, 1);
}

static LSTATUS enum_value(HKEY h, DWORD index, void *name, DWORD *cch, DWORD *type, BYTE *data, DWORD *cb, int ansi)
{
    if (!name || !cch || (data && !cb)) return ERROR_INVALID_PARAMETER;
    struct regop op;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, NULL);
    if (e) { reg_unlock(); return e; }
    if (!regop_exists(&op)) e = ERROR_KEY_DELETED;
    else {
        const struct reg_value **v;
        size_t n = merged_values(&op, &v);
        if (index >= n) e = ERROR_NO_MORE_ITEMS;
        else {
            const struct reg_value *x = v[index];
            e = ansi ? name_out_a(x->name, x->nlen, name, cch) : name_out_w(x->name, x->nlen, name, cch);
            if (!e) {
                if (type) *type = x->type;
                if (ansi && is_string_type(x->type)) {
                    DWORD asz;
                    uint8_t *a = strdata_w2a(x->data, x->size, &asz);
                    e = copy_out(a, asz, data, cb);
                    free(a);
                } else {
                    e = copy_out(x->data, x->size, data, cb);
                }
            }
        }
        free(v);
    }
    regop_end(&op);
    reg_unlock();
    return e;
}

API LSTATUS RegEnumValueW(HKEY h, DWORD index, WCHAR *name, DWORD *cch, DWORD *reserved, DWORD *type,
                          BYTE *data, DWORD *cb)
{
    (void)reserved;
    return enum_value(h, index, name, cch, type, data, cb, 0);
}

API LSTATUS RegEnumValueA(HKEY h, DWORD index, char *name, DWORD *cch, DWORD *reserved, DWORD *type,
                          BYTE *data, DWORD *cb)
{
    (void)reserved;
    return enum_value(h, index, name, cch, type, data, cb, 1);
}

/* ---- RegQueryInfoKey ---- */

static DWORD min_sd_size(void);

static size_t name_len(const WCHAR *s, size_t n, int ansi)
{
    if (!ansi) return n;
    size_t na;
    free(w2a_n(s, n, &na));
    return na;
}

static LSTATUS query_info(HKEY h, void *klass, DWORD *cchClass, DWORD *nsub, DWORD *maxsub, DWORD *maxclass,
                          DWORD *nval, DWORD *maxvname, DWORD *maxvlen, DWORD *cbsd, FILETIME *ft, int ansi)
{
    if (klass && !cchClass) return ERROR_INVALID_PARAMETER;
    struct regop op;
    reg_lock();
    LSTATUS e = regop_begin(&op, h, NULL);
    if (e) { reg_unlock(); return e; }
    if (!regop_exists(&op)) { regop_end(&op); reg_unlock(); return ERROR_KEY_DELETED; }

    struct sub_ent *s;
    const struct reg_value **v;
    size_t ns = merged_subkeys(&op, &s), nv = merged_values(&op, &v);
    size_t mx_sub = 0, mx_class = 0, mx_vname = 0, mx_vlen = 0;
    uint64_t mt = 0;
    for (size_t i = 0; i < ns; i++) {
        size_t l = name_len(s[i].name, s[i].nlen, ansi);
        if (l > mx_sub) mx_sub = l;
        if (s[i].k && s[i].k->klass) {
            l = name_len(s[i].k->klass, lsiw_wcslen(s[i].k->klass), ansi);
            if (l > mx_class) mx_class = l;
        }
    }
    for (size_t i = 0; i < nv; i++) {
        size_t l = name_len(v[i]->name, v[i]->nlen, ansi);
        if (l > mx_vname) mx_vname = l;
        size_t dl = v[i]->size;
        if (ansi && is_string_type(v[i]->type)) {
            DWORD asz;
            free(strdata_w2a(v[i]->data, v[i]->size, &asz));
            if (asz > dl) dl = asz;
        }
        if (dl > mx_vlen) mx_vlen = dl;
    }
    const WCHAR *kc = NULL;
    for (int i = 0; i < op.nc; i++) {
        struct reg_key *k = cand_key(&op.c[i]);
        if (!k) continue;
        if (k->mtime > mt) mt = k->mtime;
        if (!kc && k->klass) kc = k->klass;
    }
    if (!mt) mt = now_filetime();
    if (cchClass) {
        const WCHAR *c = kc ? kc : (const WCHAR *)u"";
        size_t cn = lsiw_wcslen(c);
        if (klass) {
            e = ansi ? name_out_a(c, cn, klass, cchClass) : name_out_w(c, cn, klass, cchClass);
            if (e == ERROR_MORE_DATA) *cchClass = (DWORD)name_len(c, cn, ansi);
        } else {
            *cchClass = (DWORD)name_len(c, cn, ansi);
        }
    }
    if (nsub) *nsub = (DWORD)ns;
    if (maxsub) *maxsub = (DWORD)mx_sub;
    if (maxclass) *maxclass = (DWORD)mx_class;
    if (nval) *nval = (DWORD)nv;
    if (maxvname) *maxvname = (DWORD)mx_vname;
    if (maxvlen) *maxvlen = (DWORD)mx_vlen;
    if (cbsd) *cbsd = min_sd_size();
    put_filetime(ft, mt);
    free(s);
    free(v);
    regop_end(&op);
    reg_unlock();
    return e;
}

API LSTATUS RegQueryInfoKeyW(HKEY h, WCHAR *klass, DWORD *cchClass, DWORD *reserved, DWORD *nsub, DWORD *maxsub,
                             DWORD *maxclass, DWORD *nval, DWORD *maxvname, DWORD *maxvlen, DWORD *cbsd, FILETIME *ft)
{
    (void)reserved;
    return query_info(h, klass, cchClass, nsub, maxsub, maxclass, nval, maxvname, maxvlen, cbsd, ft, 0);
}

API LSTATUS RegQueryInfoKeyA(HKEY h, char *klass, DWORD *cchClass, DWORD *reserved, DWORD *nsub, DWORD *maxsub,
                             DWORD *maxclass, DWORD *nval, DWORD *maxvname, DWORD *maxvlen, DWORD *cbsd, FILETIME *ft)
{
    (void)reserved;
    return query_info(h, klass, cchClass, nsub, maxsub, maxclass, nval, maxvname, maxvlen, cbsd, ft, 1);
}

/* ---- RegQueryMultipleValues ---- */

/* VALENTA / VALENTW: the name pointer differs only in type. */
struct valent { void *name; DWORD len; ULONG_PTR ptr; DWORD type; };

static LSTATUS query_multiple(HKEY h, struct valent *list, DWORD num, char *buf, DWORD *total, int ansi)
{
    if (!list || !total) return ERROR_INVALID_PARAMETER;
    uint8_t **d = xcalloc(num ? num : 1, sizeof *d);
    LSTATUS e = ERROR_SUCCESS;
    uint64_t need = 0;
    for (DWORD i = 0; i < num && !e; i++) {
        WCHAR *nm = ansi ? a2w(list[i].name) : wdup(list[i].name);
        DWORD t, size;
        e = reg_fetch(h, NULL, nm, &t, &d[i], &size);
        free(nm);
        if (e) break;
        if (ansi && is_string_type(t)) {
            DWORD asz;
            uint8_t *a = strdata_w2a(d[i], size, &asz);
            free(d[i]);
            d[i] = a;
            size = asz;
        }
        list[i].len = size;
        list[i].type = t;
        list[i].ptr = 0;
        need += size;
    }
    if (!e && need > 0xFFFFFFFFu) e = ERROR_NOT_ENOUGH_MEMORY;
    if (!e) {
        if (!buf || *total < need) {
            *total = (DWORD)need;
            e = ERROR_MORE_DATA;
        } else {
            size_t off = 0;
            for (DWORD i = 0; i < num; i++) {
                if (list[i].len) memcpy(buf + off, d[i], list[i].len);
                list[i].ptr = (ULONG_PTR)(buf + off);
                off += list[i].len;
            }
            *total = (DWORD)need;
        }
    }
    for (DWORD i = 0; i < num; i++) free(d[i]);
    free(d);
    return e;
}

API LSTATUS RegQueryMultipleValuesW(HKEY h, void *list, DWORD num, WCHAR *buf, DWORD *total)
{
    return query_multiple(h, list, num, (char *)buf, total, 0);
}

API LSTATUS RegQueryMultipleValuesA(HKEY h, void *list, DWORD num, char *buf, DWORD *total)
{
    return query_multiple(h, list, num, buf, total, 1);
}

/* ---- flushing and change notification ---- */

static LSTATUS check_key(HKEY h)
{
    struct keyref r;
    reg_lock();
    LSTATUS e = keyref_get(h, NULL, &r);
    reg_unlock();
    if (!e) free(r.path);
    return e;
}

/* Every change is already on disk when the modifying call returns. */
API LSTATUS RegFlushKey(HKEY h) { return check_key(h); }

/* Change notification. Any change to a hive file (made by any process,
 * seen through inotify on the registry folder) or to this process's
 * in-memory registry counts as a change: notifications are not filtered by
 * key, subtree or REG_NOTIFY_CHANGE_* class, so a waiter may wake for an
 * unrelated change, which Windows also allows programs to tolerate since
 * they re-read the key afterwards. */
static pthread_mutex_t g_watch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_watch_cond = PTHREAD_COND_INITIALIZER;
static unsigned long   g_watch_seq;
static int             g_watch_started;

static void reg_watchers_poke(void)
{
    pthread_mutex_lock(&g_watch_lock);
    g_watch_seq++;
    pthread_cond_broadcast(&g_watch_cond);
    pthread_mutex_unlock(&g_watch_lock);
}

static void *watch_inotify_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        /* Only completed hive files matter, not lock or temporary files. */
        int relevant = 0;
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *ev = (struct inotify_event *)p;
            size_t l = ev->len ? strlen(ev->name) : 0;
            if (l > 4 && !strcmp(ev->name + l - 4, ".reg")) relevant = 1;
            p += sizeof *ev + ev->len;
        }
        if (relevant) reg_watchers_poke();
    }
    close(fd);
    return NULL;
}

static void watch_start(void)
{
    pthread_mutex_lock(&g_watch_lock);
    if (!g_watch_started) {
        g_watch_started = 1;
        lsiw_mkdirs(reg_dir(), 0700);
        int fd = inotify_init1(IN_CLOEXEC);
        if (fd >= 0 && inotify_add_watch(fd, reg_dir(), IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE) >= 0) {
            pthread_t t;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&t, &at, watch_inotify_thread, (void *)(intptr_t)fd) != 0) close(fd);
            pthread_attr_destroy(&at);
        } else if (fd >= 0) {
            close(fd);
        }
    }
    pthread_mutex_unlock(&g_watch_lock);
}

static void signal_event(HANDLE ev)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(ev, H_EVENT);
    if (!o) return;
    lsiw_wait_lock();
    o->signaled = 1;
    lsiw_wait_broadcast();
    lsiw_wait_unlock();
}

struct async_watch { HANDLE event; unsigned long seq; };

static void *async_watch_thread(void *arg)
{
    struct async_watch *w = arg;
    pthread_mutex_lock(&g_watch_lock);
    while (g_watch_seq == w->seq) pthread_cond_wait(&g_watch_cond, &g_watch_lock);
    pthread_mutex_unlock(&g_watch_lock);
    signal_event(w->event);
    free(w);
    return NULL;
}

API LSTATUS RegNotifyChangeKeyValue(HKEY h, BOOL subtree, DWORD filter, HANDLE event, BOOL async)
{
    (void)subtree; (void)filter;
    LSTATUS e = check_key(h);
    if (e) return e;
    if (async && (!event || !lsiw_handle_get(event, H_EVENT))) return ERROR_INVALID_PARAMETER;
    watch_start();
    pthread_mutex_lock(&g_watch_lock);
    unsigned long seq = g_watch_seq;
    pthread_mutex_unlock(&g_watch_lock);
    if (async) {
        struct async_watch *w = xmalloc(sizeof *w);
        w->event = event;
        w->seq = seq;
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        int r = pthread_create(&t, &at, async_watch_thread, w);
        pthread_attr_destroy(&at);
        if (r) { free(w); return ERROR_NOT_ENOUGH_MEMORY; }
        return ERROR_SUCCESS;
    }
    /* Synchronous: block until something changes. */
    pthread_mutex_lock(&g_watch_lock);
    while (g_watch_seq == seq) pthread_cond_wait(&g_watch_cond, &g_watch_lock);
    pthread_mutex_unlock(&g_watch_lock);
    return ERROR_SUCCESS;
}

/* ---- special handles ---- */

API LSTATUS RegOpenCurrentUser(REGSAM sam, HKEY *out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = open_key_new(R_HKCU, wdup_n(NULL, 0), sam);
    return ERROR_SUCCESS;
}

API LSTATUS RegOpenUserClassesRoot(HANDLE token, DWORD options, REGSAM sam, HKEY *out)
{
    (void)token; (void)options;
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = open_key_new(R_HKCU, wdup(CLASSES_W), sam);
    return ERROR_SUCCESS;
}

/* Map a predefined handle onto another key for this process; a NULL new
 * key restores the default mapping. */
API LSTATUS RegOverridePredefKey(HKEY h, HKEY newkey)
{
    int r = predef_root(h);
    if (r < 0) return ERROR_INVALID_HANDLE;
    struct keyref ref = { 0, NULL, 0 };
    reg_lock();
    LSTATUS e = newkey ? keyref_get(newkey, NULL, &ref) : ERROR_SUCCESS;
    if (!e) {
        free(g_override[r].path);
        g_override[r].set = newkey != NULL;
        g_override[r].root = ref.root;
        g_override[r].path = ref.path;
    }
    reg_unlock();
    return e;
}

/* The predefined-handle cache only matters for impersonating services. */
API LSTATUS RegDisablePredefinedCache(void) { return ERROR_SUCCESS; }
API LSTATUS RegDisablePredefinedCacheEx(void) { return ERROR_SUCCESS; }

/* Only the local registry can be reached. */
static LSTATUS connect_registry(int local, HKEY h, HKEY *out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    if (!local) return ERROR_BAD_NETPATH;
    int r = predef_root(h);
    if (r < 0) return ERROR_INVALID_HANDLE;
    *out = open_key_new(r, wdup_n(NULL, 0), KEY_ALL_ACCESS);
    return ERROR_SUCCESS;
}

API LSTATUS RegConnectRegistryW(const WCHAR *machine, HKEY h, HKEY *out) { return connect_registry(is_local_machine_w(machine), h, out); }
API LSTATUS RegConnectRegistryA(const char *machine, HKEY h, HKEY *out) { return connect_registry(is_local_machine_a(machine), h, out); }

API LSTATUS RegConnectRegistryExW(const WCHAR *machine, HKEY h, ULONG flags, HKEY *out)
{
    (void)flags;
    return RegConnectRegistryW(machine, h, out);
}

API LSTATUS RegConnectRegistryExA(const char *machine, HKEY h, ULONG flags, HKEY *out)
{
    (void)flags;
    return RegConnectRegistryA(machine, h, out);
}

/* Merge a detached copy of a subtree into dst. */
static void key_merge_into(struct reg_key *dst, const struct reg_key *src)
{
    for (size_t i = 0; i < src->nval; i++)
        key_set_value(dst, src->val[i].name, src->val[i].nlen, src->val[i].type, src->val[i].data, src->val[i].size);
    for (size_t i = 0; i < src->nsub; i++) {
        const struct reg_key *s = src->sub[i];
        struct reg_key *d = key_child(dst, s->name, s->nlen);
        if (!d) {
            d = key_add_child(dst, s->name, s->nlen);
            d->volatile_key = key_in_volatile(dst);
            if (s->klass) d->klass = wdup(s->klass);
        }
        key_merge_into(d, s);
    }
    key_touch(dst);
}

/* RegCopyTree: copy the values and subkeys of src\sub (not the key
 * itself) into dst. */
API LSTATUS RegCopyTreeW(HKEY src, const WCHAR *sub, HKEY dst)
{
    struct regop so, dop;
    reg_lock();
    LSTATUS e = regop_begin(&so, src, sub);
    if (e) { reg_unlock(); return e; }
    if (!regop_exists(&so)) { regop_end(&so); reg_unlock(); return ERROR_FILE_NOT_FOUND; }
    /* Take a private copy first: the destination hive may be re-read (or be
     * the same tree) once its file is locked. Lower half first so the
     * per-user half of HKEY_CLASSES_ROOT wins. */
    struct reg_key *copy = key_new(NULL, 0, NULL);
    for (int i = so.nc - 1; i >= 0; i--) {
        struct reg_key *k = cand_key(&so.c[i]);
        if (k) key_merge_into(copy, k);
    }
    regop_end(&so);
    e = regop_begin(&dop, dst, NULL);
    if (!e) {
        if (!sam_allows(dop.ref.sam, KEY_CREATE_SUB_KEY | KEY_SET_VALUE)) e = ERROR_ACCESS_DENIED;
        else if (!dop.nc) e = ERROR_ACCESS_DENIED;
        else if (!regop_exists(&dop)) e = ERROR_KEY_DELETED;
        else {
            int t = regop_target(&dop);
            struct reg_tx tx;
            tx_begin(&tx, dop.c[t].h);
            struct reg_key *k = key_make_path(dop.c[t].h->root, dop.c[t].path, 0, NULL);
            key_merge_into(k, copy);
            tx_end(&tx, !key_in_volatile(k));
        }
        regop_end(&dop);
    }
    key_free(copy);
    reg_unlock();
    return e;
}

API LSTATUS RegCopyTreeA(HKEY src, const char *sub, HKEY dst)
{
    WCHAR *w = a2w(sub);
    LSTATUS e = RegCopyTreeW(src, w, dst);
    free(w);
    return e;
}

/* Loading, saving and replacing hive files need SeBackupPrivilege /
 * SeRestorePrivilege, which LSIW programs never hold. */
API LSTATUS RegLoadKeyW(HKEY h, const WCHAR *sub, const WCHAR *file) { (void)h; (void)sub; (void)file; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegLoadKeyA(HKEY h, const char *sub, const char *file) { (void)h; (void)sub; (void)file; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegUnLoadKeyW(HKEY h, const WCHAR *sub) { (void)h; (void)sub; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegUnLoadKeyA(HKEY h, const char *sub) { (void)h; (void)sub; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegSaveKeyW(HKEY h, const WCHAR *file, void *sa) { (void)h; (void)file; (void)sa; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegSaveKeyA(HKEY h, const char *file, void *sa) { (void)h; (void)file; (void)sa; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegSaveKeyExW(HKEY h, const WCHAR *file, void *sa, DWORD flags) { (void)h; (void)file; (void)sa; (void)flags; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegSaveKeyExA(HKEY h, const char *file, void *sa, DWORD flags) { (void)h; (void)file; (void)sa; (void)flags; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegRestoreKeyW(HKEY h, const WCHAR *file, DWORD flags) { (void)h; (void)file; (void)flags; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegRestoreKeyA(HKEY h, const char *file, DWORD flags) { (void)h; (void)file; (void)flags; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegReplaceKeyW(HKEY h, const WCHAR *sub, const WCHAR *nf, const WCHAR *of) { (void)h; (void)sub; (void)nf; (void)of; return ERROR_PRIVILEGE_NOT_HELD; }
API LSTATUS RegReplaceKeyA(HKEY h, const char *sub, const char *nf, const char *of) { (void)h; (void)sub; (void)nf; (void)of; return ERROR_PRIVILEGE_NOT_HELD; }

/* Key security: every key is owned by the current user with a NULL DACL
 * (full access for everyone); changes are accepted and ignored. */
static void *build_min_sd(DWORD *len, int heap);

API LSTATUS RegGetKeySecurity(HKEY h, SECURITY_INFORMATION info, void *sd, DWORD *cb)
{
    (void)info;
    LSTATUS e = check_key(h);
    if (e) return e;
    if (!cb) return ERROR_INVALID_PARAMETER;
    DWORD len;
    void *m = build_min_sd(&len, 0);
    if (!sd || *cb < len) e = ERROR_INSUFFICIENT_BUFFER;
    else memcpy(sd, m, len);
    *cb = len;
    free(m);
    return e;
}

API LSTATUS RegSetKeySecurity(HKEY h, SECURITY_INFORMATION info, void *sd)
{
    (void)info;
    if (!sd) return ERROR_INVALID_PARAMETER;
    return check_key(h);
}

/* RegLoadMUIString: plain strings are returned as stored (REG_EXPAND_SZ
 * expanded). Indirect "@file,-id" strings would need the resource loader
 * and fail with ERROR_NOT_SUPPORTED, so callers fall back to their
 * non-localised value. */
#define REG_MUI_STRING_TRUNCATE 1

API LSTATUS RegLoadMUIStringW(HKEY h, const WCHAR *value, WCHAR *out, DWORD cb, DWORD *cbData, DWORD flags,
                              const WCHAR *dir)
{
    (void)dir;
    if (flags & ~(DWORD)REG_MUI_STRING_TRUNCATE) return ERROR_INVALID_PARAMETER;
    if ((cb && !out) || (cb & 1)) return ERROR_INVALID_PARAMETER;
    if ((flags & REG_MUI_STRING_TRUNCATE) && cbData) return ERROR_INVALID_PARAMETER;
    DWORD t, size;
    uint8_t *d;
    LSTATUS e = reg_fetch(h, NULL, value, &t, &d, &size);
    if (e) return e;
    if (t != REG_SZ && t != REG_EXPAND_SZ) { free(d); return ERROR_FILE_NOT_FOUND; }
    WCHAR *s = wdup_n((const WCHAR *)d, size / 2);
    free(d);
    if (s[0] == '@') { free(s); return ERROR_NOT_SUPPORTED; }
    if (t == REG_EXPAND_SZ) { WCHAR *x = expand_env_w(s); free(s); s = x; }
    DWORD need = (DWORD)((lsiw_wcslen(s) + 1) * sizeof(WCHAR));
    if (cbData) *cbData = need;
    if (!out || cb < need) {
        if ((flags & REG_MUI_STRING_TRUNCATE) && out && cb >= sizeof(WCHAR)) {
            memcpy(out, s, cb - sizeof(WCHAR));
            out[cb / sizeof(WCHAR) - 1] = 0;
        } else {
            e = ERROR_MORE_DATA;
        }
    } else {
        memcpy(out, s, need);
    }
    free(s);
    return e;
}

/* Windows does not implement the ANSI version either. */
API LSTATUS RegLoadMUIStringA(HKEY h, const char *value, char *out, DWORD cb, DWORD *cbData, DWORD flags,
                              const char *dir)
{
    (void)h; (void)value; (void)out; (void)cb; (void)cbData; (void)flags; (void)dir;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/* ================================================================== */
/* Security identifiers                                                 */
/* ================================================================== */

/* SID: revision (1), sub-authority count, 48-bit big-endian identifier
 * authority, then the sub-authorities as little-endian DWORDs. */
#define SID_REVISION            1
#define SID_MAX_SUB_AUTHORITIES 15
#define SECURITY_MAX_SID_SIZE   68

typedef struct { BYTE Value[6]; } SID_IDENTIFIER_AUTHORITY;

enum { SidTypeUser = 1, SidTypeGroup, SidTypeDomain, SidTypeAlias, SidTypeWellKnownGroup,
       SidTypeDeletedAccount, SidTypeInvalid, SidTypeUnknown, SidTypeComputer, SidTypeLabel };

static DWORD sid_len(const void *p) { return 8u + 4u * ((const BYTE *)p)[1]; }

static int sid_valid(const void *p)
{
    const BYTE *b = p;
    return b && b[0] == SID_REVISION && b[1] <= SID_MAX_SUB_AUTHORITIES;
}

static uint64_t sid_auth(const void *p)
{
    const BYTE *b = p;
    uint64_t a = 0;
    for (int i = 0; i < 6; i++) a = (a << 8) | b[2 + i];
    return a;
}

static DWORD sid_sub(const void *p, int i)
{
    DWORD v;
    memcpy(&v, (const BYTE *)p + 8 + 4 * i, 4);
    return v;
}

/* Write a SID into out (room for 8 + 4n bytes); returns its length. */
static DWORD sid_build(void *out, uint64_t auth, int n, const DWORD *subs)
{
    BYTE *b = out;
    b[0] = SID_REVISION;
    b[1] = (BYTE)n;
    for (int i = 0; i < 6; i++) b[2 + i] = (BYTE)(auth >> (8 * (5 - i)));
    for (int i = 0; i < n; i++) memcpy(b + 8 + 4 * i, &subs[i], 4);
    return 8u + 4u * (DWORD)n;
}

static int sid_equal(const void *a, const void *b)
{
    return sid_valid(a) && sid_valid(b) && sid_len(a) == sid_len(b) && !memcmp(a, b, sid_len(a));
}

/* The machine's account domain, S-1-5-21-a-b-c with a, b and c taken from
 * the machine id, and the user's RID: 500 (the built-in Administrator) for
 * root, 1000 + uid otherwise. */
static void machine_domain(DWORD out[3])
{
    const uint8_t *m = machine_id();
    for (int i = 0; i < 3; i++)
        out[i] = (DWORD)m[4 * i] | (DWORD)m[4 * i + 1] << 8 | (DWORD)m[4 * i + 2] << 16 | (DWORD)m[4 * i + 3] << 24;
}

static DWORD domain_sid_rid(void *out, DWORD rid)
{
    DWORD subs[5] = { 21, 0, 0, 0, rid };
    machine_domain(subs + 1);
    return sid_build(out, 5, 5, subs);
}

static DWORD user_rid(void) { return getuid() == 0 ? 500u : 1000u + (DWORD)getuid(); }

static const BYTE *user_sid(void)
{
    static BYTE sid[SECURITY_MAX_SID_SIZE];
    static int done;
    if (!done) { domain_sid_rid(sid, user_rid()); done = 1; }
    return sid;
}

static int is_admin(void) { return geteuid() == 0; }

/* "S-1-5-21-..." for a SID; malloc'd. */
static char *sid_to_str(const void *sid)
{
    uint64_t a = sid_auth(sid);
    char *s = a < (1ULL << 32) ? lsiw_strdup_printf("S-1-%llu", (unsigned long long)a)
                               : lsiw_strdup_printf("S-1-0x%012llX", (unsigned long long)a);
    for (int i = 0; i < ((const BYTE *)sid)[1]; i++) {
        char *t = lsiw_strdup_printf("%s-%u", s, (unsigned)sid_sub(sid, i));
        free(s);
        s = t;
    }
    return s;
}

static const char *user_sid_string(void)
{
    static char *s;
    if (!s) s = sid_to_str(user_sid());
    return s;
}

/* Well-known SIDs: WELL_KNOWN_SID_TYPE, SDDL alias, value, account name. */
struct wk_sid {
    int         type;
    const char *alias;
    BYTE        auth, n;
    DWORD       sub[2];
    const char *name, *domain;
    int         use;
};

static const struct wk_sid g_wk[] = {
    {  0, NULL, 0,  1, { 0 },        "NULL SID", "", SidTypeWellKnownGroup },
    {  1, "WD", 1,  1, { 0 },        "Everyone", "", SidTypeWellKnownGroup },
    {  2, NULL, 2,  1, { 0 },        "LOCAL", "", SidTypeWellKnownGroup },
    {  3, "CO", 3,  1, { 0 },        "CREATOR OWNER", "", SidTypeWellKnownGroup },
    {  4, "CG", 3,  1, { 1 },        "CREATOR GROUP", "", SidTypeWellKnownGroup },
    {  5, NULL, 3,  1, { 2 },        "CREATOR OWNER SERVER", "", SidTypeWellKnownGroup },
    {  6, NULL, 3,  1, { 3 },        "CREATOR GROUP SERVER", "", SidTypeWellKnownGroup },
    {  7, NULL, 5,  0, { 0 },        "NT AUTHORITY", "NT AUTHORITY", SidTypeDomain },
    {  8, NULL, 5,  1, { 1 },        "DIALUP", "NT AUTHORITY", SidTypeWellKnownGroup },
    {  9, "NU", 5,  1, { 2 },        "NETWORK", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 10, NULL, 5,  1, { 3 },        "BATCH", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 11, "IU", 5,  1, { 4 },        "INTERACTIVE", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 12, "SU", 5,  1, { 6 },        "SERVICE", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 13, "AN", 5,  1, { 7 },        "ANONYMOUS LOGON", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 14, NULL, 5,  1, { 8 },        "PROXY", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 15, "ED", 5,  1, { 9 },        "ENTERPRISE DOMAIN CONTROLLERS", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 16, "PS", 5,  1, { 10 },       "SELF", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 17, "AU", 5,  1, { 11 },       "Authenticated Users", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 18, "RC", 5,  1, { 12 },       "RESTRICTED", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 19, NULL, 5,  1, { 13 },       "TERMINAL SERVER USER", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 20, NULL, 5,  1, { 14 },       "REMOTE INTERACTIVE LOGON", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 22, "SY", 5,  1, { 18 },       "SYSTEM", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 23, "LS", 5,  1, { 19 },       "LOCAL SERVICE", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 24, "NS", 5,  1, { 20 },       "NETWORK SERVICE", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 25, NULL, 5,  1, { 32 },       "BUILTIN", "BUILTIN", SidTypeDomain },
    { 26, "BA", 5,  2, { 32, 544 },  "Administrators", "BUILTIN", SidTypeAlias },
    { 27, "BU", 5,  2, { 32, 545 },  "Users", "BUILTIN", SidTypeAlias },
    { 28, "BG", 5,  2, { 32, 546 },  "Guests", "BUILTIN", SidTypeAlias },
    { 29, "PU", 5,  2, { 32, 547 },  "Power Users", "BUILTIN", SidTypeAlias },
    { 30, "AO", 5,  2, { 32, 548 },  "Account Operators", "BUILTIN", SidTypeAlias },
    { 31, "SO", 5,  2, { 32, 549 },  "Server Operators", "BUILTIN", SidTypeAlias },
    { 32, "PO", 5,  2, { 32, 550 },  "Print Operators", "BUILTIN", SidTypeAlias },
    { 33, "BO", 5,  2, { 32, 551 },  "Backup Operators", "BUILTIN", SidTypeAlias },
    { 34, "RE", 5,  2, { 32, 552 },  "Replicator", "BUILTIN", SidTypeAlias },
    { 35, "RU", 5,  2, { 32, 554 },  "Pre-Windows 2000 Compatible Access", "BUILTIN", SidTypeAlias },
    { 36, "RD", 5,  2, { 32, 555 },  "Remote Desktop Users", "BUILTIN", SidTypeAlias },
    { 37, "NO", 5,  2, { 32, 556 },  "Network Configuration Operators", "BUILTIN", SidTypeAlias },
    { 51, NULL, 5,  2, { 64, 10 },   "NTLM Authentication", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 52, NULL, 5,  2, { 64, 21 },   "Digest Authentication", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 53, NULL, 5,  2, { 64, 14 },   "SChannel Authentication", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 54, NULL, 5,  1, { 15 },       "This Organization", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 55, NULL, 5,  1, { 1000 },     "Other Organization", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 57, "MU", 5,  2, { 32, 558 },  "Performance Monitor Users", "BUILTIN", SidTypeAlias },
    { 58, "LU", 5,  2, { 32, 559 },  "Performance Log Users", "BUILTIN", SidTypeAlias },
    { 61, NULL, 5,  2, { 32, 562 },  "Distributed COM Users", "BUILTIN", SidTypeAlias },
    { 62, "IS", 5,  2, { 32, 568 },  "IIS_IUSRS", "BUILTIN", SidTypeAlias },
    { 63, NULL, 5,  1, { 17 },       "IUSR", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 64, "CY", 5,  2, { 32, 569 },  "Cryptographic Operators", "BUILTIN", SidTypeAlias },
    { 65, NULL, 16, 1, { 0 },        "Untrusted Mandatory Level", "Mandatory Label", SidTypeLabel },
    { 66, "LW", 16, 1, { 0x1000 },   "Low Mandatory Level", "Mandatory Label", SidTypeLabel },
    { 67, "ME", 16, 1, { 0x2000 },   "Medium Mandatory Level", "Mandatory Label", SidTypeLabel },
    { 68, "HI", 16, 1, { 0x3000 },   "High Mandatory Level", "Mandatory Label", SidTypeLabel },
    { 69, "SI", 16, 1, { 0x4000 },   "System Mandatory Level", "Mandatory Label", SidTypeLabel },
    { 70, "WR", 5,  1, { 33 },       "WRITE RESTRICTED", "NT AUTHORITY", SidTypeWellKnownGroup },
    { 71, "OW", 3,  1, { 4 },        "OWNER RIGHTS", "", SidTypeWellKnownGroup },
    { 76, "ER", 5,  2, { 32, 573 },  "Event Log Readers", "BUILTIN", SidTypeAlias },
    { 78, "CD", 5,  2, { 32, 574 },  "Certificate Service DCOM Access", "BUILTIN", SidTypeAlias },
    { 79, "MP", 16, 1, { 0x2100 },   "Medium Plus Mandatory Level", "Mandatory Label", SidTypeLabel },
    { 81, NULL, 2,  1, { 1 },        "CONSOLE LOGON", "", SidTypeWellKnownGroup },
    { 84, "AC", 15, 2, { 2, 1 },     "ALL APPLICATION PACKAGES", "APPLICATION PACKAGE AUTHORITY", SidTypeWellKnownGroup },
};

/* Well-known accounts of the machine's own domain (relative IDs). */
static const struct { int type; const char *alias; DWORD rid; const char *name; int use; } g_wk_dom[] = {
    { 38, "LA", 500, "Administrator", SidTypeUser },
    { 39, "LG", 501, "Guest", SidTypeUser },
    { 40, NULL, 502, "krbtgt", SidTypeUser },
    { 41, "DA", 512, "Domain Admins", SidTypeGroup },
    { 42, "DU", 513, "None", SidTypeGroup },
    { 43, "DG", 514, "Domain Guests", SidTypeGroup },
    { 44, "DC", 515, "Domain Computers", SidTypeGroup },
    { 45, "DD", 516, "Domain Controllers", SidTypeGroup },
    { 46, "CA", 517, "Cert Publishers", SidTypeGroup },
    { 47, "SA", 518, "Schema Admins", SidTypeGroup },
    { 48, "EA", 519, "Enterprise Admins", SidTypeGroup },
    { 49, "PA", 520, "Group Policy Creator Owners", SidTypeGroup },
    { 50, "RS", 553, "RAS and IAS Servers", SidTypeAlias },
};

static DWORD wk_build(const struct wk_sid *w, void *out) { return sid_build(out, w->auth, w->n, w->sub); }

static const struct wk_sid *wk_find_sid(const void *sid)
{
    BYTE tmp[SECURITY_MAX_SID_SIZE];
    for (size_t i = 0; i < sizeof g_wk / sizeof g_wk[0]; i++) {
        wk_build(&g_wk[i], tmp);
        if (sid_equal(tmp, sid)) return &g_wk[i];
    }
    return NULL;
}

/* Is sid in the machine's account domain? Returns its RID, or -1. */
static long long domain_rid_of(const void *sid)
{
    BYTE dom[SECURITY_MAX_SID_SIZE];
    domain_sid_rid(dom, 0);
    const BYTE *b = sid;
    if (!sid_valid(sid) || b[1] != 5 || sid_auth(sid) != 5) return -1;
    if (memcmp(b + 8, dom + 8, 16)) return -1;
    return (long long)sid_sub(sid, 4);
}

static int is_domain_sid(const void *sid)
{
    BYTE dom[SECURITY_MAX_SID_SIZE];
    DWORD subs[4] = { 21, 0, 0, 0 };
    machine_domain(subs + 1);
    sid_build(dom, 5, 4, subs);
    return sid_equal(dom, sid);
}

/* ---- strings in the caller's encoding (UTF-8 "ANSI" or UTF-16) ---- */

static size_t enc_len(const char *s, int ansi)
{
    return ansi ? strlen(s) : (size_t)lsiw_utf8_to_utf16(s, -1, NULL, 0) - 1;
}

static void enc_copy(const char *s, int ansi, void *buf)
{
    if (ansi) strcpy(buf, s);
    else lsiw_utf8_to_utf16(s, -1, buf, INT_MAX);
}

/* A malloc'd UTF-8 copy of a caller string. */
static char *enc_in(const void *s, int ansi)
{
    if (!s) return NULL;
    return ansi ? xstrdup(s) : w2a(s);
}

/* A copy in lsiw_heap memory (LocalFree-able) in the caller's encoding. */
static void *enc_heap(const char *s, int ansi)
{
    size_t n = enc_len(s, ansi) + 1;
    void *p = lsiw_heap_alloc(n * (ansi ? 1 : sizeof(WCHAR)), 0);
    if (p) enc_copy(s, ansi, p);
    return p;
}

/* ---- accounts ---- */

struct account { BYTE sid[SECURITY_MAX_SID_SIZE]; const char *name, *domain; int use; };

static int account_from_sid(const void *sid, struct account *a)
{
    if (!sid_valid(sid)) return 0;
    memcpy(a->sid, sid, sid_len(sid));
    if (sid_equal(sid, user_sid())) {
        a->name = lsiw_user_name(); a->domain = computer_name(); a->use = SidTypeUser;
        return 1;
    }
    long long rid = domain_rid_of(sid);
    for (size_t i = 0; rid >= 0 && i < sizeof g_wk_dom / sizeof g_wk_dom[0]; i++)
        if (g_wk_dom[i].rid == rid) {
            a->name = g_wk_dom[i].name; a->domain = computer_name(); a->use = g_wk_dom[i].use;
            return 1;
        }
    if (is_domain_sid(sid)) {
        a->name = computer_name(); a->domain = computer_name(); a->use = SidTypeDomain;
        return 1;
    }
    const struct wk_sid *w = wk_find_sid(sid);
    if (w) { a->name = w->name; a->domain = w->domain; a->use = w->use; return 1; }
    return 0;
}

/* Names may be qualified ("NT AUTHORITY\SYSTEM", "MACHINE\user"). */
static int account_from_name(const char *full, struct account *a)
{
    if (!full) return 0;
    const char *bs = strchr(full, '\\');
    char *dom = bs ? lsiw_strdup_printf("%.*s", (int)(bs - full), full) : NULL;
    const char *name = bs ? bs + 1 : full;
    int local = !dom || !strcasecmp(dom, computer_name()) || !strcmp(dom, ".");
    int ok = 0;
    if (local && *name && !strcasecmp(name, lsiw_user_name())) {
        memcpy(a->sid, user_sid(), sid_len(user_sid()));
        a->name = lsiw_user_name(); a->domain = computer_name(); a->use = SidTypeUser;
        ok = 1;
    }
    if (!ok && ((!dom && !strcasecmp(name, computer_name())) || (dom && local && !*name))) {
        DWORD subs[4] = { 21, 0, 0, 0 };
        machine_domain(subs + 1);
        sid_build(a->sid, 5, 4, subs);
        a->name = computer_name(); a->domain = computer_name(); a->use = SidTypeDomain;
        ok = 1;
    }
    for (size_t i = 0; !ok && local && i < sizeof g_wk_dom / sizeof g_wk_dom[0]; i++)
        if (!strcasecmp(name, g_wk_dom[i].name)) {
            domain_sid_rid(a->sid, g_wk_dom[i].rid);
            a->name = g_wk_dom[i].name; a->domain = computer_name(); a->use = g_wk_dom[i].use;
            ok = 1;
        }
    for (size_t i = 0; !ok && i < sizeof g_wk / sizeof g_wk[0]; i++)
        if (!strcasecmp(name, g_wk[i].name) && (!dom || !strcasecmp(dom, g_wk[i].domain))) {
            wk_build(&g_wk[i], a->sid);
            a->name = g_wk[i].name; a->domain = g_wk[i].domain; a->use = g_wk[i].use;
            ok = 1;
        }
    free(dom);
    return ok;
}

static BOOL lookup_sid(const void *system, int ansi, const void *sid, void *name, DWORD *cchName,
                       void *dom, DWORD *cchDom, int *use)
{
    char *sys = enc_in(system, ansi);
    int local = is_local_machine_a(sys);
    free(sys);
    if (!local) { lsiw_set_last_error(RPC_S_SERVER_UNAVAILABLE); return FALSE; }
    if (!sid_valid(sid)) { lsiw_set_last_error(ERROR_INVALID_SID); return FALSE; }
    if (!cchName || !cchDom) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct account a;
    if (!account_from_sid(sid, &a)) { lsiw_set_last_error(ERROR_NONE_MAPPED); return FALSE; }
    size_t nn = enc_len(a.name, ansi), dn = enc_len(a.domain, ansi);
    if (!name || *cchName < nn + 1 || !dom || *cchDom < dn + 1) {
        *cchName = (DWORD)nn + 1;
        *cchDom = (DWORD)dn + 1;
        lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    enc_copy(a.name, ansi, name);
    enc_copy(a.domain, ansi, dom);
    *cchName = (DWORD)nn;
    *cchDom = (DWORD)dn;
    if (use) *use = a.use;
    return TRUE;
}

API BOOL LookupAccountSidW(const WCHAR *sys, void *sid, WCHAR *name, DWORD *cchName, WCHAR *dom, DWORD *cchDom, int *use)
{
    return lookup_sid(sys, 0, sid, name, cchName, dom, cchDom, use);
}

API BOOL LookupAccountSidA(const char *sys, void *sid, char *name, DWORD *cchName, char *dom, DWORD *cchDom, int *use)
{
    return lookup_sid(sys, 1, sid, name, cchName, dom, cchDom, use);
}

API BOOL LookupAccountSidLocalW(void *sid, WCHAR *name, DWORD *cchName, WCHAR *dom, DWORD *cchDom, int *use)
{
    return lookup_sid(NULL, 0, sid, name, cchName, dom, cchDom, use);
}

API BOOL LookupAccountSidLocalA(void *sid, char *name, DWORD *cchName, char *dom, DWORD *cchDom, int *use)
{
    return lookup_sid(NULL, 1, sid, name, cchName, dom, cchDom, use);
}

static BOOL lookup_name(const void *system, int ansi, const void *account, void *sid, DWORD *cbSid,
                        void *dom, DWORD *cchDom, int *use)
{
    char *sys = enc_in(system, ansi);
    int local = is_local_machine_a(sys);
    free(sys);
    if (!local) { lsiw_set_last_error(RPC_S_SERVER_UNAVAILABLE); return FALSE; }
    if (!account || !cbSid || !cchDom) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *nm = enc_in(account, ansi);
    struct account a;
    int ok = account_from_name(nm, &a);
    free(nm);
    if (!ok) { lsiw_set_last_error(ERROR_NONE_MAPPED); return FALSE; }
    DWORD sl = sid_len(a.sid);
    size_t dn = enc_len(a.domain, ansi);
    if (!sid || *cbSid < sl || !dom || *cchDom < dn + 1) {
        *cbSid = sl;
        *cchDom = (DWORD)dn + 1;
        lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(sid, a.sid, sl);
    enc_copy(a.domain, ansi, dom);
    *cbSid = sl;
    *cchDom = (DWORD)dn;
    if (use) *use = a.use;
    return TRUE;
}

API BOOL LookupAccountNameW(const WCHAR *sys, const WCHAR *acct, void *sid, DWORD *cbSid, WCHAR *dom, DWORD *cchDom, int *use)
{
    return lookup_name(sys, 0, acct, sid, cbSid, dom, cchDom, use);
}

API BOOL LookupAccountNameA(const char *sys, const char *acct, void *sid, DWORD *cbSid, char *dom, DWORD *cchDom, int *use)
{
    return lookup_name(sys, 1, acct, sid, cbSid, dom, cchDom, use);
}

API BOOL LookupAccountNameLocalW(const WCHAR *acct, void *sid, DWORD *cbSid, WCHAR *dom, DWORD *cchDom, int *use)
{
    return lookup_name(NULL, 0, acct, sid, cbSid, dom, cchDom, use);
}

API BOOL LookupAccountNameLocalA(const char *acct, void *sid, DWORD *cbSid, char *dom, DWORD *cchDom, int *use)
{
    return lookup_name(NULL, 1, acct, sid, cbSid, dom, cchDom, use);
}

/* ---- SID functions ---- */

API BOOL AllocateAndInitializeSid(const SID_IDENTIFIER_AUTHORITY *auth, BYTE n, DWORD s0, DWORD s1, DWORD s2,
                                  DWORD s3, DWORD s4, DWORD s5, DWORD s6, DWORD s7, void **out)
{
    if (!auth || !out) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (n > 8) { lsiw_set_last_error(ERROR_INVALID_SID); return FALSE; }
    DWORD subs[8] = { s0, s1, s2, s3, s4, s5, s6, s7 };
    BYTE *p = lsiw_heap_alloc(8u + 4u * n, 1);
    if (!p) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    sid_build(p, 0, n, subs);
    memcpy(p + 2, auth->Value, 6);
    *out = p;
    return TRUE;
}

API void *FreeSid(void *sid)
{
    if (sid) lsiw_heap_free(sid);
    return NULL;
}

API BOOL InitializeSid(void *sid, const SID_IDENTIFIER_AUTHORITY *auth, BYTE n)
{
    if (!sid || !auth || n > SID_MAX_SUB_AUTHORITIES) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    BYTE *b = sid;
    b[0] = SID_REVISION;
    b[1] = n;
    memcpy(b + 2, auth->Value, 6);
    return TRUE;
}

API BOOL IsValidSid(void *sid) { return sid_valid(sid); }

API DWORD GetLengthSid(void *sid) { return sid ? sid_len(sid) : 0; }

API DWORD GetSidLengthRequired(BYTE n) { return 8u + 4u * n; }

API BOOL EqualSid(void *a, void *b)
{
    if (!sid_valid(a) || !sid_valid(b)) { lsiw_set_last_error(ERROR_INVALID_SID); return FALSE; }
    lsiw_set_last_error(ERROR_SUCCESS);
    return sid_equal(a, b);
}

/* Equal apart from the last sub-authority. */
API BOOL EqualPrefixSid(void *a, void *b)
{
    if (!sid_valid(a) || !sid_valid(b)) { lsiw_set_last_error(ERROR_INVALID_SID); return FALSE; }
    const BYTE *x = a, *y = b;
    lsiw_set_last_error(ERROR_SUCCESS);
    if (x[1] != y[1]) return FALSE;
    if (!x[1]) return !memcmp(x, y, 8);
    return !memcmp(x, y, 8u + 4u * (x[1] - 1u));
}

API BOOL CopySid(DWORD len, void *dst, void *src)
{
    if (!sid_valid(src)) { lsiw_set_last_error(ERROR_INVALID_SID); return FALSE; }
    if (!dst || len < sid_len(src)) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(dst, src, sid_len(src));
    return TRUE;
}

API DWORD *GetSidSubAuthority(void *sid, DWORD n)
{
    lsiw_set_last_error(ERROR_SUCCESS);
    return (DWORD *)((BYTE *)sid + 8 + 4 * (size_t)n);
}

API BYTE *GetSidSubAuthorityCount(void *sid)
{
    lsiw_set_last_error(ERROR_SUCCESS);
    return (BYTE *)sid + 1;
}

API SID_IDENTIFIER_AUTHORITY *GetSidIdentifierAuthority(void *sid)
{
    lsiw_set_last_error(ERROR_SUCCESS);
    return (SID_IDENTIFIER_AUTHORITY *)((BYTE *)sid + 2);
}

/* Parse "S-1-auth-sub-..." or an SDDL alias ("BA", "WD"...). *end is set
 * past what was consumed. Returns the SID length or 0. */
static DWORD parse_sid_str(const char *s, const char **end, BYTE *out)
{
    if ((s[0] == 'S' || s[0] == 's') && s[1] == '-') {
        const char *p = s + 2;
        char *e;
        errno = 0;
        unsigned long rev = strtoul(p, &e, 10);
        if (e == p || rev != 1 || *e != '-') return 0;
        p = e + 1;
        unsigned long long auth;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) auth = strtoull(p + 2, &e, 16);
        else auth = strtoull(p, &e, 10);
        if (e == p || auth >= (1ULL << 48) || errno) return 0;
        p = e;
        DWORD subs[SID_MAX_SUB_AUTHORITIES];
        int n = 0;
        while (*p == '-' && isdigit((unsigned char)p[1])) {
            unsigned long long v = strtoull(p + 1, &e, 10);
            if (v > 0xFFFFFFFFull || n == SID_MAX_SUB_AUTHORITIES) return 0;
            subs[n++] = (DWORD)v;
            p = e;
        }
        if (end) *end = p;
        return sid_build(out, auth, n, subs);
    }
    if (!isupper((unsigned char)s[0]) || !isupper((unsigned char)s[1])) return 0;
    for (size_t i = 0; i < sizeof g_wk / sizeof g_wk[0]; i++)
        if (g_wk[i].alias && !strncmp(s, g_wk[i].alias, 2)) { if (end) *end = s + 2; return wk_build(&g_wk[i], out); }
    for (size_t i = 0; i < sizeof g_wk_dom / sizeof g_wk_dom[0]; i++)
        if (g_wk_dom[i].alias && !strncmp(s, g_wk_dom[i].alias, 2)) { if (end) *end = s + 2; return domain_sid_rid(out, g_wk_dom[i].rid); }
    return 0;
}

/* The SDDL alias of a SID, if it has one. */
static const char *sid_alias(const void *sid)
{
    long long rid = domain_rid_of(sid);
    for (size_t i = 0; rid >= 0 && i < sizeof g_wk_dom / sizeof g_wk_dom[0]; i++)
        if (g_wk_dom[i].rid == rid && g_wk_dom[i].alias) return g_wk_dom[i].alias;
    const struct wk_sid *w = wk_find_sid(sid);
    return w ? w->alias : NULL;
}

static BOOL sid_to_string(void *sid, void **out, int ansi)
{
    if (!out) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!sid_valid(sid)) { lsiw_set_last_error(ERROR_INVALID_SID); return FALSE; }
    char *s = sid_to_str(sid);
    *out = enc_heap(s, ansi);
    free(s);
    if (!*out) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    return TRUE;
}

API BOOL ConvertSidToStringSidW(void *sid, WCHAR **out) { return sid_to_string(sid, (void **)out, 0); }
API BOOL ConvertSidToStringSidA(void *sid, char **out) { return sid_to_string(sid, (void **)out, 1); }

static BOOL string_to_sid(const void *str, void **out, int ansi)
{
    if (!str || !out) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *s = enc_in(str, ansi);
    BYTE tmp[SECURITY_MAX_SID_SIZE];
    const char *end = NULL;
    DWORD len = parse_sid_str(s, &end, tmp);
    int ok = len && end && !*end;
    free(s);
    if (!ok) { lsiw_set_last_error(ERROR_INVALID_SID); return FALSE; }
    void *p = lsiw_heap_alloc(len, 0);
    if (!p) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    memcpy(p, tmp, len);
    *out = p;
    return TRUE;
}

API BOOL ConvertStringSidToSidW(const WCHAR *str, void **out) { return string_to_sid(str, out, 0); }
API BOOL ConvertStringSidToSidA(const char *str, void **out) { return string_to_sid(str, out, 1); }

API BOOL IsWellKnownSid(void *sid, int type)
{
    if (!sid_valid(sid)) return FALSE;
    BYTE tmp[SECURITY_MAX_SID_SIZE];
    for (size_t i = 0; i < sizeof g_wk / sizeof g_wk[0]; i++)
        if (g_wk[i].type == type) { wk_build(&g_wk[i], tmp); return sid_equal(tmp, sid); }
    /* Domain accounts: <any S-1-5-21 domain>-<rid>. */
    const BYTE *b = sid;
    for (size_t i = 0; i < sizeof g_wk_dom / sizeof g_wk_dom[0]; i++)
        if (g_wk_dom[i].type == type)
            return b[1] == 5 && sid_auth(sid) == 5 && sid_sub(sid, 0) == 21 && sid_sub(sid, 4) == g_wk_dom[i].rid;
    return FALSE;
}

API BOOL CreateWellKnownSid(int type, void *domain, void *sid, DWORD *cb)
{
    BYTE tmp[SECURITY_MAX_SID_SIZE];
    DWORD len = 0;
    if (!cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (size_t i = 0; !len && i < sizeof g_wk / sizeof g_wk[0]; i++)
        if (g_wk[i].type == type) len = wk_build(&g_wk[i], tmp);
    for (size_t i = 0; !len && i < sizeof g_wk_dom / sizeof g_wk_dom[0]; i++)
        if (g_wk_dom[i].type == type) {
            const BYTE *d = domain;
            if (!sid_valid(domain) || d[1] != 4 || sid_auth(domain) != 5) {
                lsiw_set_last_error(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            DWORD subs[5] = { sid_sub(d, 0), sid_sub(d, 1), sid_sub(d, 2), sid_sub(d, 3), g_wk_dom[i].rid };
            len = sid_build(tmp, 5, 5, subs);
        }
    if (!len) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!sid || *cb < len) { *cb = len; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(sid, tmp, len);
    *cb = len;
    return TRUE;
}

/* ---- user name and hardware profile ---- */

static BOOL get_user_name(void *buf, DWORD *size, int ansi)
{
    if (!size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    const char *u = lsiw_user_name();
    size_t n = enc_len(u, ansi) + 1;
    if (!buf || *size < n) { *size = (DWORD)n; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    enc_copy(u, ansi, buf);
    *size = (DWORD)n;       /* GetUserName counts the terminating NUL */
    return TRUE;
}

API BOOL GetUserNameW(WCHAR *buf, DWORD *size) { return get_user_name(buf, size, 0); }
API BOOL GetUserNameA(char *buf, DWORD *size) { return get_user_name(buf, size, 1); }

#define DOCKINFO_DOCKED        0x2
#define DOCKINFO_USER_SUPPLIED 0x4

static char *machine_guid_braced(void)
{
    const uint8_t *m = machine_id();
    return lsiw_strdup_printf("{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                              m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                              m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

typedef struct { DWORD dwDockInfo; char szHwProfileGuid[39]; char szHwProfileName[80]; } HW_PROFILE_INFOA;
typedef struct { DWORD dwDockInfo; WCHAR szHwProfileGuid[39]; WCHAR szHwProfileName[80]; } HW_PROFILE_INFOW;

API BOOL GetCurrentHwProfileA(HW_PROFILE_INFOA *p)
{
    if (!p) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *g = machine_guid_braced();
    p->dwDockInfo = DOCKINFO_DOCKED | DOCKINFO_USER_SUPPLIED;
    snprintf(p->szHwProfileGuid, sizeof p->szHwProfileGuid, "%s", g);
    snprintf(p->szHwProfileName, sizeof p->szHwProfileName, "%s", "Docked Profile");
    free(g);
    return TRUE;
}

API BOOL GetCurrentHwProfileW(HW_PROFILE_INFOW *p)
{
    if (!p) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *g = machine_guid_braced();
    p->dwDockInfo = DOCKINFO_DOCKED | DOCKINFO_USER_SUPPLIED;
    lsiw_utf8_to_utf16(g, -1, p->szHwProfileGuid, 39);
    lsiw_utf8_to_utf16("Docked Profile", -1, p->szHwProfileName, 80);
    free(g);
    return TRUE;
}

/* ================================================================== */
/* Access tokens and privileges                                         */
/* ================================================================== */

/* There is one identity per process: every token describes the current
 * Linux user. Tokens are kernel handles (closed with CloseHandle); the
 * pseudo-handles GetCurrentProcessToken (-4), GetCurrentThreadToken (-5)
 * and GetCurrentThreadEffectiveToken (-6) are accepted too. */
enum { TokenPrimary = 1, TokenImpersonation = 2 };
enum { SecurityAnonymous, SecurityIdentification, SecurityImpersonation, SecurityDelegation };

struct token { int type; int level; };

static __thread int t_impersonating;    /* impersonation level + 1, 0 = none */

static void token_free(void *p)
{
    obj_remove(p, OBJ_TOKEN);
    free(p);
}

static BOOL token_new(int type, int level, HANDLE *out)
{
    if (!out) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_GENERIC, &o);
    if (!h) return FALSE;
    struct token *t = xcalloc(1, sizeof *t);
    t->type = type;
    t->level = level;
    obj_add(t, OBJ_TOKEN);
    o->data = t;
    o->free_data = token_free;
    *out = h;
    return TRUE;
}

static int token_get(HANDLE h, struct token *out)
{
    intptr_t v = (intptr_t)h;
    if (v == -4 || v == -6) {
        if (v == -6 && t_impersonating) { out->type = TokenImpersonation; out->level = t_impersonating - 1; }
        else { out->type = TokenPrimary; out->level = SecurityAnonymous; }
        return 1;
    }
    if (v == -5) {
        if (!t_impersonating) return 0;
        out->type = TokenImpersonation;
        out->level = t_impersonating - 1;
        return 1;
    }
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_GENERIC);
    if (!o || !o->data || !obj_check(o->data, OBJ_TOKEN)) return 0;
    *out = *(struct token *)o->data;
    return 1;
}

#define BAD_TOKEN(h, t) (!token_get((h), (t)) ? (lsiw_set_last_error(ERROR_INVALID_HANDLE), 1) : 0)

API BOOL OpenProcessToken(HANDLE process, DWORD access, HANDLE *out)
{
    (void)process; (void)access;
    return token_new(TokenPrimary, SecurityAnonymous, out);
}

/* A thread has a token only while it impersonates. */
API BOOL OpenThreadToken(HANDLE thread, DWORD access, BOOL self, HANDLE *out)
{
    (void)thread; (void)access; (void)self;
    if (!t_impersonating) { lsiw_set_last_error(ERROR_NO_TOKEN); return FALSE; }
    return token_new(TokenImpersonation, t_impersonating - 1, out);
}

API BOOL DuplicateToken(HANDLE h, int level, HANDLE *out)
{
    struct token t;
    if (BAD_TOKEN(h, &t)) return FALSE;
    return token_new(TokenImpersonation, level, out);
}

API BOOL DuplicateTokenEx(HANDLE h, DWORD access, void *sa, int level, int type, HANDLE *out)
{
    (void)access; (void)sa;
    struct token t;
    if (BAD_TOKEN(h, &t)) return FALSE;
    if (type != TokenPrimary && type != TokenImpersonation) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return token_new(type, level, out);
}

API BOOL ImpersonateSelf(int level)
{
    if (level < SecurityAnonymous || level > SecurityDelegation) { lsiw_set_last_error(ERROR_BAD_IMPERSONATION_LEVEL); return FALSE; }
    t_impersonating = level + 1;
    return TRUE;
}

API BOOL RevertToSelf(void) { t_impersonating = 0; return TRUE; }

API BOOL ImpersonateLoggedOnUser(HANDLE token)
{
    struct token t;
    if (BAD_TOKEN(token, &t)) return FALSE;
    t_impersonating = (t.type == TokenImpersonation ? t.level : SecurityImpersonation) + 1;
    return TRUE;
}

/* Pipe clients are the same Linux user as the server. */
API BOOL ImpersonateNamedPipeClient(HANDLE pipe)
{
    if (!pipe || pipe == INVALID_HANDLE_VALUE) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    t_impersonating = SecurityImpersonation + 1;
    return TRUE;
}

API BOOL SetThreadToken(HANDLE *thread, HANDLE token)
{
    (void)thread;           /* only the calling thread is tracked */
    if (!token) { t_impersonating = 0; return TRUE; }
    return ImpersonateLoggedOnUser(token);
}

/* ---- privileges ---- */

#define SE_PRIVILEGE_ENABLED_BY_DEFAULT 0x00000001u
#define SE_PRIVILEGE_ENABLED            0x00000002u
#define SE_PRIVILEGE_REMOVED            0x00000004u
#define SE_PRIVILEGE_USED_FOR_ACCESS    0x80000000u
#define PRIV_MAX 36

static const char *const g_priv_names[PRIV_MAX + 1] = {
    [2]  = "SeCreateTokenPrivilege",          [3]  = "SeAssignPrimaryTokenPrivilege",
    [4]  = "SeLockMemoryPrivilege",           [5]  = "SeIncreaseQuotaPrivilege",
    [6]  = "SeMachineAccountPrivilege",       [7]  = "SeTcbPrivilege",
    [8]  = "SeSecurityPrivilege",             [9]  = "SeTakeOwnershipPrivilege",
    [10] = "SeLoadDriverPrivilege",           [11] = "SeSystemProfilePrivilege",
    [12] = "SeSystemtimePrivilege",           [13] = "SeProfileSingleProcessPrivilege",
    [14] = "SeIncreaseBasePriorityPrivilege", [15] = "SeCreatePagefilePrivilege",
    [16] = "SeCreatePermanentPrivilege",      [17] = "SeBackupPrivilege",
    [18] = "SeRestorePrivilege",              [19] = "SeShutdownPrivilege",
    [20] = "SeDebugPrivilege",                [21] = "SeAuditPrivilege",
    [22] = "SeSystemEnvironmentPrivilege",    [23] = "SeChangeNotifyPrivilege",
    [24] = "SeRemoteShutdownPrivilege",       [25] = "SeUndockPrivilege",
    [26] = "SeSyncAgentPrivilege",            [27] = "SeEnableDelegationPrivilege",
    [28] = "SeManageVolumePrivilege",         [29] = "SeImpersonatePrivilege",
    [30] = "SeCreateGlobalPrivilege",         [31] = "SeTrustedCredManAccessPrivilege",
    [32] = "SeRelabelPrivilege",              [33] = "SeIncreaseWorkingSetPrivilege",
    [34] = "SeTimeZonePrivilege",             [35] = "SeCreateSymbolicLinkPrivilege",
    [36] = "SeDelegateSessionUserImpersonatePrivilege",
};

/* The privileges a standard user holds, and those of an elevated
 * administrator (root). Attributes are shared by the whole process. */
static unsigned char   g_priv_held[PRIV_MAX + 1];
static DWORD           g_priv_attr[PRIV_MAX + 1];
static int             g_priv_init;
static pthread_mutex_t g_priv_lock = PTHREAD_MUTEX_INITIALIZER;

static void priv_init(void)
{
    static const BYTE user[] = { 19, 23, 25, 33, 34 };
    static const BYTE admin[] = { 5, 8, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 22, 23, 24, 25, 28, 29, 30, 33, 34, 35, 36 };
    if (g_priv_init) return;
    const BYTE *l = is_admin() ? admin : user;
    size_t n = is_admin() ? sizeof admin : sizeof user;
    for (size_t i = 0; i < n; i++) g_priv_held[l[i]] = 1;
    g_priv_attr[23] = SE_PRIVILEGE_ENABLED_BY_DEFAULT | SE_PRIVILEGE_ENABLED;
    if (is_admin()) {
        g_priv_attr[29] = SE_PRIVILEGE_ENABLED_BY_DEFAULT | SE_PRIVILEGE_ENABLED;
        g_priv_attr[30] = SE_PRIVILEGE_ENABLED_BY_DEFAULT | SE_PRIVILEGE_ENABLED;
    }
    g_priv_init = 1;
}

struct luid_attr { LUID Luid; DWORD Attributes; };          /* LUID_AND_ATTRIBUTES, 12 bytes */
typedef struct { DWORD PrivilegeCount; struct luid_attr Privileges[1]; } TOKEN_PRIVILEGES;

static int priv_index(const LUID *l)
{
    return (l->HighPart == 0 && l->LowPart >= 2 && l->LowPart <= PRIV_MAX) ? (int)l->LowPart : -1;
}

API BOOL AdjustTokenPrivileges(HANDLE h, BOOL disable_all, TOKEN_PRIVILEGES *ns, DWORD buflen,
                               TOKEN_PRIVILEGES *prev, DWORD *retlen)
{
    struct token t;
    if (BAD_TOKEN(h, &t)) return FALSE;
    if (!disable_all && !ns) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_priv_lock);
    priv_init();
    DWORD attr[PRIV_MAX + 1];
    memcpy(attr, g_priv_attr, sizeof attr);
    int not_all = 0;
    if (disable_all) {
        for (int i = 2; i <= PRIV_MAX; i++) attr[i] &= ~SE_PRIVILEGE_ENABLED;
    } else {
        for (DWORD i = 0; i < ns->PrivilegeCount; i++) {
            int p = priv_index(&ns->Privileges[i].Luid);
            if (p < 0 || !g_priv_held[p]) { not_all = 1; continue; }
            DWORD a = ns->Privileges[i].Attributes;
            if (a & SE_PRIVILEGE_REMOVED) attr[p] = SE_PRIVILEGE_REMOVED;
            else attr[p] = (attr[p] & ~SE_PRIVILEGE_ENABLED) | (a & SE_PRIVILEGE_ENABLED);
        }
    }
    /* The previous state lists the privileges whose attributes change. */
    DWORD changed = 0;
    for (int i = 2; i <= PRIV_MAX; i++) if (g_priv_held[i] && attr[i] != g_priv_attr[i]) changed++;
    DWORD need = 4 + 12 * changed;
    if (prev) {
        if (retlen) *retlen = need;
        if (buflen < need) {
            pthread_mutex_unlock(&g_priv_lock);
            lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        prev->PrivilegeCount = 0;
        for (int i = 2; i <= PRIV_MAX; i++)
            if (g_priv_held[i] && attr[i] != g_priv_attr[i]) {
                struct luid_attr *la = &prev->Privileges[prev->PrivilegeCount++];
                la->Luid.LowPart = (DWORD)i;
                la->Luid.HighPart = 0;
                la->Attributes = g_priv_attr[i];
            }
    }
    for (int i = 2; i <= PRIV_MAX; i++) {
        if (!g_priv_held[i]) continue;
        if (attr[i] == SE_PRIVILEGE_REMOVED) { g_priv_held[i] = 0; g_priv_attr[i] = 0; }
        else g_priv_attr[i] = attr[i];
    }
    pthread_mutex_unlock(&g_priv_lock);
    /* Success either way; a privilege the user does not hold is reported
     * through the last error, as Windows does for standard users. */
    lsiw_set_last_error(not_all ? ERROR_NOT_ALL_ASSIGNED : ERROR_SUCCESS);
    return TRUE;
}

typedef struct { DWORD PrivilegeCount; DWORD Control; struct luid_attr Privilege[1]; } PRIVILEGE_SET;
#define PRIVILEGE_SET_ALL_NECESSARY 1

API BOOL PrivilegeCheck(HANDLE h, PRIVILEGE_SET *set, BOOL *result)
{
    struct token t;
    if (BAD_TOKEN(h, &t)) return FALSE;
    if (!set || !result) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_priv_lock);
    priv_init();
    DWORD on = 0;
    for (DWORD i = 0; i < set->PrivilegeCount; i++) {
        int p = priv_index(&set->Privilege[i].Luid);
        if (p >= 0 && g_priv_held[p] && (g_priv_attr[p] & SE_PRIVILEGE_ENABLED)) {
            set->Privilege[i].Attributes |= SE_PRIVILEGE_USED_FOR_ACCESS;
            on++;
        }
    }
    pthread_mutex_unlock(&g_priv_lock);
    *result = (set->Control & PRIVILEGE_SET_ALL_NECESSARY) ? on == set->PrivilegeCount : on > 0;
    return TRUE;
}

static BOOL lookup_priv_value(const void *sys, const void *name, LUID *luid, int ansi)
{
    (void)sys;
    if (!name || !luid) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *n = enc_in(name, ansi);
    int found = -1;
    for (int i = 2; i <= PRIV_MAX && found < 0; i++) if (!strcasecmp(n, g_priv_names[i])) found = i;
    free(n);
    if (found < 0) { lsiw_set_last_error(ERROR_NO_SUCH_PRIVILEGE); return FALSE; }
    luid->LowPart = (DWORD)found;
    luid->HighPart = 0;
    return TRUE;
}

API BOOL LookupPrivilegeValueW(const WCHAR *sys, const WCHAR *name, LUID *luid) { return lookup_priv_value(sys, name, luid, 0); }
API BOOL LookupPrivilegeValueA(const char *sys, const char *name, LUID *luid) { return lookup_priv_value(sys, name, luid, 1); }

static BOOL lookup_priv_name(const LUID *luid, void *name, DWORD *cch, int ansi)
{
    if (!luid || !cch) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    int p = priv_index(luid);
    if (p < 0) { lsiw_set_last_error(ERROR_NO_SUCH_PRIVILEGE); return FALSE; }
    size_t n = strlen(g_priv_names[p]);
    if (!name || *cch < n + 1) { *cch = (DWORD)n + 1; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    enc_copy(g_priv_names[p], ansi, name);
    *cch = (DWORD)n;
    return TRUE;
}

API BOOL LookupPrivilegeNameW(const WCHAR *sys, LUID *luid, WCHAR *name, DWORD *cch) { (void)sys; return lookup_priv_name(luid, name, cch, 0); }
API BOOL LookupPrivilegeNameA(const char *sys, LUID *luid, char *name, DWORD *cch) { (void)sys; return lookup_priv_name(luid, name, cch, 1); }

/* ---- groups and GetTokenInformation ---- */

#define SE_GROUP_MANDATORY          0x00000001u
#define SE_GROUP_ENABLED_BY_DEFAULT 0x00000002u
#define SE_GROUP_ENABLED            0x00000004u
#define SE_GROUP_OWNER              0x00000008u
#define SE_GROUP_INTEGRITY          0x00000020u
#define SE_GROUP_INTEGRITY_ENABLED  0x00000040u
#define SE_GROUP_LOGON_ID           0xC0000000u
#define GROUP_STD (SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED)

struct grp { BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD attr; };

/* Everyone, Users, Authenticated Users, and Administrators for root. */
static int token_groups(struct grp g[4])
{
    static const DWORD world[] = { 0 }, users[] = { 32, 545 }, auth[] = { 11 }, admins[] = { 32, 544 };
    int n = 0;
    sid_build(g[n].sid, 1, 1, world);  g[n++].attr = GROUP_STD;
    sid_build(g[n].sid, 5, 2, users);  g[n++].attr = GROUP_STD;
    sid_build(g[n].sid, 5, 1, auth);   g[n++].attr = GROUP_STD;
    if (is_admin()) { sid_build(g[n].sid, 5, 2, admins); g[n++].attr = GROUP_STD | SE_GROUP_OWNER; }
    return n;
}

static int token_has_sid(const void *sid)
{
    struct grp g[4];
    int n = token_groups(g);
    if (sid_equal(sid, user_sid())) return 1;
    for (int i = 0; i < n; i++) if (sid_equal(sid, g[i].sid)) return 1;
    return 0;
}

API BOOL CheckTokenMembership(HANDLE h, void *sid, BOOL *is)
{
    struct token t;
    if (h && BAD_TOKEN(h, &t)) return FALSE;
    if (!sid_valid(sid) || !is) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *is = token_has_sid(sid);
    return TRUE;
}

/* Small output builder for the variable-sized token structures: data is
 * assembled in tmp, and embedded pointers are computed for the caller's
 * buffer at base. */
struct tbuild { BYTE tmp[2048]; size_t n; uintptr_t base; };

static size_t tb_alloc(struct tbuild *b, size_t n, size_t align)
{
    b->n = (b->n + align - 1) & ~(align - 1);
    size_t off = b->n;
    b->n += n;
    return off;
}

static void tb_ptr(struct tbuild *b, size_t at, size_t target)
{
    uint64_t v = b->base + target;
    memcpy(b->tmp + at, &v, 8);
}

static void tb_dword(struct tbuild *b, size_t at, DWORD v) { memcpy(b->tmp + at, &v, 4); }

static size_t tb_sid(struct tbuild *b, const void *sid)
{
    size_t off = tb_alloc(b, sid_len(sid), 4);
    memcpy(b->tmp + off, sid, sid_len(sid));
    return off;
}

/* SID_AND_ATTRIBUTES (16 bytes) at `at` pointing at a copy of sid. */
static void tb_sa(struct tbuild *b, size_t at, const void *sid, DWORD attr)
{
    size_t s = tb_sid(b, sid);
    tb_ptr(b, at, s);
    tb_dword(b, at + 8, attr);
}

static void tb_groups(struct tbuild *b, const struct grp *g, int n)
{
    size_t hdr = tb_alloc(b, 8 + 16 * (size_t)n, 8);
    tb_dword(b, hdr, (DWORD)n);
    for (int i = 0; i < n; i++) tb_sa(b, hdr + 8 + 16 * (size_t)i, g[i].sid, g[i].attr);
}

static DWORD build_default_dacl(BYTE *out, size_t cap);

enum {
    TokenUser = 1, TokenGroups, TokenPrivileges, TokenOwner, TokenPrimaryGroup, TokenDefaultDacl, TokenSource,
    TokenTypeClass, TokenImpersonationLevel, TokenStatistics, TokenRestrictedSids, TokenSessionId,
    TokenGroupsAndPrivileges, TokenSessionReference, TokenSandBoxInert, TokenAuditPolicy, TokenOrigin,
    TokenElevationType, TokenLinkedToken, TokenElevation, TokenHasRestrictions, TokenAccessInformation,
    TokenVirtualizationAllowed, TokenVirtualizationEnabled, TokenIntegrityLevel, TokenUIAccess,
    TokenMandatoryPolicy, TokenLogonSid, TokenIsAppContainer, TokenCapabilities, TokenAppContainerSid,
    TokenAppContainerNumber, TokenIsRestricted = 40
};

/* Returns 0 and sets *err for classes that are not available. */
static int token_info(int cls, const struct token *t, struct tbuild *b, DWORD *err)
{
    BYTE sid[SECURITY_MAX_SID_SIZE];
    switch (cls) {
    case TokenUser:
        tb_sa(b, tb_alloc(b, 16, 8), user_sid(), 0);
        return 1;
    case TokenGroups: {
        struct grp g[4];
        tb_groups(b, g, token_groups(g));
        return 1;
    }
    case TokenPrivileges: {
        pthread_mutex_lock(&g_priv_lock);
        priv_init();
        DWORD n = 0;
        for (int i = 2; i <= PRIV_MAX; i++) n += g_priv_held[i];
        size_t hdr = tb_alloc(b, 4 + 12 * (size_t)n, 4);
        tb_dword(b, hdr, n);
        size_t at = hdr + 4;
        for (int i = 2; i <= PRIV_MAX; i++)
            if (g_priv_held[i]) {
                tb_dword(b, at, (DWORD)i);
                tb_dword(b, at + 4, 0);
                tb_dword(b, at + 8, g_priv_attr[i]);
                at += 12;
            }
        pthread_mutex_unlock(&g_priv_lock);
        return 1;
    }
    case TokenOwner: {          /* elevated administrators own new objects as Administrators */
        static const DWORD admins[] = { 32, 544 };
        size_t at = tb_alloc(b, 8, 8);
        if (is_admin()) sid_build(sid, 5, 2, admins);
        else memcpy(sid, user_sid(), sid_len(user_sid()));
        tb_ptr(b, at, tb_sid(b, sid));
        return 1;
    }
    case TokenPrimaryGroup: {
        size_t at = tb_alloc(b, 8, 8);
        domain_sid_rid(sid, 513);
        tb_ptr(b, at, tb_sid(b, sid));
        return 1;
    }
    case TokenDefaultDacl: {
        BYTE acl[256];
        DWORD n = build_default_dacl(acl, sizeof acl);
        size_t at = tb_alloc(b, 8, 8);
        size_t a = tb_alloc(b, n, 4);
        memcpy(b->tmp + a, acl, n);
        tb_ptr(b, at, a);
        return 1;
    }
    case TokenSource: {
        size_t at = tb_alloc(b, 16, 4);
        memcpy(b->tmp + at, "User32  ", 8);
        tb_dword(b, at + 8, 0x3e9);
        tb_dword(b, at + 12, 0);
        return 1;
    }
    case TokenTypeClass:
        tb_dword(b, tb_alloc(b, 4, 4), (DWORD)t->type);
        return 1;
    case TokenImpersonationLevel:
        if (t->type != TokenImpersonation) { *err = ERROR_INVALID_PARAMETER; return 0; }
        tb_dword(b, tb_alloc(b, 4, 4), (DWORD)t->level);
        return 1;
    case TokenStatistics: {
        /* TOKEN_STATISTICS: TokenId, AuthenticationId, ExpirationTime,
         * TokenType, ImpersonationLevel, DynamicCharged, DynamicAvailable,
         * GroupCount, PrivilegeCount, ModifiedId (56 bytes). */
        struct grp g[4];
        size_t at = tb_alloc(b, 56, 8);
        memset(b->tmp + at, 0, 56);
        tb_dword(b, at, 0x1000 + (DWORD)getpid());
        tb_dword(b, at + 8, 0x3e9);
        int64_t never = INT64_MAX;
        memcpy(b->tmp + at + 16, &never, 8);
        tb_dword(b, at + 24, (DWORD)t->type);
        tb_dword(b, at + 28, (DWORD)t->level);
        tb_dword(b, at + 32, 4096);
        tb_dword(b, at + 36, 4096);
        tb_dword(b, at + 40, (DWORD)token_groups(g));
        pthread_mutex_lock(&g_priv_lock);
        priv_init();
        DWORD np = 0;
        for (int i = 2; i <= PRIV_MAX; i++) np += g_priv_held[i];
        pthread_mutex_unlock(&g_priv_lock);
        tb_dword(b, at + 44, np);
        tb_dword(b, at + 48, 0x2000 + (DWORD)getpid());
        return 1;
    }
    case TokenRestrictedSids:
    case TokenCapabilities:
        tb_groups(b, NULL, 0);
        return 1;
    case TokenSessionId:
        tb_dword(b, tb_alloc(b, 4, 4), 1);
        return 1;
    case TokenOrigin: {
        size_t at = tb_alloc(b, 8, 4);
        tb_dword(b, at, 0x3e7);
        tb_dword(b, at + 4, 0);
        return 1;
    }
    case TokenElevationType:            /* TokenElevationTypeDefault: no split token */
        tb_dword(b, tb_alloc(b, 4, 4), 1);
        return 1;
    case TokenLinkedToken:
        *err = ERROR_NO_SUCH_LOGON_SESSION;
        return 0;
    case TokenElevation:
        tb_dword(b, tb_alloc(b, 4, 4), (DWORD)is_admin());
        return 1;
    case TokenMandatoryPolicy:          /* NO_WRITE_UP | NEW_PROCESS_MIN */
        tb_dword(b, tb_alloc(b, 4, 4), 3);
        return 1;
    case TokenSandBoxInert: case TokenHasRestrictions: case TokenVirtualizationAllowed:
    case TokenVirtualizationEnabled: case TokenUIAccess: case TokenIsAppContainer:
    case TokenAppContainerNumber: case TokenIsRestricted:
        tb_dword(b, tb_alloc(b, 4, 4), 0);
        return 1;
    case TokenIntegrityLevel: {
        DWORD lvl = is_admin() ? 0x3000 : 0x2000;      /* high : medium */
        sid_build(sid, 16, 1, &lvl);
        tb_sa(b, tb_alloc(b, 16, 8), sid, SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED);
        return 1;
    }
    case TokenLogonSid: {
        struct grp g;
        DWORD subs[3] = { 5, 0, (DWORD)getsid(0) };
        sid_build(g.sid, 5, 3, subs);
        g.attr = GROUP_STD | SE_GROUP_LOGON_ID;
        tb_groups(b, &g, 1);
        return 1;
    }
    case TokenAppContainerSid:
        memset(b->tmp + tb_alloc(b, 8, 8), 0, 8);
        return 1;
    }
    *err = ERROR_INVALID_PARAMETER;
    return 0;
}

API BOOL GetTokenInformation(HANDLE h, int cls, void *buf, DWORD len, DWORD *retlen)
{
    struct token t;
    if (BAD_TOKEN(h, &t)) return FALSE;
    if (!retlen) { lsiw_set_last_error(ERROR_NOACCESS); return FALSE; }
    struct tbuild *b = xcalloc(1, sizeof *b);
    b->base = (uintptr_t)buf;
    DWORD err = 0;
    BOOL ok = token_info(cls, &t, b, &err);
    if (!ok) lsiw_set_last_error(err);
    else {
        *retlen = (DWORD)b->n;
        if (!buf || len < b->n) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); ok = FALSE; }
        else memcpy(buf, b->tmp, b->n);
    }
    free(b);
    return ok;
}

/* Token settings are accepted and have no effect. */
API BOOL SetTokenInformation(HANDLE h, int cls, void *buf, DWORD len)
{
    (void)cls; (void)buf; (void)len;
    struct token t;
    return !BAD_TOKEN(h, &t);
}

/* ================================================================== */
/* ACLs and security descriptors                                        */
/* ================================================================== */

typedef struct { BYTE AclRevision, Sbz1; WORD AclSize, AceCount, Sbz2; } ACL;
typedef struct { BYTE AceType, AceFlags; WORD AceSize; } ACE_HEADER;
typedef struct { DWORD GenericRead, GenericWrite, GenericExecute, GenericAll; } GENERIC_MAPPING;

#define ACL_REVISION      2
#define ACL_REVISION_DS   4
#define ACCESS_ALLOWED_ACE_TYPE 0
#define ACCESS_DENIED_ACE_TYPE  1
#define SYSTEM_AUDIT_ACE_TYPE   2
#define SYSTEM_MANDATORY_LABEL_ACE_TYPE 0x11
#define OBJECT_INHERIT_ACE      0x01
#define CONTAINER_INHERIT_ACE   0x02
#define NO_PROPAGATE_INHERIT_ACE 0x04
#define INHERIT_ONLY_ACE        0x08
#define INHERITED_ACE           0x10
#define SUCCESSFUL_ACCESS_ACE_FLAG 0x40
#define FAILED_ACCESS_ACE_FLAG  0x80
#define VALID_INHERIT_FLAGS     0x1F

#define SECURITY_DESCRIPTOR_REVISION 1
#define SE_OWNER_DEFAULTED 0x0001
#define SE_GROUP_DEFAULTED 0x0002
#define SE_DACL_PRESENT    0x0004
#define SE_DACL_DEFAULTED  0x0008
#define SE_SACL_PRESENT    0x0010
#define SE_SACL_DEFAULTED  0x0020
#define SE_DACL_AUTO_INHERIT_REQ 0x0100
#define SE_SACL_AUTO_INHERIT_REQ 0x0200
#define SE_DACL_AUTO_INHERITED   0x0400
#define SE_SACL_AUTO_INHERITED   0x0800
#define SE_DACL_PROTECTED  0x1000
#define SE_SACL_PROTECTED  0x2000
#define SE_SELF_RELATIVE   0x8000

#define OWNER_SECURITY_INFORMATION 0x1
#define GROUP_SECURITY_INFORMATION 0x2
#define DACL_SECURITY_INFORMATION  0x4
#define SACL_SECURITY_INFORMATION  0x8
#define LABEL_SECURITY_INFORMATION 0x10

/* Absolute form (x64: 40 bytes) and self-relative form (20 bytes). */
typedef struct { BYTE Revision, Sbz1; WORD Control; void *Owner, *Group; ACL *Sacl, *Dacl; } SECURITY_DESCRIPTOR;
typedef struct { BYTE Revision, Sbz1; WORD Control; DWORD Owner, Group, Sacl, Dacl; } SECURITY_DESCRIPTOR_RELATIVE;

static int acl_valid(const ACL *a)
{
    if (!a || a->AclRevision < ACL_REVISION || a->AclRevision > ACL_REVISION_DS || a->AclSize < sizeof(ACL)) return 0;
    size_t off = sizeof(ACL);
    for (WORD i = 0; i < a->AceCount; i++) {
        if (off + sizeof(ACE_HEADER) > a->AclSize) return 0;
        const ACE_HEADER *h = (const ACE_HEADER *)((const BYTE *)a + off);
        if (h->AceSize < sizeof(ACE_HEADER) || off + h->AceSize > a->AclSize) return 0;
        off += h->AceSize;
    }
    return 1;
}

/* Bytes used by the header and the ACEs. */
static size_t acl_used(const ACL *a)
{
    size_t off = sizeof(ACL);
    for (WORD i = 0; i < a->AceCount; i++) off += ((const ACE_HEADER *)((const BYTE *)a + off))->AceSize;
    return off;
}

static ACE_HEADER *acl_ace(const ACL *a, DWORD index)
{
    size_t off = sizeof(ACL);
    for (DWORD i = 0; i < index; i++) off += ((const ACE_HEADER *)((const BYTE *)a + off))->AceSize;
    return (ACE_HEADER *)((BYTE *)a + off);
}

/* The SID of an ACCESS_ALLOWED/DENIED/AUDIT/LABEL ACE (header, mask, SID). */
static void *ace_sid(const ACE_HEADER *h) { return (BYTE *)h + 8; }
static DWORD ace_mask(const ACE_HEADER *h) { DWORD m; memcpy(&m, (const BYTE *)h + 4, 4); return m; }
static int ace_simple(const ACE_HEADER *h)
{
    return h->AceType == ACCESS_ALLOWED_ACE_TYPE || h->AceType == ACCESS_DENIED_ACE_TYPE ||
           h->AceType == SYSTEM_AUDIT_ACE_TYPE || h->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE;
}

static DWORD acl_add_ace(ACL *a, DWORD rev, BYTE type, BYTE flags, DWORD mask, const void *sid)
{
    if (!acl_valid(a)) return ERROR_INVALID_ACL;
    if (!sid_valid(sid)) return ERROR_INVALID_SID;
    if (rev < ACL_REVISION || rev > ACL_REVISION_DS) return ERROR_REVISION_MISMATCH;
    size_t used = acl_used(a), sz = 8 + sid_len(sid);
    if (used + sz > a->AclSize) return ERROR_ALLOTTED_SPACE_EXCEEDED;
    BYTE *p = (BYTE *)a + used;
    ACE_HEADER h = { type, flags, (WORD)sz };
    memcpy(p, &h, 4);
    memcpy(p + 4, &mask, 4);
    memcpy(p + 8, sid, sid_len(sid));
    a->AceCount++;
    if (rev > a->AclRevision) a->AclRevision = (BYTE)rev;
    return ERROR_SUCCESS;
}

static BOOL win32_result(DWORD e)
{
    if (e) { lsiw_set_last_error(e); return FALSE; }
    return TRUE;
}

API BOOL InitializeAcl(ACL *a, DWORD len, DWORD rev)
{
    if (!a) return win32_result(ERROR_INVALID_PARAMETER);
    if (len < sizeof(ACL)) return win32_result(ERROR_INSUFFICIENT_BUFFER);
    if (len > 0xFFFF || rev < ACL_REVISION || rev > ACL_REVISION_DS) return win32_result(ERROR_INVALID_PARAMETER);
    memset(a, 0, sizeof *a);
    a->AclRevision = (BYTE)rev;
    a->AclSize = (WORD)(len & ~3u);
    return TRUE;
}

API BOOL IsValidAcl(ACL *a) { return acl_valid(a); }

API BOOL AddAccessAllowedAce(ACL *a, DWORD rev, DWORD mask, void *sid) { return win32_result(acl_add_ace(a, rev, ACCESS_ALLOWED_ACE_TYPE, 0, mask, sid)); }
API BOOL AddAccessAllowedAceEx(ACL *a, DWORD rev, DWORD flags, DWORD mask, void *sid)
{
    if (flags & ~(DWORD)(VALID_INHERIT_FLAGS)) return win32_result(ERROR_INVALID_FLAGS);
    return win32_result(acl_add_ace(a, rev, ACCESS_ALLOWED_ACE_TYPE, (BYTE)flags, mask, sid));
}
API BOOL AddAccessDeniedAce(ACL *a, DWORD rev, DWORD mask, void *sid) { return win32_result(acl_add_ace(a, rev, ACCESS_DENIED_ACE_TYPE, 0, mask, sid)); }
API BOOL AddAccessDeniedAceEx(ACL *a, DWORD rev, DWORD flags, DWORD mask, void *sid)
{
    if (flags & ~(DWORD)(VALID_INHERIT_FLAGS)) return win32_result(ERROR_INVALID_FLAGS);
    return win32_result(acl_add_ace(a, rev, ACCESS_DENIED_ACE_TYPE, (BYTE)flags, mask, sid));
}
API BOOL AddAuditAccessAce(ACL *a, DWORD rev, DWORD mask, void *sid, BOOL success, BOOL failure)
{
    BYTE f = (BYTE)((success ? SUCCESSFUL_ACCESS_ACE_FLAG : 0) | (failure ? FAILED_ACCESS_ACE_FLAG : 0));
    return win32_result(acl_add_ace(a, rev, SYSTEM_AUDIT_ACE_TYPE, f, mask, sid));
}
API BOOL AddMandatoryAce(ACL *a, DWORD rev, DWORD flags, DWORD policy, void *sid)
{
    return win32_result(acl_add_ace(a, rev, SYSTEM_MANDATORY_LABEL_ACE_TYPE, (BYTE)flags, policy, sid));
}

/* AddAce: insert already-built ACEs at index (MAXDWORD = at the end). */
API BOOL AddAce(ACL *a, DWORD rev, DWORD index, void *list, DWORD len)
{
    if (!acl_valid(a) || !list) return win32_result(ERROR_INVALID_PARAMETER);
    (void)rev;
    DWORD n = 0;
    for (DWORD off = 0; off < len; n++) {
        if (off + sizeof(ACE_HEADER) > len) return win32_result(ERROR_INVALID_PARAMETER);
        WORD sz = ((ACE_HEADER *)((BYTE *)list + off))->AceSize;
        if (sz < sizeof(ACE_HEADER) || off + sz > len) return win32_result(ERROR_INVALID_PARAMETER);
        off += sz;
    }
    size_t used = acl_used(a);
    if (used + len > a->AclSize) return win32_result(ERROR_INSUFFICIENT_BUFFER);
    if (index > a->AceCount) index = a->AceCount;
    BYTE *at = (BYTE *)acl_ace(a, index);
    memmove(at + len, at, (size_t)((BYTE *)a + used - at));
    memcpy(at, list, len);
    a->AceCount = (WORD)(a->AceCount + n);
    return TRUE;
}

API BOOL DeleteAce(ACL *a, DWORD index)
{
    if (!acl_valid(a) || index >= a->AceCount) return win32_result(ERROR_INVALID_PARAMETER);
    size_t used = acl_used(a);
    BYTE *at = (BYTE *)acl_ace(a, index);
    WORD sz = ((ACE_HEADER *)at)->AceSize;
    memmove(at, at + sz, (size_t)((BYTE *)a + used - at - sz));
    a->AceCount--;
    return TRUE;
}

API BOOL GetAce(ACL *a, DWORD index, void **ace)
{
    if (!acl_valid(a) || !ace || index >= a->AceCount) return win32_result(ERROR_INVALID_PARAMETER);
    *ace = acl_ace(a, index);
    return TRUE;
}

API BOOL FindFirstFreeAce(ACL *a, void **ace)
{
    if (!acl_valid(a) || !ace) return win32_result(ERROR_INVALID_PARAMETER);
    *ace = (BYTE *)a + acl_used(a);
    return TRUE;
}

enum { AclRevisionInformation = 1, AclSizeInformation };

API BOOL GetAclInformation(ACL *a, void *info, DWORD len, int cls)
{
    if (!acl_valid(a) || !info) return win32_result(ERROR_INVALID_PARAMETER);
    if (cls == AclRevisionInformation) {
        if (len < 4) return win32_result(ERROR_INSUFFICIENT_BUFFER);
        *(DWORD *)info = a->AclRevision;
        return TRUE;
    }
    if (cls == AclSizeInformation) {
        if (len < 12) return win32_result(ERROR_INSUFFICIENT_BUFFER);
        DWORD used = (DWORD)acl_used(a), v[3] = { a->AceCount, used, a->AclSize - used };
        memcpy(info, v, 12);
        return TRUE;
    }
    return win32_result(ERROR_INVALID_PARAMETER);
}

API BOOL SetAclInformation(ACL *a, void *info, DWORD len, int cls)
{
    if (!acl_valid(a) || !info) return win32_result(ERROR_INVALID_PARAMETER);
    if (cls == AclRevisionInformation && len >= 4) {
        DWORD r = *(DWORD *)info;
        if (r < ACL_REVISION || r > ACL_REVISION_DS) return win32_result(ERROR_INVALID_PARAMETER);
        a->AclRevision = (BYTE)r;
    }
    return TRUE;
}

/* The default DACL of new objects: full control for the user and SYSTEM
 * (and Administrators for root). */
static DWORD build_default_dacl(BYTE *out, size_t cap)
{
    static const DWORD sys[] = { 18 }, admins[] = { 32, 544 };
    BYTE s[SECURITY_MAX_SID_SIZE];
    ACL *a = (ACL *)out;
    InitializeAcl(a, (DWORD)cap, ACL_REVISION);
    acl_add_ace(a, ACL_REVISION, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, user_sid());
    sid_build(s, 5, 1, sys);
    acl_add_ace(a, ACL_REVISION, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, s);
    if (is_admin()) { sid_build(s, 5, 2, admins); acl_add_ace(a, ACL_REVISION, ACCESS_ALLOWED_ACE_TYPE, 0, GENERIC_ALL, s); }
    a->AclSize = (WORD)acl_used(a);
    return a->AclSize;
}

/* ---- security descriptors ---- */

struct sd_parts { WORD control; void *owner, *group; ACL *sacl, *dacl; };

static int sd_get(const void *sd, struct sd_parts *p)
{
    const SECURITY_DESCRIPTOR *a = sd;
    if (!sd || a->Revision != SECURITY_DESCRIPTOR_REVISION) return 0;
    p->control = a->Control;
    if (a->Control & SE_SELF_RELATIVE) {
        const SECURITY_DESCRIPTOR_RELATIVE *r = sd;
        BYTE *b = (BYTE *)sd;
        p->owner = r->Owner ? b + r->Owner : NULL;
        p->group = r->Group ? b + r->Group : NULL;
        p->sacl = r->Sacl ? (ACL *)(b + r->Sacl) : NULL;
        p->dacl = r->Dacl ? (ACL *)(b + r->Dacl) : NULL;
    } else {
        p->owner = a->Owner; p->group = a->Group; p->sacl = a->Sacl; p->dacl = a->Dacl;
    }
    if (!(p->control & SE_DACL_PRESENT)) p->dacl = NULL;
    if (!(p->control & SE_SACL_PRESENT)) p->sacl = NULL;
    return 1;
}

static DWORD sd_rel_size(const struct sd_parts *p)
{
    return (DWORD)(sizeof(SECURITY_DESCRIPTOR_RELATIVE) + (p->owner ? sid_len(p->owner) : 0) +
                   (p->group ? sid_len(p->group) : 0) + (p->sacl ? p->sacl->AclSize : 0) +
                   (p->dacl ? p->dacl->AclSize : 0));
}

/* Serialise into the self-relative form (out has sd_rel_size bytes). */
static void sd_write_rel(const struct sd_parts *p, BYTE *out)
{
    SECURITY_DESCRIPTOR_RELATIVE r = { SECURITY_DESCRIPTOR_REVISION, 0, (WORD)(p->control | SE_SELF_RELATIVE), 0, 0, 0, 0 };
    DWORD off = sizeof r;
    if (p->owner) { r.Owner = off; memcpy(out + off, p->owner, sid_len(p->owner)); off += sid_len(p->owner); }
    if (p->group) { r.Group = off; memcpy(out + off, p->group, sid_len(p->group)); off += sid_len(p->group); }
    if (p->sacl) { r.Sacl = off; memcpy(out + off, p->sacl, p->sacl->AclSize); off += p->sacl->AclSize; }
    if (p->dacl) { r.Dacl = off; memcpy(out + off, p->dacl, p->dacl->AclSize); off += p->dacl->AclSize; }
    memcpy(out, &r, sizeof r);
}

static void *sd_alloc_rel(const struct sd_parts *p, DWORD *len, int heap)
{
    DWORD n = sd_rel_size(p);
    BYTE *m = heap ? lsiw_heap_alloc(n, 1) : xcalloc(1, n);
    if (m) sd_write_rel(p, m);
    if (len) *len = n;
    return m;
}

/* What LSIW reports for every object: owned by the user, group "None",
 * NULL DACL (everyone has full access). */
static void *build_min_sd(DWORD *len, int heap)
{
    BYTE grp[SECURITY_MAX_SID_SIZE];
    domain_sid_rid(grp, 513);
    struct sd_parts p = { SE_DACL_PRESENT, (void *)user_sid(), grp, NULL, NULL };
    return sd_alloc_rel(&p, len, heap);
}

static DWORD min_sd_size(void)
{
    DWORD n;
    free(build_min_sd(&n, 0));
    return n;
}

API BOOL InitializeSecurityDescriptor(SECURITY_DESCRIPTOR *sd, DWORD rev)
{
    if (!sd) return win32_result(ERROR_INVALID_PARAMETER);
    if (rev != SECURITY_DESCRIPTOR_REVISION) return win32_result(ERROR_UNKNOWN_REVISION);
    memset(sd, 0, sizeof *sd);
    sd->Revision = SECURITY_DESCRIPTOR_REVISION;
    return TRUE;
}

API BOOL IsValidSecurityDescriptor(void *sd)
{
    struct sd_parts p;
    if (!sd_get(sd, &p)) { lsiw_set_last_error(ERROR_INVALID_SECURITY_DESCR); return FALSE; }
    return (!p.owner || sid_valid(p.owner)) && (!p.group || sid_valid(p.group)) &&
           (!p.dacl || acl_valid(p.dacl)) && (!p.sacl || acl_valid(p.sacl));
}

API DWORD GetSecurityDescriptorLength(void *sd)
{
    struct sd_parts p;
    if (!sd_get(sd, &p)) return 0;
    DWORD n = sd_rel_size(&p);
    if (!(p.control & SE_SELF_RELATIVE)) n += sizeof(SECURITY_DESCRIPTOR) - sizeof(SECURITY_DESCRIPTOR_RELATIVE);
    return n;
}

/* The setters work on the absolute form only, like Windows. */
static SECURITY_DESCRIPTOR *sd_abs(void *sd)
{
    SECURITY_DESCRIPTOR *a = sd;
    if (!a || a->Revision != SECURITY_DESCRIPTOR_REVISION) { lsiw_set_last_error(ERROR_INVALID_SECURITY_DESCR); return NULL; }
    if (a->Control & SE_SELF_RELATIVE) { lsiw_set_last_error(ERROR_INVALID_SECURITY_DESCR); return NULL; }
    return a;
}

API BOOL SetSecurityDescriptorDacl(void *sd, BOOL present, ACL *dacl, BOOL defaulted)
{
    SECURITY_DESCRIPTOR *a = sd_abs(sd);
    if (!a) return FALSE;
    a->Control &= (WORD)~(SE_DACL_PRESENT | SE_DACL_DEFAULTED);
    if (present) a->Control |= (WORD)(SE_DACL_PRESENT | (defaulted ? SE_DACL_DEFAULTED : 0));
    a->Dacl = present ? dacl : NULL;
    return TRUE;
}

API BOOL SetSecurityDescriptorSacl(void *sd, BOOL present, ACL *sacl, BOOL defaulted)
{
    SECURITY_DESCRIPTOR *a = sd_abs(sd);
    if (!a) return FALSE;
    a->Control &= (WORD)~(SE_SACL_PRESENT | SE_SACL_DEFAULTED);
    if (present) a->Control |= (WORD)(SE_SACL_PRESENT | (defaulted ? SE_SACL_DEFAULTED : 0));
    a->Sacl = present ? sacl : NULL;
    return TRUE;
}

API BOOL SetSecurityDescriptorOwner(void *sd, void *owner, BOOL defaulted)
{
    SECURITY_DESCRIPTOR *a = sd_abs(sd);
    if (!a) return FALSE;
    a->Owner = owner;
    a->Control = (WORD)((a->Control & ~SE_OWNER_DEFAULTED) | (defaulted ? SE_OWNER_DEFAULTED : 0));
    return TRUE;
}

API BOOL SetSecurityDescriptorGroup(void *sd, void *group, BOOL defaulted)
{
    SECURITY_DESCRIPTOR *a = sd_abs(sd);
    if (!a) return FALSE;
    a->Group = group;
    a->Control = (WORD)((a->Control & ~SE_GROUP_DEFAULTED) | (defaulted ? SE_GROUP_DEFAULTED : 0));
    return TRUE;
}

/* Only the inheritance and protection bits may be set this way. */
API BOOL SetSecurityDescriptorControl(void *sd, WORD mask, WORD bits)
{
    const WORD settable = SE_DACL_AUTO_INHERIT_REQ | SE_SACL_AUTO_INHERIT_REQ | SE_DACL_AUTO_INHERITED |
                          SE_SACL_AUTO_INHERITED | SE_DACL_PROTECTED | SE_SACL_PROTECTED;
    SECURITY_DESCRIPTOR *a = sd;
    if (!a || a->Revision != SECURITY_DESCRIPTOR_REVISION) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    if (mask & ~settable) return win32_result(ERROR_INVALID_PARAMETER);
    a->Control = (WORD)((a->Control & ~mask) | (bits & mask));
    return TRUE;
}

API BOOL GetSecurityDescriptorControl(void *sd, WORD *control, DWORD *rev)
{
    struct sd_parts p;
    if (!control || !rev) return win32_result(ERROR_INVALID_PARAMETER);
    if (!sd_get(sd, &p)) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    *control = p.control;
    *rev = SECURITY_DESCRIPTOR_REVISION;
    return TRUE;
}

API BOOL GetSecurityDescriptorDacl(void *sd, BOOL *present, ACL **dacl, BOOL *defaulted)
{
    struct sd_parts p;
    if (!present || !dacl || !defaulted) return win32_result(ERROR_INVALID_PARAMETER);
    if (!sd_get(sd, &p)) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    *present = (p.control & SE_DACL_PRESENT) != 0;
    if (*present) { *dacl = p.dacl; *defaulted = (p.control & SE_DACL_DEFAULTED) != 0; }
    return TRUE;
}

API BOOL GetSecurityDescriptorSacl(void *sd, BOOL *present, ACL **sacl, BOOL *defaulted)
{
    struct sd_parts p;
    if (!present || !sacl || !defaulted) return win32_result(ERROR_INVALID_PARAMETER);
    if (!sd_get(sd, &p)) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    *present = (p.control & SE_SACL_PRESENT) != 0;
    if (*present) { *sacl = p.sacl; *defaulted = (p.control & SE_SACL_DEFAULTED) != 0; }
    return TRUE;
}

API BOOL GetSecurityDescriptorOwner(void *sd, void **owner, BOOL *defaulted)
{
    struct sd_parts p;
    if (!owner || !defaulted) return win32_result(ERROR_INVALID_PARAMETER);
    if (!sd_get(sd, &p)) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    *owner = p.owner;
    *defaulted = (p.control & SE_OWNER_DEFAULTED) != 0;
    return TRUE;
}

API BOOL GetSecurityDescriptorGroup(void *sd, void **group, BOOL *defaulted)
{
    struct sd_parts p;
    if (!group || !defaulted) return win32_result(ERROR_INVALID_PARAMETER);
    if (!sd_get(sd, &p)) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    *group = p.group;
    *defaulted = (p.control & SE_GROUP_DEFAULTED) != 0;
    return TRUE;
}

API BOOL MakeSelfRelativeSD(void *abs, void *rel, DWORD *len)
{
    struct sd_parts p;
    if (!len) return win32_result(ERROR_INVALID_PARAMETER);
    if (!sd_get(abs, &p)) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    if (p.control & SE_SELF_RELATIVE) return win32_result(ERROR_BAD_DESCRIPTOR_FORMAT);
    DWORD n = sd_rel_size(&p);
    if (!rel || *len < n) { *len = n; return win32_result(ERROR_INSUFFICIENT_BUFFER); }
    sd_write_rel(&p, rel);
    *len = n;
    return TRUE;
}

API BOOL MakeAbsoluteSD(void *rel, SECURITY_DESCRIPTOR *abs, DWORD *abslen, ACL *dacl, DWORD *dacllen,
                        ACL *sacl, DWORD *sacllen, void *owner, DWORD *ownerlen, void *group, DWORD *grouplen)
{
    struct sd_parts p;
    if (!abslen || !dacllen || !sacllen || !ownerlen || !grouplen) return win32_result(ERROR_INVALID_PARAMETER);
    if (!sd_get(rel, &p)) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    if (!(p.control & SE_SELF_RELATIVE)) return win32_result(ERROR_BAD_DESCRIPTOR_FORMAT);
    DWORD need[5] = { sizeof(SECURITY_DESCRIPTOR), p.dacl ? p.dacl->AclSize : 0u, p.sacl ? p.sacl->AclSize : 0u,
                      p.owner ? sid_len(p.owner) : 0u, p.group ? sid_len(p.group) : 0u };
    DWORD *have[5] = { abslen, dacllen, sacllen, ownerlen, grouplen };
    void *bufs[5] = { abs, dacl, sacl, owner, group };
    int small = 0;
    for (int i = 0; i < 5; i++) if (*have[i] < need[i] || (need[i] && !bufs[i])) small = 1;
    if (small) {
        for (int i = 0; i < 5; i++) *have[i] = need[i];
        return win32_result(ERROR_INSUFFICIENT_BUFFER);
    }
    memset(abs, 0, sizeof *abs);
    abs->Revision = SECURITY_DESCRIPTOR_REVISION;
    abs->Control = (WORD)(p.control & ~SE_SELF_RELATIVE);
    if (p.dacl) { memcpy(dacl, p.dacl, need[1]); abs->Dacl = dacl; }
    if (p.sacl) { memcpy(sacl, p.sacl, need[2]); abs->Sacl = sacl; }
    if (p.owner) { memcpy(owner, p.owner, need[3]); abs->Owner = owner; }
    if (p.group) { memcpy(group, p.group, need[4]); abs->Group = group; }
    for (int i = 0; i < 5; i++) *have[i] = need[i];
    return TRUE;
}

API void MapGenericMask(DWORD *mask, const GENERIC_MAPPING *m)
{
    if (!mask || !m) return;
    if (*mask & GENERIC_READ) *mask |= m->GenericRead;
    if (*mask & GENERIC_WRITE) *mask |= m->GenericWrite;
    if (*mask & GENERIC_EXECUTE) *mask |= m->GenericExecute;
    if (*mask & GENERIC_ALL) *mask |= m->GenericAll;
    *mask &= ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

/* AccessCheck grants what is asked: every object belongs to the one LSIW
 * user, and Linux permissions are what really protect files. */
API BOOL AccessCheck(void *sd, HANDLE token, DWORD desired, const GENERIC_MAPPING *map, PRIVILEGE_SET *ps,
                     DWORD *pslen, DWORD *granted, BOOL *status)
{
    struct token t;
    if (BAD_TOKEN(token, &t)) return FALSE;
    if (!IsValidSecurityDescriptor(sd)) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    if (!map || !granted || !status) return win32_result(ERROR_INVALID_PARAMETER);
    if (pslen) {
        if (*pslen < 8 || !ps) { *pslen = 8; return win32_result(ERROR_INSUFFICIENT_BUFFER); }
        ps->PrivilegeCount = 0;
        ps->Control = 0;
    }
    DWORD m = desired;
    if (m & MAXIMUM_ALLOWED) m = (m & ~MAXIMUM_ALLOWED) | GENERIC_ALL;
    MapGenericMask(&m, map);
    *granted = m;
    *status = TRUE;
    return TRUE;
}

API BOOL AreAllAccessesGranted(DWORD granted, DWORD desired) { return (granted & desired) == desired; }
API BOOL AreAnyAccessesGranted(DWORD granted, DWORD desired) { return (granted & desired) != 0; }

/* ---- security of files and other objects ---- */

/* Every object reports build_min_sd(); changes are accepted and ignored
 * (Linux permissions stay in charge). */
enum { SE_UNKNOWN_OBJECT_TYPE, SE_FILE_OBJECT, SE_SERVICE, SE_PRINTER, SE_REGISTRY_KEY };

static DWORD object_security(int info, void **owner, void **group, ACL **dacl, ACL **sacl, void **sdout)
{
    /* Without ppSecurityDescriptor the returned pointers must stay valid
     * with nothing to free: they point into one descriptor kept for the
     * life of the process. */
    static void *shared;
    BYTE *sd;
    if (sdout) sd = build_min_sd(NULL, 1);
    else {
        if (!__atomic_load_n(&shared, __ATOMIC_ACQUIRE)) {
            void *n = build_min_sd(NULL, 0), *expect = NULL;
            if (!__atomic_compare_exchange_n(&shared, &expect, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) free(n);
        }
        sd = shared;
    }
    if (!sd) return ERROR_NOT_ENOUGH_MEMORY;
    struct sd_parts p;
    sd_get(sd, &p);
    if (owner) *owner = (info & OWNER_SECURITY_INFORMATION) ? p.owner : NULL;
    if (group) *group = (info & GROUP_SECURITY_INFORMATION) ? p.group : NULL;
    if (dacl) *dacl = NULL;             /* NULL DACL */
    if (sacl) *sacl = NULL;
    if (sdout) *sdout = sd;
    return ERROR_SUCCESS;
}

static int path_exists_w(const WCHAR *w)
{
    char *p = lsiw_path_w(w);
    struct stat st;
    int ok = p && stat(p, &st) == 0;
    free(p);
    return ok;
}

API DWORD GetSecurityInfo(HANDLE h, int type, DWORD info, void **owner, void **group, ACL **dacl, ACL **sacl, void **sd)
{
    (void)type;
    if (!h || h == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    return object_security((int)info, owner, group, dacl, sacl, sd);
}

API DWORD SetSecurityInfo(HANDLE h, int type, DWORD info, void *owner, void *group, ACL *dacl, ACL *sacl)
{
    (void)type; (void)info; (void)owner; (void)group; (void)dacl; (void)sacl;
    return (!h || h == INVALID_HANDLE_VALUE) ? ERROR_INVALID_HANDLE : ERROR_SUCCESS;
}

API DWORD GetNamedSecurityInfoW(const WCHAR *name, int type, DWORD info, void **owner, void **group, ACL **dacl,
                                ACL **sacl, void **sd)
{
    if (!name) return ERROR_INVALID_PARAMETER;
    if (type == SE_FILE_OBJECT && !path_exists_w(name)) return ERROR_FILE_NOT_FOUND;
    return object_security((int)info, owner, group, dacl, sacl, sd);
}

API DWORD GetNamedSecurityInfoA(const char *name, int type, DWORD info, void **owner, void **group, ACL **dacl,
                                ACL **sacl, void **sd)
{
    WCHAR *w = a2w(name);
    DWORD e = GetNamedSecurityInfoW(w, type, info, owner, group, dacl, sacl, sd);
    free(w);
    return e;
}

API DWORD SetNamedSecurityInfoW(WCHAR *name, int type, DWORD info, void *owner, void *group, ACL *dacl, ACL *sacl)
{
    (void)info; (void)owner; (void)group; (void)dacl; (void)sacl;
    if (!name) return ERROR_INVALID_PARAMETER;
    if (type == SE_FILE_OBJECT && !path_exists_w(name)) return ERROR_FILE_NOT_FOUND;
    return ERROR_SUCCESS;
}

API DWORD SetNamedSecurityInfoA(char *name, int type, DWORD info, void *owner, void *group, ACL *dacl, ACL *sacl)
{
    WCHAR *w = a2w(name);
    DWORD e = SetNamedSecurityInfoW(w, type, info, owner, group, dacl, sacl);
    free(w);
    return e;
}

API BOOL GetFileSecurityW(const WCHAR *name, DWORD info, void *sd, DWORD len, DWORD *needed)
{
    (void)info;
    if (!name || !needed) return win32_result(ERROR_INVALID_PARAMETER);
    if (!path_exists_w(name)) return win32_result(ERROR_FILE_NOT_FOUND);
    DWORD n;
    void *m = build_min_sd(&n, 0);
    *needed = n;
    BOOL ok = TRUE;
    if (!sd || len < n) ok = win32_result(ERROR_INSUFFICIENT_BUFFER);
    else memcpy(sd, m, n);
    free(m);
    return ok;
}

API BOOL GetFileSecurityA(const char *name, DWORD info, void *sd, DWORD len, DWORD *needed)
{
    WCHAR *w = a2w(name);
    BOOL r = GetFileSecurityW(w, info, sd, len, needed);
    free(w);
    return r;
}

API BOOL SetFileSecurityW(const WCHAR *name, DWORD info, void *sd)
{
    (void)info; (void)sd;
    if (!name) return win32_result(ERROR_INVALID_PARAMETER);
    return path_exists_w(name) ? TRUE : win32_result(ERROR_FILE_NOT_FOUND);
}

API BOOL SetFileSecurityA(const char *name, DWORD info, void *sd)
{
    WCHAR *w = a2w(name);
    BOOL r = SetFileSecurityW(w, info, sd);
    free(w);
    return r;
}

/* ---- trustees and SetEntriesInAcl ---- */

enum { TRUSTEE_IS_SID, TRUSTEE_IS_NAME, TRUSTEE_BAD_FORM, TRUSTEE_IS_OBJECTS_AND_SID, TRUSTEE_IS_OBJECTS_AND_NAME };
enum { NOT_USED_ACCESS, GRANT_ACCESS, SET_ACCESS, DENY_ACCESS, REVOKE_ACCESS, SET_AUDIT_SUCCESS, SET_AUDIT_FAILURE };

/* TRUSTEE_A/W (32 bytes) and EXPLICIT_ACCESS_A/W (48 bytes); the name is a
 * char* or WCHAR* (or a PSID for TRUSTEE_IS_SID). */
typedef struct trustee { struct trustee *pMultipleTrustee; int MultipleTrusteeOperation; int TrusteeForm;
                         int TrusteeType; void *ptstrName; } TRUSTEE;
typedef struct { DWORD grfAccessPermissions; int grfAccessMode; DWORD grfInheritance; TRUSTEE Trustee; } EXPLICIT_ACCESS;

static void build_trustee(TRUSTEE *t, int form, void *name)
{
    t->pMultipleTrustee = NULL;
    t->MultipleTrusteeOperation = 0;
    t->TrusteeForm = form;
    t->TrusteeType = 0;             /* TRUSTEE_IS_UNKNOWN */
    t->ptstrName = name;
}

API void BuildTrusteeWithSidW(TRUSTEE *t, void *sid) { if (t) build_trustee(t, TRUSTEE_IS_SID, sid); }
API void BuildTrusteeWithSidA(TRUSTEE *t, void *sid) { if (t) build_trustee(t, TRUSTEE_IS_SID, sid); }
API void BuildTrusteeWithNameW(TRUSTEE *t, WCHAR *name) { if (t) build_trustee(t, TRUSTEE_IS_NAME, name); }
API void BuildTrusteeWithNameA(TRUSTEE *t, char *name) { if (t) build_trustee(t, TRUSTEE_IS_NAME, name); }

static void build_ea(EXPLICIT_ACCESS *ea, void *name, DWORD perms, int mode, DWORD inherit)
{
    if (!ea) return;
    ea->grfAccessPermissions = perms;
    ea->grfAccessMode = mode;
    ea->grfInheritance = inherit;
    build_trustee(&ea->Trustee, TRUSTEE_IS_NAME, name);
}

API void BuildExplicitAccessWithNameW(EXPLICIT_ACCESS *ea, WCHAR *name, DWORD perms, int mode, DWORD inherit) { build_ea(ea, name, perms, mode, inherit); }
API void BuildExplicitAccessWithNameA(EXPLICIT_ACCESS *ea, char *name, DWORD perms, int mode, DWORD inherit) { build_ea(ea, name, perms, mode, inherit); }

static DWORD trustee_sid(const TRUSTEE *t, int ansi, BYTE *out)
{
    if (t->TrusteeForm == TRUSTEE_IS_SID) {
        if (!sid_valid(t->ptstrName)) return 0;
        memcpy(out, t->ptstrName, sid_len(t->ptstrName));
        return sid_len(out);
    }
    if (t->TrusteeForm != TRUSTEE_IS_NAME || !t->ptstrName) return 0;
    char *n = enc_in(t->ptstrName, ansi);
    struct account a;
    int ok = !strcmp(n, "CURRENT_USER") ? (memcpy(a.sid, user_sid(), sid_len(user_sid())), 1) : account_from_name(n, &a);
    free(n);
    if (!ok) return 0;
    memcpy(out, a.sid, sid_len(a.sid));
    return sid_len(out);
}

struct ace_item { BYTE type, flags; DWORD mask; BYTE sid[SECURITY_MAX_SID_SIZE]; BYTE *raw; WORD rawsize; };

/* Build the new ACL: entries from the old ACL, then the explicit entries
 * applied in order (SET/REVOKE first remove the trustee's explicit ACEs,
 * GRANT merges into an existing allow ACE). The result is in canonical
 * order: explicit deny, explicit allow, then inherited ACEs. */
static DWORD set_entries(DWORD count, EXPLICIT_ACCESS *list, ACL *old, ACL **out, int ansi)
{
    if (!out || (count && !list)) return ERROR_INVALID_PARAMETER;
    if (old && !acl_valid(old)) return ERROR_INVALID_ACL;
    size_t n = 0, cap = (old ? old->AceCount : 0) + count + 1;
    struct ace_item *v = xcalloc(cap, sizeof *v);
    for (WORD i = 0; old && i < old->AceCount; i++) {
        ACE_HEADER *h = acl_ace(old, i);
        struct ace_item *it = &v[n++];
        it->type = h->AceType;
        it->flags = h->AceFlags;
        if (ace_simple(h) && h->AceSize >= 8 + 8 && sid_valid(ace_sid(h))) {
            it->mask = ace_mask(h);
            memcpy(it->sid, ace_sid(h), sid_len(ace_sid(h)));
        } else {
            it->raw = xmalloc(h->AceSize);
            memcpy(it->raw, h, h->AceSize);
            it->rawsize = h->AceSize;
        }
    }
    DWORD e = ERROR_SUCCESS;
    for (DWORD i = 0; i < count && !e; i++) {
        EXPLICIT_ACCESS *ea = &list[i];
        BYTE sid[SECURITY_MAX_SID_SIZE];
        if (ea->grfAccessMode == NOT_USED_ACCESS) continue;
        if (ea->Trustee.TrusteeForm >= TRUSTEE_BAD_FORM) { e = ERROR_INVALID_PARAMETER; break; }
        if (!trustee_sid(&ea->Trustee, ansi, sid)) { e = ERROR_NONE_MAPPED; break; }
        BYTE flags = (BYTE)(ea->grfInheritance & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE | INHERIT_ONLY_ACE));
        int mode = ea->grfAccessMode;
        if (mode == SET_ACCESS || mode == REVOKE_ACCESS) {
            for (size_t j = 0; j < n; )
                if (!v[j].raw && !(v[j].flags & INHERITED_ACE) && v[j].type <= ACCESS_DENIED_ACE_TYPE && sid_equal(v[j].sid, sid))
                    v[j] = v[--n];
                else j++;
        }
        if (mode == REVOKE_ACCESS) continue;
        BYTE type = mode == DENY_ACCESS ? ACCESS_DENIED_ACE_TYPE
                  : (mode == SET_AUDIT_SUCCESS || mode == SET_AUDIT_FAILURE) ? SYSTEM_AUDIT_ACE_TYPE
                  : ACCESS_ALLOWED_ACE_TYPE;
        if (type == SYSTEM_AUDIT_ACE_TYPE)
            flags |= mode == SET_AUDIT_SUCCESS ? SUCCESSFUL_ACCESS_ACE_FLAG : FAILED_ACCESS_ACE_FLAG;
        size_t j;
        for (j = 0; j < n; j++)
            if (!v[j].raw && v[j].type == type && v[j].flags == flags && sid_equal(v[j].sid, sid)) break;
        if (j < n) { v[j].mask |= ea->grfAccessPermissions; continue; }
        struct ace_item *it = &v[n++];
        memset(it, 0, sizeof *it);
        it->type = type;
        it->flags = flags;
        it->mask = ea->grfAccessPermissions;
        memcpy(it->sid, sid, sid_len(sid));
    }
    if (!e) {
        size_t size = sizeof(ACL);
        for (size_t j = 0; j < n; j++) size += v[j].raw ? v[j].rawsize : 8 + sid_len(v[j].sid);
        if (size > 0xFFFC) e = ERROR_INVALID_PARAMETER;
        ACL *a = e ? NULL : lsiw_heap_alloc(size, 1);
        if (!e && !a) e = ERROR_NOT_ENOUGH_MEMORY;
        if (!e) {
            InitializeAcl(a, (DWORD)size, ACL_REVISION);
            /* pass 0: explicit deny, 1: explicit allow and the rest, 2: inherited */
            for (int pass = 0; pass < 3; pass++)
                for (size_t j = 0; j < n; j++) {
                    int inh = (v[j].flags & INHERITED_ACE) != 0;
                    int p = inh ? 2 : (!v[j].raw && v[j].type == ACCESS_DENIED_ACE_TYPE) ? 0 : 1;
                    if (p != pass) continue;
                    if (v[j].raw) {
                        memcpy((BYTE *)a + acl_used(a), v[j].raw, v[j].rawsize);
                        a->AceCount++;
                    } else {
                        acl_add_ace(a, ACL_REVISION, v[j].type, v[j].flags, v[j].mask, v[j].sid);
                    }
                }
            *out = a;
        }
    }
    for (size_t j = 0; j < n; j++) free(v[j].raw);
    free(v);
    return e;
}

API DWORD SetEntriesInAclW(DWORD count, EXPLICIT_ACCESS *list, ACL *old, ACL **out) { return set_entries(count, list, old, out, 0); }
API DWORD SetEntriesInAclA(DWORD count, EXPLICIT_ACCESS *list, ACL *old, ACL **out) { return set_entries(count, list, old, out, 1); }

/* ---- SDDL ---- */

static const struct { const char *code; DWORD mask; } g_sddl_rights[] = {
    { "GA", GENERIC_ALL }, { "GR", GENERIC_READ }, { "GW", GENERIC_WRITE }, { "GX", GENERIC_EXECUTE },
    { "RC", READ_CONTROL }, { "SD", DELETE }, { "WD", WRITE_DAC }, { "WO", WRITE_OWNER },
    { "RP", 0x10 }, { "WP", 0x20 }, { "CC", 0x1 }, { "DC", 0x2 }, { "LC", 0x4 }, { "SW", 0x8 },
    { "LO", 0x80 }, { "DT", 0x40 }, { "CR", 0x100 },
    { "FA", 0x1F01FF }, { "FR", 0x120089 }, { "FW", 0x120116 }, { "FX", 0x1200A0 },
    { "KA", 0xF003F }, { "KR", 0x20019 }, { "KW", 0x20006 }, { "KX", 0x20019 },
    { "NR", 0x1 }, { "NW", 0x2 }, { "NX", 0x4 },
};

static const struct { const char *code; BYTE flag; } g_sddl_aceflags[] = {
    { "OI", OBJECT_INHERIT_ACE }, { "CI", CONTAINER_INHERIT_ACE }, { "NP", NO_PROPAGATE_INHERIT_ACE },
    { "IO", INHERIT_ONLY_ACE }, { "ID", INHERITED_ACE }, { "SA", SUCCESSFUL_ACCESS_ACE_FLAG }, { "FA", FAILED_ACCESS_ACE_FLAG },
};

/* Parse "(type;flags;rights;;;sid)" ACEs into acl (room for cap bytes). */
static int sddl_acl(const char **pp, BYTE *buf, size_t cap, WORD *control, int dacl, int *null_acl)
{
    const char *p = *pp;
    *null_acl = 0;
    for (;;) {             /* ACL flags */
        if (*p == 'P') { *control |= dacl ? SE_DACL_PROTECTED : SE_SACL_PROTECTED; p++; }
        else if (!strncmp(p, "AI", 2)) { *control |= dacl ? SE_DACL_AUTO_INHERITED : SE_SACL_AUTO_INHERITED; p += 2; }
        else if (!strncmp(p, "AR", 2)) { *control |= dacl ? SE_DACL_AUTO_INHERIT_REQ : SE_SACL_AUTO_INHERIT_REQ; p += 2; }
        else if (!strncmp(p, "NO_ACCESS_CONTROL", 17)) { *null_acl = 1; p += 17; }
        else break;
    }
    ACL *a = (ACL *)buf;
    InitializeAcl(a, (DWORD)cap, ACL_REVISION);
    while (*p == '(') {
        p++;
        BYTE type;
        if (!strncmp(p, "AU", 2)) { type = SYSTEM_AUDIT_ACE_TYPE; p += 2; }
        else if (!strncmp(p, "ML", 2)) { type = SYSTEM_MANDATORY_LABEL_ACE_TYPE; p += 2; }
        else if (*p == 'A') { type = ACCESS_ALLOWED_ACE_TYPE; p++; }
        else if (*p == 'D') { type = ACCESS_DENIED_ACE_TYPE; p++; }
        else return 0;      /* object ACEs (OA, OD...) are not supported */
        if (*p++ != ';') return 0;
        BYTE flags = 0;
        while (*p && *p != ';') {
            size_t i;
            for (i = 0; i < sizeof g_sddl_aceflags / sizeof g_sddl_aceflags[0]; i++)
                if (!strncmp(p, g_sddl_aceflags[i].code, 2)) { flags |= g_sddl_aceflags[i].flag; p += 2; break; }
            if (i == sizeof g_sddl_aceflags / sizeof g_sddl_aceflags[0]) return 0;
        }
        if (*p++ != ';') return 0;
        DWORD mask = 0;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            char *e;
            mask = (DWORD)strtoul(p + 2, &e, 16);
            p = e;
        } else {
            while (*p && *p != ';') {
                size_t i;
                for (i = 0; i < sizeof g_sddl_rights / sizeof g_sddl_rights[0]; i++)
                    if (!strncmp(p, g_sddl_rights[i].code, 2)) { mask |= g_sddl_rights[i].mask; p += 2; break; }
                if (i == sizeof g_sddl_rights / sizeof g_sddl_rights[0]) return 0;
            }
        }
        if (*p++ != ';') return 0;
        if (*p++ != ';') return 0;      /* object type GUID: unsupported, must be empty */
        if (*p++ != ';') return 0;      /* inherited object type GUID */
        BYTE sid[SECURITY_MAX_SID_SIZE];
        if (!parse_sid_str(p, &p, sid)) return 0;
        if (*p++ != ')') return 0;
        if (acl_add_ace(a, ACL_REVISION, type, flags, mask, sid)) return 0;
    }
    a->AclSize = (WORD)acl_used(a);
    *pp = p;
    return 1;
}

static BOOL sddl_to_sd(const char *s, void **out, DWORD *outlen)
{
    BYTE owner[SECURITY_MAX_SID_SIZE], group[SECURITY_MAX_SID_SIZE];
    size_t cap = strlen(s) * 16 + 64;       /* every ACE string is longer than 1/16 of its ACE */
    if (cap > 0xFFFC) cap = 0xFFFC;
    BYTE *dbuf = xcalloc(1, cap), *sbuf = xcalloc(1, cap);
    struct sd_parts p = { 0, NULL, NULL, NULL, NULL };
    int ok = 1;
    const char *q = s;
    while (ok && *q) {
        while (*q == ' ') q++;
        if (!*q) break;
        char k = q[0];
        if (q[1] != ':') { ok = 0; break; }
        q += 2;
        int null_acl;
        switch (k) {
        case 'O': ok = parse_sid_str(q, &q, owner) != 0; p.owner = owner; break;
        case 'G': ok = parse_sid_str(q, &q, group) != 0; p.group = group; break;
        case 'D':
            ok = sddl_acl(&q, dbuf, cap, &p.control, 1, &null_acl);
            p.control |= SE_DACL_PRESENT;
            p.dacl = null_acl ? NULL : (ACL *)dbuf;
            break;
        case 'S':
            ok = sddl_acl(&q, sbuf, cap, &p.control, 0, &null_acl);
            p.control |= SE_SACL_PRESENT;
            p.sacl = null_acl ? NULL : (ACL *)sbuf;
            break;
        default: ok = 0;
        }
    }
    if (ok) {
        *out = sd_alloc_rel(&p, outlen, 1);
        if (!*out) { ok = 0; lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); }
    } else {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
    }
    free(dbuf);
    free(sbuf);
    return ok;
}

#define SDDL_REVISION_1 1

API BOOL ConvertStringSecurityDescriptorToSecurityDescriptorW(const WCHAR *str, DWORD rev, void **sd, ULONG *len)
{
    if (!str || !sd) return win32_result(ERROR_INVALID_PARAMETER);
    if (rev != SDDL_REVISION_1) return win32_result(ERROR_UNKNOWN_REVISION);
    char *s = w2a(str);
    DWORD n = 0;
    BOOL r = sddl_to_sd(s, sd, &n);
    free(s);
    if (r && len) *len = n;
    return r;
}

API BOOL ConvertStringSecurityDescriptorToSecurityDescriptorA(const char *str, DWORD rev, void **sd, ULONG *len)
{
    if (!str || !sd) return win32_result(ERROR_INVALID_PARAMETER);
    if (rev != SDDL_REVISION_1) return win32_result(ERROR_UNKNOWN_REVISION);
    DWORD n = 0;
    BOOL r = sddl_to_sd(str, sd, &n);
    if (r && len) *len = n;
    return r;
}

static void sb_add(char **s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_add(char **s, const char *fmt, ...)
{
    va_list ap;
    char *add;
    va_start(ap, fmt);
    if (vasprintf(&add, fmt, ap) < 0) { va_end(ap); lsiw_log("advapi32: out of memory\n"); abort(); }
    va_end(ap);
    char *t = lsiw_strdup_printf("%s%s", *s, add);
    free(*s);
    free(add);
    *s = t;
}

static void sb_sid(char **s, const void *sid)
{
    const char *a = sid_alias(sid);
    if (a) sb_add(s, "%s", a);
    else { char *t = sid_to_str(sid); sb_add(s, "%s", t); free(t); }
}

static void sb_acl(char **s, const ACL *a, WORD control, int dacl)
{
    if (control & (dacl ? SE_DACL_PROTECTED : SE_SACL_PROTECTED)) sb_add(s, "P");
    if (control & (dacl ? SE_DACL_AUTO_INHERIT_REQ : SE_SACL_AUTO_INHERIT_REQ)) sb_add(s, "AR");
    if (control & (dacl ? SE_DACL_AUTO_INHERITED : SE_SACL_AUTO_INHERITED)) sb_add(s, "AI");
    if (!a) { sb_add(s, "NO_ACCESS_CONTROL"); return; }
    for (WORD i = 0; i < a->AceCount; i++) {
        const ACE_HEADER *h = acl_ace(a, i);
        if (!ace_simple(h)) continue;
        const char *t = h->AceType == ACCESS_ALLOWED_ACE_TYPE ? "A" : h->AceType == ACCESS_DENIED_ACE_TYPE ? "D"
                      : h->AceType == SYSTEM_AUDIT_ACE_TYPE ? "AU" : "ML";
        sb_add(s, "(%s;", t);
        for (size_t f = 0; f < sizeof g_sddl_aceflags / sizeof g_sddl_aceflags[0]; f++)
            if (h->AceFlags & g_sddl_aceflags[f].flag) sb_add(s, "%s", g_sddl_aceflags[f].code);
        DWORD m = ace_mask(h);
        const char *code = NULL;
        static const char *const combined[] = { "FA", "FR", "FW", "FX", "KA", "KR", "KW", "GA", "GR", "GW", "GX" };
        for (size_t c = 0; !code && c < sizeof combined / sizeof combined[0]; c++)
            for (size_t r = 0; r < sizeof g_sddl_rights / sizeof g_sddl_rights[0]; r++)
                if (!strcmp(g_sddl_rights[r].code, combined[c]) && g_sddl_rights[r].mask == m) { code = combined[c]; break; }
        if (code) sb_add(s, ";%s;;;", code);
        else sb_add(s, ";0x%x;;;", (unsigned)m);
        sb_sid(s, ace_sid(h));
        sb_add(s, ")");
    }
}

static BOOL sd_to_sddl(void *sd, DWORD info, void **out, ULONG *len, int ansi)
{
    struct sd_parts p;
    if (!out) return win32_result(ERROR_INVALID_PARAMETER);
    if (!sd_get(sd, &p)) return win32_result(ERROR_INVALID_SECURITY_DESCR);
    char *s = xstrdup("");
    if ((info & OWNER_SECURITY_INFORMATION) && p.owner) { sb_add(&s, "O:"); sb_sid(&s, p.owner); }
    if ((info & GROUP_SECURITY_INFORMATION) && p.group) { sb_add(&s, "G:"); sb_sid(&s, p.group); }
    if ((info & DACL_SECURITY_INFORMATION) && (p.control & SE_DACL_PRESENT)) { sb_add(&s, "D:"); sb_acl(&s, p.dacl, p.control, 1); }
    if ((info & SACL_SECURITY_INFORMATION) && (p.control & SE_SACL_PRESENT)) { sb_add(&s, "S:"); sb_acl(&s, p.sacl, p.control, 0); }
    *out = enc_heap(s, ansi);
    if (len) *len = (ULONG)enc_len(s, ansi) + 1;
    free(s);
    return *out ? TRUE : win32_result(ERROR_NOT_ENOUGH_MEMORY);
}

API BOOL ConvertSecurityDescriptorToStringSecurityDescriptorW(void *sd, DWORD rev, DWORD info, WCHAR **out, ULONG *len)
{
    if (rev != SDDL_REVISION_1) return win32_result(ERROR_UNKNOWN_REVISION);
    return sd_to_sddl(sd, info, (void **)out, len, 0);
}

API BOOL ConvertSecurityDescriptorToStringSecurityDescriptorA(void *sd, DWORD rev, DWORD info, char **out, ULONG *len)
{
    if (rev != SDDL_REVISION_1) return win32_result(ERROR_UNKNOWN_REVISION);
    return sd_to_sddl(sd, info, (void **)out, len, 1);
}

/* ================================================================== */
/* CryptoAPI: providers, hashes, HMAC keys, random numbers              */
/* ================================================================== */

#define NTE_BAD_HASH     ((DWORD)0x80090002)
#define NTE_BAD_KEY      ((DWORD)0x80090003)
#define NTE_BAD_LEN      ((DWORD)0x80090004)
#define NTE_BAD_DATA     ((DWORD)0x80090005)
#define NTE_BAD_ALGID    ((DWORD)0x80090008)
#define NTE_BAD_FLAGS    ((DWORD)0x80090009)
#define NTE_BAD_TYPE     ((DWORD)0x8009000A)
#define NTE_BAD_UID      ((DWORD)0x80090001)
#define NTE_BAD_HASH_STATE ((DWORD)0x8009000C)
#define NTE_BAD_KEYSET   ((DWORD)0x80090016)
#define NTE_EXISTS       ((DWORD)0x8009000F)

#define CALG_MD5     0x8003
#define CALG_SHA1    0x8004
#define CALG_HMAC    0x8009
#define CALG_SHA_256 0x800c
#define CALG_SHA_384 0x800d
#define CALG_SHA_512 0x800e

#define HP_ALGID     1
#define HP_HASHVAL   2
#define HP_HASHSIZE  4
#define HP_HMAC_INFO 5

#define PP_ENUMALGS    1
#define PP_CONTAINER   6
#define PP_NAME        4
#define PP_VERSION     5
#define PP_IMPTYPE     3
#define PP_PROVTYPE    16
#define PP_UNIQUE_CONTAINER 36

#define CRYPT_NEWKEYSET     0x08
#define CRYPT_DELETEKEYSET  0x10

#define PLAINTEXTKEYBLOB 0x8
#define CRYPT_IPSEC_HMAC_KEY 0x100

struct crypt_prov { DWORD type; char *container; };
struct crypt_key  { ALG_ID alg; uint8_t *data; size_t len; };
struct crypt_hash {
    ALG_ID   algid;         /* as passed to CryptCreateHash              */
    int      alg;           /* LSIW_HASH_*: the digest (HMAC's inner one) */
    struct lsiw_hash ctx;   /* plain hashes stream into this              */
    uint8_t *key; size_t klen;              /* HMAC                        */
    uint8_t *buf; size_t blen, bcap;        /* HMAC: data, hashed at the end */
    int      hmac_ready;
    int      done;
    uint8_t  digest[LSIW_HASH_MAX_DIGEST];
    size_t   dlen;
};

static int calg_to_hash(ALG_ID a)
{
    switch (a) {
    case CALG_MD5: return LSIW_HASH_MD5;
    case CALG_SHA1: return LSIW_HASH_SHA1;
    case CALG_SHA_256: return LSIW_HASH_SHA256;
    case CALG_SHA_384: return LSIW_HASH_SHA384;
    case CALG_SHA_512: return LSIW_HASH_SHA512;
    }
    return 0;
}

static BOOL crypt_fail(DWORD e) { lsiw_set_last_error(e); return FALSE; }

/* Every context succeeds: there is one in-memory provider and no
 * persistent key containers (CRYPT_DELETEKEYSET just succeeds). */
static BOOL acquire_context(HCRYPTPROV *out, const char *container, DWORD type, DWORD flags)
{
    if (!out) return crypt_fail(ERROR_INVALID_PARAMETER);
    *out = 0;
    if (flags & CRYPT_DELETEKEYSET) return TRUE;
    struct crypt_prov *p = xcalloc(1, sizeof *p);
    p->type = type;
    p->container = xstrdup(container ? container : lsiw_user_name());
    obj_add(p, OBJ_PROV);
    *out = (HCRYPTPROV)p;
    return TRUE;
}

API BOOL CryptAcquireContextA(HCRYPTPROV *out, const char *container, const char *prov, DWORD type, DWORD flags)
{
    (void)prov;
    return acquire_context(out, container, type, flags);
}

API BOOL CryptAcquireContextW(HCRYPTPROV *out, const WCHAR *container, const WCHAR *prov, DWORD type, DWORD flags)
{
    (void)prov;
    char *c = w2a(container);
    BOOL r = acquire_context(out, c, type, flags);
    free(c);
    return r;
}

static struct crypt_prov *prov_get(HCRYPTPROV h) { return obj_check((void *)h, OBJ_PROV) ? (struct crypt_prov *)h : NULL; }

API BOOL CryptReleaseContext(HCRYPTPROV h, DWORD flags)
{
    if (flags) return crypt_fail(NTE_BAD_FLAGS);
    struct crypt_prov *p = prov_get(h);
    if (!p || !obj_remove(p, OBJ_PROV)) return crypt_fail(NTE_BAD_UID);
    free(p->container);
    free(p);
    return TRUE;
}

API BOOL CryptContextAddRef(HCRYPTPROV h, DWORD *reserved, DWORD flags)
{
    (void)reserved; (void)flags;
    /* Contexts are not reference counted: the first release frees it, so
     * report the call as unsupported rather than pretend. */
    return prov_get(h) ? crypt_fail(ERROR_CALL_NOT_IMPLEMENTED) : crypt_fail(NTE_BAD_UID);
}

static void fill_random(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) { if (errno == EINTR) continue; break; }
        p += r;
        n -= (size_t)r;
    }
    if (n) {        /* no getrandom (very old kernels): /dev/urandom */
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        while (fd >= 0 && n) {
            ssize_t r = read(fd, p, n);
            if (r <= 0) break;
            p += r;
            n -= (size_t)r;
        }
        if (fd >= 0) close(fd);
    }
}

API BOOL CryptGenRandom(HCRYPTPROV h, DWORD len, BYTE *buf)
{
    if (!prov_get(h)) return crypt_fail(NTE_BAD_UID);
    if (len && !buf) return crypt_fail(ERROR_INVALID_PARAMETER);
    fill_random(buf, len);
    return TRUE;
}

/* RtlGenRandom. */
API BOOLEAN SystemFunction036(void *buf, ULONG len)
{
    if (len && !buf) return FALSE;
    fill_random(buf, len);
    return TRUE;
}

API BOOL CryptGetProvParam(HCRYPTPROV h, DWORD param, BYTE *data, DWORD *len, DWORD flags)
{
    (void)flags;
    struct crypt_prov *p = prov_get(h);
    if (!p) return crypt_fail(NTE_BAD_UID);
    if (!len) return crypt_fail(ERROR_INVALID_PARAMETER);
    const char *str = NULL;
    DWORD num = 0;
    int is_num = 0;
    switch (param) {
    case PP_NAME: str = p->type == 24 ? "Microsoft Enhanced RSA and AES Cryptographic Provider"
                                      : "Microsoft Base Cryptographic Provider v1.0"; break;
    case PP_CONTAINER: case PP_UNIQUE_CONTAINER: str = p->container; break;
    case PP_VERSION: num = 0x200; is_num = 1; break;
    case PP_IMPTYPE: num = 1; is_num = 1; break;            /* CRYPT_IMPL_SOFTWARE */
    case PP_PROVTYPE: num = p->type; is_num = 1; break;
    case PP_ENUMALGS: return crypt_fail(ERROR_NO_MORE_ITEMS);
    default: return crypt_fail(NTE_BAD_TYPE);
    }
    DWORD need = is_num ? 4 : (DWORD)strlen(str) + 1;
    if (!data) { *len = need; return TRUE; }
    if (*len < need) { *len = need; return crypt_fail(ERROR_MORE_DATA); }
    if (is_num) memcpy(data, &num, 4); else memcpy(data, str, need);
    *len = need;
    return TRUE;
}

API BOOL CryptSetProvParam(HCRYPTPROV h, DWORD param, const BYTE *data, DWORD flags)
{
    (void)param; (void)data; (void)flags;
    return prov_get(h) ? TRUE : crypt_fail(NTE_BAD_UID);
}

/* ---- keys: only plaintext key blobs, for HMAC ---- */

typedef struct { BYTE bType, bVersion; WORD reserved; ALG_ID aiKeyAlg; } BLOBHEADER;

static struct crypt_key *key_get(HCRYPTKEY h) { return obj_check((void *)h, OBJ_KEY) ? (struct crypt_key *)h : NULL; }

API BOOL CryptImportKey(HCRYPTPROV hp, const BYTE *data, DWORD len, HCRYPTKEY pub, DWORD flags, HCRYPTKEY *out)
{
    (void)pub; (void)flags;
    if (!prov_get(hp)) return crypt_fail(NTE_BAD_UID);
    if (!data || !out || len < sizeof(BLOBHEADER) + 4) return crypt_fail(ERROR_INVALID_PARAMETER);
    BLOBHEADER bh;
    memcpy(&bh, data, sizeof bh);
    if (bh.bType != PLAINTEXTKEYBLOB) return crypt_fail(NTE_BAD_TYPE);
    DWORD klen;
    memcpy(&klen, data + sizeof bh, 4);
    if (klen > len - sizeof bh - 4) return crypt_fail(NTE_BAD_DATA);
    struct crypt_key *k = xcalloc(1, sizeof *k);
    k->alg = bh.aiKeyAlg;
    k->len = klen;
    k->data = xmalloc(klen);
    memcpy(k->data, data + sizeof bh + 4, klen);
    obj_add(k, OBJ_KEY);
    *out = (HCRYPTKEY)k;
    return TRUE;
}

API BOOL CryptDestroyKey(HCRYPTKEY h)
{
    struct crypt_key *k = key_get(h);
    if (!k || !obj_remove(k, OBJ_KEY)) return crypt_fail(NTE_BAD_KEY);
    memset(k->data, 0, k->len);
    free(k->data);
    free(k);
    return TRUE;
}

/* Encryption, key generation, derivation and export are not provided: they
 * fail the way a provider without the algorithm does. */
API BOOL CryptGenKey(HCRYPTPROV hp, ALG_ID alg, DWORD flags, HCRYPTKEY *out)
{
    (void)alg; (void)flags;
    if (out) *out = 0;
    return prov_get(hp) ? crypt_fail(NTE_BAD_ALGID) : crypt_fail(NTE_BAD_UID);
}

API BOOL CryptDeriveKey(HCRYPTPROV hp, ALG_ID alg, HCRYPTHASH hh, DWORD flags, HCRYPTKEY *out)
{
    (void)alg; (void)hh; (void)flags;
    if (out) *out = 0;
    return prov_get(hp) ? crypt_fail(NTE_BAD_ALGID) : crypt_fail(NTE_BAD_UID);
}

API BOOL CryptExportKey(HCRYPTKEY h, HCRYPTKEY exp, DWORD type, DWORD flags, BYTE *data, DWORD *len)
{
    (void)exp; (void)type; (void)flags; (void)data; (void)len;
    return key_get(h) ? crypt_fail(NTE_BAD_ALGID) : crypt_fail(NTE_BAD_KEY);
}

API BOOL CryptEncrypt(HCRYPTKEY h, HCRYPTHASH hh, BOOL final, DWORD flags, BYTE *data, DWORD *len, DWORD buflen)
{
    (void)hh; (void)final; (void)flags; (void)data; (void)len; (void)buflen;
    return key_get(h) ? crypt_fail(NTE_BAD_ALGID) : crypt_fail(NTE_BAD_KEY);
}

API BOOL CryptDecrypt(HCRYPTKEY h, HCRYPTHASH hh, BOOL final, DWORD flags, BYTE *data, DWORD *len)
{
    (void)hh; (void)final; (void)flags; (void)data; (void)len;
    return key_get(h) ? crypt_fail(NTE_BAD_ALGID) : crypt_fail(NTE_BAD_KEY);
}

/* ---- hashes ---- */

static struct crypt_hash *hash_get(HCRYPTHASH h) { return obj_check((void *)h, OBJ_HASH) ? (struct crypt_hash *)h : NULL; }

API BOOL CryptCreateHash(HCRYPTPROV hp, ALG_ID alg, HCRYPTKEY hk, DWORD flags, HCRYPTHASH *out)
{
    if (!prov_get(hp)) return crypt_fail(NTE_BAD_UID);
    if (!out) return crypt_fail(ERROR_INVALID_PARAMETER);
    if (flags) return crypt_fail(NTE_BAD_FLAGS);
    struct crypt_hash *h = xcalloc(1, sizeof *h);
    h->algid = alg;
    if (alg == CALG_HMAC) {
        struct crypt_key *k = key_get(hk);
        if (!k) { free(h); return crypt_fail(NTE_BAD_KEY); }
        h->klen = k->len;
        h->key = xmalloc(k->len ? k->len : 1);
        memcpy(h->key, k->data, k->len);
    } else {
        h->alg = calg_to_hash(alg);
        if (!h->alg) { free(h); return crypt_fail(NTE_BAD_ALGID); }
        lsiw_hash_init(&h->ctx, h->alg);
    }
    obj_add(h, OBJ_HASH);
    *out = (HCRYPTHASH)h;
    return TRUE;
}

API BOOL CryptHashData(HCRYPTHASH hh, const BYTE *data, DWORD len, DWORD flags)
{
    (void)flags;
    struct crypt_hash *h = hash_get(hh);
    if (!h) return crypt_fail(NTE_BAD_HASH);
    if (h->done) return crypt_fail(NTE_BAD_HASH_STATE);
    if (len && !data) return crypt_fail(ERROR_INVALID_PARAMETER);
    if (h->algid == CALG_HMAC) {
        if (!h->hmac_ready) return crypt_fail(NTE_BAD_HASH_STATE);  /* HP_HMAC_INFO first */
        if (h->blen + len > h->bcap) {
            h->bcap = (h->blen + len) * 2;
            h->buf = xrealloc(h->buf, h->bcap);
        }
        memcpy(h->buf + h->blen, data, len);
        h->blen += len;
    } else {
        lsiw_hash_update(&h->ctx, data, len);
    }
    return TRUE;
}

API BOOL CryptHashSessionKey(HCRYPTHASH hh, HCRYPTKEY hk, DWORD flags)
{
    (void)flags;
    struct crypt_key *k = key_get(hk);
    if (!hash_get(hh)) return crypt_fail(NTE_BAD_HASH);
    if (!k) return crypt_fail(NTE_BAD_KEY);
    return CryptHashData(hh, k->data, (DWORD)k->len, 0);
}

static void hash_finish(struct crypt_hash *h)
{
    if (h->done) return;
    if (h->algid == CALG_HMAC) h->dlen = lsiw_hmac(h->alg, h->key, h->klen, h->buf, h->blen, h->digest);
    else h->dlen = lsiw_hash_final(&h->ctx, h->digest);
    h->done = 1;
}

API BOOL CryptGetHashParam(HCRYPTHASH hh, DWORD param, BYTE *data, DWORD *len, DWORD flags)
{
    (void)flags;
    struct crypt_hash *h = hash_get(hh);
    if (!h) return crypt_fail(NTE_BAD_HASH);
    if (!len) return crypt_fail(ERROR_INVALID_PARAMETER);
    DWORD v;
    const void *src;
    DWORD need;
    switch (param) {
    case HP_ALGID: v = h->algid; src = &v; need = 4; break;
    case HP_HASHSIZE:
        if (!h->alg) return crypt_fail(NTE_BAD_HASH_STATE);
        v = (DWORD)lsiw_hash_size(h->alg); src = &v; need = 4;
        break;
    case HP_HASHVAL:
        if (!h->alg) return crypt_fail(NTE_BAD_HASH_STATE);
        need = (DWORD)lsiw_hash_size(h->alg);
        if (!data) { *len = need; return TRUE; }
        if (*len < need) { *len = need; return crypt_fail(ERROR_MORE_DATA); }
        hash_finish(h);         /* after this, CryptHashData fails */
        src = h->digest;
        break;
    default: return crypt_fail(NTE_BAD_TYPE);
    }
    if (!data) { *len = need; return TRUE; }
    if (*len < need) { *len = need; return crypt_fail(ERROR_MORE_DATA); }
    memcpy(data, src, need);
    *len = need;
    return TRUE;
}

/* HMAC_INFO: HashAlgid, pbInnerString, cbInnerString, pbOuterString,
 * cbOuterString. Only the standard 0x36/0x5c pads are supported. */
typedef struct { ALG_ID HashAlgid; BYTE *pbInnerString; DWORD cbInnerString; BYTE *pbOuterString; DWORD cbOuterString; } HMAC_INFO;

API BOOL CryptSetHashParam(HCRYPTHASH hh, DWORD param, const BYTE *data, DWORD flags)
{
    (void)flags;
    struct crypt_hash *h = hash_get(hh);
    if (!h) return crypt_fail(NTE_BAD_HASH);
    if (!data) return crypt_fail(ERROR_INVALID_PARAMETER);
    if (param == HP_HMAC_INFO) {
        if (h->algid != CALG_HMAC || h->hmac_ready) return crypt_fail(NTE_BAD_TYPE);
        const HMAC_INFO *hi = (const HMAC_INFO *)data;
        if (hi->cbInnerString || hi->cbOuterString) return crypt_fail(NTE_BAD_DATA);
        h->alg = calg_to_hash(hi->HashAlgid);
        if (!h->alg) return crypt_fail(NTE_BAD_ALGID);
        h->hmac_ready = 1;
        return TRUE;
    }
    if (param == HP_HASHVAL) {
        if (!h->alg) return crypt_fail(NTE_BAD_HASH_STATE);
        h->dlen = lsiw_hash_size(h->alg);
        memcpy(h->digest, data, h->dlen);
        h->done = 1;
        return TRUE;
    }
    return crypt_fail(NTE_BAD_TYPE);
}

API BOOL CryptDestroyHash(HCRYPTHASH hh)
{
    struct crypt_hash *h = hash_get(hh);
    if (!h || !obj_remove(h, OBJ_HASH)) return crypt_fail(NTE_BAD_HASH);
    if (h->key) { memset(h->key, 0, h->klen); free(h->key); }
    free(h->buf);
    free(h);
    return TRUE;
}

API BOOL CryptDuplicateHash(HCRYPTHASH hh, DWORD *reserved, DWORD flags, HCRYPTHASH *out)
{
    (void)reserved;
    struct crypt_hash *h = hash_get(hh);
    if (!h) return crypt_fail(NTE_BAD_HASH);
    if (!out || flags) return crypt_fail(ERROR_INVALID_PARAMETER);
    struct crypt_hash *d = xmalloc(sizeof *d);
    *d = *h;
    if (h->key) { d->key = xmalloc(h->klen ? h->klen : 1); memcpy(d->key, h->key, h->klen); }
    if (h->buf) { d->buf = xmalloc(h->bcap); memcpy(d->buf, h->buf, h->blen); }
    obj_add(d, OBJ_HASH);
    *out = (HCRYPTHASH)d;
    return TRUE;
}

API BOOL CryptDuplicateKey(HCRYPTKEY hk, DWORD *reserved, DWORD flags, HCRYPTKEY *out)
{
    (void)reserved;
    struct crypt_key *k = key_get(hk);
    if (!k) return crypt_fail(NTE_BAD_KEY);
    if (!out || flags) return crypt_fail(ERROR_INVALID_PARAMETER);
    struct crypt_key *d = xcalloc(1, sizeof *d);
    d->alg = k->alg;
    d->len = k->len;
    d->data = xmalloc(k->len ? k->len : 1);
    memcpy(d->data, k->data, k->len);
    obj_add(d, OBJ_KEY);
    *out = (HCRYPTKEY)d;
    return TRUE;
}

/* RtlEncryptMemory / RtlDecryptMemory: memory must be a multiple of 8
 * bytes. This is a reversible XOR with a random per-process key; it keeps
 * secrets out of casual memory dumps but is NOT real encryption, and
 * the cross-process flags are not honoured. */
#define RTL_ENCRYPT_MEMORY_SIZE 8

static uint8_t        g_mem_key[64];
static pthread_once_t g_mem_key_once = PTHREAD_ONCE_INIT;

static void mem_key_init(void) { fill_random(g_mem_key, sizeof g_mem_key); }

static NTSTATUS memory_xor(void *mem, ULONG len)
{
    if (len % RTL_ENCRYPT_MEMORY_SIZE || (!mem && len)) return STATUS_INVALID_PARAMETER;
    pthread_once(&g_mem_key_once, mem_key_init);
    uint8_t *p = mem;
    for (ULONG i = 0; i < len; i++) p[i] ^= g_mem_key[i % sizeof g_mem_key];
    return STATUS_SUCCESS;
}

API NTSTATUS SystemFunction040(void *mem, ULONG len, ULONG flags) { (void)flags; return memory_xor(mem, len); }
API NTSTATUS SystemFunction041(void *mem, ULONG len, ULONG flags) { (void)flags; return memory_xor(mem, len); }

/* ================================================================== */
/* Event log                                                            */
/* ================================================================== */

/* Programs may report events; with --verbose they are printed on stderr.
 * There is no stored log to read back: logs open and are empty. */
struct evsrc { char *name; };

static const char *ev_type_name(WORD t)
{
    switch (t) {
    case 0x0001: return "error";
    case 0x0002: return "warning";
    case 0x0004: return "information";
    case 0x0008: return "audit success";
    case 0x0010: return "audit failure";
    }
    return "success";
}

static HANDLE register_source(const char *server, const char *name)
{
    if (!is_local_machine_a(server)) { lsiw_set_last_error(RPC_S_SERVER_UNAVAILABLE); return NULL; }
    struct evsrc *s = xcalloc(1, sizeof *s);
    s->name = xstrdup(name ? name : "Application");
    obj_add(s, OBJ_EVSRC);
    return s;
}

API HANDLE RegisterEventSourceA(const char *server, const char *name) { return register_source(server, name); }

API HANDLE RegisterEventSourceW(const WCHAR *server, const WCHAR *name)
{
    char *a = w2a(server), *b = w2a(name);
    HANDLE h = register_source(a, b);
    free(a); free(b);
    return h;
}

API BOOL DeregisterEventSource(HANDLE h)
{
    struct evsrc *s = h;
    if (!obj_remove(s, OBJ_EVSRC)) return win32_result(ERROR_INVALID_HANDLE);
    free(s->name);
    free(s);
    return TRUE;
}

static BOOL report_event(HANDLE h, WORD type, WORD cat, DWORD id, WORD n, const void *const *strings, int ansi)
{
    struct evsrc *s = h;
    if (!obj_check(s, OBJ_EVSRC)) return win32_result(ERROR_INVALID_HANDLE);
    if (n && !strings) return win32_result(ERROR_INVALID_PARAMETER);
    if (lsiw_verbose) {
        char *msg = xstrdup("");
        for (WORD i = 0; i < n; i++) {
            char *u = strings[i] ? enc_in(strings[i], ansi) : xstrdup("");
            sb_add(&msg, "%s%s", i ? " | " : "", u);
            free(u);
        }
        lsiw_log("event log: %s %s (category %u, id %u): %s\n", s->name, ev_type_name(type), (unsigned)cat,
                 (unsigned)(id & 0xFFFF), msg);
        free(msg);
    }
    return TRUE;
}

API BOOL ReportEventA(HANDLE h, WORD type, WORD cat, DWORD id, void *sid, WORD n, DWORD datalen,
                      const char **strings, void *data)
{
    (void)sid; (void)datalen; (void)data;
    return report_event(h, type, cat, id, n, (const void *const *)strings, 1);
}

API BOOL ReportEventW(HANDLE h, WORD type, WORD cat, DWORD id, void *sid, WORD n, DWORD datalen,
                      const WCHAR **strings, void *data)
{
    (void)sid; (void)datalen; (void)data;
    return report_event(h, type, cat, id, n, (const void *const *)strings, 0);
}

static HANDLE open_log(int local)
{
    if (!local) { lsiw_set_last_error(RPC_S_SERVER_UNAVAILABLE); return NULL; }
    void *p = xcalloc(1, 1);
    obj_add(p, OBJ_EVLOG);
    return p;
}

API HANDLE OpenEventLogA(const char *server, const char *source) { (void)source; return open_log(is_local_machine_a(server)); }
API HANDLE OpenEventLogW(const WCHAR *server, const WCHAR *source) { (void)source; return open_log(is_local_machine_w(server)); }

API BOOL CloseEventLog(HANDLE h)
{
    if (!obj_remove(h, OBJ_EVLOG)) return win32_result(ERROR_INVALID_HANDLE);
    free(h);
    return TRUE;
}

static BOOL log_ok(HANDLE h) { return obj_check(h, OBJ_EVLOG) ? TRUE : win32_result(ERROR_INVALID_HANDLE); }

API BOOL ReadEventLogA(HANDLE h, DWORD flags, DWORD off, void *buf, DWORD size, DWORD *read, DWORD *needed)
{
    (void)flags; (void)off; (void)buf; (void)size;
    if (!log_ok(h)) return FALSE;
    if (read) *read = 0;
    if (needed) *needed = 0;
    return win32_result(ERROR_HANDLE_EOF);
}

API BOOL ReadEventLogW(HANDLE h, DWORD flags, DWORD off, void *buf, DWORD size, DWORD *read, DWORD *needed)
{
    return ReadEventLogA(h, flags, off, buf, size, read, needed);
}

API BOOL GetNumberOfEventLogRecords(HANDLE h, DWORD *n)
{
    if (!log_ok(h)) return FALSE;
    if (!n) return win32_result(ERROR_INVALID_PARAMETER);
    *n = 0;
    return TRUE;
}

API BOOL GetOldestEventLogRecord(HANDLE h, DWORD *n) { return GetNumberOfEventLogRecords(h, n); }
API BOOL ClearEventLogA(HANDLE h, const char *backup) { (void)backup; return log_ok(h); }
API BOOL ClearEventLogW(HANDLE h, const WCHAR *backup) { (void)backup; return log_ok(h); }
API BOOL BackupEventLogA(HANDLE h, const char *file) { (void)file; return log_ok(h); }
API BOOL BackupEventLogW(HANDLE h, const WCHAR *file) { (void)file; return log_ok(h); }

/* ================================================================== */
/* Event Tracing for Windows                                            */
/* ================================================================== */

/* Providers register normally, but no trace session ever enables them, so
 * EventEnabled is always FALSE and writes are dropped. Starting or
 * controlling sessions needs administrator rights LSIW programs lack. */
#define ERROR_WMI_INSTANCE_NOT_FOUND 4201

static uint64_t g_etw_next = 1;

API ULONG EventRegister(const GUID *provider, void *cb, void *ctx, REGHANDLE *out)
{
    (void)cb; (void)ctx;
    if (!provider || !out) return ERROR_INVALID_PARAMETER;
    *out = __atomic_fetch_add(&g_etw_next, 1, __ATOMIC_RELAXED);
    return ERROR_SUCCESS;
}

API ULONG EventUnregister(REGHANDLE h) { (void)h; return ERROR_SUCCESS; }
API BOOLEAN EventEnabled(REGHANDLE h, const void *desc) { (void)h; (void)desc; return FALSE; }
API BOOLEAN EventProviderEnabled(REGHANDLE h, BYTE level, ULONGLONG kw) { (void)h; (void)level; (void)kw; return FALSE; }
API ULONG EventWrite(REGHANDLE h, const void *desc, ULONG n, void *data) { (void)h; (void)desc; (void)n; (void)data; return ERROR_SUCCESS; }

API ULONG EventWriteEx(REGHANDLE h, const void *desc, ULONGLONG filter, ULONG flags, const GUID *act, const GUID *rel,
                       ULONG n, void *data)
{
    (void)h; (void)desc; (void)filter; (void)flags; (void)act; (void)rel; (void)n; (void)data;
    return ERROR_SUCCESS;
}

API ULONG EventWriteTransfer(REGHANDLE h, const void *desc, const GUID *act, const GUID *rel, ULONG n, void *data)
{
    (void)h; (void)desc; (void)act; (void)rel; (void)n; (void)data;
    return ERROR_SUCCESS;
}

API ULONG EventWriteString(REGHANDLE h, BYTE level, ULONGLONG kw, const WCHAR *s) { (void)h; (void)level; (void)kw; (void)s; return ERROR_SUCCESS; }
API ULONG EventWriteNoRegistration(const GUID *g, const void *desc, ULONG n, void *data) { (void)g; (void)desc; (void)n; (void)data; return ERROR_SUCCESS; }
API ULONG EventSetInformation(REGHANDLE h, int cls, void *info, ULONG len) { (void)h; (void)cls; (void)info; (void)len; return ERROR_SUCCESS; }

/* The calling thread's activity ID. */
static __thread GUID t_activity;

enum { EVENT_ACTIVITY_CTRL_GET_ID = 1, EVENT_ACTIVITY_CTRL_SET_ID, EVENT_ACTIVITY_CTRL_CREATE_ID,
       EVENT_ACTIVITY_CTRL_GET_SET_ID, EVENT_ACTIVITY_CTRL_CREATE_SET_ID };

API ULONG EventActivityIdControl(ULONG code, GUID *id)
{
    if (!id) return ERROR_INVALID_PARAMETER;
    GUID tmp;
    switch (code) {
    case EVENT_ACTIVITY_CTRL_GET_ID: *id = t_activity; break;
    case EVENT_ACTIVITY_CTRL_SET_ID: t_activity = *id; break;
    case EVENT_ACTIVITY_CTRL_CREATE_ID:
        fill_random(id, sizeof *id);
        id->Data3 = (WORD)((id->Data3 & 0x0FFF) | 0x4000);
        id->Data4[0] = (BYTE)((id->Data4[0] & 0x3F) | 0x80);
        break;
    case EVENT_ACTIVITY_CTRL_GET_SET_ID: tmp = t_activity; t_activity = *id; *id = tmp; break;
    case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
        tmp = t_activity;
        EventActivityIdControl(EVENT_ACTIVITY_CTRL_CREATE_ID, &t_activity);
        *id = tmp;
        break;
    default: return ERROR_INVALID_PARAMETER;
    }
    return ERROR_SUCCESS;
}

API ULONG TraceEvent(TRACEHANDLE h, void *hdr) { (void)h; (void)hdr; return ERROR_SUCCESS; }
API ULONG TraceEventInstance(TRACEHANDLE h, void *hdr, void *inst, void *parent) { (void)h; (void)hdr; (void)inst; (void)parent; return ERROR_SUCCESS; }
API ULONG TraceMessage(TRACEHANDLE h, ULONG flags, const GUID *g, USHORT n, ...) { (void)h; (void)flags; (void)g; (void)n; return ERROR_SUCCESS; }

static ULONG register_trace(TRACEHANDLE *out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = __atomic_fetch_add(&g_etw_next, 1, __ATOMIC_RELAXED);
    return ERROR_SUCCESS;
}

API ULONG RegisterTraceGuidsW(void *cb, void *ctx, const GUID *g, ULONG n, void *regs, const WCHAR *mof,
                              const WCHAR *res, TRACEHANDLE *out)
{
    (void)cb; (void)ctx; (void)g; (void)n; (void)regs; (void)mof; (void)res;
    return register_trace(out);
}

API ULONG RegisterTraceGuidsA(void *cb, void *ctx, const GUID *g, ULONG n, void *regs, const char *mof,
                              const char *res, TRACEHANDLE *out)
{
    (void)cb; (void)ctx; (void)g; (void)n; (void)regs; (void)mof; (void)res;
    return register_trace(out);
}

API ULONG UnregisterTraceGuids(TRACEHANDLE h) { (void)h; return ERROR_SUCCESS; }

/* Only valid inside an enable callback, which never runs. */
API TRACEHANDLE GetTraceLoggerHandle(void *buf)
{
    (void)buf;
    lsiw_set_last_error(ERROR_INVALID_HANDLE);
    return (TRACEHANDLE)INVALID_HANDLE_VALUE;
}

API ULONG GetTraceEnableFlags(TRACEHANDLE h) { (void)h; lsiw_set_last_error(ERROR_SUCCESS); return 0; }
API BYTE GetTraceEnableLevel(TRACEHANDLE h) { (void)h; lsiw_set_last_error(ERROR_SUCCESS); return 0; }

API ULONG StartTraceW(TRACEHANDLE *h, const WCHAR *name, void *props) { (void)name; (void)props; if (h) *h = 0; return ERROR_ACCESS_DENIED; }
API ULONG StartTraceA(TRACEHANDLE *h, const char *name, void *props) { (void)name; (void)props; if (h) *h = 0; return ERROR_ACCESS_DENIED; }
API ULONG ControlTraceW(TRACEHANDLE h, const WCHAR *name, void *props, ULONG code) { (void)h; (void)name; (void)props; (void)code; return ERROR_WMI_INSTANCE_NOT_FOUND; }
API ULONG ControlTraceA(TRACEHANDLE h, const char *name, void *props, ULONG code) { (void)h; (void)name; (void)props; (void)code; return ERROR_WMI_INSTANCE_NOT_FOUND; }
API ULONG StopTraceW(TRACEHANDLE h, const WCHAR *name, void *props) { (void)h; (void)name; (void)props; return ERROR_WMI_INSTANCE_NOT_FOUND; }

API ULONG EnableTraceEx2(TRACEHANDLE h, const GUID *g, ULONG ctl, BYTE level, ULONGLONG any, ULONGLONG all,
                         ULONG timeout, void *params)
{
    (void)h; (void)g; (void)ctl; (void)level; (void)any; (void)all; (void)timeout; (void)params;
    return ERROR_ACCESS_DENIED;
}

/* ================================================================== */
/* Service control manager                                              */
/* ================================================================== */

/* There are no Windows services. The manager can be opened (programs
 * often check for a service before installing or starting one); every
 * service is reported as not existing and installing one is refused. */
typedef struct { DWORD dwServiceType, dwCurrentState, dwControlsAccepted, dwWin32ExitCode,
                 dwServiceSpecificExitCode, dwCheckPoint, dwWaitHint; } SERVICE_STATUS;

static void *open_scm(int local, const char *db)
{
    if (!local) { lsiw_set_last_error(RPC_S_SERVER_UNAVAILABLE); return NULL; }
    if (db && strcasecmp(db, "ServicesActive")) { lsiw_set_last_error(ERROR_DATABASE_DOES_NOT_EXIST); return NULL; }
    void *p = xcalloc(1, 1);
    obj_add(p, OBJ_SCM);
    return p;
}

API void *OpenSCManagerA(const char *machine, const char *db, DWORD access) { (void)access; return open_scm(is_local_machine_a(machine), db); }

API void *OpenSCManagerW(const WCHAR *machine, const WCHAR *db, DWORD access)
{
    (void)access;
    char *d = w2a(db);
    void *r = open_scm(is_local_machine_w(machine), d);
    free(d);
    return r;
}

static int scm_ok(void *h) { if (obj_check(h, OBJ_SCM)) return 1; lsiw_set_last_error(ERROR_INVALID_HANDLE); return 0; }

API BOOL CloseServiceHandle(void *h)
{
    if (!obj_remove(h, OBJ_SCM)) return win32_result(ERROR_INVALID_HANDLE);
    free(h);
    return TRUE;
}

static void *open_service(void *scm, const void *name)
{
    if (!scm_ok(scm)) return NULL;
    lsiw_set_last_error(name ? ERROR_SERVICE_DOES_NOT_EXIST : ERROR_INVALID_NAME);
    return NULL;
}

API void *OpenServiceA(void *scm, const char *name, DWORD access) { (void)access; return open_service(scm, name); }
API void *OpenServiceW(void *scm, const WCHAR *name, DWORD access) { (void)access; return open_service(scm, name); }

static void *create_service(void *scm)
{
    if (!scm_ok(scm)) return NULL;
    lsiw_set_last_error(ERROR_ACCESS_DENIED);
    return NULL;
}

API void *CreateServiceA(void *scm, const char *name, const char *disp, DWORD access, DWORD type, DWORD start,
                         DWORD err, const char *path, const char *group, DWORD *tag, const char *deps,
                         const char *user, const char *pw)
{
    (void)name; (void)disp; (void)access; (void)type; (void)start; (void)err; (void)path; (void)group;
    (void)tag; (void)deps; (void)user; (void)pw;
    return create_service(scm);
}

API void *CreateServiceW(void *scm, const WCHAR *name, const WCHAR *disp, DWORD access, DWORD type, DWORD start,
                         DWORD err, const WCHAR *path, const WCHAR *group, DWORD *tag, const WCHAR *deps,
                         const WCHAR *user, const WCHAR *pw)
{
    (void)name; (void)disp; (void)access; (void)type; (void)start; (void)err; (void)path; (void)group;
    (void)tag; (void)deps; (void)user; (void)pw;
    return create_service(scm);
}

/* No service handle can exist, so calls that need one fail. */
static BOOL no_service(void) { return win32_result(ERROR_INVALID_HANDLE); }

API BOOL QueryServiceStatus(void *h, SERVICE_STATUS *st) { (void)h; (void)st; return no_service(); }
API BOOL QueryServiceStatusEx(void *h, int level, BYTE *buf, DWORD size, DWORD *needed) { (void)h; (void)level; (void)buf; (void)size; (void)needed; return no_service(); }
API BOOL ControlService(void *h, DWORD ctl, SERVICE_STATUS *st) { (void)h; (void)ctl; (void)st; return no_service(); }
API BOOL DeleteService(void *h) { (void)h; return no_service(); }
API BOOL StartServiceA(void *h, DWORD argc, const char **argv) { (void)h; (void)argc; (void)argv; return no_service(); }
API BOOL StartServiceW(void *h, DWORD argc, const WCHAR **argv) { (void)h; (void)argc; (void)argv; return no_service(); }
API BOOL QueryServiceConfigA(void *h, void *cfg, DWORD size, DWORD *needed) { (void)h; (void)cfg; (void)size; (void)needed; return no_service(); }
API BOOL QueryServiceConfigW(void *h, void *cfg, DWORD size, DWORD *needed) { (void)h; (void)cfg; (void)size; (void)needed; return no_service(); }
API BOOL QueryServiceConfig2A(void *h, DWORD lvl, BYTE *buf, DWORD size, DWORD *needed) { (void)h; (void)lvl; (void)buf; (void)size; (void)needed; return no_service(); }
API BOOL QueryServiceConfig2W(void *h, DWORD lvl, BYTE *buf, DWORD size, DWORD *needed) { (void)h; (void)lvl; (void)buf; (void)size; (void)needed; return no_service(); }

API BOOL ChangeServiceConfigA(void *h, DWORD type, DWORD start, DWORD err, const char *path, const char *group,
                              DWORD *tag, const char *deps, const char *user, const char *pw, const char *disp)
{
    (void)h; (void)type; (void)start; (void)err; (void)path; (void)group; (void)tag; (void)deps; (void)user; (void)pw; (void)disp;
    return no_service();
}

API BOOL ChangeServiceConfigW(void *h, DWORD type, DWORD start, DWORD err, const WCHAR *path, const WCHAR *group,
                              DWORD *tag, const WCHAR *deps, const WCHAR *user, const WCHAR *pw, const WCHAR *disp)
{
    (void)h; (void)type; (void)start; (void)err; (void)path; (void)group; (void)tag; (void)deps; (void)user; (void)pw; (void)disp;
    return no_service();
}

API BOOL ChangeServiceConfig2A(void *h, DWORD lvl, void *info) { (void)h; (void)lvl; (void)info; return no_service(); }
API BOOL ChangeServiceConfig2W(void *h, DWORD lvl, void *info) { (void)h; (void)lvl; (void)info; return no_service(); }

API BOOL GetServiceKeyNameA(void *scm, const char *disp, char *name, DWORD *cch)
{
    (void)disp; (void)name; (void)cch;
    return scm_ok(scm) ? win32_result(ERROR_SERVICE_DOES_NOT_EXIST) : FALSE;
}

API BOOL GetServiceKeyNameW(void *scm, const WCHAR *disp, WCHAR *name, DWORD *cch)
{
    (void)disp; (void)name; (void)cch;
    return scm_ok(scm) ? win32_result(ERROR_SERVICE_DOES_NOT_EXIST) : FALSE;
}

API BOOL GetServiceDisplayNameA(void *scm, const char *name, char *disp, DWORD *cch) { return GetServiceKeyNameA(scm, name, disp, cch); }
API BOOL GetServiceDisplayNameW(void *scm, const WCHAR *name, WCHAR *disp, DWORD *cch) { return GetServiceKeyNameW(scm, name, disp, cch); }

static BOOL enum_services(void *scm, DWORD *needed, DWORD *count, DWORD *resume)
{
    if (!scm_ok(scm)) return FALSE;
    if (!needed || !count) return win32_result(ERROR_INVALID_PARAMETER);
    *needed = 0;
    *count = 0;
    if (resume) *resume = 0;
    return TRUE;
}

API BOOL EnumServicesStatusA(void *scm, DWORD type, DWORD state, void *buf, DWORD size, DWORD *needed, DWORD *count, DWORD *resume)
{
    (void)type; (void)state; (void)buf; (void)size;
    return enum_services(scm, needed, count, resume);
}

API BOOL EnumServicesStatusW(void *scm, DWORD type, DWORD state, void *buf, DWORD size, DWORD *needed, DWORD *count, DWORD *resume)
{
    (void)type; (void)state; (void)buf; (void)size;
    return enum_services(scm, needed, count, resume);
}

API BOOL EnumServicesStatusExA(void *scm, int level, DWORD type, DWORD state, BYTE *buf, DWORD size, DWORD *needed,
                               DWORD *count, DWORD *resume, const char *group)
{
    (void)type; (void)state; (void)buf; (void)size; (void)group;
    if (level != 0) return win32_result(ERROR_INVALID_LEVEL);
    return enum_services(scm, needed, count, resume);
}

API BOOL EnumServicesStatusExW(void *scm, int level, DWORD type, DWORD state, BYTE *buf, DWORD size, DWORD *needed,
                               DWORD *count, DWORD *resume, const WCHAR *group)
{
    (void)type; (void)state; (void)buf; (void)size; (void)group;
    if (level != 0) return win32_result(ERROR_INVALID_LEVEL);
    return enum_services(scm, needed, count, resume);
}

API BOOL EnumDependentServicesA(void *h, DWORD state, void *buf, DWORD size, DWORD *needed, DWORD *count) { (void)h; (void)state; (void)buf; (void)size; (void)needed; (void)count; return no_service(); }
API BOOL EnumDependentServicesW(void *h, DWORD state, void *buf, DWORD size, DWORD *needed, DWORD *count) { (void)h; (void)state; (void)buf; (void)size; (void)needed; (void)count; return no_service(); }

/* A program started outside the service manager cannot become a service. */
API BOOL StartServiceCtrlDispatcherA(const void *table) { (void)table; return win32_result(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); }
API BOOL StartServiceCtrlDispatcherW(const void *table) { (void)table; return win32_result(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); }

static void *service_status_handle(const void *name, const void *handler)
{
    if (!name || !handler) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    void *p = xcalloc(1, 1);
    obj_add(p, OBJ_SVCSTATUS);
    return p;
}

API void *RegisterServiceCtrlHandlerA(const char *name, void *fn) { return service_status_handle(name, fn); }
API void *RegisterServiceCtrlHandlerW(const WCHAR *name, void *fn) { return service_status_handle(name, fn); }
API void *RegisterServiceCtrlHandlerExA(const char *name, void *fn, void *ctx) { (void)ctx; return service_status_handle(name, fn); }
API void *RegisterServiceCtrlHandlerExW(const WCHAR *name, void *fn, void *ctx) { (void)ctx; return service_status_handle(name, fn); }

API BOOL SetServiceStatus(void *h, SERVICE_STATUS *st)
{
    if (!obj_check(h, OBJ_SVCSTATUS)) return win32_result(ERROR_INVALID_HANDLE);
    if (!st) return win32_result(ERROR_INVALID_DATA);
    return TRUE;
}

API DWORD NotifyServiceStatusChangeW(void *h, DWORD mask, void *notify) { (void)h; (void)mask; (void)notify; return ERROR_NOT_SUPPORTED; }
API DWORD NotifyServiceStatusChangeA(void *h, DWORD mask, void *notify) { (void)h; (void)mask; (void)notify; return ERROR_NOT_SUPPORTED; }

/* ================================================================== */
/* Processes under other identities, logon                              */
/* ================================================================== */

/* There is one user: a token always names it, so these start the process
 * normally through kernel32. */
typedef BOOL (WINAPI *create_process_w_fn)(const WCHAR *, WCHAR *, void *, void *, BOOL, DWORD, void *,
                                           const WCHAR *, void *, void *);
typedef BOOL (WINAPI *create_process_a_fn)(const char *, char *, void *, void *, BOOL, DWORD, void *,
                                           const char *, void *, void *);

static void *k32_fn(const char *name)
{
    void *f = lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), name);
    if (!f) lsiw_set_last_error(ERROR_PROC_NOT_FOUND);
    return f;
}

API BOOL CreateProcessAsUserW(HANDLE token, const WCHAR *app, WCHAR *cmd, void *psa, void *tsa, BOOL inherit,
                              DWORD flags, void *env, const WCHAR *cwd, void *si, void *pi)
{
    (void)token;
    create_process_w_fn f = (create_process_w_fn)k32_fn("CreateProcessW");
    return f ? f(app, cmd, psa, tsa, inherit, flags, env, cwd, si, pi) : FALSE;
}

API BOOL CreateProcessAsUserA(HANDLE token, const char *app, char *cmd, void *psa, void *tsa, BOOL inherit,
                              DWORD flags, void *env, const char *cwd, void *si, void *pi)
{
    (void)token;
    create_process_a_fn f = (create_process_a_fn)k32_fn("CreateProcessA");
    return f ? f(app, cmd, psa, tsa, inherit, flags, env, cwd, si, pi) : FALSE;
}

API BOOL CreateProcessWithTokenW(HANDLE token, DWORD logon, const WCHAR *app, WCHAR *cmd, DWORD flags, void *env,
                                 const WCHAR *cwd, void *si, void *pi)
{
    (void)token; (void)logon;
    create_process_w_fn f = (create_process_w_fn)k32_fn("CreateProcessW");
    return f ? f(app, cmd, NULL, NULL, FALSE, flags, env, cwd, si, pi) : FALSE;
}

/* Only the current user can "log on" (passwords cannot be checked, so any
 * other account fails like a wrong password, consistent with LogonUser). */
API BOOL CreateProcessWithLogonW(const WCHAR *user, const WCHAR *domain, const WCHAR *pw, DWORD logon,
                                 const WCHAR *app, WCHAR *cmd, DWORD flags, void *env, const WCHAR *cwd,
                                 void *si, void *pi)
{
    (void)domain; (void)pw; (void)logon;
    char *u = w2a(user);
    int self = u && !strcasecmp(u, lsiw_user_name());
    free(u);
    if (!self) return win32_result(ERROR_LOGON_FAILURE);
    create_process_w_fn f = (create_process_w_fn)k32_fn("CreateProcessW");
    return f ? f(app, cmd, NULL, NULL, FALSE, flags, env, cwd, si, pi) : FALSE;
}

/* Credentials cannot be verified, so every logon fails. */
API BOOL LogonUserA(const char *user, const char *domain, const char *pw, DWORD type, DWORD prov, HANDLE *token)
{
    (void)user; (void)domain; (void)pw; (void)type; (void)prov;
    if (token) *token = NULL;
    return win32_result(ERROR_LOGON_FAILURE);
}

API BOOL LogonUserW(const WCHAR *user, const WCHAR *domain, const WCHAR *pw, DWORD type, DWORD prov, HANDLE *token)
{
    (void)user; (void)domain; (void)pw; (void)type; (void)prov;
    if (token) *token = NULL;
    return win32_result(ERROR_LOGON_FAILURE);
}

API BOOL LogonUserExA(const char *user, const char *domain, const char *pw, DWORD type, DWORD prov, HANDLE *token,
                      void **sid, void **profile, DWORD *plen, void *quota)
{
    (void)sid; (void)profile; (void)plen; (void)quota;
    return LogonUserA(user, domain, pw, type, prov, token);
}

API BOOL LogonUserExW(const WCHAR *user, const WCHAR *domain, const WCHAR *pw, DWORD type, DWORD prov, HANDLE *token,
                      void **sid, void **profile, DWORD *plen, void *quota)
{
    (void)sid; (void)profile; (void)plen; (void)quota;
    return LogonUserW(user, domain, pw, type, prov, token);
}

/* Shutting the machine down needs SeShutdownPrivilege enabled and a real
 * Windows session: refused. */
API BOOL InitiateSystemShutdownA(char *m, char *msg, DWORD t, BOOL f, BOOL r) { (void)m; (void)msg; (void)t; (void)f; (void)r; return win32_result(ERROR_PRIVILEGE_NOT_HELD); }
API BOOL InitiateSystemShutdownW(WCHAR *m, WCHAR *msg, DWORD t, BOOL f, BOOL r) { (void)m; (void)msg; (void)t; (void)f; (void)r; return win32_result(ERROR_PRIVILEGE_NOT_HELD); }
API BOOL InitiateSystemShutdownExA(char *m, char *msg, DWORD t, BOOL f, BOOL r, DWORD why) { (void)why; return InitiateSystemShutdownA(m, msg, t, f, r); }
API BOOL InitiateSystemShutdownExW(WCHAR *m, WCHAR *msg, DWORD t, BOOL f, BOOL r, DWORD why) { (void)why; return InitiateSystemShutdownW(m, msg, t, f, r); }
API DWORD InitiateShutdownA(char *m, char *msg, DWORD t, DWORD f, DWORD why) { (void)m; (void)msg; (void)t; (void)f; (void)why; return ERROR_PRIVILEGE_NOT_HELD; }
API DWORD InitiateShutdownW(WCHAR *m, WCHAR *msg, DWORD t, DWORD f, DWORD why) { (void)m; (void)msg; (void)t; (void)f; (void)why; return ERROR_PRIVILEGE_NOT_HELD; }
API BOOL AbortSystemShutdownA(char *m) { (void)m; return win32_result(ERROR_PRIVILEGE_NOT_HELD); }
API BOOL AbortSystemShutdownW(WCHAR *m) { (void)m; return win32_result(ERROR_PRIVILEGE_NOT_HELD); }

/* ================================================================== */
/* Miscellaneous                                                        */
/* ================================================================== */

#define IS_TEXT_UNICODE_ASCII16           0x0001
#define IS_TEXT_UNICODE_STATISTICS        0x0002
#define IS_TEXT_UNICODE_CONTROLS          0x0004
#define IS_TEXT_UNICODE_SIGNATURE         0x0008
#define IS_TEXT_UNICODE_REVERSE_ASCII16   0x0010
#define IS_TEXT_UNICODE_REVERSE_STATISTICS 0x0020
#define IS_TEXT_UNICODE_REVERSE_CONTROLS  0x0040
#define IS_TEXT_UNICODE_REVERSE_SIGNATURE 0x0080
#define IS_TEXT_UNICODE_ILLEGAL_CHARS     0x0100
#define IS_TEXT_UNICODE_ODD_LENGTH        0x0200
#define IS_TEXT_UNICODE_NULL_BYTES        0x1000
#define IS_TEXT_UNICODE_UNICODE_MASK      0x000F
#define IS_TEXT_UNICODE_NOT_UNICODE_MASK  0x0F00

/* Heuristic test for UTF-16LE text: byte order marks, characters that
 * are all below 0x100, the share of zero bytes in odd versus even
 * positions, common control characters, and invalid code units. */
API BOOL IsTextUnicode(const void *buf, int len, int *result)
{
    const BYTE *b = buf;
    int out = 0;
    if (!b || len < 2) { if (result) *result = 0; return FALSE; }
    size_t n = (size_t)len / 2;
    const WCHAR *w = buf;
    if (len & 1) out |= IS_TEXT_UNICODE_ODD_LENGTH;
    if (b[0] == 0xFF && b[1] == 0xFE) out |= IS_TEXT_UNICODE_SIGNATURE;
    if (b[0] == 0xFE && b[1] == 0xFF) out |= IS_TEXT_UNICODE_REVERSE_SIGNATURE;
    int ascii16 = 1, rascii16 = 1, controls = 0, rcontrols = 0;
    size_t zodd = 0, zeven = 0;
    for (size_t i = 0; i < n; i++) {
        WCHAR c = w[i], r = (WCHAR)((c >> 8) | (c << 8));
        if (c >= 0x100 || !c) ascii16 = 0;
        if (r >= 0x100 || !r) rascii16 = 0;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') controls = 1;
        if (r == '\r' || r == '\n' || r == '\t' || r == ' ') rcontrols = 1;
        if (c == 0xFFFF || c == 0xFFFE) out |= IS_TEXT_UNICODE_ILLEGAL_CHARS;
        if (!c) out |= IS_TEXT_UNICODE_NULL_BYTES;
        if (!b[2 * i]) zeven++;
        if (!b[2 * i + 1]) zodd++;
    }
    if (ascii16) out |= IS_TEXT_UNICODE_ASCII16;
    if (rascii16) out |= IS_TEXT_UNICODE_REVERSE_ASCII16;
    if (controls) out |= IS_TEXT_UNICODE_CONTROLS;
    if (rcontrols) out |= IS_TEXT_UNICODE_REVERSE_CONTROLS;
    if (zodd > zeven * 3 + 1) out |= IS_TEXT_UNICODE_STATISTICS;
    if (zeven > zodd * 3 + 1) out |= IS_TEXT_UNICODE_REVERSE_STATISTICS;
    BOOL uni = (out & IS_TEXT_UNICODE_UNICODE_MASK) && !(out & IS_TEXT_UNICODE_NOT_UNICODE_MASK);
    if (out & IS_TEXT_UNICODE_SIGNATURE) uni = TRUE;
    if (result) {
        int want = *result ? *result : ~0;
        *result = out & want;
    }
    return uni;
}

/* Extensions Windows treats as executable content. */
API BOOL SaferiIsExecutableFileType(const WCHAR *path, BOOLEAN shell)
{
    static const char *const ext[] = {
        "ade", "adp", "app", "asp", "bas", "bat", "cer", "chm", "cmd", "cnt", "com", "cpl", "crt", "csh",
        "der", "exe", "fxp", "gadget", "grp", "hlp", "hpj", "hta", "inf", "ins", "isp", "its", "js", "jse",
        "ksh", "lnk", "mad", "maf", "mag", "mam", "maq", "mar", "mas", "mat", "mau", "mav", "maw", "mda",
        "mdb", "mde", "mdt", "mdw", "mdz", "msc", "msh", "msh1", "msh1xml", "msh2", "msh2xml", "mshxml",
        "msi", "msp", "mst", "ocx", "ops", "pcd", "pif", "plg", "prf", "prg", "ps1", "ps1xml", "ps2",
        "ps2xml", "psc1", "psc2", "pst", "reg", "scf", "scr", "sct", "shb", "shs", "tmp", "url", "vb",
        "vbe", "vbp", "vbs", "vsmacros", "vsw", "ws", "wsc", "wsf", "wsh", "xnk",
    };
    (void)shell;
    if (!path) return FALSE;
    char *u = w2a(path);
    const char *dot = strrchr(u, '.'), *sep = strrchr(u, '\\');
    BOOL r = FALSE;
    if (dot && (!sep || dot > sep))
        for (size_t i = 0; i < sizeof ext / sizeof ext[0] && !r; i++) r = !strcasecmp(dot + 1, ext[i]);
    free(u);
    return r;
}

/* ================================================================== */
/* Credential manager                                                   */
/* ================================================================== */

/* Credentials are kept in <lsiw_data_dir()>/credentials, a text file with
 * mode 0600 readable only by the user. It is NOT encrypted: anyone who can
 * read the user's files can read the stored secrets. One credential per
 * line:
 *     <type> <flags> <persist> <last-write FILETIME hex> <target> <user> <comment> <alias> <blob>
 * where every string field is the UTF-8 text in hex ("-" when empty or
 * absent) and the blob is the raw bytes in hex. Writers lock the file with
 * flock() and replace it atomically. Attributes are not stored. */
#define CRED_TYPE_GENERIC 1
#define CRED_TYPE_MAXIMUM 7
#define CRED_MAX_CREDENTIAL_BLOB_SIZE (5 * 512)
#define CRED_PRESERVE_CREDENTIAL_BLOB 0x1
#define CRED_ENUMERATE_ALL_CREDENTIALS 0x1

typedef struct {
    DWORD Flags, Type;
    void *TargetName, *Comment;
    FILETIME LastWritten;
    DWORD CredentialBlobSize;
    BYTE *CredentialBlob;
    DWORD Persist, AttributeCount;
    void *Attributes;
    void *TargetAlias, *UserName;
} CREDENTIAL;

struct cred { DWORD type, flags, persist; uint64_t written; char *target, *user, *comment, *alias; uint8_t *blob; size_t blen; };

static pthread_mutex_t g_cred_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *cred_path(void)
{
    static char *p;
    if (!p) p = lsiw_strdup_printf("%s/credentials", lsiw_data_dir());
    return p;
}

static void cred_free(struct cred *c)
{
    free(c->target); free(c->user); free(c->comment); free(c->alias);
    if (c->blob) { memset(c->blob, 0, c->blen); free(c->blob); }
}

static void put_hexfield(FILE *f, const void *d, size_t n)
{
    if (!d || !n) fputs("-", f);
    else put_hex_bytes(f, d, n, 0);
}

static uint8_t *get_hexfield(const char *s, size_t *n)
{
    *n = 0;
    if (!strcmp(s, "-")) return NULL;
    size_t l = strlen(s) / 2;
    uint8_t *d = xmalloc(l + 1);
    for (size_t i = 0; i < l; i++) {
        int h = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (h < 0 || lo < 0) { free(d); return NULL; }
        d[i] = (uint8_t)(h * 16 + lo);
    }
    d[l] = 0;
    *n = l;
    return d;
}

static size_t cred_load(struct cred **out)
{
    struct cred *v = NULL;
    size_t n = 0;
    FILE *f = fopen(cred_path(), "re");
    if (f) {
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, f) > 0) {
            char *fld[9], *save = NULL, *p = line;
            int k = 0;
            for (char *t; k < 9 && (t = strtok_r(p, " \t\r\n", &save)); p = NULL) fld[k++] = t;
            if (k != 9 || line[0] == ';') continue;
            struct cred c = { 0 };
            size_t l;
            c.type = (DWORD)strtoul(fld[0], NULL, 10);
            c.flags = (DWORD)strtoul(fld[1], NULL, 10);
            c.persist = (DWORD)strtoul(fld[2], NULL, 10);
            c.written = strtoull(fld[3], NULL, 16);
            c.target = (char *)get_hexfield(fld[4], &l);
            c.user = (char *)get_hexfield(fld[5], &l);
            c.comment = (char *)get_hexfield(fld[6], &l);
            c.alias = (char *)get_hexfield(fld[7], &l);
            c.blob = get_hexfield(fld[8], &c.blen);
            if (!c.target) { cred_free(&c); continue; }
            v = xrealloc(v, (n + 1) * sizeof *v);
            v[n++] = c;
        }
        free(line);
        fclose(f);
    }
    *out = v;
    return n;
}

static int cred_save(const struct cred *v, size_t n)
{
    lsiw_mkdirs(lsiw_data_dir(), 0700);
    char *tmp = lsiw_strdup_printf("%s.tmp.%d", cred_path(), (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) { if (fd >= 0) close(fd); free(tmp); return -1; }
    fputs("; LSIW credential store (plain text, not encrypted); see src/native/advapi32.c\n", f);
    for (size_t i = 0; i < n; i++) {
        const struct cred *c = &v[i];
        fprintf(f, "%u %u %u %016llx ", (unsigned)c->type, (unsigned)c->flags, (unsigned)c->persist,
                (unsigned long long)c->written);
        put_hexfield(f, c->target, c->target ? strlen(c->target) : 0); putc(' ', f);
        put_hexfield(f, c->user, c->user ? strlen(c->user) : 0); putc(' ', f);
        put_hexfield(f, c->comment, c->comment ? strlen(c->comment) : 0); putc(' ', f);
        put_hexfield(f, c->alias, c->alias ? strlen(c->alias) : 0); putc(' ', f);
        put_hexfield(f, c->blob, c->blen);
        putc('\n', f);
    }
    int bad = fflush(f) != 0 || ferror(f);
    if (fclose(f)) bad = 1;
    if (bad || rename(tmp, cred_path())) { unlink(tmp); free(tmp); return -1; }
    free(tmp);
    return 0;
}

static int cred_lock_file(void)
{
    lsiw_mkdirs(lsiw_data_dir(), 0700);
    char *lp = lsiw_strdup_printf("%s.lock", cred_path());
    int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(lp);
    if (fd >= 0) while (flock(fd, LOCK_EX) < 0 && errno == EINTR) {}
    return fd;
}

static void cred_unlock_file(int fd) { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }

/* Size of one CREDENTIAL with its strings and blob in the caller's encoding. */
static size_t cred_out_size(const struct cred *c, int ansi)
{
    size_t cs = ansi ? 1 : sizeof(WCHAR);
    size_t n = sizeof(CREDENTIAL);
    const char *s[4] = { c->target, c->user, c->comment, c->alias };
    for (int i = 0; i < 4; i++) if (s[i]) n += (enc_len(s[i], ansi) + 1) * cs;
    return ((n + 7) & ~(size_t)7) + c->blen;
}

/* Lay one credential out at p (cred_out_size bytes). */
static void cred_out(const struct cred *c, int ansi, BYTE *p)
{
    CREDENTIAL *o = (CREDENTIAL *)p;
    size_t cs = ansi ? 1 : sizeof(WCHAR), off = sizeof(CREDENTIAL);
    memset(o, 0, sizeof *o);
    o->Flags = c->flags;
    o->Type = c->type;
    o->Persist = c->persist;
    put_filetime(&o->LastWritten, c->written);
    const char *s[4] = { c->target, c->user, c->comment, c->alias };
    void **dst[4] = { &o->TargetName, &o->UserName, &o->Comment, &o->TargetAlias };
    for (int i = 0; i < 4; i++) {
        if (!s[i]) continue;
        enc_copy(s[i], ansi, p + off);
        *dst[i] = p + off;
        off += (enc_len(s[i], ansi) + 1) * cs;
    }
    off = (off + 7) & ~(size_t)7;
    o->CredentialBlobSize = (DWORD)c->blen;
    if (c->blen) { memcpy(p + off, c->blob, c->blen); o->CredentialBlob = p + off; }
}

static ssize_t cred_find(const struct cred *v, size_t n, const char *target, DWORD type)
{
    for (size_t i = 0; i < n; i++) if (v[i].type == type && !strcasecmp(v[i].target, target)) return (ssize_t)i;
    return -1;
}

static void creds_free(struct cred *v, size_t n)
{
    for (size_t i = 0; i < n; i++) cred_free(&v[i]);
    free(v);
}

static BOOL cred_read(const void *target, DWORD type, DWORD flags, CREDENTIAL **out, int ansi)
{
    (void)flags;
    if (!target || !out) return win32_result(ERROR_INVALID_PARAMETER);
    if (type < CRED_TYPE_GENERIC || type > CRED_TYPE_MAXIMUM) return win32_result(ERROR_INVALID_PARAMETER);
    char *t = enc_in(target, ansi);
    pthread_mutex_lock(&g_cred_lock);
    struct cred *v;
    size_t n = cred_load(&v);
    ssize_t i = cred_find(v, n, t, type);
    BOOL ok = FALSE;
    if (i < 0) lsiw_set_last_error(ERROR_NOT_FOUND);
    else {
        BYTE *p = lsiw_heap_alloc(cred_out_size(&v[i], ansi), 1);
        if (!p) lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        else { cred_out(&v[i], ansi, p); *out = (CREDENTIAL *)p; ok = TRUE; }
    }
    creds_free(v, n);
    pthread_mutex_unlock(&g_cred_lock);
    free(t);
    return ok;
}

API BOOL CredReadW(const WCHAR *target, DWORD type, DWORD flags, CREDENTIAL **out) { return cred_read(target, type, flags, out, 0); }
API BOOL CredReadA(const char *target, DWORD type, DWORD flags, CREDENTIAL **out) { return cred_read(target, type, flags, out, 1); }

static BOOL cred_write(const CREDENTIAL *in, DWORD flags, int ansi)
{
    if (!in || !in->TargetName) return win32_result(ERROR_INVALID_PARAMETER);
    if (in->Type < CRED_TYPE_GENERIC || in->Type > CRED_TYPE_MAXIMUM) return win32_result(ERROR_INVALID_PARAMETER);
    if (in->CredentialBlobSize > CRED_MAX_CREDENTIAL_BLOB_SIZE || (in->CredentialBlobSize && !in->CredentialBlob))
        return win32_result(ERROR_INVALID_PARAMETER);
    struct cred c = { in->Type, in->Flags, in->Persist, now_filetime(),
                      enc_in(in->TargetName, ansi), enc_in(in->UserName, ansi), enc_in(in->Comment, ansi),
                      enc_in(in->TargetAlias, ansi), NULL, in->CredentialBlobSize };
    if (c.blen) { c.blob = xmalloc(c.blen); memcpy(c.blob, in->CredentialBlob, c.blen); }
    pthread_mutex_lock(&g_cred_lock);
    int fd = cred_lock_file();
    struct cred *v;
    size_t n = cred_load(&v);
    ssize_t i = cred_find(v, n, c.target, c.type);
    if (i >= 0 && (flags & CRED_PRESERVE_CREDENTIAL_BLOB)) {
        free(c.blob);
        c.blob = v[i].blob; c.blen = v[i].blen;
        v[i].blob = NULL; v[i].blen = 0;
    }
    if (i >= 0) { cred_free(&v[i]); v[i] = c; }
    else { v = xrealloc(v, (n + 1) * sizeof *v); v[n++] = c; }
    int r = cred_save(v, n);
    creds_free(v, n);
    cred_unlock_file(fd);
    pthread_mutex_unlock(&g_cred_lock);
    return r ? win32_result(ERROR_ACCESS_DENIED) : TRUE;
}

API BOOL CredWriteW(const CREDENTIAL *c, DWORD flags) { return cred_write(c, flags, 0); }
API BOOL CredWriteA(const CREDENTIAL *c, DWORD flags) { return cred_write(c, flags, 1); }

static BOOL cred_delete(const void *target, DWORD type, int ansi)
{
    if (!target) return win32_result(ERROR_INVALID_PARAMETER);
    if (type < CRED_TYPE_GENERIC || type > CRED_TYPE_MAXIMUM) return win32_result(ERROR_INVALID_PARAMETER);
    char *t = enc_in(target, ansi);
    pthread_mutex_lock(&g_cred_lock);
    int fd = cred_lock_file();
    struct cred *v;
    size_t n = cred_load(&v);
    ssize_t i = cred_find(v, n, t, type);
    BOOL ok = TRUE;
    if (i < 0) ok = win32_result(ERROR_NOT_FOUND);
    else {
        cred_free(&v[i]);
        v[i] = v[--n];
        if (cred_save(v, n)) ok = win32_result(ERROR_ACCESS_DENIED);
    }
    creds_free(v, n);
    cred_unlock_file(fd);
    pthread_mutex_unlock(&g_cred_lock);
    free(t);
    return ok;
}

API BOOL CredDeleteW(const WCHAR *target, DWORD type, DWORD flags) { (void)flags; return cred_delete(target, type, 0); }
API BOOL CredDeleteA(const char *target, DWORD type, DWORD flags) { (void)flags; return cred_delete(target, type, 1); }

/* The filter is a target name, optionally ending in '*'. */
static BOOL cred_enum(const void *filter, DWORD flags, DWORD *count, CREDENTIAL ***out, int ansi)
{
    if (!count || !out) return win32_result(ERROR_INVALID_PARAMETER);
    if ((flags & CRED_ENUMERATE_ALL_CREDENTIALS) && filter) return win32_result(ERROR_INVALID_FLAGS);
    char *f = enc_in(filter, ansi);
    size_t fl = f ? strlen(f) : 0;
    int prefix = fl && f[fl - 1] == '*';
    if (prefix) fl--;
    pthread_mutex_lock(&g_cred_lock);
    struct cred *v;
    size_t n = cred_load(&v), m = 0, size = 0;
    unsigned char *sel = xcalloc(n + 1, 1);
    for (size_t i = 0; i < n; i++) {
        int match = !f || (prefix ? !strncasecmp(v[i].target, f, fl) : !strcasecmp(v[i].target, f));
        if (!match) continue;
        sel[i] = 1;
        m++;
        size += (cred_out_size(&v[i], ansi) + 7) & ~(size_t)7;
    }
    BOOL ok = TRUE;
    if (!m) ok = win32_result(ERROR_NOT_FOUND);
    else {
        BYTE *p = lsiw_heap_alloc(m * sizeof(void *) + size, 1);
        if (!p) ok = win32_result(ERROR_NOT_ENOUGH_MEMORY);
        else {
            CREDENTIAL **arr = (CREDENTIAL **)p;
            size_t off = m * sizeof(void *), k = 0;
            for (size_t i = 0; i < n; i++) {
                if (!sel[i]) continue;
                cred_out(&v[i], ansi, p + off);
                arr[k++] = (CREDENTIAL *)(p + off);
                off += (cred_out_size(&v[i], ansi) + 7) & ~(size_t)7;
            }
            *count = (DWORD)m;
            *out = arr;
        }
    }
    free(sel);
    creds_free(v, n);
    pthread_mutex_unlock(&g_cred_lock);
    free(f);
    return ok;
}

API BOOL CredEnumerateW(const WCHAR *filter, DWORD flags, DWORD *count, CREDENTIAL ***out) { return cred_enum(filter, flags, count, out, 0); }
API BOOL CredEnumerateA(const char *filter, DWORD flags, DWORD *count, CREDENTIAL ***out) { return cred_enum(filter, flags, count, out, 1); }

API void CredFree(void *p) { if (p) lsiw_heap_free(p); }

/* ================================================================== */
/* Local Security Authority                                             */
/* ================================================================== */

/* A stand-alone machine in workgroup "WORKGROUP": the policy can be
 * opened and its domain information queried; nothing can be changed. */
typedef struct { USHORT Length, MaximumLength; WCHAR *Buffer; } LSA_UNICODE_STRING;

enum { PolicyPrimaryDomainInformation = 3, PolicyAccountDomainInformation = 5, PolicyDnsDomainInformation = 12 };

API NTSTATUS LsaOpenPolicy(LSA_UNICODE_STRING *system, void *attrs, DWORD access, void **out)
{
    (void)attrs; (void)access;
    if (!out) return STATUS_INVALID_PARAMETER;
    if (system && system->Buffer && system->Length) {
        WCHAR *w = wdup_n(system->Buffer, system->Length / 2);
        int local = is_local_machine_w(w);
        free(w);
        if (!local) return (NTSTATUS)0xC0020017;     /* RPC_NT_SERVER_UNAVAILABLE */
    }
    void *p = xcalloc(1, 1);
    obj_add(p, OBJ_LSA);
    *out = p;
    return STATUS_SUCCESS;
}

API NTSTATUS LsaClose(void *h)
{
    if (!obj_remove(h, OBJ_LSA)) return STATUS_INVALID_HANDLE;
    free(h);
    return STATUS_SUCCESS;
}

API NTSTATUS LsaFreeMemory(void *p)
{
    if (p) lsiw_heap_free(p);
    return STATUS_SUCCESS;
}

/* Append a UTF-16 copy of s at *off in block p and point u at it. */
static void lsa_string(BYTE *p, size_t *off, LSA_UNICODE_STRING *u, const char *s)
{
    size_t n = enc_len(s, 0);
    WCHAR *w = (WCHAR *)(p + *off);
    enc_copy(s, 0, w);
    u->Length = (USHORT)(n * 2);
    u->MaximumLength = (USHORT)(n * 2 + 2);
    u->Buffer = w;
    *off += (n + 1) * 2;
}

API NTSTATUS LsaQueryInformationPolicy(void *h, int cls, void **out)
{
    if (!obj_check(h, OBJ_LSA)) return STATUS_INVALID_HANDLE;
    if (!out) return STATUS_INVALID_PARAMETER;
    const char *wg = "WORKGROUP", *cn = computer_name();
    size_t strs = (strlen(wg) + strlen(cn) + 4) * 2 * 4, off;
    BYTE *p;
    switch (cls) {
    case PolicyPrimaryDomainInformation: {      /* { Name; Sid } */
        p = lsiw_heap_alloc(24 + strs, 1);
        if (!p) return STATUS_NO_MEMORY;
        off = 24;
        lsa_string(p, &off, (LSA_UNICODE_STRING *)p, wg);
        break;
    }
    case PolicyAccountDomainInformation: {      /* { DomainName; DomainSid } */
        p = lsiw_heap_alloc(24 + strs + SECURITY_MAX_SID_SIZE, 1);
        if (!p) return STATUS_NO_MEMORY;
        off = 24;
        lsa_string(p, &off, (LSA_UNICODE_STRING *)p, cn);
        off = (off + 3) & ~(size_t)3;
        DWORD subs[4] = { 21, 0, 0, 0 };
        machine_domain(subs + 1);
        sid_build(p + off, 5, 4, subs);
        void *sid = p + off;
        memcpy(p + 16, &sid, 8);
        break;
    }
    case PolicyDnsDomainInformation: {          /* { Name; DnsDomainName; DnsForestName; DomainGuid; Sid } */
        p = lsiw_heap_alloc(72 + strs, 1);
        if (!p) return STATUS_NO_MEMORY;
        off = 72;
        lsa_string(p, &off, (LSA_UNICODE_STRING *)p, wg);
        break;
    }
    default:
        return STATUS_ACCESS_DENIED;
    }
    *out = p;
    return STATUS_SUCCESS;
}

API NTSTATUS LsaSetInformationPolicy(void *h, int cls, void *buf)
{
    (void)cls; (void)buf;
    return obj_check(h, OBJ_LSA) ? STATUS_ACCESS_DENIED : STATUS_INVALID_HANDLE;
}

API ULONG LsaNtStatusToWinError(NTSTATUS s)
{
    static const struct { uint32_t st; ULONG err; } map[] = {
        { 0x00000000, ERROR_SUCCESS },          { 0xC0000022, ERROR_ACCESS_DENIED },
        { 0xC0000008, ERROR_INVALID_HANDLE },   { 0xC0000017, ERROR_NOT_ENOUGH_MEMORY },
        { 0xC000009A, 1450 },                   { 0xC000000D, ERROR_INVALID_PARAMETER },
        { 0xC0000023, ERROR_INSUFFICIENT_BUFFER }, { 0xC0000034, ERROR_FILE_NOT_FOUND },
        { 0xC0000060, ERROR_NO_SUCH_PRIVILEGE }, { 0xC0000061, ERROR_PRIVILEGE_NOT_HELD },
        { 0xC0000064, 1317 },                   { 0xC000006A, 1323 },
        { 0xC000006D, ERROR_LOGON_FAILURE },    { 0xC0000073, ERROR_NONE_MAPPED },
        { 0xC0000078, ERROR_INVALID_SID },      { 0x00000107, ERROR_SOME_NOT_MAPPED },
        { 0xC0000002, ERROR_INVALID_FUNCTION }, { 0xC00000BB, ERROR_NOT_SUPPORTED },
        { 0xC0000035, ERROR_ALREADY_EXISTS },   { 0xC0000003, ERROR_INVALID_PARAMETER },
        { 0x8000001A, ERROR_NO_MORE_ITEMS },    { 0xC0020017, RPC_S_SERVER_UNAVAILABLE },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i].st == (uint32_t)s) return map[i].err;
    typedef ULONG (WINAPI *conv_fn)(NTSTATUS);
    conv_fn f = (conv_fn)lsiw_builtin_lookup(lsiw_builtin_find("ntdll.dll"), "RtlNtStatusToDosError");
    return f ? f(s) : ERROR_MR_MID_NOT_FOUND;
}

/* ================================================================== */
/* Export table                                                         */
/* ================================================================== */

const struct lsiw_export lsiw_advapi32_exports[] = {
    E(RegOpenKeyExW), E(RegOpenKeyExA), E(RegOpenKeyW), E(RegOpenKeyA), E(RegOpenKeyTransactedW),
    E(RegOpenKeyTransactedA), E(RegCreateKeyExW), E(RegCreateKeyExA), E(RegCreateKeyW), E(RegCreateKeyA),
    E(RegCreateKeyTransactedW), E(RegCreateKeyTransactedA), E(RegCloseKey), E(RegDeleteKeyW),
    E(RegDeleteKeyA), E(RegDeleteKeyExW), E(RegDeleteKeyExA), E(RegDeleteKeyTransactedW),
    E(RegDeleteKeyTransactedA), E(RegDeleteTreeW), E(RegDeleteTreeA), E(RegQueryValueExW),
    E(RegQueryValueExA), E(RegSetValueExW), E(RegSetValueExA), E(RegSetKeyValueW), E(RegSetKeyValueA),
    E(RegSetValueW), E(RegSetValueA), E(RegQueryValueW), E(RegQueryValueA), E(RegDeleteValueW),
    E(RegDeleteValueA), E(RegDeleteKeyValueW), E(RegDeleteKeyValueA), E(RegGetValueW), E(RegGetValueA),
    E(RegEnumKeyExW), E(RegEnumKeyExA), E(RegEnumKeyW), E(RegEnumKeyA), E(RegEnumValueW), E(RegEnumValueA),
    E(RegQueryInfoKeyW), E(RegQueryInfoKeyA), E(RegQueryMultipleValuesW), E(RegQueryMultipleValuesA),
    E(RegFlushKey), E(RegNotifyChangeKeyValue), E(RegOpenCurrentUser), E(RegOpenUserClassesRoot),
    E(RegOverridePredefKey), E(RegDisablePredefinedCache), E(RegDisablePredefinedCacheEx),
    E(RegConnectRegistryW), E(RegConnectRegistryA), E(RegConnectRegistryExW), E(RegConnectRegistryExA),
    E(RegCopyTreeW), E(RegCopyTreeA), E(RegLoadKeyW), E(RegLoadKeyA), E(RegUnLoadKeyW), E(RegUnLoadKeyA),
    E(RegSaveKeyW), E(RegSaveKeyA), E(RegSaveKeyExW), E(RegSaveKeyExA), E(RegRestoreKeyW), E(RegRestoreKeyA),
    E(RegReplaceKeyW), E(RegReplaceKeyA), E(RegGetKeySecurity), E(RegSetKeySecurity), E(RegLoadMUIStringW),
    E(RegLoadMUIStringA), E(LookupAccountSidW), E(LookupAccountSidA), E(LookupAccountSidLocalW),
    E(LookupAccountSidLocalA), E(LookupAccountNameW), E(LookupAccountNameA), E(LookupAccountNameLocalW),
    E(LookupAccountNameLocalA), E(AllocateAndInitializeSid), E(FreeSid), E(InitializeSid), E(IsValidSid),
    E(GetLengthSid), E(GetSidLengthRequired), E(EqualSid), E(EqualPrefixSid), E(CopySid),
    E(GetSidSubAuthority), E(GetSidSubAuthorityCount), E(GetSidIdentifierAuthority),
    E(ConvertSidToStringSidW), E(ConvertSidToStringSidA), E(ConvertStringSidToSidW),
    E(ConvertStringSidToSidA), E(IsWellKnownSid), E(CreateWellKnownSid), E(GetUserNameW), E(GetUserNameA),
    E(GetCurrentHwProfileA), E(GetCurrentHwProfileW), E(OpenProcessToken), E(OpenThreadToken),
    E(DuplicateToken), E(DuplicateTokenEx), E(ImpersonateSelf), E(RevertToSelf), E(ImpersonateLoggedOnUser),
    E(ImpersonateNamedPipeClient), E(SetThreadToken), E(AdjustTokenPrivileges), E(PrivilegeCheck),
    E(LookupPrivilegeValueW), E(LookupPrivilegeValueA), E(LookupPrivilegeNameW), E(LookupPrivilegeNameA),
    E(CheckTokenMembership), E(GetTokenInformation), E(SetTokenInformation), E(InitializeAcl), E(IsValidAcl),
    E(AddAccessAllowedAce), E(AddAccessAllowedAceEx), E(AddAccessDeniedAce), E(AddAccessDeniedAceEx),
    E(AddAuditAccessAce), E(AddMandatoryAce), E(AddAce), E(DeleteAce), E(GetAce), E(FindFirstFreeAce),
    E(GetAclInformation), E(SetAclInformation), E(InitializeSecurityDescriptor), E(IsValidSecurityDescriptor),
    E(GetSecurityDescriptorLength), E(SetSecurityDescriptorDacl), E(SetSecurityDescriptorSacl),
    E(SetSecurityDescriptorOwner), E(SetSecurityDescriptorGroup), E(SetSecurityDescriptorControl),
    E(GetSecurityDescriptorControl), E(GetSecurityDescriptorDacl), E(GetSecurityDescriptorSacl),
    E(GetSecurityDescriptorOwner), E(GetSecurityDescriptorGroup), E(MakeSelfRelativeSD), E(MakeAbsoluteSD),
    E(MapGenericMask), E(AccessCheck), E(AreAllAccessesGranted), E(AreAnyAccessesGranted), E(GetSecurityInfo),
    E(SetSecurityInfo), E(GetNamedSecurityInfoW), E(GetNamedSecurityInfoA), E(SetNamedSecurityInfoW),
    E(SetNamedSecurityInfoA), E(GetFileSecurityW), E(GetFileSecurityA), E(SetFileSecurityW),
    E(SetFileSecurityA), E(BuildTrusteeWithSidW), E(BuildTrusteeWithSidA), E(BuildTrusteeWithNameW),
    E(BuildTrusteeWithNameA), E(BuildExplicitAccessWithNameW), E(BuildExplicitAccessWithNameA),
    E(SetEntriesInAclW), E(SetEntriesInAclA), E(ConvertStringSecurityDescriptorToSecurityDescriptorW),
    E(ConvertStringSecurityDescriptorToSecurityDescriptorA),
    E(ConvertSecurityDescriptorToStringSecurityDescriptorW),
    E(ConvertSecurityDescriptorToStringSecurityDescriptorA), E(CryptAcquireContextA), E(CryptAcquireContextW),
    E(CryptReleaseContext), E(CryptContextAddRef), E(CryptGenRandom), E(SystemFunction036),
    E(CryptGetProvParam), E(CryptSetProvParam), E(CryptImportKey), E(CryptDestroyKey), E(CryptGenKey),
    E(CryptDeriveKey), E(CryptExportKey), E(CryptEncrypt), E(CryptDecrypt), E(CryptCreateHash),
    E(CryptHashData), E(CryptHashSessionKey), E(CryptGetHashParam), E(CryptSetHashParam), E(CryptDestroyHash),
    E(CryptDuplicateHash), E(CryptDuplicateKey), E(SystemFunction040), E(SystemFunction041),
    E(RegisterEventSourceA), E(RegisterEventSourceW), E(DeregisterEventSource), E(ReportEventA),
    E(ReportEventW), E(OpenEventLogA), E(OpenEventLogW), E(CloseEventLog), E(ReadEventLogA), E(ReadEventLogW),
    E(GetNumberOfEventLogRecords), E(GetOldestEventLogRecord), E(ClearEventLogA), E(ClearEventLogW),
    E(BackupEventLogA), E(BackupEventLogW), E(EventRegister), E(EventUnregister), E(EventEnabled),
    E(EventProviderEnabled), E(EventWrite), E(EventWriteEx), E(EventWriteTransfer), E(EventWriteString),
    E(EventWriteNoRegistration), E(EventSetInformation), E(EventActivityIdControl), E(TraceEvent),
    E(TraceEventInstance), E(TraceMessage), E(RegisterTraceGuidsW), E(RegisterTraceGuidsA),
    E(UnregisterTraceGuids), E(GetTraceLoggerHandle), E(GetTraceEnableFlags), E(GetTraceEnableLevel),
    E(StartTraceW), E(StartTraceA), E(ControlTraceW), E(ControlTraceA), E(StopTraceW), E(EnableTraceEx2),
    E(OpenSCManagerA), E(OpenSCManagerW), E(CloseServiceHandle), E(OpenServiceA), E(OpenServiceW),
    E(CreateServiceA), E(CreateServiceW), E(QueryServiceStatus), E(QueryServiceStatusEx), E(ControlService),
    E(DeleteService), E(StartServiceA), E(StartServiceW), E(QueryServiceConfigA), E(QueryServiceConfigW),
    E(QueryServiceConfig2A), E(QueryServiceConfig2W), E(ChangeServiceConfigA), E(ChangeServiceConfigW),
    E(ChangeServiceConfig2A), E(ChangeServiceConfig2W), E(GetServiceKeyNameA), E(GetServiceKeyNameW),
    E(GetServiceDisplayNameA), E(GetServiceDisplayNameW), E(EnumServicesStatusA), E(EnumServicesStatusW),
    E(EnumServicesStatusExA), E(EnumServicesStatusExW), E(EnumDependentServicesA), E(EnumDependentServicesW),
    E(StartServiceCtrlDispatcherA), E(StartServiceCtrlDispatcherW), E(RegisterServiceCtrlHandlerA),
    E(RegisterServiceCtrlHandlerW), E(RegisterServiceCtrlHandlerExA), E(RegisterServiceCtrlHandlerExW),
    E(SetServiceStatus), E(NotifyServiceStatusChangeW), E(NotifyServiceStatusChangeA),
    E(CreateProcessAsUserW), E(CreateProcessAsUserA), E(CreateProcessWithTokenW), E(CreateProcessWithLogonW),
    E(LogonUserA), E(LogonUserW), E(LogonUserExA), E(LogonUserExW), E(InitiateSystemShutdownA),
    E(InitiateSystemShutdownW), E(InitiateSystemShutdownExA), E(InitiateSystemShutdownExW),
    E(InitiateShutdownA), E(InitiateShutdownW), E(AbortSystemShutdownA), E(AbortSystemShutdownW),
    E(IsTextUnicode), E(SaferiIsExecutableFileType), E(CredReadW), E(CredReadA), E(CredWriteW), E(CredWriteA),
    E(CredDeleteW), E(CredDeleteA), E(CredEnumerateW), E(CredEnumerateA), E(CredFree), E(LsaOpenPolicy),
    E(LsaClose), E(LsaFreeMemory), E(LsaQueryInformationPolicy), E(LsaSetInformationPolicy),
    E(LsaNtStatusToWinError),
    { NULL, NULL }
};
