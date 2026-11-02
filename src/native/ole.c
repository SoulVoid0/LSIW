/* ole32 / combase / oleaut32 / rpcrt4 / crypt32 / bcrypt: the COM runtime
 * basics (apartments, task memory, GUID strings, class objects, HGLOBAL
 * streams, error info, HSTRING), Automation types (BSTR, VARIANT,
 * PROPVARIANT, SAFEARRAY and their conversions), UUIDs, base64/hex
 * encoding, DPAPI, and the CNG hash / RNG providers.
 *
 * There is no out-of-process COM, no marshaling and no type library
 * support: those entry points fail with the error Windows gives when the
 * class or library is not available.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native.h"
#include "hash.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Types and constants (x64 layouts)                                   */
/* ------------------------------------------------------------------ */

typedef LONG     HRESULT;
typedef int16_t  SHORT;
typedef uint16_t USHORT;
typedef uint16_t VARTYPE;
typedef int16_t  VARIANT_BOOL;
typedef double   DATE;
typedef WCHAR    OLECHAR;
typedef WCHAR   *BSTR;
typedef DWORD    LCID;

typedef struct { DWORD Data1; WORD Data2; WORD Data3; BYTE Data4[8]; } GUID;
typedef GUID IID;
typedef GUID CLSID;

typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;

#define S_OK                      ((HRESULT)0)
#define S_FALSE                   ((HRESULT)1)
#define E_NOTIMPL                 ((HRESULT)0x80004001u)
#define E_NOINTERFACE             ((HRESULT)0x80004002u)
#define E_POINTER                 ((HRESULT)0x80004003u)
#define E_FAIL                    ((HRESULT)0x80004005u)
#define E_UNEXPECTED              ((HRESULT)0x8000FFFFu)
#define E_OUTOFMEMORY             ((HRESULT)0x8007000Eu)
#define E_INVALIDARG              ((HRESULT)0x80070057u)
#define CLASS_E_NOAGGREGATION     ((HRESULT)0x80040110u)
#define REGDB_E_CLASSNOTREG       ((HRESULT)0x80040154u)
#define CO_E_NOTINITIALIZED       ((HRESULT)0x800401F0u)
#define CO_E_CLASSSTRING          ((HRESULT)0x800401F3u)
#define CO_E_IIDSTRING            ((HRESULT)0x800401F4u)
#define MK_E_UNAVAILABLE          ((HRESULT)0x800401E3u)
#define CO_S_NOTALLINTERFACES     ((HRESULT)0x00080012)
#define RPC_E_CHANGED_MODE        ((HRESULT)0x80010106u)
#define RPC_S_CALLPENDING         ((HRESULT)0x80010115u)
#define DRAGDROP_E_NOTREGISTERED  ((HRESULT)0x80040100u)
#define DRAGDROP_E_INVALIDHWND    ((HRESULT)0x80040102u)
#define DISP_E_PARAMNOTFOUND      ((HRESULT)0x80020004u)
#define DISP_E_TYPEMISMATCH       ((HRESULT)0x80020005u)
#define DISP_E_BADVARTYPE         ((HRESULT)0x80020008u)
#define DISP_E_OVERFLOW           ((HRESULT)0x8002000Au)
#define DISP_E_BADINDEX           ((HRESULT)0x8002000Bu)
#define DISP_E_ARRAYISLOCKED      ((HRESULT)0x8002000Du)
#define TYPE_E_LIBNOTREGISTERED   ((HRESULT)0x8002801Du)
#define TYPE_E_CANTLOADLIBRARY    ((HRESULT)0x80029C4Au)
#define STG_E_INVALIDFUNCTION     ((HRESULT)0x80030001u)
#define STG_E_INVALIDPOINTER      ((HRESULT)0x80030009u)
#define STG_E_MEDIUMFULL          ((HRESULT)0x80030070u)
#define STG_E_INVALIDPARAMETER    ((HRESULT)0x80030057u)
#define FAILED(hr)    ((HRESULT)(hr) < 0)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)

static HRESULT hr_from_win32(DWORD e) { return e ? (HRESULT)(0x80070000u | (e & 0xFFFF)) : S_OK; }

/* COM interfaces: an object starts with a pointer to its method table. */
typedef struct IUnknown IUnknown;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(IUnknown *, const IID *, void **);
    ULONG   (WINAPI *AddRef)(IUnknown *);
    ULONG   (WINAPI *Release)(IUnknown *);
} IUnknownVtbl;
struct IUnknown { const IUnknownVtbl *lpVtbl; };

static void unk_addref(void *p)  { if (p) ((IUnknown *)p)->lpVtbl->AddRef(p); }
static void unk_release(void *p) { if (p) ((IUnknown *)p)->lpVtbl->Release(p); }

typedef struct {
    IUnknownVtbl unk;
    HRESULT (WINAPI *CreateInstance)(IUnknown *, IUnknown *, const IID *, void **);
    HRESULT (WINAPI *LockServer)(IUnknown *, BOOL);
} IClassFactoryVtbl;

static const GUID GUID_NULL_          = { 0, 0, 0, { 0 } };
static const IID IID_IUnknown_        = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID IID_IClassFactory_   = { 0x00000001, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID IID_IMalloc_         = { 0x00000002, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID IID_IStream_         = { 0x0000000C, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID IID_ISequentialStream_ = { 0x0c733a30, 0x2a1c, 0x11ce, { 0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d } };
static const IID IID_IDispatch_       = { 0x00020400, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID IID_IErrorInfo_      = { 0x1CF2B120, 0x547D, 0x101B, { 0x8E, 0x65, 0x08, 0x00, 0x2B, 0x2B, 0xD1, 0x19 } };
static const IID IID_ICreateErrorInfo_ = { 0x22F03340, 0x547D, 0x101B, { 0x8E, 0x65, 0x08, 0x00, 0x2B, 0x2B, 0xD1, 0x19 } };

static int guid_eq(const GUID *a, const GUID *b) { return !memcmp(a, b, sizeof *a); }

/* Functions of other builtin DLLs, looked up once. */
static void *k32_fn(const char *name)
{
    return lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), name);
}

/* Cryptographic randomness; getrandom() only fails on bad arguments. */
static void fill_random(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            if (fd < 0) abort();
            while (n) {
                ssize_t k = read(fd, p, n);
                if (k <= 0) { if (k < 0 && errno == EINTR) continue; abort(); }
                p += k; n -= (size_t)k;
            }
            close(fd);
            return;
        }
        p += r; n -= (size_t)r;
    }
}

/* Locale-independent number formatting and parsing: Automation's
 * conversions here behave as in the C / en-US locale ('.' decimal point). */
static locale_t g_c_locale;
static pthread_once_t g_c_locale_once = PTHREAD_ONCE_INIT;
static void c_locale_init(void) { g_c_locale = newlocale(LC_ALL_MASK, "C", (locale_t)0); }
static locale_t c_locale(void) { pthread_once(&g_c_locale_once, c_locale_init); return g_c_locale; }

__attribute__((format(printf, 3, 4)))
static int fmt_c(char *buf, size_t n, const char *fmt, ...)
{
    locale_t old = uselocale(c_locale());
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    uselocale(old);
    return r;
}

static WCHAR *wdup_ascii(const char *s, WCHAR *out)
{
    size_t i = 0;
    for (; s[i]; i++) out[i] = (unsigned char)s[i];
    out[i] = 0;
    return out;
}

/* ------------------------------------------------------------------ */
/* Apartments                                                          */
/* ------------------------------------------------------------------ */

#define COINIT_MULTITHREADED      0x0
#define COINIT_APARTMENTTHREADED  0x2

static __thread int      t_com_inits;    /* unbalanced CoInitialize(Ex) calls       */
static __thread int      t_com_sta;      /* 1 = single-threaded apartment            */
static __thread int      t_ole_inits;
static __thread int      t_main_sta;
static __thread unsigned t_apt_id;       /* nonzero id of this thread's STA          */
static __thread IUnknown *t_errinfo;
static __thread IUnknown *t_msgfilter;
static __thread DWORD    t_cogcp;
static int      g_mta_users;             /* MTA threads + CoIncrementMTAUsage        */
static int      g_main_sta_taken;
static unsigned g_next_apt = 1;

static int com_ready(void) { return t_com_inits > 0 || __atomic_load_n(&g_mta_users, __ATOMIC_SEQ_CST) > 0; }
static void revoke_apartment(unsigned apt);

API HRESULT CoInitializeEx(void *reserved, DWORD coinit)
{
    if (reserved) return E_INVALIDARG;
    int sta = (coinit & COINIT_APARTMENTTHREADED) != 0;
    if (t_com_inits) {
        /* A thread stays in the apartment model it first chose. */
        if (t_com_sta != sta) return RPC_E_CHANGED_MODE;
        t_com_inits++;
        return S_FALSE;
    }
    t_com_inits = 1;
    t_com_sta = sta;
    if (sta) {
        int expected = 0;
        t_apt_id = __atomic_fetch_add(&g_next_apt, 1, __ATOMIC_SEQ_CST) + 1;
        if (__atomic_compare_exchange_n(&g_main_sta_taken, &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            t_main_sta = 1;
    } else {
        __atomic_add_fetch(&g_mta_users, 1, __ATOMIC_SEQ_CST);
    }
    return S_OK;
}

API HRESULT CoInitialize(void *reserved) { return CoInitializeEx(reserved, COINIT_APARTMENTTHREADED); }

API void CoUninitialize(void)
{
    if (!t_com_inits || --t_com_inits) return;
    /* The last CoUninitialize of an apartment revokes the class objects
     * registered in it. */
    if (t_com_sta) {
        revoke_apartment(t_apt_id);
        if (t_main_sta) { t_main_sta = 0; __atomic_store_n(&g_main_sta_taken, 0, __ATOMIC_SEQ_CST); }
        t_apt_id = 0;
    } else if (__atomic_sub_fetch(&g_mta_users, 1, __ATOMIC_SEQ_CST) == 0) {
        revoke_apartment(0);
    }
}

API HRESULT OleInitialize(void *reserved)
{
    HRESULT hr = CoInitializeEx(reserved, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return hr;
    if (t_ole_inits++) return S_FALSE;
    return hr;
}

API void OleUninitialize(void)
{
    if (!t_ole_inits) return;
    t_ole_inits--;
    CoUninitialize();
}

#define APTTYPE_CURRENT (-1)
#define APTTYPE_STA      0
#define APTTYPE_MTA      1
#define APTTYPE_MAINSTA  3
#define APTTYPEQUALIFIER_NONE         0
#define APTTYPEQUALIFIER_IMPLICIT_MTA 1

API HRESULT CoGetApartmentType(int *type, int *qualifier)
{
    if (!type || !qualifier) return E_INVALIDARG;
    *qualifier = APTTYPEQUALIFIER_NONE;
    if (t_com_inits) {
        *type = t_com_sta ? (t_main_sta ? APTTYPE_MAINSTA : APTTYPE_STA) : APTTYPE_MTA;
        return S_OK;
    }
    if (__atomic_load_n(&g_mta_users, __ATOMIC_SEQ_CST) > 0) {
        /* Uninitialised threads belong to the implicit MTA once one exists. */
        *type = APTTYPE_MTA;
        *qualifier = APTTYPEQUALIFIER_IMPLICIT_MTA;
        return S_OK;
    }
    *type = APTTYPE_CURRENT;
    return CO_E_NOTINITIALIZED;
}

API HRESULT CoIncrementMTAUsage(void **cookie)
{
    if (!cookie) return E_INVALIDARG;
    __atomic_add_fetch(&g_mta_users, 1, __ATOMIC_SEQ_CST);
    *cookie = &g_mta_users;
    return S_OK;
}

API HRESULT CoDecrementMTAUsage(void *cookie)
{
    if (!cookie) return E_INVALIDARG;
    if (__atomic_sub_fetch(&g_mta_users, 1, __ATOMIC_SEQ_CST) == 0) revoke_apartment(0);
    return S_OK;
}

/* Despite its name, this identifies the calling thread. */
API DWORD CoGetCurrentProcess(void)
{
    static DWORD next = 1;
    if (!t_cogcp) t_cogcp = __atomic_fetch_add(&next, 1, __ATOMIC_SEQ_CST);
    return t_cogcp;
}

/* Security, call context and server lifetime: there are no remote callers,
 * so these succeed trivially or report that no call context exists. */
API HRESULT CoInitializeSecurity(void *sd, LONG nsvc, void *svc, void *r1, DWORD authn, DWORD imp,
                                 void *authlist, DWORD caps, void *r3)
{
    (void)sd; (void)nsvc; (void)svc; (void)r1; (void)authn; (void)imp; (void)authlist; (void)caps; (void)r3;
    return S_OK;
}
API HRESULT CoSetProxyBlanket(IUnknown *proxy, DWORD authn, DWORD authz, WCHAR *name, DWORD level,
                              DWORD imp, void *authinfo, DWORD caps)
{
    (void)proxy; (void)authn; (void)authz; (void)name; (void)level; (void)imp; (void)authinfo; (void)caps;
    return S_OK;
}
API HRESULT CoQueryProxyBlanket(IUnknown *proxy, DWORD *authn, DWORD *authz, WCHAR **name, DWORD *level,
                                DWORD *imp, void **authinfo, DWORD *caps)
{
    (void)proxy; (void)authn; (void)authz; (void)name; (void)level; (void)imp; (void)authinfo; (void)caps;
    return E_NOINTERFACE;   /* in-process objects are never proxies */
}
API HRESULT CoImpersonateClient(void) { return S_OK; }
API HRESULT CoRevertToSelf(void) { return S_OK; }
API HRESULT CoGetCallContext(const IID *riid, void **ppv)
{
    (void)riid;
    if (ppv) *ppv = NULL;
    return E_NOINTERFACE;
}
API void CoFreeUnusedLibraries(void) { }
API void CoFreeUnusedLibrariesEx(DWORD delay, DWORD reserved) { (void)delay; (void)reserved; }
static ULONG g_server_refs;
API ULONG CoAddRefServerProcess(void) { return __atomic_add_fetch(&g_server_refs, 1, __ATOMIC_SEQ_CST); }
API ULONG CoReleaseServerProcess(void)
{
    ULONG v = __atomic_load_n(&g_server_refs, __ATOMIC_SEQ_CST);
    while (v && !__atomic_compare_exchange_n(&g_server_refs, &v, v - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { }
    return v ? v - 1 : 0;
}
API HRESULT CoGetObjectContext(const IID *riid, void **ppv) { (void)riid; if (ppv) *ppv = NULL; return E_NOTIMPL; }
API HRESULT CoCreateFreeThreadedMarshaler(IUnknown *outer, IUnknown **marshal)
{
    (void)outer;
    if (marshal) *marshal = NULL;
    return E_NOTIMPL;
}
API HRESULT CoMarshalInterface(IUnknown *stm, const IID *riid, IUnknown *unk, DWORD dest, void *destctx, DWORD flags)
{
    (void)stm; (void)riid; (void)unk; (void)dest; (void)destctx; (void)flags;
    return E_NOTIMPL;
}
API HRESULT CoUnmarshalInterface(IUnknown *stm, const IID *riid, void **ppv)
{
    (void)stm; (void)riid;
    if (ppv) *ppv = NULL;
    return E_NOTIMPL;
}
API HRESULT CreateBindCtx(DWORD reserved, void **ppbc) { (void)reserved; if (ppbc) *ppbc = NULL; return E_NOTIMPL; }
API HRESULT OleRun(IUnknown *unk) { (void)unk; return S_OK; }

/* ---- Running Object Table: a per-process table (there is no RPCSS to
 * share it between processes, so other processes' objects are never seen). ---- */
#define MK_S_MONIKERALREADYREGISTERED ((HRESULT)0x000401E7)
#define MK_E_UNAVAILABLE              ((HRESULT)0x800401E3u)
#define ROTFLAGS_ALLOWANYCLIENT       0x2

static const IID IID_IRunningObjectTable_ = { 0x00000010, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID IID_IEnumMoniker_        = { 0x00000102, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

/* IMoniker::IsEqual is slot 13 (IUnknown 3, IPersist 1, IPersistStream 4, then
 * BindToObject .. Enum), IMoniker::Hash slot 14. */
typedef HRESULT (WINAPI *mk_is_equal_fn)(IUnknown *, IUnknown *);
static HRESULT mk_is_equal(IUnknown *a, IUnknown *b)
{
    return ((mk_is_equal_fn)((void *const *)a->lpVtbl)[13])(a, b);
}

struct rot_entry { DWORD cookie; IUnknown *obj, *mk; uint64_t changed; struct rot_entry *next; };
static struct rot_entry *g_rot;
static DWORD g_rot_cookie;
static pthread_mutex_t g_rot_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t rot_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ULL;
}

/* Find an entry by moniker; called with g_rot_lock held. */
static struct rot_entry *rot_find(IUnknown *mk)
{
    for (struct rot_entry *e = g_rot; e; e = e->next)
        if (e->mk == mk || mk_is_equal(mk, e->mk) == S_OK) return e;
    return NULL;
}

typedef struct { IUnknown unk; } rot_obj;
static HRESULT WINAPI rot_qi(IUnknown *This, const IID *riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (riid && (guid_eq(riid, &IID_IUnknown_) || guid_eq(riid, &IID_IRunningObjectTable_))) { *ppv = This; return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI rot_addref(IUnknown *This) { (void)This; return 2; }   /* a process-wide singleton */
static ULONG WINAPI rot_release(IUnknown *This) { (void)This; return 1; }
static HRESULT WINAPI rot_register(IUnknown *This, DWORD flags, IUnknown *obj, IUnknown *mk, DWORD *cookie)
{
    (void)This; (void)flags;
    if (!obj || !mk || !cookie) return E_INVALIDARG;
    pthread_mutex_lock(&g_rot_lock);
    HRESULT hr = rot_find(mk) ? MK_S_MONIKERALREADYREGISTERED : S_OK;
    struct rot_entry *e = calloc(1, sizeof *e);
    if (!e) { pthread_mutex_unlock(&g_rot_lock); return E_OUTOFMEMORY; }
    e->cookie = ++g_rot_cookie;
    e->obj = obj; e->mk = mk; e->changed = rot_now();
    unk_addref(obj); unk_addref(mk);
    e->next = g_rot; g_rot = e;
    *cookie = e->cookie;
    pthread_mutex_unlock(&g_rot_lock);
    return hr;
}
static HRESULT WINAPI rot_revoke(IUnknown *This, DWORD cookie)
{
    (void)This;
    pthread_mutex_lock(&g_rot_lock);
    for (struct rot_entry **pp = &g_rot; *pp; pp = &(*pp)->next) {
        if ((*pp)->cookie != cookie) continue;
        struct rot_entry *e = *pp;
        *pp = e->next;
        pthread_mutex_unlock(&g_rot_lock);
        unk_release(e->obj); unk_release(e->mk);
        free(e);
        return S_OK;
    }
    pthread_mutex_unlock(&g_rot_lock);
    return E_INVALIDARG;
}
static HRESULT WINAPI rot_is_running(IUnknown *This, IUnknown *mk)
{
    (void)This;
    if (!mk) return E_INVALIDARG;
    pthread_mutex_lock(&g_rot_lock);
    HRESULT hr = rot_find(mk) ? S_OK : S_FALSE;
    pthread_mutex_unlock(&g_rot_lock);
    return hr;
}
static HRESULT WINAPI rot_get_object(IUnknown *This, IUnknown *mk, IUnknown **obj)
{
    (void)This;
    if (!obj) return E_POINTER;
    *obj = NULL;
    if (!mk) return E_INVALIDARG;
    pthread_mutex_lock(&g_rot_lock);
    struct rot_entry *e = rot_find(mk);
    if (e) { *obj = e->obj; unk_addref(e->obj); }
    pthread_mutex_unlock(&g_rot_lock);
    return e ? S_OK : MK_E_UNAVAILABLE;
}
static HRESULT WINAPI rot_note_change(IUnknown *This, DWORD cookie, const uint64_t *ft)
{
    (void)This;
    if (!ft) return E_INVALIDARG;
    HRESULT hr = E_INVALIDARG;
    pthread_mutex_lock(&g_rot_lock);
    for (struct rot_entry *e = g_rot; e; e = e->next) if (e->cookie == cookie) { e->changed = *ft; hr = S_OK; break; }
    pthread_mutex_unlock(&g_rot_lock);
    return hr;
}
static HRESULT WINAPI rot_time_of_change(IUnknown *This, IUnknown *mk, uint64_t *ft)
{
    (void)This;
    if (!mk || !ft) return E_INVALIDARG;
    pthread_mutex_lock(&g_rot_lock);
    struct rot_entry *e = rot_find(mk);
    if (e) *ft = e->changed;
    pthread_mutex_unlock(&g_rot_lock);
    return e ? S_OK : MK_E_UNAVAILABLE;
}

/* IEnumMoniker over a snapshot of the registered monikers. */
typedef struct { IUnknown unk; LONG refs; ULONG pos, n; IUnknown **mks; } enum_mk;
static HRESULT WINAPI emk_qi(IUnknown *This, const IID *riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (riid && (guid_eq(riid, &IID_IUnknown_) || guid_eq(riid, &IID_IEnumMoniker_))) { *ppv = This; unk_addref(This); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI emk_addref(IUnknown *This) { return (ULONG)__atomic_add_fetch(&((enum_mk *)This)->refs, 1, __ATOMIC_SEQ_CST); }
static ULONG WINAPI emk_release(IUnknown *This)
{
    enum_mk *e = (enum_mk *)This;
    LONG r = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);
    if (!r) { for (ULONG i = 0; i < e->n; i++) unk_release(e->mks[i]); free(e->mks); free(e); }
    return (ULONG)r;
}
static HRESULT WINAPI emk_next(IUnknown *This, ULONG want, IUnknown **out, ULONG *got)
{
    enum_mk *e = (enum_mk *)This;
    if (!out || (want > 1 && !got)) return E_INVALIDARG;
    ULONG k = 0;
    while (k < want && e->pos < e->n) { out[k] = e->mks[e->pos++]; unk_addref(out[k]); k++; }
    if (got) *got = k;
    return k == want ? S_OK : S_FALSE;
}
static HRESULT WINAPI emk_skip(IUnknown *This, ULONG n)
{
    enum_mk *e = (enum_mk *)This;
    ULONG left = e->n - e->pos;
    e->pos += n < left ? n : left;
    return n <= left ? S_OK : S_FALSE;
}
static HRESULT WINAPI emk_reset(IUnknown *This) { ((enum_mk *)This)->pos = 0; return S_OK; }
static HRESULT enum_mk_new(IUnknown **mks, ULONG n, ULONG pos, IUnknown **out);
static HRESULT WINAPI emk_clone(IUnknown *This, IUnknown **out)
{
    enum_mk *e = (enum_mk *)This;
    IUnknown **copy = malloc((e->n ? e->n : 1) * sizeof *copy);
    if (!copy) return E_OUTOFMEMORY;
    for (ULONG i = 0; i < e->n; i++) { copy[i] = e->mks[i]; unk_addref(copy[i]); }
    return enum_mk_new(copy, e->n, e->pos, out);
}
static const struct {
    IUnknownVtbl unk;
    HRESULT (WINAPI *Next)(IUnknown *, ULONG, IUnknown **, ULONG *);
    HRESULT (WINAPI *Skip)(IUnknown *, ULONG);
    HRESULT (WINAPI *Reset)(IUnknown *);
    HRESULT (WINAPI *Clone)(IUnknown *, IUnknown **);
} g_emk_vtbl = { { emk_qi, emk_addref, emk_release }, emk_next, emk_skip, emk_reset, emk_clone };

static HRESULT enum_mk_new(IUnknown **mks, ULONG n, ULONG pos, IUnknown **out)
{
    enum_mk *e = calloc(1, sizeof *e);
    if (!e) { for (ULONG i = 0; i < n; i++) unk_release(mks[i]); free(mks); return E_OUTOFMEMORY; }
    e->unk.lpVtbl = (const IUnknownVtbl *)&g_emk_vtbl;
    e->refs = 1; e->mks = mks; e->n = n; e->pos = pos;
    *out = &e->unk;
    return S_OK;
}
static HRESULT WINAPI rot_enum(IUnknown *This, IUnknown **out)
{
    (void)This;
    if (!out) return E_POINTER;
    pthread_mutex_lock(&g_rot_lock);
    ULONG n = 0;
    for (struct rot_entry *e = g_rot; e; e = e->next) n++;
    IUnknown **mks = malloc((n ? n : 1) * sizeof *mks);
    if (!mks) { pthread_mutex_unlock(&g_rot_lock); return E_OUTOFMEMORY; }
    ULONG i = 0;
    for (struct rot_entry *e = g_rot; e; e = e->next) { mks[i] = e->mk; unk_addref(mks[i]); i++; }
    pthread_mutex_unlock(&g_rot_lock);
    return enum_mk_new(mks, n, 0, out);
}
static const struct {
    IUnknownVtbl unk;
    HRESULT (WINAPI *Register)(IUnknown *, DWORD, IUnknown *, IUnknown *, DWORD *);
    HRESULT (WINAPI *Revoke)(IUnknown *, DWORD);
    HRESULT (WINAPI *IsRunning)(IUnknown *, IUnknown *);
    HRESULT (WINAPI *GetObject)(IUnknown *, IUnknown *, IUnknown **);
    HRESULT (WINAPI *NoteChangeTime)(IUnknown *, DWORD, const uint64_t *);
    HRESULT (WINAPI *GetTimeOfLastChange)(IUnknown *, IUnknown *, uint64_t *);
    HRESULT (WINAPI *EnumRunning)(IUnknown *, IUnknown **);
} g_rot_vtbl = { { rot_qi, rot_addref, rot_release }, rot_register, rot_revoke, rot_is_running, rot_get_object,
                 rot_note_change, rot_time_of_change, rot_enum };
static rot_obj g_rot_obj = { { (const IUnknownVtbl *)&g_rot_vtbl } };

API HRESULT GetRunningObjectTable(DWORD reserved, IUnknown **pprot)
{
    if (!pprot) return E_INVALIDARG;
    if (reserved) { *pprot = NULL; return E_INVALIDARG; }
    *pprot = &g_rot_obj.unk;
    return S_OK;
}

/* Drag and drop needs a window system this engine does not provide; as on
 * Windows, a thread without OleInitialize gets E_OUTOFMEMORY. */
API HRESULT RegisterDragDrop(void *hwnd, IUnknown *target)
{
    (void)hwnd; (void)target;
    return t_ole_inits ? DRAGDROP_E_INVALIDHWND : E_OUTOFMEMORY;
}
API HRESULT RevokeDragDrop(void *hwnd) { (void)hwnd; return DRAGDROP_E_NOTREGISTERED; }
API HRESULT OleGetClipboard(void **data) { if (data) *data = NULL; return E_NOTIMPL; }

/* The message filter is kept per thread; the previous one is handed back
 * with its reference, as COM does. */
API HRESULT CoRegisterMessageFilter(IUnknown *filter, IUnknown **previous)
{
    IUnknown *old = t_msgfilter;
    unk_addref(filter);
    t_msgfilter = filter;
    if (previous) *previous = old; else unk_release(old);
    return S_OK;
}

#define COWAIT_WAITALL   1
#define COWAIT_ALERTABLE 2
#define WAIT_IO_COMPLETION 0xC0

API HRESULT CoWaitForMultipleHandles(DWORD flags, DWORD timeout, ULONG count, HANDLE *handles, DWORD *index)
{
    static DWORD (WINAPI *wait)(DWORD, const HANDLE *, BOOL, DWORD, BOOL);
    if (!index) return E_INVALIDARG;
    *index = 0;
    if (!handles || !count) return E_INVALIDARG;
    if (!wait) wait = k32_fn("WaitForMultipleObjectsEx");
    if (!wait) return E_UNEXPECTED;
    DWORD r = wait(count, handles, (flags & COWAIT_WAITALL) != 0, timeout, (flags & COWAIT_ALERTABLE) != 0);
    if (r == WAIT_TIMEOUT) return RPC_S_CALLPENDING;
    if (r == WAIT_FAILED) return hr_from_win32(lsiw_get_last_error());
    *index = r;   /* the signalled / abandoned index, or WAIT_IO_COMPLETION */
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Task memory and IMalloc                                             */
/* ------------------------------------------------------------------ */

/* Memory freed with CoTaskMemFree may also be freed with LocalFree /
 * HeapFree by some programs, so it all comes from the process heap. */
API void *CoTaskMemAlloc(SIZE_T n) { return lsiw_heap_alloc(n, 0); }
API void CoTaskMemFree(void *p) { if (p) lsiw_heap_free(p); }
API void *CoTaskMemRealloc(void *p, SIZE_T n)
{
    if (!p) return lsiw_heap_alloc(n, 0);
    if (!n) { lsiw_heap_free(p); return NULL; }
    return lsiw_heap_realloc(p, n);
}


typedef struct { const void *lpVtbl; } MallocObj;
API HRESULT malloc_QueryInterface(IUnknown *This, const IID *riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (riid && (guid_eq(riid, &IID_IUnknown_) || guid_eq(riid, &IID_IMalloc_))) { *ppv = This; return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
API ULONG malloc_AddRef(IUnknown *This) { (void)This; return 1; }   /* process-lifetime singleton */
API ULONG malloc_Release(IUnknown *This) { (void)This; return 1; }
API void *malloc_Alloc(IUnknown *This, SIZE_T n) { (void)This; return CoTaskMemAlloc(n); }
API void *malloc_Realloc(IUnknown *This, void *p, SIZE_T n) { (void)This; return CoTaskMemRealloc(p, n); }
API void malloc_Free(IUnknown *This, void *p) { (void)This; CoTaskMemFree(p); }
API SIZE_T malloc_GetSize(IUnknown *This, void *p) { (void)This; return p ? lsiw_heap_size(p) : (SIZE_T)-1; }
API int malloc_DidAlloc(IUnknown *This, void *p) { (void)This; return p ? lsiw_heap_owns(p) : -1; }
API void malloc_HeapMinimize(IUnknown *This) { (void)This; }
static const void *const g_malloc_vtbl[] = {
    (void *)malloc_QueryInterface, (void *)malloc_AddRef, (void *)malloc_Release, (void *)malloc_Alloc,
    (void *)malloc_Realloc, (void *)malloc_Free, (void *)malloc_GetSize, (void *)malloc_DidAlloc,
    (void *)malloc_HeapMinimize };
static MallocObj g_malloc = { g_malloc_vtbl };

#define MEMCTX_TASK 1
API HRESULT CoGetMalloc(DWORD ctx, void **out)
{
    if (!out) return E_INVALIDARG;
    if (ctx != MEMCTX_TASK) { *out = NULL; return E_INVALIDARG; }
    *out = &g_malloc;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* GUIDs                                                               */
/* ------------------------------------------------------------------ */

static void random_guid(GUID *g)
{
    fill_random(g, sizeof *g);
    g->Data3 = (WORD)((g->Data3 & 0x0FFF) | 0x4000);     /* version 4      */
    g->Data4[0] = (BYTE)((g->Data4[0] & 0x3F) | 0x80);   /* RFC 4122 variant */
}

API HRESULT CoCreateGuid(GUID *g)
{
    if (!g) return E_INVALIDARG;
    random_guid(g);
    return S_OK;
}

/* "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" (36 chars) */
static void guid_fmt(const GUID *g, char *out, int upper)
{
    snprintf(out, 37, upper ? "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X"
                            : "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             (unsigned)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
             g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static int hexval(unsigned c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

/* Parse exactly 36 characters in the dashed form; get(i) yields char i. */
static int guid_parse36(const WCHAR *w, const char *a, GUID *g)
{
    uint8_t b[16];
    int n = 0;
    for (int i = 0; i < 36; i++) {
        unsigned c = w ? w[i] : (unsigned char)a[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return 0; continue; }
        int v = hexval(c);
        if (v < 0) return 0;
        if (n & 1) b[n / 2] = (uint8_t)(b[n / 2] << 4 | v); else b[n / 2] = (uint8_t)v;
        n++;
    }
    g->Data1 = (DWORD)b[0] << 24 | (DWORD)b[1] << 16 | (DWORD)b[2] << 8 | b[3];
    g->Data2 = (WORD)(b[4] << 8 | b[5]);
    g->Data3 = (WORD)(b[6] << 8 | b[7]);
    memcpy(g->Data4, b + 8, 8);
    return 1;
}

/* Strict "{...}" form, 38 characters followed by the terminator. */
static int guid_parse_braced(const WCHAR *s, GUID *g)
{
    if (s[0] != '{') return 0;
    for (int i = 1; i < 37; i++) if (!s[i]) return 0;
    if (s[37] != '}' || s[38]) return 0;
    return guid_parse36(s + 1, NULL, g);
}

API int StringFromGUID2(const GUID *g, WCHAR *out, int cch)
{
    char buf[40];
    if (!g || !out || cch < 39) return 0;
    buf[0] = '{';
    guid_fmt(g, buf + 1, 1);
    buf[37] = '}'; buf[38] = 0;
    wdup_ascii(buf, out);
    return 39;
}

API HRESULT StringFromCLSID(const CLSID *g, WCHAR **out)
{
    if (!out) return E_INVALIDARG;
    *out = CoTaskMemAlloc(39 * sizeof(WCHAR));
    if (!*out) return E_OUTOFMEMORY;
    StringFromGUID2(g ? g : &GUID_NULL_, *out, 39);
    return S_OK;
}
API HRESULT StringFromIID(const IID *g, WCHAR **out) { return StringFromCLSID(g, out); }

API HRESULT CLSIDFromProgID(const WCHAR *progid, CLSID *clsid);

API HRESULT CLSIDFromString(const WCHAR *s, CLSID *clsid)
{
    if (!clsid) return E_INVALIDARG;
    if (!s) { *clsid = GUID_NULL_; return S_OK; }
    if (s[0] == '{') {
        if (guid_parse_braced(s, clsid)) return S_OK;
        *clsid = GUID_NULL_;
        return CO_E_CLASSSTRING;
    }
    /* A string that is not a GUID is looked up as a ProgID. */
    if (SUCCEEDED(CLSIDFromProgID(s, clsid))) return S_OK;
    *clsid = GUID_NULL_;
    return CO_E_CLASSSTRING;
}

API HRESULT IIDFromString(const WCHAR *s, IID *iid)
{
    if (!iid) return E_INVALIDARG;
    if (!s) { *iid = GUID_NULL_; return S_OK; }
    /* A string of the wrong length is rejected as an invalid argument; a
     * well-sized but malformed one as an invalid interface string. */
    if (lsiw_wcslen(s) != 38) return E_INVALIDARG;
    return guid_parse_braced(s, iid) ? S_OK : CO_E_IIDSTRING;
}

/* ------------------------------------------------------------------ */
/* Registry lookups (HKEY_CLASSES_ROOT through advapi32)               */
/* ------------------------------------------------------------------ */

#define HKEY_CLASSES_ROOT_ ((HANDLE)(intptr_t)(int32_t)0x80000000)
#define RRF_RT_REG_SZ 0x00000002

/* Reads the default (unnamed) REG_SZ value of HKCR\<a><b><c>. */
static int hkcr_read(const WCHAR *a, const WCHAR *b, const WCHAR *c, WCHAR *out, DWORD outbytes)
{
    static LONG (WINAPI *get)(HANDLE, const WCHAR *, const WCHAR *, DWORD, DWORD *, void *, DWORD *);
    if (!get) get = lsiw_builtin_lookup(lsiw_builtin_find("advapi32.dll"), "RegGetValueW");
    if (!get) return -1;
    size_t la = lsiw_wcslen(a), lb = lsiw_wcslen(b), lc = lsiw_wcslen(c);
    WCHAR *key = malloc((la + lb + lc + 1) * sizeof(WCHAR));
    if (!key) return -1;
    memcpy(key, a, la * 2); memcpy(key + la, b, lb * 2); memcpy(key + la + lb, c, lc * 2);
    key[la + lb + lc] = 0;
    DWORD cb = outbytes;
    LONG r = get(HKEY_CLASSES_ROOT_, key, NULL, RRF_RT_REG_SZ, NULL, out, &cb);
    free(key);
    return r == ERROR_SUCCESS ? 0 : -1;
}

/* An unknown ProgID gives CO_E_CLASSSTRING ("Invalid class string"), the
 * value Windows returns. */
API HRESULT CLSIDFromProgID(const WCHAR *progid, CLSID *clsid)
{
    WCHAR buf[64];
    if (!progid || !clsid) return E_INVALIDARG;
    if (!progid[0] || hkcr_read(progid, u"\\CLSID", u"", buf, sizeof buf) || !guid_parse_braced(buf, clsid)) {
        *clsid = GUID_NULL_;
        return CO_E_CLASSSTRING;
    }
    return S_OK;
}
API HRESULT CLSIDFromProgIDEx(const WCHAR *progid, CLSID *clsid) { return CLSIDFromProgID(progid, clsid); }

API HRESULT ProgIDFromCLSID(const CLSID *clsid, WCHAR **out)
{
    WCHAR g[40], buf[260];
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!clsid) return E_INVALIDARG;
    StringFromGUID2(clsid, g, 40);
    if (hkcr_read(u"CLSID\\", g, u"\\ProgID", buf, sizeof buf)) return REGDB_E_CLASSNOTREG;
    size_t n = lsiw_wcslen(buf);
    if (!(*out = CoTaskMemAlloc((n + 1) * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    memcpy(*out, buf, (n + 1) * sizeof(WCHAR));
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Class objects                                                       */
/* ------------------------------------------------------------------ */

#define CLSCTX_INPROC_SERVER  0x1
#define CLSCTX_INPROC_HANDLER 0x2
#define CLSCTX_LOCAL_SERVER   0x4
#define CLSCTX_REMOTE_SERVER  0x10
#define CLSCTX_SERVERS        0x17
#define REGCLS_MULTIPLEUSE    1
#define REGCLS_SUSPENDED      4

struct classreg {
    struct classreg *next;
    CLSID     clsid;
    IUnknown *unk;
    DWORD     ctx, flags, cookie;
    unsigned  apt;                /* STA id, 0 for the MTA */
    int       suspended;
};
static pthread_mutex_t g_class_lock = PTHREAD_MUTEX_INITIALIZER;
static struct classreg *g_classes;
static DWORD g_next_cookie = 1;

API HRESULT CoRegisterClassObject(const CLSID *clsid, IUnknown *unk, DWORD ctx, DWORD flags, DWORD *cookie)
{
    if (!cookie) return E_INVALIDARG;
    *cookie = 0;
    if (!clsid || !unk) return E_INVALIDARG;
    if (!com_ready()) return CO_E_NOTINITIALIZED;
    struct classreg *r = calloc(1, sizeof *r);
    if (!r) return E_OUTOFMEMORY;
    r->clsid = *clsid;
    r->unk = unk;
    unk_addref(unk);
    /* A multiple-use local server class is also usable in-process. */
    if ((flags & REGCLS_MULTIPLEUSE) && (ctx & CLSCTX_LOCAL_SERVER)) ctx |= CLSCTX_INPROC_SERVER;
    r->ctx = ctx;
    r->flags = flags;
    r->apt = t_com_inits && t_com_sta ? t_apt_id : 0;
    r->suspended = (flags & REGCLS_SUSPENDED) != 0;
    pthread_mutex_lock(&g_class_lock);
    r->cookie = g_next_cookie++;
    r->next = g_classes;
    g_classes = r;
    pthread_mutex_unlock(&g_class_lock);
    *cookie = r->cookie;
    return S_OK;
}

API HRESULT CoRevokeClassObject(DWORD cookie)
{
    struct classreg *found = NULL;
    pthread_mutex_lock(&g_class_lock);
    for (struct classreg **pp = &g_classes; *pp; pp = &(*pp)->next)
        if ((*pp)->cookie == cookie) { found = *pp; *pp = found->next; break; }
    pthread_mutex_unlock(&g_class_lock);
    if (!found) return E_INVALIDARG;
    unk_release(found->unk);
    free(found);
    return S_OK;
}

static void revoke_apartment(unsigned apt)
{
    struct classreg *list = NULL;
    pthread_mutex_lock(&g_class_lock);
    for (struct classreg **pp = &g_classes; *pp;) {
        struct classreg *r = *pp;
        if (r->apt == apt) { *pp = r->next; r->next = list; list = r; }
        else pp = &r->next;
    }
    pthread_mutex_unlock(&g_class_lock);
    while (list) { struct classreg *n = list->next; unk_release(list->unk); free(list); list = n; }
}

static void set_suspended(int s)
{
    pthread_mutex_lock(&g_class_lock);
    for (struct classreg *r = g_classes; r; r = r->next) r->suspended = s;
    pthread_mutex_unlock(&g_class_lock);
}
API HRESULT CoResumeClassObjects(void) { set_suspended(0); return S_OK; }
API HRESULT CoSuspendClassObjects(void) { set_suspended(1); return S_OK; }

/* Only classes registered with CoRegisterClassObject can be created: there
 * is no loading of COM servers from InprocServer32 / LocalServer32. */
API HRESULT CoGetClassObject(const CLSID *clsid, DWORD ctx, void *server, const IID *riid, void **ppv)
{
    (void)server;
    if (!ppv) return E_INVALIDARG;
    *ppv = NULL;
    if (!clsid || !riid) return E_INVALIDARG;
    if (!com_ready()) return CO_E_NOTINITIALIZED;
    IUnknown *unk = NULL;
    pthread_mutex_lock(&g_class_lock);
    for (struct classreg *r = g_classes; r; r = r->next)
        if (!r->suspended && guid_eq(&r->clsid, clsid) && (r->ctx & ctx & CLSCTX_SERVERS)) {
            unk = r->unk;
            unk_addref(unk);
            break;
        }
    pthread_mutex_unlock(&g_class_lock);
    if (!unk) return REGDB_E_CLASSNOTREG;
    HRESULT hr = unk->lpVtbl->QueryInterface(unk, riid, ppv);
    unk_release(unk);
    return hr;
}

API HRESULT CoCreateInstance(const CLSID *clsid, IUnknown *outer, DWORD ctx, const IID *riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!clsid || !riid) return E_INVALIDARG;
    IUnknown *cf;
    HRESULT hr = CoGetClassObject(clsid, ctx, NULL, &IID_IClassFactory_, (void **)&cf);
    if (FAILED(hr)) return hr;
    hr = ((const IClassFactoryVtbl *)cf->lpVtbl)->CreateInstance(cf, outer, riid, ppv);
    unk_release(cf);
    return hr;
}

typedef struct { const IID *pIID; IUnknown *pItf; HRESULT hr; } MULTI_QI;

API HRESULT CoCreateInstanceEx(const CLSID *clsid, IUnknown *outer, DWORD ctx, void *server, ULONG count, MULTI_QI *res)
{
    (void)server;
    if (!count || !res) return E_INVALIDARG;
    for (ULONG i = 0; i < count; i++) { res[i].pItf = NULL; res[i].hr = E_NOINTERFACE; }
    IUnknown *unk;
    HRESULT hr = CoCreateInstance(clsid, outer, ctx, &IID_IUnknown_, (void **)&unk);
    if (FAILED(hr)) {
        for (ULONG i = 0; i < count; i++) res[i].hr = hr;
        return hr;
    }
    ULONG ok = 0;
    for (ULONG i = 0; i < count; i++) {
        res[i].hr = unk->lpVtbl->QueryInterface(unk, res[i].pIID, (void **)&res[i].pItf);
        if (SUCCEEDED(res[i].hr)) ok++;
    }
    unk_release(unk);
    return ok == count ? S_OK : ok ? CO_S_NOTALLINTERFACES : E_NOINTERFACE;
}

/* ------------------------------------------------------------------ */
/* IStream over an HGLOBAL                                             */
/* ------------------------------------------------------------------ */

#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define STREAM_SEEK_SET 0
#define STREAM_SEEK_CUR 1
#define STREAM_SEEK_END 2
#define STGTY_STREAM 2
#define STGM_READWRITE 2

typedef struct {
    WCHAR    *pwcsName;
    DWORD     type;
    ULONGLONG cbSize;
    FILETIME  mtime, ctime, atime;
    DWORD     grfMode, grfLocksSupported;
    CLSID     clsid;
    DWORD     grfStateBits, reserved;
} STATSTG;

static struct {
    HANDLE (WINAPI *alloc)(UINT, SIZE_T);
    HANDLE (WINAPI *realloc)(HANDLE, SIZE_T, UINT);
    void  *(WINAPI *lock)(HANDLE);
    BOOL   (WINAPI *unlock)(HANDLE);
    SIZE_T (WINAPI *size)(HANDLE);
    HANDLE (WINAPI *free)(HANDLE);
} g_gmem;

static int gmem_init(void)
{
    if (g_gmem.free) return 1;
    g_gmem.alloc   = k32_fn("GlobalAlloc");
    g_gmem.realloc = k32_fn("GlobalReAlloc");
    g_gmem.lock    = k32_fn("GlobalLock");
    g_gmem.unlock  = k32_fn("GlobalUnlock");
    g_gmem.size    = k32_fn("GlobalSize");
    __atomic_store_n(&g_gmem.free, (HANDLE (WINAPI *)(HANDLE))k32_fn("GlobalFree"), __ATOMIC_SEQ_CST);
    return g_gmem.alloc && g_gmem.realloc && g_gmem.lock && g_gmem.unlock && g_gmem.size && g_gmem.free;
}

/* The data block is shared by a stream and its clones. */
struct hg_data {
    LONG            refs;
    HANDLE          hg;
    ULONGLONG       size;           /* stream size, == GlobalSize(hg) */
    int             delete_on_release;
    pthread_mutex_t lock;
};
struct hg_stream {
    const void     *lpVtbl;
    LONG            refs;
    struct hg_data *d;
    ULONGLONG       pos;
};
static const void *const g_hgstream_vtbl[];

/* Caller holds d->lock. The block may move, so the handle is updated. */
static HRESULT hg_resize(struct hg_data *d, ULONGLONG n)
{
    if (n >> 32) return STG_E_MEDIUMFULL;
    if (n == d->size) return S_OK;
    HANDLE h = g_gmem.realloc(d->hg, (SIZE_T)n, GMEM_MOVEABLE | GMEM_ZEROINIT);
    if (!h) return STG_E_MEDIUMFULL;
    d->hg = h;
    d->size = n;
    return S_OK;
}

static struct hg_stream *hg_new(struct hg_data *d, ULONGLONG pos)
{
    struct hg_stream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->lpVtbl = g_hgstream_vtbl;
    s->refs = 1;
    s->d = d;
    s->pos = pos;
    return s;
}

API HRESULT hgs_QueryInterface(IUnknown *This, const IID *riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (riid && (guid_eq(riid, &IID_IUnknown_) || guid_eq(riid, &IID_IStream_) || guid_eq(riid, &IID_ISequentialStream_))) {
        *ppv = This;
        unk_addref(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
API ULONG hgs_AddRef(IUnknown *This) { return (ULONG)__atomic_add_fetch(&((struct hg_stream *)This)->refs, 1, __ATOMIC_SEQ_CST); }
API ULONG hgs_Release(IUnknown *This)
{
    struct hg_stream *s = (struct hg_stream *)This;
    LONG r = __atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);
    if (r) return (ULONG)r;
    struct hg_data *d = s->d;
    if (!__atomic_sub_fetch(&d->refs, 1, __ATOMIC_SEQ_CST)) {
        if (d->delete_on_release) g_gmem.free(d->hg);
        pthread_mutex_destroy(&d->lock);
        free(d);
    }
    free(s);
    return 0;
}
API HRESULT hgs_Read(IUnknown *This, void *pv, ULONG cb, ULONG *pcb)
{
    struct hg_stream *s = (struct hg_stream *)This;
    if (pcb) *pcb = 0;
    if (!pv) return STG_E_INVALIDPOINTER;
    pthread_mutex_lock(&s->d->lock);
    ULONG n = 0;
    if (s->pos < s->d->size) {
        ULONGLONG left = s->d->size - s->pos;
        n = left < cb ? (ULONG)left : cb;
        uint8_t *p = g_gmem.lock(s->d->hg);
        memcpy(pv, p + s->pos, n);
        g_gmem.unlock(s->d->hg);
        s->pos += n;
    }
    pthread_mutex_unlock(&s->d->lock);
    if (pcb) *pcb = n;
    return S_OK;   /* a short read is not an error */
}
API HRESULT hgs_Write(IUnknown *This, const void *pv, ULONG cb, ULONG *pcb)
{
    struct hg_stream *s = (struct hg_stream *)This;
    if (pcb) *pcb = 0;
    if (!pv) return STG_E_INVALIDPOINTER;
    if (!cb) return S_OK;
    pthread_mutex_lock(&s->d->lock);
    HRESULT hr = S_OK;
    if (s->pos + cb > s->d->size) hr = hg_resize(s->d, s->pos + cb);
    if (SUCCEEDED(hr)) {
        uint8_t *p = g_gmem.lock(s->d->hg);
        memcpy(p + s->pos, pv, cb);
        g_gmem.unlock(s->d->hg);
        s->pos += cb;
        if (pcb) *pcb = cb;
    }
    pthread_mutex_unlock(&s->d->lock);
    return hr;
}
API HRESULT hgs_Seek(IUnknown *This, LONGLONG move, DWORD origin, ULONGLONG *newpos)
{
    struct hg_stream *s = (struct hg_stream *)This;
    LONGLONG base;
    pthread_mutex_lock(&s->d->lock);
    switch (origin) {
    case STREAM_SEEK_SET: base = 0; break;
    case STREAM_SEEK_CUR: base = (LONGLONG)s->pos; break;
    case STREAM_SEEK_END: base = (LONGLONG)s->d->size; break;
    default: pthread_mutex_unlock(&s->d->lock); return STG_E_INVALIDFUNCTION;
    }
    pthread_mutex_unlock(&s->d->lock);
    /* Positions past the end are allowed; before the start they are not. */
    if (base + move < 0) return STG_E_INVALIDFUNCTION;
    s->pos = (ULONGLONG)(base + move);
    if (newpos) *newpos = s->pos;
    return S_OK;
}
API HRESULT hgs_SetSize(IUnknown *This, ULONGLONG n)
{
    struct hg_stream *s = (struct hg_stream *)This;
    if (n >> 32) return STG_E_INVALIDFUNCTION;
    pthread_mutex_lock(&s->d->lock);
    HRESULT hr = hg_resize(s->d, n);
    pthread_mutex_unlock(&s->d->lock);
    return hr;
}
API HRESULT hgs_CopyTo(IUnknown *This, IUnknown *dst, ULONGLONG cb, ULONGLONG *pread, ULONGLONG *pwritten)
{
    typedef HRESULT (WINAPI *write_fn)(IUnknown *, const void *, ULONG, ULONG *);
    ULONGLONG rd = 0, wr = 0;
    HRESULT hr = S_OK;
    if (pread) *pread = 0;
    if (pwritten) *pwritten = 0;
    if (!dst) return STG_E_INVALIDPOINTER;
    uint8_t buf[16384];
    while (cb) {
        ULONG want = cb < sizeof buf ? (ULONG)cb : (ULONG)sizeof buf, got = 0, put = 0;
        hgs_Read(This, buf, want, &got);
        if (!got) break;
        rd += got;
        hr = ((write_fn)((void *const *)dst->lpVtbl)[4])(dst, buf, got, &put);
        wr += put;
        if (FAILED(hr) || put != got) break;
        cb -= got;
    }
    if (pread) *pread = rd;
    if (pwritten) *pwritten = wr;
    return hr;
}
API HRESULT hgs_Commit(IUnknown *This, DWORD flags) { (void)This; (void)flags; return S_OK; }
API HRESULT hgs_Revert(IUnknown *This) { (void)This; return S_OK; }
/* Region locking is not supported by HGLOBAL streams on Windows either. */
API HRESULT hgs_LockRegion(IUnknown *This, ULONGLONG off, ULONGLONG cb, DWORD type)
{
    (void)This; (void)off; (void)cb; (void)type;
    return STG_E_INVALIDFUNCTION;
}
API HRESULT hgs_Stat(IUnknown *This, STATSTG *st, DWORD flags)
{
    struct hg_stream *s = (struct hg_stream *)This;
    (void)flags;   /* HGLOBAL streams have no name, so STATFLAG_NONAME changes nothing */
    if (!st) return STG_E_INVALIDPOINTER;
    memset(st, 0, sizeof *st);
    st->type = STGTY_STREAM;
    st->cbSize = s->d->size;
    st->grfMode = STGM_READWRITE;
    return S_OK;
}
API HRESULT hgs_Clone(IUnknown *This, IUnknown **out)
{
    struct hg_stream *s = (struct hg_stream *)This;
    if (!out) return STG_E_INVALIDPOINTER;
    struct hg_stream *c = hg_new(s->d, s->pos);
    if (!c) { *out = NULL; return E_OUTOFMEMORY; }
    __atomic_add_fetch(&s->d->refs, 1, __ATOMIC_SEQ_CST);
    *out = (IUnknown *)c;
    return S_OK;
}
static const void *const g_hgstream_vtbl[] = {
    (void *)hgs_QueryInterface, (void *)hgs_AddRef, (void *)hgs_Release, (void *)hgs_Read, (void *)hgs_Write,
    (void *)hgs_Seek, (void *)hgs_SetSize, (void *)hgs_CopyTo, (void *)hgs_Commit, (void *)hgs_Revert,
    (void *)hgs_LockRegion, (void *)hgs_LockRegion, (void *)hgs_Stat, (void *)hgs_Clone };

API HRESULT CreateStreamOnHGlobal(HANDLE hg, BOOL delete_on_release, IUnknown **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!gmem_init()) return E_UNEXPECTED;
    struct hg_data *d = calloc(1, sizeof *d);
    if (!d) return E_OUTOFMEMORY;
    if (!hg) {
        if (!(hg = g_gmem.alloc(GMEM_MOVEABLE, 0))) { free(d); return E_OUTOFMEMORY; }
        d->size = 0;
    } else {
        d->size = g_gmem.size(hg);   /* the stream starts as large as the block */
    }
    d->hg = hg;
    d->refs = 1;
    d->delete_on_release = delete_on_release;
    pthread_mutex_init(&d->lock, NULL);
    struct hg_stream *s = hg_new(d, 0);
    if (!s) { pthread_mutex_destroy(&d->lock); free(d); return E_OUTOFMEMORY; }
    *out = (IUnknown *)s;
    return S_OK;
}

API HRESULT GetHGlobalFromStream(IUnknown *stm, HANDLE *out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!stm || stm->lpVtbl != (const void *)g_hgstream_vtbl) return E_INVALIDARG;
    *out = ((struct hg_stream *)stm)->d->hg;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Error info                                                          */
/* ------------------------------------------------------------------ */

API BSTR SysAllocString(const OLECHAR *s);
API void SysFreeString(BSTR b);

struct errinfo {
    const void *vt_err;           /* IErrorInfo       */
    const void *vt_create;        /* ICreateErrorInfo */
    LONG   refs;
    GUID   guid;
    BSTR   source, desc, helpfile;
    DWORD  helpctx;
};
#define EI_FROM_ERR(p)    ((struct errinfo *)(p))
#define EI_FROM_CREATE(p) ((struct errinfo *)((char *)(p) - offsetof(struct errinfo, vt_create)))

static HRESULT ei_qi(struct errinfo *e, const IID *riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!riid) return E_INVALIDARG;
    if (guid_eq(riid, &IID_IUnknown_) || guid_eq(riid, &IID_IErrorInfo_)) *ppv = &e->vt_err;
    else if (guid_eq(riid, &IID_ICreateErrorInfo_)) *ppv = &e->vt_create;
    else return E_NOINTERFACE;
    __atomic_add_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);
    return S_OK;
}
static ULONG ei_release(struct errinfo *e)
{
    LONG r = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);
    if (!r) { SysFreeString(e->source); SysFreeString(e->desc); SysFreeString(e->helpfile); free(e); }
    return (ULONG)r;
}
static HRESULT ei_get(BSTR v, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    *out = v ? SysAllocString(v) : NULL;
    return v && !*out ? E_OUTOFMEMORY : S_OK;
}
static HRESULT ei_set(BSTR *slot, const WCHAR *v)
{
    BSTR n = v ? SysAllocString(v) : NULL;
    if (v && !n) return E_OUTOFMEMORY;
    SysFreeString(*slot);
    *slot = n;
    return S_OK;
}

API HRESULT eie_QueryInterface(IUnknown *This, const IID *riid, void **ppv) { return ei_qi(EI_FROM_ERR(This), riid, ppv); }
API ULONG eie_AddRef(IUnknown *This) { return (ULONG)__atomic_add_fetch(&EI_FROM_ERR(This)->refs, 1, __ATOMIC_SEQ_CST); }
API ULONG eie_Release(IUnknown *This) { return ei_release(EI_FROM_ERR(This)); }
API HRESULT eie_GetGUID(IUnknown *This, GUID *g) { if (!g) return E_INVALIDARG; *g = EI_FROM_ERR(This)->guid; return S_OK; }
API HRESULT eie_GetSource(IUnknown *This, BSTR *out) { return ei_get(EI_FROM_ERR(This)->source, out); }
API HRESULT eie_GetDescription(IUnknown *This, BSTR *out) { return ei_get(EI_FROM_ERR(This)->desc, out); }
API HRESULT eie_GetHelpFile(IUnknown *This, BSTR *out) { return ei_get(EI_FROM_ERR(This)->helpfile, out); }
API HRESULT eie_GetHelpContext(IUnknown *This, DWORD *out) { if (!out) return E_INVALIDARG; *out = EI_FROM_ERR(This)->helpctx; return S_OK; }
static const void *const g_errinfo_vtbl[] = {
    (void *)eie_QueryInterface, (void *)eie_AddRef, (void *)eie_Release, (void *)eie_GetGUID,
    (void *)eie_GetSource, (void *)eie_GetDescription, (void *)eie_GetHelpFile, (void *)eie_GetHelpContext };

API HRESULT eic_QueryInterface(IUnknown *This, const IID *riid, void **ppv) { return ei_qi(EI_FROM_CREATE(This), riid, ppv); }
API ULONG eic_AddRef(IUnknown *This) { return (ULONG)__atomic_add_fetch(&EI_FROM_CREATE(This)->refs, 1, __ATOMIC_SEQ_CST); }
API ULONG eic_Release(IUnknown *This) { return ei_release(EI_FROM_CREATE(This)); }
API HRESULT eic_SetGUID(IUnknown *This, const GUID *g) { if (!g) return E_INVALIDARG; EI_FROM_CREATE(This)->guid = *g; return S_OK; }
API HRESULT eic_SetSource(IUnknown *This, const WCHAR *v) { return ei_set(&EI_FROM_CREATE(This)->source, v); }
API HRESULT eic_SetDescription(IUnknown *This, const WCHAR *v) { return ei_set(&EI_FROM_CREATE(This)->desc, v); }
API HRESULT eic_SetHelpFile(IUnknown *This, const WCHAR *v) { return ei_set(&EI_FROM_CREATE(This)->helpfile, v); }
API HRESULT eic_SetHelpContext(IUnknown *This, DWORD v) { EI_FROM_CREATE(This)->helpctx = v; return S_OK; }
static const void *const g_createerrinfo_vtbl[] = {
    (void *)eic_QueryInterface, (void *)eic_AddRef, (void *)eic_Release, (void *)eic_SetGUID,
    (void *)eic_SetSource, (void *)eic_SetDescription, (void *)eic_SetHelpFile, (void *)eic_SetHelpContext };

API HRESULT CreateErrorInfo(IUnknown **out)
{
    if (!out) return E_INVALIDARG;
    struct errinfo *e = calloc(1, sizeof *e);
    if (!e) { *out = NULL; return E_OUTOFMEMORY; }
    e->vt_err = g_errinfo_vtbl;
    e->vt_create = g_createerrinfo_vtbl;
    e->refs = 1;
    *out = (IUnknown *)&e->vt_create;
    return S_OK;
}

/* One error object per thread; GetErrorInfo hands it over and clears it. */
API HRESULT SetErrorInfo(ULONG reserved, IUnknown *info)
{
    if (reserved) return E_INVALIDARG;
    unk_addref(info);
    IUnknown *old = t_errinfo;
    t_errinfo = info;
    unk_release(old);
    return S_OK;
}
API HRESULT GetErrorInfo(ULONG reserved, IUnknown **out)
{
    if (!out || reserved) return E_INVALIDARG;
    *out = t_errinfo;
    t_errinfo = NULL;
    return *out ? S_OK : S_FALSE;
}

/* ------------------------------------------------------------------ */
/* HSTRING and the WinRT entry points                                  */
/* ------------------------------------------------------------------ */

/* An HSTRING is NULL for the empty string, otherwise a pointer to this
 * header. "Fast-pass" references built by WindowsCreateStringReference
 * live in the caller's 24-byte HSTRING_HEADER and point at the caller's
 * characters. */
struct hstring {
    uint32_t     flags;
    uint32_t     length;
    const WCHAR *buf;
    int32_t      refs;
    uint32_t     pad;
    WCHAR        data[];
};
#define HSTR_REFERENCE 1
_Static_assert(offsetof(struct hstring, data) == 24, "HSTRING_HEADER is 24 bytes on x64");
typedef struct hstring *HSTRING;

API HRESULT WindowsCreateString(const WCHAR *src, uint32_t len, HSTRING *out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!len) return S_OK;
    if (!src) return E_POINTER;
    struct hstring *h = malloc(sizeof *h + ((size_t)len + 1) * sizeof(WCHAR));
    if (!h) return E_OUTOFMEMORY;
    h->flags = 0;
    h->length = len;
    h->refs = 1;
    h->pad = 0;
    memcpy(h->data, src, (size_t)len * sizeof(WCHAR));
    h->data[len] = 0;
    h->buf = h->data;
    *out = h;
    return S_OK;
}

API HRESULT WindowsCreateStringReference(const WCHAR *src, uint32_t len, void *header, HSTRING *out)
{
    if (!out || !header) return E_INVALIDARG;
    *out = NULL;
    if (!len) return S_OK;
    if (!src) return E_POINTER;
    if (src[len]) return E_INVALIDARG;   /* must be NUL-terminated */
    struct hstring *h = header;
    h->flags = HSTR_REFERENCE;
    h->length = len;
    h->buf = src;
    h->refs = 0;
    h->pad = 0;
    *out = h;
    return S_OK;
}

API HRESULT WindowsDeleteString(HSTRING h)
{
    if (h && !(h->flags & HSTR_REFERENCE) && !__atomic_sub_fetch(&h->refs, 1, __ATOMIC_SEQ_CST)) free(h);
    return S_OK;
}

API HRESULT WindowsDuplicateString(HSTRING h, HSTRING *out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!h) return S_OK;
    if (h->flags & HSTR_REFERENCE) return WindowsCreateString(h->buf, h->length, out);
    __atomic_add_fetch(&h->refs, 1, __ATOMIC_SEQ_CST);
    *out = h;
    return S_OK;
}

API const WCHAR *WindowsGetStringRawBuffer(HSTRING h, uint32_t *len)
{
    static const WCHAR empty[1] = { 0 };
    if (len) *len = h ? h->length : 0;
    return h ? h->buf : empty;
}
API uint32_t WindowsGetStringLen(HSTRING h) { return h ? h->length : 0; }
API BOOL WindowsIsStringEmpty(HSTRING h) { return !h || !h->length; }

API HRESULT WindowsCompareStringOrdinal(HSTRING a, HSTRING b, int32_t *result)
{
    if (!result) return E_INVALIDARG;
    uint32_t la = a ? a->length : 0, lb = b ? b->length : 0, n = la < lb ? la : lb;
    *result = 0;
    for (uint32_t i = 0; i < n && !*result; i++)
        if (a->buf[i] != b->buf[i]) *result = a->buf[i] < b->buf[i] ? -1 : 1;
    if (!*result && la != lb) *result = la < lb ? -1 : 1;
    return S_OK;
}

#define RO_INIT_SINGLETHREADED 0
API HRESULT RoInitialize(int type)
{
    return CoInitializeEx(NULL, type == RO_INIT_SINGLETHREADED ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED);
}
API void RoUninitialize(void) { CoUninitialize(); }
/* No Windows Runtime classes are available. */
API HRESULT RoGetActivationFactory(HSTRING cls, const IID *riid, void **factory)
{
    (void)cls; (void)riid;
    if (factory) *factory = NULL;
    return REGDB_E_CLASSNOTREG;
}
API HRESULT RoActivateInstance(HSTRING cls, IUnknown **inst)
{
    (void)cls;
    if (inst) *inst = NULL;
    return REGDB_E_CLASSNOTREG;
}

/* ------------------------------------------------------------------ */
/* BSTR                                                                */
/* ------------------------------------------------------------------ */

/* A BSTR points at UTF-16 data preceded by its length in bytes (the 32-bit
 * word just before the first character) and followed by a NUL WCHAR. The
 * block comes from the process heap with an 8-byte header, so the data is
 * 8-byte aligned; the header's first word is the pin count used by
 * SysAddRefString / SysReleaseString, with the top bit recording a
 * SysFreeString that has to wait until the string is unpinned. */
struct bstr_hdr { uint32_t pins; uint32_t bytes; };
#define BSTR_FREE_PENDING 0x80000000u

static struct bstr_hdr *bstr_hdr(BSTR b) { return (struct bstr_hdr *)b - 1; }

static BSTR bstr_alloc(const void *src, size_t bytes)
{
    if (bytes > 0x7FFFFFF0u) return NULL;
    struct bstr_hdr *h = lsiw_heap_alloc(sizeof *h + bytes + 3, 0);
    if (!h) return NULL;
    h->pins = 0;
    h->bytes = (uint32_t)bytes;
    uint8_t *d = (uint8_t *)(h + 1);
    if (src) memcpy(d, src, bytes); else memset(d, 0, bytes);
    /* a NUL WCHAR follows the data, even after an odd byte count */
    d[bytes] = d[bytes + 1] = d[bytes + 2] = 0;
    return (BSTR)d;
}

API UINT SysStringLen(BSTR b) { return b ? bstr_hdr(b)->bytes / 2 : 0; }
API UINT SysStringByteLen(BSTR b) { return b ? bstr_hdr(b)->bytes : 0; }
API BSTR SysAllocStringLen(const OLECHAR *s, UINT len) { return bstr_alloc(s, (size_t)len * 2); }
API BSTR SysAllocString(const OLECHAR *s) { return s ? bstr_alloc(s, lsiw_wcslen(s) * 2) : NULL; }
API BSTR SysAllocStringByteLen(const char *s, UINT len) { return bstr_alloc(s, len); }

static BSTR bstr_dup(BSTR b) { return b ? bstr_alloc(b, bstr_hdr(b)->bytes) : NULL; }

API void SysFreeString(BSTR b)
{
    if (!b) return;
    struct bstr_hdr *h = bstr_hdr(b);
    uint32_t old = __atomic_load_n(&h->pins, __ATOMIC_SEQ_CST);
    for (;;) {
        if (!(old & ~BSTR_FREE_PENDING)) { lsiw_heap_free(h); return; }
        if (__atomic_compare_exchange_n(&h->pins, &old, old | BSTR_FREE_PENDING, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return;
    }
}

API HRESULT SysAddRefString(BSTR b)
{
    if (!b) return E_INVALIDARG;
    __atomic_add_fetch(&bstr_hdr(b)->pins, 1, __ATOMIC_SEQ_CST);
    return S_OK;
}

API void SysReleaseString(BSTR b)
{
    if (!b) return;
    struct bstr_hdr *h = bstr_hdr(b);
    uint32_t old = __atomic_load_n(&h->pins, __ATOMIC_SEQ_CST);
    for (;;) {
        if (!(old & ~BSTR_FREE_PENDING)) return;   /* not pinned */
        if (__atomic_compare_exchange_n(&h->pins, &old, old - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
    }
    if (old - 1 == BSTR_FREE_PENDING) lsiw_heap_free(h);
}

/* psz may point into the old string, so the new one is built first. A
 * NULL psz keeps the old contents (as far as they fit). */
API BOOL SysReAllocStringLen(BSTR *pb, const OLECHAR *s, UINT len)
{
    if (!pb) return FALSE;
    BSTR n = bstr_alloc(s, (size_t)len * 2);
    if (!n) return FALSE;
    if (!s && *pb) {
        size_t keep = bstr_hdr(*pb)->bytes;
        if (keep > (size_t)len * 2) keep = (size_t)len * 2;
        memcpy(n, *pb, keep);
    }
    SysFreeString(*pb);
    *pb = n;
    return TRUE;
}
API BOOL SysReAllocString(BSTR *pb, const OLECHAR *s)
{
    return SysReAllocStringLen(pb, s, s ? (UINT)lsiw_wcslen(s) : 0);
}

/* ------------------------------------------------------------------ */
/* VARIANT, PROPVARIANT, SAFEARRAY layouts                             */
/* ------------------------------------------------------------------ */

enum {
    VT_EMPTY = 0, VT_NULL = 1, VT_I2 = 2, VT_I4 = 3, VT_R4 = 4, VT_R8 = 5, VT_CY = 6, VT_DATE = 7,
    VT_BSTR = 8, VT_DISPATCH = 9, VT_ERROR = 10, VT_BOOL = 11, VT_VARIANT = 12, VT_UNKNOWN = 13,
    VT_DECIMAL = 14, VT_I1 = 16, VT_UI1 = 17, VT_UI2 = 18, VT_UI4 = 19, VT_I8 = 20, VT_UI8 = 21,
    VT_INT = 22, VT_UINT = 23, VT_LPSTR = 30, VT_LPWSTR = 31, VT_RECORD = 36, VT_FILETIME = 64,
    VT_BLOB = 65, VT_STREAM = 66, VT_STORAGE = 67, VT_STREAMED_OBJECT = 68, VT_STORED_OBJECT = 69,
    VT_BLOB_OBJECT = 70, VT_CF = 71, VT_CLSID = 72,
    VT_VECTOR = 0x1000, VT_ARRAY = 0x2000, VT_BYREF = 0x4000, VT_RESERVED = 0x8000, VT_TYPEMASK = 0x0FFF
};
#define VARIANT_TRUE  ((VARIANT_BOOL)-1)
#define VARIANT_FALSE ((VARIANT_BOOL)0)

typedef struct { USHORT wReserved; BYTE scale; BYTE sign; ULONG Hi32; ULONGLONG Lo64; } DECIMAL;
typedef struct { ULONG cElements; LONG lLbound; } SAFEARRAYBOUND;
typedef struct SAFEARRAY {
    USHORT cDims;
    USHORT fFeatures;
    ULONG  cbElements;
    ULONG  cLocks;
    void  *pvData;
    SAFEARRAYBOUND rgsabound[1];
} SAFEARRAY;
_Static_assert(offsetof(SAFEARRAY, pvData) == 16 && offsetof(SAFEARRAY, rgsabound) == 24, "SAFEARRAY layout");

typedef struct {
    VARTYPE vt;
    WORD    wReserved1, wReserved2, wReserved3;
    union {
        LONGLONG llVal; LONG lVal; BYTE bVal; SHORT iVal; float fltVal; double dblVal;
        VARIANT_BOOL boolVal; LONG scode; LONGLONG cyVal; DATE date; BSTR bstrVal;
        IUnknown *punkVal; IUnknown *pdispVal; SAFEARRAY *parray; void *byref;
        signed char cVal; USHORT uiVal; ULONG ulVal; ULONGLONG ullVal; int intVal; UINT uintVal;
        struct { void *pvRecord; IUnknown *pRecInfo; } rec;
    } u;
} VARIANT;
_Static_assert(sizeof(VARIANT) == 24, "VARIANT layout");

typedef struct { ULONG cbSize; LONG ulClipFmt; BYTE *pClipData; } CLIPDATA;
typedef struct {
    VARTYPE vt;
    WORD    wReserved1, wReserved2, wReserved3;
    union {
        LONGLONG llVal; BSTR bstrVal; IUnknown *punkVal; SAFEARRAY *parray; void *byref;
        char *pszVal; WCHAR *pwszVal; GUID *puuid; CLIPDATA *pclipdata;
        struct { ULONG cbSize; BYTE *pBlobData; } blob;
        struct { ULONG cElems; void *pElems; } ca;
    } u;
} PROPVARIANT;
_Static_assert(sizeof(PROPVARIANT) == 24, "PROPVARIANT layout");

/* IRecordInfo methods used for VT_RECORD values. */
typedef struct {
    IUnknownVtbl unk;
    HRESULT (WINAPI *RecordInit)(IUnknown *, void *);
    HRESULT (WINAPI *RecordClear)(IUnknown *, void *);
    HRESULT (WINAPI *RecordCopy)(IUnknown *, void *, void *);
    void *GetGuid, *GetName;
    HRESULT (WINAPI *GetSize)(IUnknown *, ULONG *);
    void *GetTypeInfo, *GetField, *GetFieldNoCopy, *PutField, *PutFieldNoCopy, *GetFieldNames, *IsMatchingType;
    void *(WINAPI *RecordCreate)(IUnknown *);
    HRESULT (WINAPI *RecordCreateCopy)(IUnknown *, void *, void **);
    HRESULT (WINAPI *RecordDestroy)(IUnknown *, void *);
} IRecordInfoVtbl;
#define RECINFO(p) ((const IRecordInfoVtbl *)((IUnknown *)(p))->lpVtbl)

API HRESULT SafeArrayDestroy(SAFEARRAY *psa);
API HRESULT SafeArrayCopy(SAFEARRAY *psa, SAFEARRAY **out);

/* Which VARTYPEs may appear in a VARIANT. */
static HRESULT vt_check(VARTYPE vt)
{
    VARTYPE base = vt & VT_TYPEMASK, extra = vt & ~VT_TYPEMASK;
    if (extra & (VT_VECTOR | VT_RESERVED)) return DISP_E_BADVARTYPE;
    if (!((base <= VT_UINT && base != 15) || base == VT_RECORD)) return DISP_E_BADVARTYPE;
    if ((extra & (VT_BYREF | VT_ARRAY)) && base <= VT_NULL) return DISP_E_BADVARTYPE;
    return S_OK;
}

/* Size of one value of a scalar Automation type, 0 if not a scalar. */
static size_t vt_size(VARTYPE vt)
{
    switch (vt) {
    case VT_I1: case VT_UI1: return 1;
    case VT_I2: case VT_UI2: case VT_BOOL: return 2;
    case VT_I4: case VT_UI4: case VT_R4: case VT_ERROR: case VT_INT: case VT_UINT: return 4;
    case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: case VT_FILETIME: return 8;
    case VT_BSTR: case VT_UNKNOWN: case VT_DISPATCH: case VT_LPSTR: case VT_LPWSTR: return sizeof(void *);
    case VT_DECIMAL: case VT_CLSID: case VT_CF: return 16;
    case VT_VARIANT: return sizeof(VARIANT);
    default: return 0;
    }
}

API void VariantInit(VARIANT *v) { if (v) v->vt = VT_EMPTY; }

API HRESULT VariantClear(VARIANT *v)
{
    if (!v) return E_INVALIDARG;
    HRESULT hr = vt_check(v->vt);
    if (FAILED(hr)) return hr;
    if (!(v->vt & VT_BYREF)) {
        if (v->vt & VT_ARRAY) {
            if (FAILED(hr = SafeArrayDestroy(v->u.parray))) return hr;
        } else switch (v->vt) {
        case VT_BSTR: SysFreeString(v->u.bstrVal); break;
        case VT_DISPATCH: case VT_UNKNOWN: unk_release(v->u.punkVal); break;
        case VT_RECORD:
            if (v->u.rec.pRecInfo) {
                RECINFO(v->u.rec.pRecInfo)->RecordClear(v->u.rec.pRecInfo, v->u.rec.pvRecord);
                unk_release(v->u.rec.pRecInfo);
            }
            break;
        }
    }
    v->vt = VT_EMPTY;
    return S_OK;
}

/* Deep copy of src (not VT_BYREF-dereferencing) into an empty dst. */
static HRESULT variant_copy_into(VARIANT *dst, const VARIANT *src)
{
    *dst = *src;   /* also copies a DECIMAL, which overlays the whole VARIANT */
    if (src->vt & VT_BYREF) return S_OK;
    HRESULT hr = S_OK;
    if (src->vt & VT_ARRAY) {
        hr = SafeArrayCopy(src->u.parray, &dst->u.parray);
    } else switch (src->vt) {
    case VT_BSTR:
        dst->u.bstrVal = bstr_dup(src->u.bstrVal);
        if (src->u.bstrVal && !dst->u.bstrVal) hr = E_OUTOFMEMORY;
        break;
    case VT_DISPATCH: case VT_UNKNOWN: unk_addref(src->u.punkVal); break;
    case VT_RECORD:
        if (src->u.rec.pRecInfo) {
            hr = RECINFO(src->u.rec.pRecInfo)->RecordCreateCopy(src->u.rec.pRecInfo, src->u.rec.pvRecord, &dst->u.rec.pvRecord);
            if (SUCCEEDED(hr)) unk_addref(src->u.rec.pRecInfo);
        }
        break;
    }
    if (FAILED(hr)) dst->vt = VT_EMPTY;
    return hr;
}

API HRESULT VariantCopy(VARIANT *dst, const VARIANT *src)
{
    if (!dst || !src) return E_INVALIDARG;
    HRESULT hr = vt_check(src->vt);
    if (FAILED(hr)) return hr;
    if (dst == src) return S_OK;
    if (FAILED(hr = VariantClear(dst))) return hr;
    return variant_copy_into(dst, src);
}

API HRESULT VariantCopyInd(VARIANT *dst, const VARIANT *src)
{
    if (!dst || !src) return E_INVALIDARG;
    if (!(src->vt & VT_BYREF)) return VariantCopy(dst, src);
    HRESULT hr = vt_check(src->vt);
    if (FAILED(hr)) return hr;
    VARTYPE base = src->vt & ~VT_BYREF;
    VARIANT tmp;
    memset(&tmp, 0, sizeof tmp);
    if (base == VT_VARIANT) {
        const VARIANT *inner = src->u.byref;
        if (!inner) return E_INVALIDARG;
        if (inner->vt & VT_BYREF) return E_INVALIDARG;   /* only one level of indirection */
        if (FAILED(hr = vt_check(inner->vt))) return hr;
        hr = variant_copy_into(&tmp, inner);
    } else if (base & VT_ARRAY) {
        tmp.vt = base;
        hr = SafeArrayCopy(src->u.byref ? *(SAFEARRAY **)src->u.byref : NULL, &tmp.u.parray);
    } else if (base == VT_RECORD) {
        VARIANT r = *src;
        r.vt = VT_RECORD;
        hr = variant_copy_into(&tmp, &r);
    } else {
        if (!src->u.byref) return E_INVALIDARG;
        if (base == VT_DECIMAL) {
            memcpy(&tmp, src->u.byref, sizeof(DECIMAL));
        } else if (base == VT_BSTR) {
            BSTR b = *(BSTR *)src->u.byref;
            tmp.u.bstrVal = bstr_dup(b);
            if (b && !tmp.u.bstrVal) hr = E_OUTOFMEMORY;
        } else if (base == VT_UNKNOWN || base == VT_DISPATCH) {
            tmp.u.punkVal = *(IUnknown **)src->u.byref;
            unk_addref(tmp.u.punkVal);
        } else {
            memcpy(&tmp.u, src->u.byref, vt_size(base));
        }
        tmp.vt = base;
    }
    if (FAILED(hr)) return hr;
    if (dst != src && FAILED(hr = VariantClear(dst))) { VariantClear(&tmp); return hr; }
    *dst = tmp;
    return S_OK;
}

/* ---- PROPVARIANT ---- */

static int pv_scalar_ok(VARTYPE vt)
{
    switch (vt) {
    case VT_EMPTY: case VT_NULL: case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_BOOL:
    case VT_I4: case VT_UI4: case VT_R4: case VT_ERROR: case VT_INT: case VT_UINT: case VT_I8:
    case VT_UI8: case VT_R8: case VT_CY: case VT_DATE: case VT_FILETIME: case VT_BSTR: case VT_LPSTR:
    case VT_LPWSTR: case VT_CLSID: case VT_BLOB: case VT_BLOB_OBJECT: case VT_UNKNOWN: case VT_DISPATCH:
    case VT_STREAM: case VT_STREAMED_OBJECT: case VT_STORAGE: case VT_STORED_OBJECT: case VT_CF:
    case VT_DECIMAL:
        return 1;
    }
    return 0;
}

API HRESULT PropVariantClear(PROPVARIANT *pv);

static void pv_free_elem(VARTYPE base, void *e)
{
    switch (base) {
    case VT_BSTR: SysFreeString(*(BSTR *)e); break;
    case VT_LPSTR: case VT_LPWSTR: CoTaskMemFree(*(void **)e); break;
    case VT_VARIANT: PropVariantClear(e); break;
    case VT_CF: CoTaskMemFree(((CLIPDATA *)e)->pClipData); break;
    }
}

API HRESULT PropVariantClear(PROPVARIANT *pv)
{
    if (!pv) return S_OK;
    VARTYPE vt = pv->vt;
    if (vt & VT_BYREF) {
        /* nothing is owned */
    } else if (vt & VT_ARRAY) {
        SafeArrayDestroy(pv->u.parray);
    } else if (vt & VT_VECTOR) {
        VARTYPE base = vt & VT_TYPEMASK;
        size_t sz = vt_size(base);
        if (!sz || (vt & ~(VT_VECTOR | VT_TYPEMASK))) return STG_E_INVALIDPARAMETER;
        for (ULONG i = 0; pv->u.ca.pElems && i < pv->u.ca.cElems; i++)
            pv_free_elem(base, (char *)pv->u.ca.pElems + i * sz);
        CoTaskMemFree(pv->u.ca.pElems);
    } else {
        if (!pv_scalar_ok(vt)) return STG_E_INVALIDPARAMETER;
        switch (vt) {
        case VT_BSTR: SysFreeString(pv->u.bstrVal); break;
        case VT_LPSTR: case VT_LPWSTR: case VT_CLSID: CoTaskMemFree(pv->u.byref); break;
        case VT_BLOB: case VT_BLOB_OBJECT: CoTaskMemFree(pv->u.blob.pBlobData); break;
        case VT_UNKNOWN: case VT_DISPATCH: case VT_STREAM: case VT_STREAMED_OBJECT:
        case VT_STORAGE: case VT_STORED_OBJECT: unk_release(pv->u.punkVal); break;
        case VT_CF:
            if (pv->u.pclipdata) { CoTaskMemFree(pv->u.pclipdata->pClipData); CoTaskMemFree(pv->u.pclipdata); }
            break;
        }
    }
    memset(pv, 0, sizeof *pv);
    return S_OK;
}

static void *task_dup(const void *src, size_t n)
{
    void *p = CoTaskMemAlloc(n ? n : 1);
    if (p && n) memcpy(p, src, n);
    return p;
}

static HRESULT clip_copy(CLIPDATA *dst, const CLIPDATA *src)
{
    *dst = *src;
    size_t n = src->cbSize >= 4 ? src->cbSize - 4 : 0;   /* cbSize counts ulClipFmt */
    dst->pClipData = NULL;
    if (src->pClipData && !(dst->pClipData = task_dup(src->pClipData, n))) return E_OUTOFMEMORY;
    return S_OK;
}

API HRESULT PropVariantCopy(PROPVARIANT *dst, const PROPVARIANT *src);

static HRESULT pv_copy_elem(VARTYPE base, void *d, const void *s)
{
    switch (base) {
    case VT_BSTR:
        *(BSTR *)d = bstr_dup(*(BSTR *)s);
        return *(BSTR *)s && !*(BSTR *)d ? E_OUTOFMEMORY : S_OK;
    case VT_LPSTR: {
        const char *p = *(char **)s;
        *(char **)d = p ? task_dup(p, strlen(p) + 1) : NULL;
        return p && !*(char **)d ? E_OUTOFMEMORY : S_OK;
    }
    case VT_LPWSTR: {
        const WCHAR *p = *(WCHAR **)s;
        *(WCHAR **)d = p ? task_dup(p, (lsiw_wcslen(p) + 1) * sizeof(WCHAR)) : NULL;
        return p && !*(WCHAR **)d ? E_OUTOFMEMORY : S_OK;
    }
    case VT_VARIANT: return PropVariantCopy(d, s);
    case VT_CF: return clip_copy(d, s);
    default: memcpy(d, s, vt_size(base)); return S_OK;
    }
}

API HRESULT PropVariantCopy(PROPVARIANT *dst, const PROPVARIANT *src)
{
    if (!dst || !src) return E_INVALIDARG;
    VARTYPE vt = src->vt;
    if (!(vt & (VT_BYREF | VT_ARRAY | VT_VECTOR)) && !pv_scalar_ok(vt)) return STG_E_INVALIDPARAMETER;
    *dst = *src;
    HRESULT hr = S_OK;
    if (vt & VT_BYREF) return S_OK;
    if (vt & VT_ARRAY) {
        hr = SafeArrayCopy(src->u.parray, &dst->u.parray);
    } else if (vt & VT_VECTOR) {
        VARTYPE base = vt & VT_TYPEMASK;
        size_t sz = vt_size(base);
        if (!sz) { dst->vt = VT_EMPTY; return STG_E_INVALIDPARAMETER; }
        ULONG n = src->u.ca.cElems;
        dst->u.ca.pElems = NULL;
        if (src->u.ca.pElems) {
            if (!(dst->u.ca.pElems = lsiw_heap_alloc((size_t)n * sz + 1, 1))) hr = E_OUTOFMEMORY;
            for (ULONG i = 0; SUCCEEDED(hr) && i < n; i++)
                hr = pv_copy_elem(base, (char *)dst->u.ca.pElems + i * sz, (const char *)src->u.ca.pElems + i * sz);
            if (FAILED(hr)) { dst->u.ca.cElems = n; dst->vt = vt; PropVariantClear(dst); }
        }
    } else switch (vt) {
    case VT_BSTR: case VT_LPSTR: case VT_LPWSTR:
        hr = pv_copy_elem(vt, &dst->u.byref, &src->u.byref);
        break;
    case VT_CLSID:
        if (src->u.puuid && !(dst->u.puuid = task_dup(src->u.puuid, sizeof(GUID)))) hr = E_OUTOFMEMORY;
        break;
    case VT_BLOB: case VT_BLOB_OBJECT:
        if (src->u.blob.pBlobData && !(dst->u.blob.pBlobData = task_dup(src->u.blob.pBlobData, src->u.blob.cbSize)))
            hr = E_OUTOFMEMORY;
        break;
    case VT_UNKNOWN: case VT_DISPATCH: case VT_STREAM: case VT_STREAMED_OBJECT:
    case VT_STORAGE: case VT_STORED_OBJECT:
        unk_addref(src->u.punkVal);
        break;
    case VT_CF:
        if (src->u.pclipdata) {
            CLIPDATA *c = CoTaskMemAlloc(sizeof *c);
            if (!c || FAILED(clip_copy(c, src->u.pclipdata))) { CoTaskMemFree(c); hr = E_OUTOFMEMORY; }
            else dst->u.pclipdata = c;
        }
        break;
    }
    if (FAILED(hr)) dst->vt = VT_EMPTY;
    return hr;
}

/* ------------------------------------------------------------------ */
/* Dates                                                               */
/* ------------------------------------------------------------------ */

/* An OLE DATE counts days from 1899-12-30; the fraction is the time of
 * day and, for dates before the epoch, is added away from zero
 * (-1.25 is 1899-12-29 06:00). Valid years are 100..9999. */
#define DATE_MIN (-657434.0)
#define DATE_MAX 2958466.0      /* exclusive: 10000-01-01 */
#define UNIX_TO_OLE_DAYS 25569

/* Proleptic Gregorian calendar <-> days since 1970-01-01. */
static long days_from_civil(long y, long m, long d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(long z, long *y, long *m, long *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

static int date_from_parts(long y, long mo, long d, long h, long mi, long s, double *out)
{
    if (y < 100 || y > 9999 || mo < 1 || mo > 12 || d < 0 || d > 31 || h < 0 || h > 23 ||
        mi < 0 || mi > 59 || s < 0 || s > 59) return 0;
    long days = days_from_civil(y, mo, d) + UNIX_TO_OLE_DAYS;
    double frac = (double)(h * 3600 + mi * 60 + s) / 86400.0;
    *out = days >= 0 ? days + frac : days - frac;
    return 1;
}

static int date_to_parts(double t, SYSTEMTIME *st)
{
    if (!(t >= DATE_MIN && t < DATE_MAX)) return 0;
    double ip = trunc(t);
    long days = (long)ip;
    long secs = (long)floor(fabs(t - ip) * 86400.0 + 0.5);   /* nearest second */
    if (secs >= 86400) { secs -= 86400; days++; }
    long y, m, d;
    civil_from_days(days - UNIX_TO_OLE_DAYS, &y, &m, &d);
    st->wYear = (WORD)y; st->wMonth = (WORD)m; st->wDay = (WORD)d;
    st->wDayOfWeek = (WORD)(((days % 7) + 7 + 6) % 7);   /* 1899-12-30 was a Saturday */
    st->wHour = (WORD)(secs / 3600); st->wMinute = (WORD)(secs / 60 % 60); st->wSecond = (WORD)(secs % 60);
    st->wMilliseconds = 0;
    return 1;
}

API int VariantTimeToSystemTime(double t, SYSTEMTIME *st) { return st && date_to_parts(t, st); }

API int SystemTimeToVariantTime(SYSTEMTIME *st, double *out)
{
    if (!st || !out) return FALSE;
    long y = st->wYear;
    if (y < 30) y += 2000; else if (y < 100) y += 1900;   /* two-digit years */
    return date_from_parts(y, st->wMonth, st->wDay, st->wHour, st->wMinute, st->wSecond, out);
}

API int DosDateTimeToVariantTime(USHORT dosdate, USHORT dostime, double *out)
{
    if (!out || !(dosdate & 0x1F)) return FALSE;
    return date_from_parts(1980 + (dosdate >> 9), (dosdate >> 5) & 0xF, dosdate & 0x1F,
                           dostime >> 11, (dostime >> 5) & 0x3F, (dostime & 0x1F) * 2, out);
}

API int VariantTimeToDosDateTime(double t, USHORT *dosdate, USHORT *dostime)
{
    SYSTEMTIME st;
    if (!dosdate || !dostime || !date_to_parts(t, &st) || st.wYear < 1980 || st.wYear > 2107) return FALSE;
    *dosdate = (USHORT)((st.wYear - 1980) << 9 | st.wMonth << 5 | st.wDay);
    *dostime = (USHORT)(st.wHour << 11 | st.wMinute << 5 | st.wSecond / 2);
    return TRUE;
}

#define VAR_TIMEVALUEONLY 0x1
#define VAR_DATEVALUEONLY 0x2

/* en-US layout: M/D/YYYY h:mm:ss AM; the epoch date alone shows only the
 * time, and midnight shows only the date. */
static void date_to_str(double t, DWORD flags, char *out, size_t n)
{
    SYSTEMTIME st;
    if (!date_to_parts(t, &st)) { snprintf(out, n, "%s", ""); return; }
    int has_date = trunc(t) != 0;
    int has_time = st.wHour || st.wMinute || st.wSecond || !has_date;
    if (flags & VAR_TIMEVALUEONLY) { has_date = 0; has_time = 1; }
    if (flags & VAR_DATEVALUEONLY) { has_date = 1; has_time = 0; }
    size_t k = 0;
    if (has_date) k += (size_t)snprintf(out, n, "%u/%u/%u", st.wMonth, st.wDay, st.wYear);
    if (has_time && k < n) {
        unsigned h12 = st.wHour % 12 ? st.wHour % 12 : 12;
        snprintf(out + k, n - k, "%s%u:%02u:%02u %s", has_date ? " " : "", h12, st.wMinute, st.wSecond,
                 st.wHour < 12 ? "AM" : "PM");
    }
}

/* Accepts YYYY-MM-DD / YYYY/MM/DD, M/D/YYYY (two-digit years allowed), an
 * optional time H:MM[:SS] [AM|PM], or a time alone. */
static int date_from_str(const char *s, double *out)
{
    long v[3] = { 0, 0, 0 }, digits0 = 0;
    long y = 1899, mo = 12, d = 30, h = 0, mi = 0, sec = 0;
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    int ndate = 0;
    const char *q = p;
    for (; ndate < 3; ndate++) {
        if (*q < '0' || *q > '9') break;
        const char *start = q;
        long x = 0;
        while (*q >= '0' && *q <= '9') { x = x * 10 + (*q - '0'); q++; if (x > 99999) return 0; }
        if (ndate == 0) digits0 = q - start;
        v[ndate] = x;
        if (ndate < 2 && (*q == '/' || *q == '-' || *q == '.')) { q++; continue; }
        ndate++;
        break;
    }
    if (ndate == 3) {
        if (digits0 >= 3) { y = v[0]; mo = v[1]; d = v[2]; }
        else { mo = v[0]; d = v[1]; y = v[2]; if (y < 30) y += 2000; else if (y < 100) y += 1900; }
        p = q;
        while (*p == ' ' || *p == '\t' || *p == 'T') p++;
    }
    if (*p) {
        if (sscanf(p, "%ld:%ld", &h, &mi) != 2) return 0;
        while (*p >= '0' && *p <= '9') p++;
        p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == ':') { p++; if (*p < '0' || *p > '9') return 0; sec = strtol(p, (char **)&p, 10); }
        while (*p == ' ') p++;
        if (!strncasecmp(p, "AM", 2) || !strncasecmp(p, "PM", 2)) {
            if (h < 1 || h > 12) return 0;
            int pm = (p[0] | 0x20) == 'p';
            h = h % 12 + (pm ? 12 : 0);
            p += 2;
        }
        while (*p == ' ') p++;
        if (*p) return 0;
    } else if (ndate != 3) {
        return 0;
    }
    if (ndate == 3) {
        /* reject impossible days such as 2/30 */
        long ry, rm, rd;
        civil_from_days(days_from_civil(y, mo, d), &ry, &rm, &rd);
        if (rm != mo || rd != d) return 0;
    }
    return date_from_parts(y, mo, d, h, mi, sec, out);
}

/* ------------------------------------------------------------------ */
/* Numeric conversions                                                 */
/* ------------------------------------------------------------------ */

/* Automation rounds to even ("banker's rounding"). Done by hand so the
 * caller's FPU rounding mode does not matter. */
static long double round_even(long double x)
{
    long double f = floorl(x), d = x - f;
    if (d > 0.5L || (d == 0.5L && fmodl(f, 2.0L) != 0)) f += 1;
    return f;
}

static long double pow10l_int(int n) { long double r = 1; while (n-- > 0) r *= 10; return r; }

static long double dec_to_ld(const DECIMAL *d)
{
    long double v = (long double)d->Hi32 * 18446744073709551616.0L + (long double)d->Lo64;
    v /= pow10l_int(d->scale);
    return (d->sign & 0x80) ? -v : v;
}

static HRESULT ld_to_dec(long double x, DECIMAL *d)
{
    memset(d, 0, sizeof *d);
    if (x < 0) { d->sign = 0x80; x = -x; }
    if (!(x < 79228162514264337593543950336.0L)) return DISP_E_OVERFLOW;
    unsigned __int128 m;
    int scale = 0;
    if (x == floorl(x) && x < 18446744073709551616.0L) {
        m = (unsigned long long)x;
    } else {
        /* 15 significant digits, as Automation keeps for doubles */
        char buf[64];
        fmt_c(buf, sizeof buf, "%.14Le", x);
        char *e = strchr(buf, 'e');
        int ex = atoi(e + 1);
        m = 0;
        for (char *p = buf; p < e; p++) if (*p >= '0' && *p <= '9') m = m * 10 + (unsigned)(*p - '0');
        int sh = ex - 14;
        if (sh >= 0) { while (sh--) m *= 10; }
        else {
            scale = -sh;
            while (scale > 28) { unsigned r = (unsigned)(m % 10); m /= 10; if (r >= 5) m++; scale--; }
        }
        while (scale > 0 && m % 10 == 0) { m /= 10; scale--; }
        if (m >> 96) return DISP_E_OVERFLOW;
    }
    d->scale = (BYTE)scale;
    d->Hi32 = (ULONG)(m >> 64);
    d->Lo64 = (ULONGLONG)m;
    if (!m) d->sign = 0;
    return S_OK;
}

static void dec_to_str(const DECIMAL *d, char *out)
{
    unsigned __int128 m = (unsigned __int128)d->Hi32 << 64 | d->Lo64;
    char digits[64];
    int n = 0;
    do { digits[n++] = (char)('0' + (int)(m % 10)); m /= 10; } while (m);
    int scale = d->scale;
    while (n <= scale) digits[n++] = '0';
    int k = 0, first = 0;
    if ((d->sign & 0x80) && (d->Hi32 || d->Lo64)) out[k++] = '-';
    /* trailing fractional zeros are dropped */
    while (scale > 0 && digits[first] == '0') { first++; scale--; }
    for (int i = n - 1; i >= first; i--) {
        out[k++] = digits[i];
        if (i - first == scale && scale > 0) out[k++] = '.';
    }
    out[k] = 0;
}

static void cy_to_str(LONGLONG cy, char *out)
{
    ULONGLONG a = cy < 0 ? 0 - (ULONGLONG)cy : (ULONGLONG)cy;
    int k = snprintf(out, 32, "%s%llu", cy < 0 ? "-" : "", (unsigned long long)(a / 10000));
    unsigned frac = (unsigned)(a % 10000);
    if (frac) {
        snprintf(out + k, 8, ".%04u", frac);
        char *e = out + strlen(out) - 1;
        while (*e == '0') *e-- = 0;
    }
}

/* Parses a number the way VarR8FromStr does in the C locale: optional
 * spaces, sign, digits, '.', exponent; &H / &O prefixes for hex / octal. */
static HRESULT str_to_ld(const WCHAR *w, long double *out)
{
    size_t n = w ? lsiw_wcslen(w) : 0;
    char *s = malloc(n + 1);
    if (!s) return E_OUTOFMEMORY;
    for (size_t i = 0; i < n; i++) {
        if (w[i] > 0x7F) { free(s); return DISP_E_TYPEMISMATCH; }
        s[i] = (char)w[i];
    }
    s[n] = 0;
    char *p = s, *end;
    HRESULT hr = DISP_E_TYPEMISMATCH;
    while (*p == ' ' || *p == '\t') p++;
    char *e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    if (p[0] == '&' && (p[1] == 'H' || p[1] == 'h' || p[1] == 'O' || p[1] == 'o')) {
        int base = (p[1] | 0x20) == 'h' ? 16 : 8;
        if (p[2] && p[2] != '-' && p[2] != '+' && p[2] != ' ') {
            errno = 0;
            unsigned long long v = strtoull(p + 2, &end, base);
            if (!*end && !errno) { *out = (long double)v; hr = S_OK; }
            else if (errno == ERANGE) hr = DISP_E_OVERFLOW;
        }
    } else {
        /* validate the form first: strtold would also take inf/nan/hex */
        const char *q = p;
        int digits = 0;
        if (*q == '+' || *q == '-') q++;
        while (*q >= '0' && *q <= '9') { q++; digits++; }
        if (*q == '.') { q++; while (*q >= '0' && *q <= '9') { q++; digits++; } }
        if (digits && (*q == 'e' || *q == 'E')) {
            q++;
            if (*q == '+' || *q == '-') q++;
            if (*q < '0' || *q > '9') digits = 0;
            while (*q >= '0' && *q <= '9') q++;
        }
        if (digits && !*q) {
            *out = strtold_l(p, &end, c_locale());
            hr = isinf(*out) ? DISP_E_OVERFLOW : S_OK;
        }
    }
    free(s);
    return hr;
}

static HRESULT ld_to_var(long double x, VARTYPE vt, VARIANT *out)
{
    long double r = round_even(x);
#define RANGE(lo, hi) if (!(r >= (lo) && r <= (hi))) return DISP_E_OVERFLOW
    switch (vt) {
    case VT_I1:  RANGE(-128.0L, 127.0L); out->u.cVal = (signed char)r; break;
    case VT_UI1: RANGE(0.0L, 255.0L); out->u.bVal = (BYTE)r; break;
    case VT_I2:  RANGE(-32768.0L, 32767.0L); out->u.iVal = (SHORT)r; break;
    case VT_UI2: RANGE(0.0L, 65535.0L); out->u.uiVal = (USHORT)r; break;
    case VT_I4: case VT_INT: RANGE(-2147483648.0L, 2147483647.0L); out->u.lVal = (LONG)r; break;
    case VT_UI4: case VT_UINT: RANGE(0.0L, 4294967295.0L); out->u.ulVal = (ULONG)r; break;
    case VT_I8:  RANGE(-9223372036854775808.0L, 9223372036854775807.0L); out->u.llVal = (LONGLONG)r; break;
    case VT_UI8: RANGE(0.0L, 18446744073709551615.0L); out->u.ullVal = (ULONGLONG)r; break;
    case VT_R4:  if (fabsl(x) > FLT_MAX) return DISP_E_OVERFLOW; out->u.fltVal = (float)x; break;
    case VT_R8:  if (fabsl(x) > DBL_MAX) return DISP_E_OVERFLOW; out->u.dblVal = (double)x; break;
    case VT_DATE: if (!(x >= DATE_MIN && x < DATE_MAX)) return DISP_E_OVERFLOW; out->u.date = (double)x; break;
    case VT_BOOL: out->u.boolVal = x != 0 ? VARIANT_TRUE : VARIANT_FALSE; break;
    case VT_CY:
        r = round_even(x * 10000.0L);
        RANGE(-9223372036854775808.0L, 9223372036854775807.0L);
        out->u.cyVal = (LONGLONG)r;
        break;
    case VT_DECIMAL: {
        DECIMAL d;
        HRESULT hr = ld_to_dec(x, &d);
        if (FAILED(hr)) return hr;
        memcpy(out, &d, sizeof d);
        break;
    }
    default: return DISP_E_TYPEMISMATCH;
    }
#undef RANGE
    out->vt = vt;
    return S_OK;
}

static HRESULT var_to_ld(const VARIANT *v, long double *x)
{
    switch (v->vt) {
    case VT_EMPTY: *x = 0; break;
    case VT_I1: *x = v->u.cVal; break;
    case VT_UI1: *x = v->u.bVal; break;
    case VT_I2: *x = v->u.iVal; break;
    case VT_UI2: *x = v->u.uiVal; break;
    case VT_I4: case VT_INT: *x = v->u.lVal; break;
    case VT_UI4: case VT_UINT: *x = v->u.ulVal; break;
    case VT_I8: *x = v->u.llVal; break;
    case VT_UI8: *x = v->u.ullVal; break;
    case VT_R4: *x = v->u.fltVal; break;
    case VT_R8: case VT_DATE: *x = v->u.dblVal; break;
    case VT_BOOL: *x = v->u.boolVal; break;
    case VT_CY: *x = (long double)v->u.cyVal / 10000.0L; break;
    case VT_DECIMAL: { DECIMAL d; memcpy(&d, v, sizeof d); *x = dec_to_ld(&d); break; }
    case VT_BSTR: return str_to_ld(v->u.bstrVal, x);
    default: return DISP_E_TYPEMISMATCH;
    }
    return S_OK;
}

static HRESULT bstr_from_ascii(const char *s, BSTR *out)
{
    size_t n = strlen(s);
    if (!(*out = bstr_alloc(NULL, n * 2))) return E_OUTOFMEMORY;
    for (size_t i = 0; i < n; i++) (*out)[i] = (unsigned char)s[i];
    return S_OK;
}

static char *wide_to_ascii_dup(const WCHAR *w)
{
    size_t n = w ? lsiw_wcslen(w) : 0;
    char *s = malloc(n + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < n; i++) s[i] = w[i] < 0x80 ? (char)w[i] : '\x01';
    s[n] = 0;
    return s;
}

static int bool_word(const WCHAR *w, VARIANT_BOOL *b)
{
    char *s = wide_to_ascii_dup(w);
    if (!s) return 0;
    char *p = s, *e;
    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    int r = 1;
    if (!strcasecmp(p, "true") || !strcasecmp(p, "#TRUE#")) *b = VARIANT_TRUE;
    else if (!strcasecmp(p, "false") || !strcasecmp(p, "#FALSE#")) *b = VARIANT_FALSE;
    else r = 0;
    free(s);
    return r;
}

#define VARIANT_NOVALUEPROP 0x01
#define VARIANT_ALPHABOOL   0x02
#define VARIANT_LOCALBOOL   0x10

static HRESULT var_to_bstr(const VARIANT *v, int alphabool, DWORD dateflags, BSTR *out)
{
    char buf[128];
    switch (v->vt) {
    case VT_EMPTY: buf[0] = 0; break;
    case VT_I1: case VT_I2: case VT_I4: case VT_INT: case VT_I8: case VT_UI1: case VT_UI2:
    case VT_UI4: case VT_UINT: case VT_UI8: {
        long double x;
        var_to_ld(v, &x);
        if (v->vt == VT_UI8) snprintf(buf, sizeof buf, "%llu", (unsigned long long)v->u.ullVal);
        else if (v->vt == VT_I8) snprintf(buf, sizeof buf, "%lld", (long long)v->u.llVal);
        else snprintf(buf, sizeof buf, "%lld", (long long)x);
        break;
    }
    case VT_R4: fmt_c(buf, sizeof buf, "%.7G", (double)v->u.fltVal); break;
    case VT_R8: fmt_c(buf, sizeof buf, "%.15G", v->u.dblVal); break;
    case VT_BOOL:
        if (alphabool) snprintf(buf, sizeof buf, "%s", v->u.boolVal ? "True" : "False");
        else snprintf(buf, sizeof buf, "%d", (int)v->u.boolVal);
        break;
    case VT_DATE:
        if (!(v->u.date >= DATE_MIN && v->u.date < DATE_MAX)) return DISP_E_OVERFLOW;
        date_to_str(v->u.date, dateflags, buf, sizeof buf);
        break;
    case VT_CY: cy_to_str(v->u.cyVal, buf); break;
    case VT_DECIMAL: { DECIMAL d; memcpy(&d, v, sizeof d); if (d.scale > 28) return E_INVALIDARG; dec_to_str(&d, buf); break; }
    case VT_BSTR:
        *out = v->u.bstrVal ? bstr_dup(v->u.bstrVal) : bstr_alloc(NULL, 0);
        return *out ? S_OK : E_OUTOFMEMORY;
    default: return DISP_E_TYPEMISMATCH;
    }
    return bstr_from_ascii(buf, out);
}

typedef struct { VARIANT *rgvarg; LONG *rgdispidNamedArgs; UINT cArgs; UINT cNamedArgs; } DISPPARAMS;
typedef HRESULT (WINAPI *invoke_fn)(IUnknown *, LONG, const IID *, LCID, WORD, DISPPARAMS *, VARIANT *, void *, UINT *);

static HRESULT var_change(VARIANT *out, const VARIANT *in, LCID lcid, USHORT flags, VARTYPE vt);
static SAFEARRAY *sa_vector(VARTYPE vt, LONG lbound, ULONG count);

/* Converting an object to a scalar reads its default (DISPID_VALUE) property. */
static HRESULT disp_value_change(VARIANT *out, IUnknown *disp, LCID lcid, USHORT flags, VARTYPE vt)
{
    if (!disp) return DISP_E_TYPEMISMATCH;
    DISPPARAMS dp = { NULL, NULL, 0, 0 };
    VARIANT val;
    val.vt = VT_EMPTY;
    HRESULT hr = ((invoke_fn)((void *const *)disp->lpVtbl)[6])(disp, 0, &GUID_NULL_, lcid, 2 /* DISPATCH_PROPERTYGET */,
                                                              &dp, &val, NULL, NULL);
    if (FAILED(hr)) return hr;
    if (val.vt == VT_DISPATCH || (val.vt & VT_BYREF)) hr = DISP_E_TYPEMISMATCH;
    else hr = var_change(out, &val, lcid, flags | VARIANT_NOVALUEPROP, vt);
    VariantClear(&val);
    return hr;
}

/* in is not VT_BYREF; out is overwritten (treated as empty). */
static HRESULT var_change(VARIANT *out, const VARIANT *in, LCID lcid, USHORT flags, VARTYPE vt)
{
    VARTYPE from = in->vt;
    memset(out, 0, sizeof *out);
    if (from == vt) return variant_copy_into(out, in);
    if (vt == VT_EMPTY) return S_OK;
    if (vt == VT_NULL) { if (from != VT_EMPTY) return DISP_E_TYPEMISMATCH; out->vt = VT_NULL; return S_OK; }
    if (from == VT_NULL) return DISP_E_TYPEMISMATCH;
    /* byte arrays and strings convert into each other byte for byte */
    if (from == (VT_ARRAY | VT_UI1) && vt == VT_BSTR) {
        SAFEARRAY *sa = in->u.parray;
        if (!sa || sa->cDims != 1) return DISP_E_TYPEMISMATCH;
        if (!(out->u.bstrVal = bstr_alloc(sa->pvData, sa->rgsabound[0].cElements))) return E_OUTOFMEMORY;
        out->vt = VT_BSTR;
        return S_OK;
    }
    if (from == VT_BSTR && vt == (VT_ARRAY | VT_UI1)) {
        UINT n = SysStringByteLen(in->u.bstrVal);
        SAFEARRAY *sa = sa_vector(VT_UI1, 0, n);
        if (!sa) return E_OUTOFMEMORY;
        if (n) memcpy(sa->pvData, in->u.bstrVal, n);
        out->u.parray = sa;
        out->vt = vt;
        return S_OK;
    }
    if ((vt & (VT_ARRAY | VT_BYREF)) || (from & VT_ARRAY)) return DISP_E_TYPEMISMATCH;
    if (vt == VT_UNKNOWN || vt == VT_DISPATCH) {
        if (from != VT_UNKNOWN && from != VT_DISPATCH) return DISP_E_TYPEMISMATCH;
        out->vt = vt;
        if (!in->u.punkVal) return S_OK;
        HRESULT hr = in->u.punkVal->lpVtbl->QueryInterface(in->u.punkVal, vt == VT_UNKNOWN ? &IID_IUnknown_ : &IID_IDispatch_,
                                                           (void **)&out->u.punkVal);
        if (FAILED(hr)) out->vt = VT_EMPTY;
        return hr;
    }
    if (from == VT_DISPATCH && !(flags & VARIANT_NOVALUEPROP)) return disp_value_change(out, in->u.pdispVal, lcid, flags, vt);
    if (from == VT_DISPATCH || from == VT_UNKNOWN) return DISP_E_TYPEMISMATCH;
    if (vt == VT_ERROR || from == VT_ERROR) {
        VARTYPE other = vt == VT_ERROR ? from : vt;
        if (other != VT_I4 && other != VT_UI4 && other != VT_INT && other != VT_UINT) return DISP_E_TYPEMISMATCH;
        out->u.lVal = in->u.lVal;
        out->vt = vt;
        return S_OK;
    }
    if (vt == VT_BSTR) {
        HRESULT hr = var_to_bstr(in, (flags & (VARIANT_ALPHABOOL | VARIANT_LOCALBOOL)) != 0, 0, &out->u.bstrVal);
        if (SUCCEEDED(hr)) out->vt = VT_BSTR;
        return hr;
    }
    if (from == VT_BSTR && vt == VT_BOOL) {
        VARIANT_BOOL b;
        if (bool_word(in->u.bstrVal, &b)) { out->u.boolVal = b; out->vt = VT_BOOL; return S_OK; }
    }
    if (from == VT_BSTR && vt == VT_DATE) {
        char *s = wide_to_ascii_dup(in->u.bstrVal);
        double d;
        int ok = s && date_from_str(s, &d);
        free(s);
        if (!ok) return DISP_E_TYPEMISMATCH;
        out->u.date = d;
        out->vt = VT_DATE;
        return S_OK;
    }
    long double x;
    HRESULT hr = var_to_ld(in, &x);
    if (FAILED(hr)) return hr;
    if (vt == VT_DECIMAL && from == VT_CY) {
        /* exact: the currency value with scale 4 */
        DECIMAL d;
        memset(&d, 0, sizeof d);
        LONGLONG c = in->u.cyVal;
        d.sign = c < 0 ? 0x80 : 0;
        d.Lo64 = c < 0 ? 0 - (ULONGLONG)c : (ULONGLONG)c;
        d.scale = 4;
        memcpy(out, &d, sizeof d);
        out->vt = VT_DECIMAL;
        return S_OK;
    }
    return ld_to_var(x, vt, out);
}

#define LOCALE_USER_DEFAULT 0x400

API HRESULT VariantChangeTypeEx(VARIANT *dst, const VARIANT *src, LCID lcid, USHORT flags, VARTYPE vt)
{
    if (!dst || !src) return E_INVALIDARG;
    HRESULT hr = vt_check(src->vt);
    if (FAILED(hr)) return hr;
    if (FAILED(vt_check(vt))) return DISP_E_BADVARTYPE;
    if (vt & VT_BYREF) return DISP_E_TYPEMISMATCH;
    VARIANT ind, res;
    const VARIANT *s = src;
    ind.vt = VT_EMPTY;
    if (src->vt & VT_BYREF) {
        if (FAILED(hr = VariantCopyInd(&ind, src))) return hr;
        s = &ind;
    }
    hr = var_change(&res, s, lcid, flags, vt);
    if (s == &ind) VariantClear(&ind);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = VariantClear(dst))) { VariantClear(&res); return hr; }
    *dst = res;
    return S_OK;
}

API HRESULT VariantChangeType(VARIANT *dst, const VARIANT *src, USHORT flags, VARTYPE vt)
{
    return VariantChangeTypeEx(dst, src, LOCALE_USER_DEFAULT, flags, vt);
}

/* VarXxxFromStr: the string is NUL-terminated (not necessarily a BSTR). */
static HRESULT from_str(const WCHAR *s, VARTYPE vt, VARIANT *res)
{
    VARIANT in;
    if (!s) return DISP_E_TYPEMISMATCH;
    in.vt = VT_BSTR;
    in.u.bstrVal = (BSTR)s;
    return var_change(res, &in, 0, 0, vt);
}
#define VAR_FROM_STR(name, T, vt, member) \
    API HRESULT name(const WCHAR *s, LCID lcid, ULONG flags, T *out) \
    { \
        VARIANT r; (void)lcid; (void)flags; \
        if (!out) return E_INVALIDARG; \
        HRESULT hr = from_str(s, vt, &r); \
        if (SUCCEEDED(hr)) *out = r.u.member; \
        return hr; \
    }
VAR_FROM_STR(VarI2FromStr, SHORT, VT_I2, iVal)
VAR_FROM_STR(VarI4FromStr, LONG, VT_I4, lVal)
VAR_FROM_STR(VarI8FromStr, LONGLONG, VT_I8, llVal)
VAR_FROM_STR(VarUI1FromStr, BYTE, VT_UI1, bVal)
VAR_FROM_STR(VarUI2FromStr, USHORT, VT_UI2, uiVal)
VAR_FROM_STR(VarUI4FromStr, ULONG, VT_UI4, ulVal)
VAR_FROM_STR(VarUI8FromStr, ULONGLONG, VT_UI8, ullVal)
VAR_FROM_STR(VarR4FromStr, float, VT_R4, fltVal)
VAR_FROM_STR(VarR8FromStr, double, VT_R8, dblVal)
VAR_FROM_STR(VarBoolFromStr, VARIANT_BOOL, VT_BOOL, boolVal)
VAR_FROM_STR(VarDateFromStr, DATE, VT_DATE, date)
VAR_FROM_STR(VarCyFromStr, LONGLONG, VT_CY, cyVal)

#define VAR_BSTR_FROM(name, T, vtype, member) \
    API HRESULT name(T in, LCID lcid, ULONG flags, BSTR *out) \
    { \
        VARIANT v; (void)lcid; \
        if (!out) return E_INVALIDARG; \
        v.vt = vtype; v.u.llVal = 0; v.u.member = in; \
        return var_to_bstr(&v, 1, flags, out); \
    }
VAR_BSTR_FROM(VarBstrFromI2, SHORT, VT_I2, iVal)
VAR_BSTR_FROM(VarBstrFromI4, LONG, VT_I4, lVal)
VAR_BSTR_FROM(VarBstrFromI8, LONGLONG, VT_I8, llVal)
VAR_BSTR_FROM(VarBstrFromUI1, BYTE, VT_UI1, bVal)
VAR_BSTR_FROM(VarBstrFromUI2, USHORT, VT_UI2, uiVal)
VAR_BSTR_FROM(VarBstrFromUI4, ULONG, VT_UI4, ulVal)
VAR_BSTR_FROM(VarBstrFromUI8, ULONGLONG, VT_UI8, ullVal)
VAR_BSTR_FROM(VarBstrFromR4, float, VT_R4, fltVal)
VAR_BSTR_FROM(VarBstrFromR8, double, VT_R8, dblVal)
VAR_BSTR_FROM(VarBstrFromDate, DATE, VT_DATE, date)
VAR_BSTR_FROM(VarBstrFromCy, LONGLONG, VT_CY, cyVal)
/* VarBstrFromBool always spells the value ("True" / "False"). */
VAR_BSTR_FROM(VarBstrFromBool, VARIANT_BOOL, VT_BOOL, boolVal)

#define VARCMP_LT   0
#define VARCMP_EQ   1
#define VARCMP_GT   2
#define NORM_IGNORECASE 0x1

static unsigned fold(unsigned c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    return c;
}

/* lcid 0 compares bytes. Otherwise this approximates a linguistic compare:
 * case-insensitive first, then lower case before upper case. */
API HRESULT VarBstrCmp(BSTR l, BSTR r, LCID lcid, ULONG flags)
{
    UINT ll = SysStringLen(l), lr = SysStringLen(r);
    if (!l) return (!r || !lr) ? VARCMP_EQ : VARCMP_LT;
    if (!r) return !ll ? VARCMP_EQ : VARCMP_GT;
    if (!lcid) {
        UINT bl = SysStringByteLen(l), br = SysStringByteLen(r);
        int c = memcmp(l, r, bl < br ? bl : br);
        if (!c) return bl == br ? VARCMP_EQ : bl < br ? VARCMP_LT : VARCMP_GT;
        return c < 0 ? VARCMP_LT : VARCMP_GT;
    }
    UINT n = ll < lr ? ll : lr;
    for (UINT i = 0; i < n; i++) {
        unsigned a = fold(l[i]), b = fold(r[i]);
        if (a != b) return a < b ? VARCMP_LT : VARCMP_GT;
    }
    if (ll != lr) return ll < lr ? VARCMP_LT : VARCMP_GT;
    if (!(flags & NORM_IGNORECASE))
        for (UINT i = 0; i < n; i++)
            if (l[i] != r[i]) return l[i] > r[i] ? VARCMP_LT : VARCMP_GT;   /* 'a' sorts before 'A' */
    return VARCMP_EQ;
}

API HRESULT VarBstrCat(BSTR l, BSTR r, BSTR *out)
{
    if (!out) return E_INVALIDARG;
    UINT bl = SysStringByteLen(l), br = SysStringByteLen(r);
    BSTR b = bstr_alloc(NULL, (size_t)bl + br);
    if (!b) return E_OUTOFMEMORY;
    if (bl) memcpy(b, l, bl);
    if (br) memcpy((char *)b + bl, r, br);
    *out = b;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* SAFEARRAY                                                           */
/* ------------------------------------------------------------------ */

#define FADF_AUTO         0x0001
#define FADF_STATIC       0x0002
#define FADF_EMBEDDED     0x0004
#define FADF_FIXEDSIZE    0x0010
#define FADF_RECORD       0x0020
#define FADF_HAVEIID      0x0040
#define FADF_HAVEVARTYPE  0x0080
#define FADF_BSTR         0x0100
#define FADF_UNKNOWN      0x0200
#define FADF_DISPATCH     0x0400
#define FADF_VARIANT      0x0800

/* A descriptor is allocated with 16 hidden bytes in front of it: the IID
 * (FADF_HAVEIID) at -16, the IRecordInfo pointer (FADF_RECORD) at -8, or
 * the VARTYPE as a DWORD (FADF_HAVEVARTYPE) at -4.
 *
 * rgsabound is stored in reverse order: rgsabound[0] is the right-most
 * dimension and dimension 1 (the left-most) is rgsabound[cDims - 1]. Index
 * vectors list dimension 1 first, and dimension 1 varies fastest. */
#define SA_HIDDEN 16
static DWORD *sa_vt_slot(SAFEARRAY *sa) { return (DWORD *)sa - 1; }
static IUnknown **sa_rec_slot(SAFEARRAY *sa) { return (IUnknown **)sa - 1; }
static GUID *sa_iid_slot(SAFEARRAY *sa) { return (GUID *)sa - 1; }

static size_t sa_count(const SAFEARRAY *sa)
{
    size_t n = 1;
    for (USHORT i = 0; i < sa->cDims; i++) n *= sa->rgsabound[i].cElements;
    return n;
}

API HRESULT SafeArrayAllocDescriptor(UINT dims, SAFEARRAY **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!dims || dims > 65535) return E_INVALIDARG;
    uint8_t *p = lsiw_heap_alloc(SA_HIDDEN + offsetof(SAFEARRAY, rgsabound) + dims * sizeof(SAFEARRAYBOUND), 1);
    if (!p) return E_OUTOFMEMORY;
    SAFEARRAY *sa = (SAFEARRAY *)(p + SA_HIDDEN);
    sa->cDims = (USHORT)dims;
    *out = sa;
    return S_OK;
}

static ULONG sa_elem_size(VARTYPE vt)
{
    switch (vt) {
    case VT_I1: case VT_UI1: case VT_I2: case VT_UI2: case VT_BOOL: case VT_I4: case VT_UI4: case VT_R4:
    case VT_ERROR: case VT_INT: case VT_UINT: case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
    case VT_BSTR: case VT_UNKNOWN: case VT_DISPATCH: case VT_DECIMAL: case VT_VARIANT:
        return (ULONG)vt_size(vt);
    default: return 0;
    }
}

API HRESULT SafeArrayAllocDescriptorEx(VARTYPE vt, UINT dims, SAFEARRAY **out)
{
    if (vt != VT_RECORD && !sa_elem_size(vt)) return E_INVALIDARG;
    HRESULT hr = SafeArrayAllocDescriptor(dims, out);
    if (FAILED(hr)) return hr;
    SAFEARRAY *sa = *out;
    switch (vt) {
    case VT_RECORD: sa->fFeatures = FADF_RECORD; break;
    case VT_UNKNOWN: sa->fFeatures = FADF_HAVEIID | FADF_UNKNOWN; *sa_iid_slot(sa) = IID_IUnknown_; break;
    case VT_DISPATCH: sa->fFeatures = FADF_HAVEIID | FADF_DISPATCH; *sa_iid_slot(sa) = IID_IDispatch_; break;
    default:
        sa->fFeatures = FADF_HAVEVARTYPE;
        if (vt == VT_BSTR) sa->fFeatures |= FADF_BSTR;
        if (vt == VT_VARIANT) sa->fFeatures |= FADF_VARIANT;
        *sa_vt_slot(sa) = vt;
        break;
    }
    sa->cbElements = vt == VT_RECORD ? 0 : sa_elem_size(vt);
    return S_OK;
}

API HRESULT SafeArrayAllocData(SAFEARRAY *sa)
{
    if (!sa) return E_INVALIDARG;
    size_t n = sa_count(sa) * sa->cbElements;
    if (!(sa->pvData = lsiw_heap_alloc(n, 1))) return E_OUTOFMEMORY;
    return S_OK;
}

/* Release the resources each element owns. */
static void sa_clear_elems(SAFEARRAY *sa, size_t from, size_t to)
{
    if (!sa->pvData) return;
    for (size_t i = from; i < to; i++) {
        void *e = (char *)sa->pvData + i * sa->cbElements;
        if (sa->fFeatures & FADF_BSTR) { SysFreeString(*(BSTR *)e); *(BSTR *)e = NULL; }
        else if (sa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) { unk_release(*(IUnknown **)e); *(IUnknown **)e = NULL; }
        else if (sa->fFeatures & FADF_VARIANT) VariantClear(e);
        else if ((sa->fFeatures & FADF_RECORD) && *sa_rec_slot(sa))
            RECINFO(*sa_rec_slot(sa))->RecordClear(*sa_rec_slot(sa), e);
    }
}

API HRESULT SafeArrayDestroyData(SAFEARRAY *sa)
{
    if (!sa) return E_INVALIDARG;
    if (sa->cLocks) return DISP_E_ARRAYISLOCKED;
    sa_clear_elems(sa, 0, sa_count(sa));
    if (!sa->pvData) return S_OK;
    if (sa->fFeatures & FADF_STATIC) { memset(sa->pvData, 0, sa_count(sa) * sa->cbElements); return S_OK; }
    if (!(sa->fFeatures & (FADF_AUTO | FADF_EMBEDDED))) { lsiw_heap_free(sa->pvData); sa->pvData = NULL; }
    return S_OK;
}

API HRESULT SafeArrayDestroyDescriptor(SAFEARRAY *sa)
{
    if (!sa) return E_INVALIDARG;
    if (sa->cLocks) return DISP_E_ARRAYISLOCKED;
    if (sa->fFeatures & FADF_RECORD) unk_release(*sa_rec_slot(sa));
    lsiw_heap_free((uint8_t *)sa - SA_HIDDEN);
    return S_OK;
}

API HRESULT SafeArrayDestroy(SAFEARRAY *sa)
{
    if (!sa) return S_OK;
    if (sa->cLocks) return DISP_E_ARRAYISLOCKED;
    HRESULT hr = SafeArrayDestroyData(sa);
    if (FAILED(hr)) return hr;
    return SafeArrayDestroyDescriptor(sa);
}

API SAFEARRAY *SafeArrayCreate(VARTYPE vt, UINT dims, SAFEARRAYBOUND *bounds)
{
    SAFEARRAY *sa;
    if (!bounds || !dims || !sa_elem_size(vt)) return NULL;
    if (FAILED(SafeArrayAllocDescriptorEx(vt, dims, &sa))) return NULL;
    for (UINT i = 0; i < dims; i++) sa->rgsabound[i] = bounds[dims - 1 - i];
    if (FAILED(SafeArrayAllocData(sa))) { SafeArrayDestroyDescriptor(sa); return NULL; }
    return sa;
}

static SAFEARRAY *sa_vector(VARTYPE vt, LONG lbound, ULONG count)
{
    SAFEARRAYBOUND b = { count, lbound };
    return SafeArrayCreate(vt, 1, &b);
}
API SAFEARRAY *SafeArrayCreateVector(VARTYPE vt, LONG lbound, ULONG count) { return sa_vector(vt, lbound, count); }

API UINT SafeArrayGetDim(SAFEARRAY *sa) { return sa ? sa->cDims : 0; }
API UINT SafeArrayGetElemsize(SAFEARRAY *sa) { return sa ? sa->cbElements : 0; }

API HRESULT SafeArrayGetLBound(SAFEARRAY *sa, UINT dim, LONG *out)
{
    if (!sa || !out) return E_INVALIDARG;
    if (dim < 1 || dim > sa->cDims) return DISP_E_BADINDEX;
    *out = sa->rgsabound[sa->cDims - dim].lLbound;
    return S_OK;
}
API HRESULT SafeArrayGetUBound(SAFEARRAY *sa, UINT dim, LONG *out)
{
    if (!sa || !out) return E_INVALIDARG;
    if (dim < 1 || dim > sa->cDims) return DISP_E_BADINDEX;
    const SAFEARRAYBOUND *b = &sa->rgsabound[sa->cDims - dim];
    *out = (LONG)((int64_t)b->lLbound + b->cElements - 1);
    return S_OK;
}

API HRESULT SafeArrayLock(SAFEARRAY *sa)
{
    if (!sa) return E_INVALIDARG;
    if (__atomic_add_fetch(&sa->cLocks, 1, __ATOMIC_SEQ_CST) > 0xFFFF) {
        __atomic_sub_fetch(&sa->cLocks, 1, __ATOMIC_SEQ_CST);
        return E_UNEXPECTED;
    }
    return S_OK;
}
API HRESULT SafeArrayUnlock(SAFEARRAY *sa)
{
    if (!sa) return E_INVALIDARG;
    ULONG v = __atomic_load_n(&sa->cLocks, __ATOMIC_SEQ_CST);
    do { if (!v) return E_UNEXPECTED; }
    while (!__atomic_compare_exchange_n(&sa->cLocks, &v, v - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return S_OK;
}
API HRESULT SafeArrayAccessData(SAFEARRAY *sa, void **out)
{
    if (!sa || !out) return E_INVALIDARG;
    HRESULT hr = SafeArrayLock(sa);
    *out = SUCCEEDED(hr) ? sa->pvData : NULL;
    return hr;
}
API HRESULT SafeArrayUnaccessData(SAFEARRAY *sa) { return SafeArrayUnlock(sa); }

API HRESULT SafeArrayPtrOfIndex(SAFEARRAY *sa, LONG *idx, void **out)
{
    if (!sa || !idx || !out) return E_INVALIDARG;
    size_t cell = 0, mult = 1;
    for (USHORT d = 0; d < sa->cDims; d++) {
        const SAFEARRAYBOUND *b = &sa->rgsabound[sa->cDims - 1 - d];
        int64_t off = (int64_t)idx[d] - b->lLbound;
        if (off < 0 || off >= (int64_t)b->cElements) return DISP_E_BADINDEX;
        cell += (size_t)off * mult;
        mult *= b->cElements;
    }
    if (!sa->pvData) return E_INVALIDARG;
    *out = (char *)sa->pvData + cell * sa->cbElements;
    return S_OK;
}

/* Elements are copied out: a new BSTR, an AddRef'd interface, a copied
 * VARIANT (the destination is treated as uninitialised). */
API HRESULT SafeArrayGetElement(SAFEARRAY *sa, LONG *idx, void *pv)
{
    void *e;
    if (!sa || !idx || !pv) return E_INVALIDARG;
    HRESULT hr = SafeArrayLock(sa);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = SafeArrayPtrOfIndex(sa, idx, &e))) {
        if (sa->fFeatures & FADF_BSTR) {
            BSTR b = *(BSTR *)e;
            *(BSTR *)pv = bstr_dup(b);
            if (b && !*(BSTR *)pv) hr = E_OUTOFMEMORY;
        } else if (sa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
            *(IUnknown **)pv = *(IUnknown **)e;
            unk_addref(*(IUnknown **)pv);
        } else if (sa->fFeatures & FADF_VARIANT) {
            ((VARIANT *)pv)->vt = VT_EMPTY;
            hr = VariantCopy(pv, e);
        } else if ((sa->fFeatures & FADF_RECORD) && *sa_rec_slot(sa)) {
            hr = RECINFO(*sa_rec_slot(sa))->RecordCopy(*sa_rec_slot(sa), e, pv);
        } else {
            memcpy(pv, e, sa->cbElements);
        }
    }
    SafeArrayUnlock(sa);
    return hr;
}

/* For BSTR / interface arrays pv is the value itself, not a pointer to it. */
API HRESULT SafeArrayPutElement(SAFEARRAY *sa, LONG *idx, void *pv)
{
    void *e;
    if (!sa || !idx) return E_INVALIDARG;
    if (!pv && !(sa->fFeatures & (FADF_BSTR | FADF_UNKNOWN | FADF_DISPATCH))) return E_INVALIDARG;
    HRESULT hr = SafeArrayLock(sa);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = SafeArrayPtrOfIndex(sa, idx, &e))) {
        if (sa->fFeatures & FADF_BSTR) {
            BSTR n = bstr_dup((BSTR)pv);
            if (pv && !n) hr = E_OUTOFMEMORY;
            else { SysFreeString(*(BSTR *)e); *(BSTR *)e = n; }
        } else if (sa->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
            unk_addref(pv);
            unk_release(*(IUnknown **)e);
            *(IUnknown **)e = pv;
        } else if (sa->fFeatures & FADF_VARIANT) {
            hr = VariantCopy(e, pv);
        } else if ((sa->fFeatures & FADF_RECORD) && *sa_rec_slot(sa)) {
            hr = RECINFO(*sa_rec_slot(sa))->RecordCopy(*sa_rec_slot(sa), pv, e);
        } else {
            memcpy(e, pv, sa->cbElements);
        }
    }
    SafeArrayUnlock(sa);
    return hr;
}

static HRESULT sa_copy_data(SAFEARRAY *src, SAFEARRAY *dst)
{
    size_t n = sa_count(src);
    HRESULT hr = S_OK;
    for (size_t i = 0; i < n && SUCCEEDED(hr); i++) {
        void *s = (char *)src->pvData + i * src->cbElements, *d = (char *)dst->pvData + i * dst->cbElements;
        if (src->fFeatures & FADF_BSTR) {
            *(BSTR *)d = bstr_dup(*(BSTR *)s);
            if (*(BSTR *)s && !*(BSTR *)d) hr = E_OUTOFMEMORY;
        } else if (src->fFeatures & (FADF_UNKNOWN | FADF_DISPATCH)) {
            *(IUnknown **)d = *(IUnknown **)s;
            unk_addref(*(IUnknown **)d);
        } else if (src->fFeatures & FADF_VARIANT) {
            ((VARIANT *)d)->vt = VT_EMPTY;
            hr = VariantCopy(d, s);
        } else if ((src->fFeatures & FADF_RECORD) && *sa_rec_slot(src)) {
            hr = RECINFO(*sa_rec_slot(src))->RecordCopy(*sa_rec_slot(src), s, d);
        } else {
            memcpy(d, s, src->cbElements);
        }
    }
    return hr;
}

API HRESULT SafeArrayCopy(SAFEARRAY *sa, SAFEARRAY **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!sa) return S_OK;
    SAFEARRAY *n;
    HRESULT hr = SafeArrayAllocDescriptor(sa->cDims, &n);
    if (FAILED(hr)) return hr;
    memcpy((uint8_t *)n - SA_HIDDEN, (uint8_t *)sa - SA_HIDDEN, SA_HIDDEN);
    n->fFeatures = sa->fFeatures & ~(FADF_AUTO | FADF_STATIC | FADF_EMBEDDED);
    n->cbElements = sa->cbElements;
    memcpy(n->rgsabound, sa->rgsabound, sa->cDims * sizeof(SAFEARRAYBOUND));
    if (n->fFeatures & FADF_RECORD) unk_addref(*sa_rec_slot(n));
    if (sa->pvData) {
        if (FAILED(hr = SafeArrayAllocData(n)) || FAILED(hr = sa_copy_data(sa, n))) {
            SafeArrayDestroy(n);
            return hr;
        }
    }
    *out = n;
    return S_OK;
}

/* Only the right-most dimension (rgsabound[0]) can change, so the data
 * just grows or shrinks at its end. */
API HRESULT SafeArrayRedim(SAFEARRAY *sa, SAFEARRAYBOUND *nb)
{
    if (!sa || !nb) return E_INVALIDARG;
    if (sa->cLocks || (sa->fFeatures & FADF_FIXEDSIZE)) return DISP_E_ARRAYISLOCKED;
    size_t inner = 1;
    for (USHORT i = 1; i < sa->cDims; i++) inner *= sa->rgsabound[i].cElements;
    size_t oldn = inner * sa->rgsabound[0].cElements, newn = inner * nb->cElements;
    if (newn < oldn) sa_clear_elems(sa, newn, oldn);
    void *p = sa->pvData ? lsiw_heap_realloc(sa->pvData, newn * sa->cbElements) : lsiw_heap_alloc(newn * sa->cbElements, 1);
    if (!p && (newn * sa->cbElements) != 0) return E_OUTOFMEMORY;
    if (p && newn > oldn && sa->pvData) memset((char *)p + oldn * sa->cbElements, 0, (newn - oldn) * sa->cbElements);
    sa->pvData = p;
    sa->rgsabound[0] = *nb;
    return S_OK;
}

API HRESULT SafeArrayGetVartype(SAFEARRAY *sa, VARTYPE *vt)
{
    if (!sa || !vt) return E_INVALIDARG;
    if (sa->fFeatures & FADF_RECORD) *vt = VT_RECORD;
    else if (sa->fFeatures & FADF_HAVEIID) *vt = (sa->fFeatures & FADF_DISPATCH) ? VT_DISPATCH : VT_UNKNOWN;
    else if (sa->fFeatures & FADF_HAVEVARTYPE) *vt = (VARTYPE)*sa_vt_slot(sa);
    else return E_INVALIDARG;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* IDispatch helpers, type libraries, pictures                         */
/* ------------------------------------------------------------------ */

/* Positional arguments are stored last-first in rgvarg; a named argument
 * whose DISPID equals the position takes precedence. */
API HRESULT DispGetParam(DISPPARAMS *dp, UINT pos, VARTYPE vt, VARIANT *res, UINT *argerr)
{
    if (!dp || !res) return E_INVALIDARG;
    int idx = -1;
    for (UINT i = 0; i < dp->cNamedArgs; i++)
        if (dp->rgdispidNamedArgs && dp->rgdispidNamedArgs[i] == (LONG)pos) { idx = (int)i; break; }
    if (idx < 0) {
        if (pos >= dp->cArgs - dp->cNamedArgs) return DISP_E_PARAMNOTFOUND;
        idx = (int)(dp->cArgs - 1 - pos);
    }
    res->vt = VT_EMPTY;
    HRESULT hr = VariantChangeType(res, &dp->rgvarg[idx], 0, vt);
    if ((hr == DISP_E_TYPEMISMATCH || hr == DISP_E_OVERFLOW) && argerr) *argerr = (UINT)idx;
    return hr;
}

/* Type libraries cannot be loaded: there is no typelib reader. */
API HRESULT LoadTypeLib(const WCHAR *file, void **lib) { (void)file; if (lib) *lib = NULL; return TYPE_E_CANTLOADLIBRARY; }
API HRESULT LoadTypeLibEx(const WCHAR *file, int kind, void **lib) { (void)kind; return LoadTypeLib(file, lib); }
API HRESULT LoadRegTypeLib(const GUID *g, WORD maj, WORD min, LCID lcid, void **lib)
{
    (void)g; (void)maj; (void)min; (void)lcid;
    if (lib) *lib = NULL;
    return TYPE_E_LIBNOTREGISTERED;
}
API HRESULT RegisterTypeLib(void *lib, const WCHAR *path, const WCHAR *help)
{
    (void)path; (void)help;
    return lib ? TYPE_E_CANTLOADLIBRARY : E_INVALIDARG;
}
API HRESULT UnRegisterTypeLib(const GUID *g, WORD maj, WORD min, LCID lcid, int syskind)
{
    (void)g; (void)maj; (void)min; (void)lcid; (void)syskind;
    return TYPE_E_CANTLOADLIBRARY;
}
/* No running object table exists, so nothing is ever active. */
API HRESULT GetActiveObject(const CLSID *clsid, void *reserved, IUnknown **out)
{
    (void)clsid; (void)reserved;
    if (out) *out = NULL;
    return MK_E_UNAVAILABLE;
}
API HRESULT CreateDispTypeInfo(void *idata, LCID lcid, void **info)
{
    (void)idata; (void)lcid;
    if (info) *info = NULL;
    return E_NOTIMPL;
}
API HRESULT CreateStdDispatch(IUnknown *outer, void *obj, void *info, IUnknown **out)
{
    (void)outer; (void)obj; (void)info;
    if (out) *out = NULL;
    return E_NOTIMPL;
}
API HRESULT OleLoadPicture(IUnknown *stm, LONG size, BOOL runmode, const IID *riid, void **out)
{
    (void)stm; (void)size; (void)runmode; (void)riid;
    if (out) *out = NULL;
    return E_NOTIMPL;   /* no image decoders */
}

/* ------------------------------------------------------------------ */
/* rpcrt4: UUIDs                                                       */
/* ------------------------------------------------------------------ */

#define RPC_S_OK                   0
#define RPC_S_OUT_OF_MEMORY        14
#define RPC_S_INVALID_ARG          87
#define RPC_S_INVALID_STRING_UUID  1705
#define RPC_S_UUID_LOCAL_ONLY      1824

API LONG UuidCreate(GUID *u)
{
    if (!u) return RPC_S_INVALID_ARG;
    random_guid(u);
    return RPC_S_OK;
}

API LONG UuidCreateNil(GUID *u)
{
    if (!u) return RPC_S_INVALID_ARG;
    *u = GUID_NULL_;
    return RPC_S_OK;
}

/* Node id: the first non-loopback network interface's MAC address, or a
 * random multicast id (then RPC_S_UUID_LOCAL_ONLY, as on Windows). */
static int read_mac(uint8_t node[6])
{
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
        char path[300], buf[64];
        snprintf(path, sizeof path, "/sys/class/net/%s/address", e->d_name);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        unsigned m[6];
        if (fgets(buf, sizeof buf, f) &&
            sscanf(buf, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6 &&
            (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) {
            for (int i = 0; i < 6; i++) node[i] = (uint8_t)m[i];
            found = 1;
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* Version-1 UUID: 60-bit count of 100 ns intervals since 1582-10-15. */
API LONG UuidCreateSequential(GUID *u)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t last;
    static uint16_t clockseq;
    static uint8_t node[6];
    static int have_node = -1;
    if (!u) return RPC_S_INVALID_ARG;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 10000000u + (uint64_t)ts.tv_nsec / 100 + 0x01B21DD213814000ULL;
    pthread_mutex_lock(&lock);
    if (have_node < 0) {
        have_node = read_mac(node);
        if (!have_node) { fill_random(node, 6); node[0] |= 0x01; }
        fill_random(&clockseq, sizeof clockseq);
    }
    if (t <= last) t = last + 1;   /* strictly increasing within the process */
    last = t;
    uint16_t cs = clockseq;
    pthread_mutex_unlock(&lock);
    u->Data1 = (DWORD)t;
    u->Data2 = (WORD)(t >> 32);
    u->Data3 = (WORD)(((t >> 48) & 0x0FFF) | 0x1000);
    u->Data4[0] = (BYTE)(((cs >> 8) & 0x3F) | 0x80);
    u->Data4[1] = (BYTE)cs;
    memcpy(u->Data4 + 2, node, 6);
    return have_node ? RPC_S_OK : RPC_S_UUID_LOCAL_ONLY;
}

/* Strings from UuidToString are freed with RpcStringFree. */
API LONG UuidToStringA(const GUID *u, char **out)
{
    if (!out) return RPC_S_INVALID_ARG;
    if (!(*out = lsiw_heap_alloc(37, 0))) return RPC_S_OUT_OF_MEMORY;
    guid_fmt(u ? u : &GUID_NULL_, *out, 0);
    return RPC_S_OK;
}
API LONG UuidToStringW(const GUID *u, WCHAR **out)
{
    char buf[40];
    if (!out) return RPC_S_INVALID_ARG;
    if (!(*out = lsiw_heap_alloc(37 * sizeof(WCHAR), 0))) return RPC_S_OUT_OF_MEMORY;
    guid_fmt(u ? u : &GUID_NULL_, buf, 0);
    wdup_ascii(buf, *out);
    return RPC_S_OK;
}
API LONG UuidFromStringA(const char *s, GUID *u)
{
    if (!u) return RPC_S_INVALID_ARG;
    if (!s) { *u = GUID_NULL_; return RPC_S_OK; }
    if (strlen(s) != 36 || !guid_parse36(NULL, s, u)) return RPC_S_INVALID_STRING_UUID;
    return RPC_S_OK;
}
API LONG UuidFromStringW(const WCHAR *s, GUID *u)
{
    if (!u) return RPC_S_INVALID_ARG;
    if (!s) { *u = GUID_NULL_; return RPC_S_OK; }
    if (lsiw_wcslen(s) != 36 || !guid_parse36(s, NULL, u)) return RPC_S_INVALID_STRING_UUID;
    return RPC_S_OK;
}
API LONG RpcStringFreeA(char **s) { if (!s) return RPC_S_INVALID_ARG; lsiw_heap_free(*s); *s = NULL; return RPC_S_OK; }
API LONG RpcStringFreeW(WCHAR **s) { if (!s) return RPC_S_INVALID_ARG; lsiw_heap_free(*s); *s = NULL; return RPC_S_OK; }

/* A NULL UUID pointer means the nil UUID. */
API int UuidCompare(const GUID *a, const GUID *b, LONG *status)
{
    if (status) *status = RPC_S_OK;
    if (!a) a = &GUID_NULL_;
    if (!b) b = &GUID_NULL_;
    if (a->Data1 != b->Data1) return a->Data1 < b->Data1 ? -1 : 1;
    if (a->Data2 != b->Data2) return a->Data2 < b->Data2 ? -1 : 1;
    if (a->Data3 != b->Data3) return a->Data3 < b->Data3 ? -1 : 1;
    int c = memcmp(a->Data4, b->Data4, 8);
    return c < 0 ? -1 : c > 0;
}
API int UuidEqual(const GUID *a, const GUID *b, LONG *status) { return UuidCompare(a, b, status) == 0; }
API int UuidIsNil(const GUID *u, LONG *status) { return UuidCompare(u, NULL, status) == 0; }

/* The DCE hash of the 16 bytes. */
API USHORT UuidHash(const GUID *u, LONG *status)
{
    const uint8_t *p = (const uint8_t *)(u ? u : &GUID_NULL_);
    int c0 = 0, c1 = 0;
    if (status) *status = RPC_S_OK;
    for (int i = 0; i < 16; i++) { c0 += p[i]; c1 += c0; }
    int x = -c1 % 255; if (x < 0) x += 255;
    int y = (c1 - c0) % 255; if (y < 0) y += 255;
    return (USHORT)(y * 256 + x);
}

/* Faults that indicate a broken process are not handled by RPC stubs. */
API int I_RpcExceptionFilter(ULONG code)
{
    switch (code) {
    case 0xC0000005: case 0xC0000194: case 0xC00000AA: case 0x80000002: case 0xC0000096:
    case 0xC000001D: case 0x80000003: case 0xC00000FD: case 0xC0000235: case 0xC0000006:
    case 0xC0000420: case 0xC0000409: case 0x80000001: case 0xC00002C9:
        return 0;   /* EXCEPTION_CONTINUE_SEARCH */
    default:
        return 1;   /* EXCEPTION_EXECUTE_HANDLER */
    }
}

API void RpcRaiseException(LONG code)
{
    void (WINAPI *raise)(DWORD, DWORD, DWORD, const ULONG_PTR *) = k32_fn("RaiseException");
    if (raise) raise((DWORD)code, 1 /* EXCEPTION_NONCONTINUABLE */, 0, NULL);
    lsiw_raise((DWORD)code, 1, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* crypt32: base64 / hex strings                                       */
/* ------------------------------------------------------------------ */

#define CRYPT_STRING_BASE64HEADER         0
#define CRYPT_STRING_BASE64               1
#define CRYPT_STRING_BINARY               2
#define CRYPT_STRING_BASE64REQUESTHEADER  3
#define CRYPT_STRING_HEX                  4
#define CRYPT_STRING_HEXASCII             5
#define CRYPT_STRING_BASE64_ANY           6
#define CRYPT_STRING_ANY                  7
#define CRYPT_STRING_HEX_ANY              8
#define CRYPT_STRING_BASE64X509CRLHEADER  9
#define CRYPT_STRING_HEXADDR              10
#define CRYPT_STRING_HEXASCIIADDR         11
#define CRYPT_STRING_HEXRAW               12
#define CRYPT_STRING_NOCRLF               0x40000000u
#define CRYPT_STRING_NOCR                 0x80000000u

/* Growable output buffer. */
struct sbuf { char *p; size_t n, cap; int oom; };
static void sb_put(struct sbuf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = (b->cap ? b->cap * 2 : 256);
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) { b->oom = 1; return; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_str(struct sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(struct sbuf *b, const uint8_t *d, size_t n, const char *eol)
{
    size_t col = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        char q[4] = { b64chars[v >> 18 & 63], b64chars[v >> 12 & 63],
                      i + 1 < n ? b64chars[v >> 6 & 63] : '=', i + 2 < n ? b64chars[v & 63] : '=' };
        sb_put(b, q, 4);
        col += 4;
        if (col == 64 && i + 3 < n) { sb_str(b, eol); col = 0; }
    }
    if (n) sb_str(b, eol);   /* each line, the last one too, ends with the separator */
}

/* Hex layouts: 16 bytes per line, a double space after the eighth;
 * optional "%04x  " offsets and an ASCII column. */
static void hex_encode(struct sbuf *b, const uint8_t *d, size_t n, int addr, int ascii, int raw, const char *eol)
{
    static const char hx[] = "0123456789abcdef";
    if (raw) {
        for (size_t i = 0; i < n; i++) { char c[2] = { hx[d[i] >> 4], hx[d[i] & 15] }; sb_put(b, c, 2); }
        sb_str(b, eol);
        return;
    }
    for (size_t line = 0; line < n; line += 16) {
        char buf[128];
        size_t k = 0, cnt = n - line < 16 ? n - line : 16;
        if (addr) k += (size_t)snprintf(buf, sizeof buf, "%04zx  ", line);
        for (size_t i = 0; i < 16; i++) {
            if (i < cnt) { buf[k++] = hx[d[line + i] >> 4]; buf[k++] = hx[d[line + i] & 15]; }
            else if (ascii) { buf[k++] = ' '; buf[k++] = ' '; }
            else break;
            if (i < 15 && (i + 1 < cnt || ascii)) { buf[k++] = ' '; if (i == 7) buf[k++] = ' '; }
        }
        if (ascii) {
            buf[k++] = ' '; buf[k++] = ' '; buf[k++] = ' ';
            for (size_t i = 0; i < cnt; i++) { uint8_t c = d[line + i]; buf[k++] = c >= 0x20 && c < 0x7F ? (char)c : '.'; }
        }
        sb_put(b, buf, k);
        sb_str(b, eol);
    }
}

static const char *pem_label(DWORD fmt)
{
    switch (fmt) {
    case CRYPT_STRING_BASE64HEADER: return "CERTIFICATE";
    case CRYPT_STRING_BASE64REQUESTHEADER: return "NEW CERTIFICATE REQUEST";
    case CRYPT_STRING_BASE64X509CRLHEADER: return "X509 CRL";
    }
    return NULL;
}

/* Builds the text form; returns 0 with the error set on failure. */
static int bin_to_string(const uint8_t *d, DWORD n, DWORD flags, struct sbuf *b)
{
    DWORD fmt = flags & 0xFFFF;
    const char *eol = (flags & CRYPT_STRING_NOCRLF) ? "" : (flags & CRYPT_STRING_NOCR) ? "\n" : "\r\n";
    const char *label;
    switch (fmt) {
    case CRYPT_STRING_BASE64: b64_encode(b, d, n, eol); break;
    case CRYPT_STRING_BASE64HEADER: case CRYPT_STRING_BASE64REQUESTHEADER: case CRYPT_STRING_BASE64X509CRLHEADER:
        label = pem_label(fmt);
        sb_str(b, "-----BEGIN "); sb_str(b, label); sb_str(b, "-----"); sb_str(b, eol);
        b64_encode(b, d, n, eol);
        sb_str(b, "-----END "); sb_str(b, label); sb_str(b, "-----"); sb_str(b, eol);
        break;
    case CRYPT_STRING_BINARY: sb_put(b, (const char *)d, n); break;
    case CRYPT_STRING_HEX: hex_encode(b, d, n, 0, 0, 0, eol); break;
    case CRYPT_STRING_HEXASCII: hex_encode(b, d, n, 0, 1, 0, eol); break;
    case CRYPT_STRING_HEXADDR: hex_encode(b, d, n, 1, 0, 0, eol); break;
    case CRYPT_STRING_HEXASCIIADDR: hex_encode(b, d, n, 1, 1, 0, eol); break;
    case CRYPT_STRING_HEXRAW: hex_encode(b, d, n, 0, 0, 1, eol); break;
    default: lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0;
    }
    if (b->oom) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    if (!b->p) sb_put(b, "", 0);
    return 1;
}

/* Sizing call (no buffer): the count includes the terminator. Filled
 * buffer: the count excludes it. Binary output has no terminator. */
API BOOL CryptBinaryToStringA(const BYTE *d, DWORD n, DWORD flags, char *out, DWORD *cch)
{
    struct sbuf b = { 0 };
    if (!cch || (!d && n)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!bin_to_string(d, n, flags, &b)) { free(b.p); return FALSE; }
    int binary = (flags & 0xFFFF) == CRYPT_STRING_BINARY;
    DWORD need = (DWORD)b.n + (binary ? 0 : 1);
    BOOL ok = TRUE;
    if (!out) *cch = need;
    else if (*cch < need) { *cch = need; lsiw_set_last_error(ERROR_MORE_DATA); ok = FALSE; }
    else { memcpy(out, b.p, b.n); if (!binary) out[b.n] = 0; *cch = (DWORD)b.n; }
    free(b.p);
    return ok;
}

API BOOL CryptBinaryToStringW(const BYTE *d, DWORD n, DWORD flags, WCHAR *out, DWORD *cch)
{
    struct sbuf b = { 0 };
    if (!cch || (!d && n)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!bin_to_string(d, n, flags, &b)) { free(b.p); return FALSE; }
    int binary = (flags & 0xFFFF) == CRYPT_STRING_BINARY;
    DWORD chars = binary ? (DWORD)(b.n + 1) / 2 : (DWORD)b.n;
    DWORD need = chars + (binary ? 0 : 1);
    BOOL ok = TRUE;
    if (!out) *cch = need;
    else if (*cch < need) { *cch = need; lsiw_set_last_error(ERROR_MORE_DATA); ok = FALSE; }
    else {
        if (binary) memcpy(out, b.p, b.n);
        else { for (size_t i = 0; i < b.n; i++) out[i] = (unsigned char)b.p[i]; out[b.n] = 0; }
        *cch = chars;
    }
    free(b.p);
    return ok;
}

static int b64val(int c)
{
    const char *p = c ? strchr(b64chars, c) : NULL;
    return p ? (int)(p - b64chars) : -1;
}

/* Decodes base64 in s[0..n), ignoring whitespace; out may be NULL. */
static int b64_decode(const char *s, size_t n, uint8_t *out, size_t *outn)
{
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') { pad++; continue; }
        int v = b64val(c);
        if (v < 0 || pad) return 0;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; if (out) out[k] = (uint8_t)(acc >> bits); k++; }
    }
    if (pad > 2 || bits >= 6) return 0;
    *outn = k;
    return 1;
}

static const char *find_str(const char *s, size_t n, const char *needle)
{
    size_t l = strlen(needle);
    for (size_t i = 0; i + l <= n; i++) if (!memcmp(s + i, needle, l)) return s + i;
    return NULL;
}

/* "-----BEGIN <label>-----" ... "-----END <label>-----" */
static int pem_decode(const char *s, size_t n, DWORD fmt, uint8_t *out, size_t *outn, DWORD *skip)
{
    const char *beg = find_str(s, n, "-----BEGIN ");
    if (!beg) return 0;
    const char *lab = beg + 11, *end_s = s + n;
    const char *close = find_str(lab, (size_t)(end_s - lab), "-----");
    if (!close) return 0;
    const char *want = fmt == CRYPT_STRING_BASE64REQUESTHEADER ? "CERTIFICATE REQUEST"
                     : fmt == CRYPT_STRING_BASE64X509CRLHEADER ? "X509 CRL" : NULL;
    if (want) {
        size_t ll = (size_t)(close - lab), wl = strlen(want);
        if (ll < wl || memcmp(close - wl, want, wl)) return 0;   /* accepts "NEW CERTIFICATE REQUEST" too */
    }
    const char *body = close + 5;
    const char *endm = find_str(body, (size_t)(end_s - body), "-----END ");
    if (!endm) return 0;
    if (!b64_decode(body, (size_t)(endm - body), out, outn)) return 0;
    if (skip) *skip = (DWORD)(beg - s);
    return 1;
}

static int hex_decode(const char *s, size_t n, int addr, int ascii, uint8_t *out, size_t *outn)
{
    size_t k = 0, i = 0;
    while (i < n) {
        size_t le = i;
        while (le < n && s[le] != '\n') le++;
        size_t j = i, stop = le;
        if (le > i && s[le - 1] == '\r') stop = le - 1;
        while (j < stop && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j < stop) {
            size_t col0;
            if (addr) {
                size_t a0 = j;
                unsigned long a = 0;
                while (j < stop && hexval((unsigned char)s[j]) >= 0) a = a * 16 + (unsigned long)hexval((unsigned char)s[j++]);
                if (j - a0 < 4 || a != k || j + 2 > stop || s[j] != ' ' || s[j + 1] != ' ') return 0;
                j += 2;
            }
            col0 = j;
            /* the hex area of an ASCII layout is 16 * 3 + 1 columns wide */
            size_t limit = ascii ? (col0 + 49 < stop ? col0 + 49 : stop) : stop;
            int cnt = 0;
            while (j < limit) {
                if (s[j] == ' ' || s[j] == '\t') { j++; continue; }
                int h = hexval((unsigned char)s[j]), l = j + 1 < limit ? hexval((unsigned char)s[j + 1]) : -1;
                if (h < 0 || l < 0) return 0;
                if (out) out[k] = (uint8_t)(h << 4 | l);
                k++; j += 2; cnt++;
                if (ascii && cnt == 16) break;
            }
        }
        i = le + 1;
    }
    *outn = k;
    return 1;
}

/* Decode in format fmt; returns 0 when the text is not in that format. */
static int str_decode(const char *s, size_t n, DWORD fmt, uint8_t *out, size_t *outn, DWORD *skip)
{
    *skip = 0;
    switch (fmt) {
    case CRYPT_STRING_BASE64: return b64_decode(s, n, out, outn);
    case CRYPT_STRING_BASE64HEADER: case CRYPT_STRING_BASE64REQUESTHEADER: case CRYPT_STRING_BASE64X509CRLHEADER:
        return pem_decode(s, n, fmt, out, outn, skip);
    case CRYPT_STRING_BINARY: if (out) memcpy(out, s, n); *outn = n; return 1;
    case CRYPT_STRING_HEX: case CRYPT_STRING_HEXRAW: return hex_decode(s, n, 0, 0, out, outn);
    case CRYPT_STRING_HEXASCII: return hex_decode(s, n, 0, 1, out, outn);
    case CRYPT_STRING_HEXADDR: return hex_decode(s, n, 1, 0, out, outn);
    case CRYPT_STRING_HEXASCIIADDR: return hex_decode(s, n, 1, 1, out, outn);
    }
    return 0;
}

static BOOL string_to_binary(const char *s, size_t n, DWORD flags, BYTE *out, DWORD *cb, DWORD *pskip, DWORD *pflags)
{
    static const DWORD any_b64[] = { CRYPT_STRING_BASE64HEADER, CRYPT_STRING_BASE64 };
    static const DWORD any[] = { CRYPT_STRING_BASE64HEADER, CRYPT_STRING_BASE64, CRYPT_STRING_BINARY };
    static const DWORD any_hex[] = { CRYPT_STRING_HEXADDR, CRYPT_STRING_HEXASCIIADDR, CRYPT_STRING_HEX,
                                     CRYPT_STRING_HEXRAW, CRYPT_STRING_HEXASCII };
    DWORD fmt = flags & 0xFFFF, one[1] = { fmt };
    const DWORD *list = one;
    size_t cnt = 1;
    if (fmt == CRYPT_STRING_BASE64_ANY) { list = any_b64; cnt = 2; }
    else if (fmt == CRYPT_STRING_ANY) { list = any; cnt = 3; }
    else if (fmt == CRYPT_STRING_HEX_ANY) { list = any_hex; cnt = 5; }
    else if (fmt > CRYPT_STRING_HEXRAW) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (size_t i = 0; i < cnt; i++) {
        size_t need;
        DWORD skip;
        if (!str_decode(s, n, list[i], NULL, &need, &skip)) continue;
        if (pskip) *pskip = skip;
        if (pflags) *pflags = list[i];
        if (!out) { *cb = (DWORD)need; return TRUE; }
        if (*cb < need) { *cb = (DWORD)need; lsiw_set_last_error(ERROR_MORE_DATA); return FALSE; }
        str_decode(s, n, list[i], out, &need, &skip);
        *cb = (DWORD)need;
        return TRUE;
    }
    lsiw_set_last_error(ERROR_INVALID_DATA);
    return FALSE;
}

API BOOL CryptStringToBinaryA(const char *s, DWORD cch, DWORD flags, BYTE *out, DWORD *cb, DWORD *skip, DWORD *pflags)
{
    if (!s || !cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return string_to_binary(s, cch ? cch : strlen(s), flags, out, cb, skip, pflags);
}

API BOOL CryptStringToBinaryW(const WCHAR *s, DWORD cch, DWORD flags, BYTE *out, DWORD *cb, DWORD *skip, DWORD *pflags)
{
    if (!s || !cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    size_t n = cch ? cch : lsiw_wcslen(s);
    if ((flags & 0xFFFF) == CRYPT_STRING_BINARY)   /* the raw UTF-16 bytes */
        return string_to_binary((const char *)s, n * 2, flags, out, cb, skip, pflags);
    char *a = malloc(n + 1);
    if (!a) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (size_t i = 0; i < n; i++) a[i] = s[i] < 0x80 ? (char)s[i] : '\x01';
    a[n] = 0;
    BOOL r = string_to_binary(a, n, flags, out, cb, skip, pflags);
    free(a);
    return r;
}

/* ------------------------------------------------------------------ */
/* crypt32: DPAPI                                                      */
/* ------------------------------------------------------------------ */

/* Data protection is not compatible with Windows DPAPI blobs. A random
 * 32-byte per-user master key is kept in <data dir>/dpapi.key (mode 0600).
 * Each blob has a random salt; the encryption and MAC keys are
 * HMAC-SHA256(master, label || salt || SHA-256(entropy)); the payload is
 * XORed with an HMAC-SHA256 counter-mode keystream and the whole blob is
 * authenticated with HMAC-SHA256. A wrong entropy, another user's key or a
 * modified blob fail with ERROR_INVALID_DATA. */

typedef struct { DWORD cbData; BYTE *pbData; } DATA_BLOB;

struct hmac_ctx { struct lsiw_hash in, out; };
static void hmac_init(struct hmac_ctx *c, int alg, const void *key, size_t klen)
{
    size_t bs = lsiw_hash_block_size(alg);
    uint8_t k[128], pad[128];
    memset(k, 0, sizeof k);
    if (klen > bs) lsiw_hash_buf(alg, key, klen, k); else if (klen) memcpy(k, key, klen);
    for (size_t i = 0; i < bs; i++) pad[i] = k[i] ^ 0x36;
    lsiw_hash_init(&c->in, alg);
    lsiw_hash_update(&c->in, pad, bs);
    for (size_t i = 0; i < bs; i++) pad[i] = k[i] ^ 0x5c;
    lsiw_hash_init(&c->out, alg);
    lsiw_hash_update(&c->out, pad, bs);
}
static void hmac_update(struct hmac_ctx *c, const void *d, size_t n) { lsiw_hash_update(&c->in, d, n); }
static size_t hmac_final(struct hmac_ctx *c, uint8_t *out)
{
    uint8_t inner[LSIW_HASH_MAX_DIGEST];
    size_t n = lsiw_hash_final(&c->in, inner);
    lsiw_hash_update(&c->out, inner, n);
    return lsiw_hash_final(&c->out, out);
}

static int dpapi_master_key(uint8_t key[32])
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static int loaded;
    static uint8_t cached[32];
    int ok = 0;
    pthread_mutex_lock(&lock);
    if (loaded) { memcpy(key, cached, 32); pthread_mutex_unlock(&lock); return 1; }
    const char *dir = lsiw_data_dir();
    char *path = dir ? lsiw_strdup_printf("%s/dpapi.key", dir) : NULL;
    if (path) {
        lsiw_mkdirs(dir, 0700);
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd >= 0) {
                ok = read(fd, cached, 32) == 32;   /* a damaged key is never replaced */
                close(fd);
                break;
            }
            if (errno != ENOENT) break;
            /* Create it atomically: write a temporary file, then link it
             * into place so a concurrent creator wins cleanly. */
            char *tmp = lsiw_strdup_printf("%s/.dpapi.key.XXXXXX", dir);
            int tfd = tmp ? mkstemp(tmp) : -1;
            if (tfd < 0) { free(tmp); break; }
            uint8_t fresh[32];
            fill_random(fresh, 32);
            int w = write(tfd, fresh, 32) == 32 && fsync(tfd) == 0;
            close(tfd);
            if (w && link(tmp, path) == 0) { memcpy(cached, fresh, 32); ok = 1; }
            else if (w && errno != EEXIST) {
                int fd2 = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (fd2 >= 0) {
                    ok = write(fd2, fresh, 32) == 32 && fsync(fd2) == 0;
                    close(fd2);
                    if (ok) memcpy(cached, fresh, 32);
                }
            }
            unlink(tmp);
            free(tmp);
        }
        free(path);
    }
    if (ok) { loaded = 1; memcpy(key, cached, 32); }
    pthread_mutex_unlock(&lock);
    return ok;
}

static void derive(const uint8_t master[32], const char *label, const uint8_t *salt, size_t slen,
                   const uint8_t enth[32], uint8_t out[32])
{
    struct hmac_ctx c;
    hmac_init(&c, LSIW_HASH_SHA256, master, 32);
    hmac_update(&c, label, strlen(label));
    hmac_update(&c, salt, slen);
    if (enth) hmac_update(&c, enth, 32);
    hmac_final(&c, out);
}

static void keystream_xor(const uint8_t key[32], uint8_t *d, size_t n)
{
    uint8_t blk[32];
    for (uint64_t i = 0; i * 32 < n; i++) {
        uint8_t ctr[8];
        for (int j = 0; j < 8; j++) ctr[j] = (uint8_t)(i >> (56 - 8 * j));
        lsiw_hmac(LSIW_HASH_SHA256, key, 32, ctr, 8, blk);
        for (size_t j = 0; j < 32 && i * 32 + j < n; j++) d[i * 32 + j] ^= blk[j];
    }
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

#define DPAPI_MAGIC "LSDP"
/* blob: magic[4] version[4] flags[4] salt[16] desc_bytes[4] desc data_len[4] data mac[32] */

API BOOL CryptProtectData(DATA_BLOB *in, const WCHAR *desc, DATA_BLOB *entropy, void *reserved,
                          void *prompt, DWORD flags, DATA_BLOB *out)
{
    uint8_t master[32], salt[16], enth[32], kenc[32], kmac[32];
    (void)reserved; (void)prompt;   /* no UI: prompts are never shown */
    if (!in || !out || (!in->pbData && in->cbData)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    out->cbData = 0;
    out->pbData = NULL;
    if (!dpapi_master_key(master)) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    size_t dlen = desc ? lsiw_wcslen(desc) * 2 : 0;
    size_t total = 4 + 4 + 4 + 16 + 4 + dlen + 4 + in->cbData + 32;
    if (total > 0xFFFFFFFFu) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    uint8_t *b = lsiw_heap_alloc(total, 0);   /* freed by the caller with LocalFree */
    if (!b) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    fill_random(salt, 16);
    lsiw_hash_buf(LSIW_HASH_SHA256, entropy && entropy->pbData ? entropy->pbData : (const BYTE *)"",
                  entropy && entropy->pbData ? entropy->cbData : 0, enth);
    derive(master, "LSIW-DPAPI-ENC", salt, 16, enth, kenc);
    derive(master, "LSIW-DPAPI-MAC", salt, 16, enth, kmac);
    uint8_t *p = b;
    memcpy(p, DPAPI_MAGIC, 4); p += 4;
    put32(p, 1); p += 4;
    put32(p, flags); p += 4;
    memcpy(p, salt, 16); p += 16;
    put32(p, (uint32_t)dlen); p += 4;
    if (dlen) memcpy(p, desc, dlen);
    p += dlen;
    put32(p, in->cbData); p += 4;
    if (in->cbData) memcpy(p, in->pbData, in->cbData);
    keystream_xor(kenc, p, in->cbData);
    p += in->cbData;
    lsiw_hmac(LSIW_HASH_SHA256, kmac, 32, b, (size_t)(p - b), p);
    out->pbData = b;
    out->cbData = (DWORD)total;
    memset(master, 0, sizeof master); memset(kenc, 0, sizeof kenc); memset(kmac, 0, sizeof kmac);
    return TRUE;
}

API BOOL CryptUnprotectData(DATA_BLOB *in, WCHAR **pdesc, DATA_BLOB *entropy, void *reserved,
                            void *prompt, DWORD flags, DATA_BLOB *out)
{
    uint8_t master[32], enth[32], kenc[32], kmac[32], mac[32];
    (void)reserved; (void)prompt; (void)flags;
    if (pdesc) *pdesc = NULL;
    if (!in || !out || !in->pbData) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    out->cbData = 0;
    out->pbData = NULL;
    const uint8_t *b = in->pbData;
    size_t n = in->cbData;
    if (n < 68 || memcmp(b, DPAPI_MAGIC, 4) || get32(b + 4) != 1) goto bad;
    size_t dlen = get32(b + 28);
    if (dlen > n - 68) goto bad;
    size_t clen = get32(b + 32 + dlen);
    if (clen != n - 68 - dlen) goto bad;
    if (!dpapi_master_key(master)) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    lsiw_hash_buf(LSIW_HASH_SHA256, entropy && entropy->pbData ? entropy->pbData : (const BYTE *)"",
                  entropy && entropy->pbData ? entropy->cbData : 0, enth);
    derive(master, "LSIW-DPAPI-ENC", b + 12, 16, enth, kenc);
    derive(master, "LSIW-DPAPI-MAC", b + 12, 16, enth, kmac);
    memset(master, 0, sizeof master);
    lsiw_hmac(LSIW_HASH_SHA256, kmac, 32, b, n - 32, mac);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= mac[i] ^ b[n - 32 + i];
    if (diff) goto bad;
    uint8_t *d = lsiw_heap_alloc(clen, 0);
    if (!d) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (clen) memcpy(d, b + 36 + dlen, clen);
    keystream_xor(kenc, d, clen);
    if (pdesc) {
        WCHAR *w = lsiw_heap_alloc(dlen + 2, 1);
        if (!w) { lsiw_heap_free(d); lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        memcpy(w, b + 32, dlen);
        *pdesc = w;
    }
    out->pbData = d;
    out->cbData = (DWORD)clen;
    return TRUE;
bad:
    lsiw_set_last_error(ERROR_INVALID_DATA);
    return FALSE;
}

/* CryptProtectMemory: a deterministic, length-preserving encryption of a
 * multiple of 16 bytes. It is a 4-round Feistel network over the two
 * halves of the buffer whose round function is HMAC-SHA256 (a wide-block
 * pseudorandom permutation), keyed per process for SAME_PROCESS and from
 * the user's master key for CROSS_PROCESS / SAME_LOGON. */
#define CRYPTPROTECTMEMORY_BLOCK_SIZE    16
#define CRYPTPROTECTMEMORY_SAME_PROCESS  0
#define CRYPTPROTECTMEMORY_CROSS_PROCESS 1
#define CRYPTPROTECTMEMORY_SAME_LOGON    2

static void feistel_round(const uint8_t key[32], int round, const uint8_t *src, uint8_t *dst, size_t h)
{
    uint8_t seed[32], r = (uint8_t)round;
    struct hmac_ctx c;
    hmac_init(&c, LSIW_HASH_SHA256, key, 32);
    hmac_update(&c, &r, 1);
    hmac_update(&c, src, h);
    hmac_final(&c, seed);
    keystream_xor(seed, dst, h);
}

static int memory_key(DWORD flags, uint8_t key[32])
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static int have;
    static uint8_t proc_key[32];
    if (flags == CRYPTPROTECTMEMORY_SAME_PROCESS) {
        pthread_mutex_lock(&lock);
        if (!have) { fill_random(proc_key, 32); have = 1; }
        memcpy(key, proc_key, 32);
        pthread_mutex_unlock(&lock);
        return 1;
    }
    uint8_t master[32], f = (uint8_t)flags;
    if (!dpapi_master_key(master)) return 0;
    derive(master, "LSIW-PROTECTMEMORY", &f, 1, NULL, key);
    memset(master, 0, sizeof master);
    return 1;
}

static BOOL protect_memory(void *data, DWORD n, DWORD flags, int decrypt)
{
    uint8_t key[32];
    if (!data || n % CRYPTPROTECTMEMORY_BLOCK_SIZE || flags > CRYPTPROTECTMEMORY_SAME_LOGON) {
        lsiw_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!n) return TRUE;
    if (!memory_key(flags, key)) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    size_t h = n / 2;
    uint8_t *L = data, *R = L + h;
    for (int i = 0; i < 4; i++) {
        int r = decrypt ? 3 - i : i;
        if (r % 2 == 0) feistel_round(key, r, R, L, h);
        else feistel_round(key, r, L, R, h);
    }
    memset(key, 0, sizeof key);
    return TRUE;
}
API BOOL CryptProtectMemory(void *data, DWORD n, DWORD flags) { return protect_memory(data, n, flags, 0); }
API BOOL CryptUnprotectMemory(void *data, DWORD n, DWORD flags) { return protect_memory(data, n, flags, 1); }

/* ------------------------------------------------------------------ */
/* crypt32: certificate stores                                         */
/* ------------------------------------------------------------------ */

/* Stores open successfully but are always empty: there is no certificate
 * database. */
#define CRYPT_E_NOT_FOUND 0x80092004u
#define CERT_STORE_DELETE_FLAG 0x10
#define STORE_MAGIC 0x53545231u
struct certstore { uint32_t magic; };

static void *new_store(void)
{
    struct certstore *s = calloc(1, sizeof *s);
    if (!s) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    s->magic = STORE_MAGIC;
    return s;
}
API void *CertOpenStore(const char *provider, DWORD enc, void *prov, DWORD flags, const void *para)
{
    (void)provider; (void)enc; (void)prov; (void)para;
    if (flags & CERT_STORE_DELETE_FLAG) { lsiw_set_last_error(0); return NULL; }
    return new_store();
}
API void *CertOpenSystemStoreA(void *prov, const char *name) { (void)prov; (void)name; return new_store(); }
API void *CertOpenSystemStoreW(void *prov, const WCHAR *name) { (void)prov; (void)name; return new_store(); }
API BOOL CertCloseStore(void *store, DWORD flags)
{
    struct certstore *s = store;
    (void)flags;
    if (s && s->magic == STORE_MAGIC) { s->magic = 0; free(s); }
    return TRUE;
}
API const void *CertEnumCertificatesInStore(void *store, const void *prev)
{
    (void)store; (void)prev;
    lsiw_set_last_error(CRYPT_E_NOT_FOUND);
    return NULL;
}
API const void *CertFindCertificateInStore(void *store, DWORD enc, DWORD fflags, DWORD type, const void *para, const void *prev)
{
    (void)store; (void)enc; (void)fflags; (void)type; (void)para; (void)prev;
    lsiw_set_last_error(CRYPT_E_NOT_FOUND);
    return NULL;
}
API BOOL CertFreeCertificateContext(const void *ctx) { (void)ctx; return TRUE; }
API const void *CertDuplicateCertificateContext(const void *ctx) { return ctx; }
API BOOL CertGetCertificateChain(void *engine, const void *cert, void *time, void *store, void *para,
                                 DWORD flags, void *reserved, const void **chain)
{
    (void)engine; (void)cert; (void)time; (void)store; (void)para; (void)flags; (void)reserved;
    if (chain) *chain = NULL;
    lsiw_set_last_error(CRYPT_E_NOT_FOUND);
    return FALSE;
}
API void CertFreeCertificateChain(const void *chain) { (void)chain; }

/* ------------------------------------------------------------------ */
/* bcrypt / bcryptprimitives                                           */
/* ------------------------------------------------------------------ */

#define STATUS_SUCCESS           ((NTSTATUS)0)
#define STATUS_INVALID_HANDLE    ((NTSTATUS)0xC0000008u)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define STATUS_NO_MEMORY         ((NTSTATUS)0xC0000017u)
#define STATUS_BUFFER_TOO_SMALL  ((NTSTATUS)0xC0000023u)
#define STATUS_NOT_SUPPORTED     ((NTSTATUS)0xC00000BBu)
#define STATUS_NOT_FOUND         ((NTSTATUS)0xC0000225u)
#define BCRYPT_ALG_HANDLE_HMAC_FLAG      0x8
#define BCRYPT_HASH_REUSABLE_FLAG        0x20
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG  0x2
#define ALG_RNG   100
#define ALG_MAGIC  0x21474c41u
#define HASH_MAGIC 0x21485348u

struct bc_alg { uint32_t magic; int alg, hmac, reusable; };
struct bc_hash {
    uint32_t magic;
    int alg, hmac, reusable, owned, done;
    void *base;                       /* start of the memory holding this object */
    struct lsiw_hash cur, start, outer;
};
#define HASH_OBJECT_LENGTH ((ULONG)(sizeof(struct bc_hash) + 16))

static const struct { const char *name; int alg; } g_bc_algs[] = {
    { "MD5", LSIW_HASH_MD5 }, { "SHA1", LSIW_HASH_SHA1 }, { "SHA256", LSIW_HASH_SHA256 },
    { "SHA384", LSIW_HASH_SHA384 }, { "SHA512", LSIW_HASH_SHA512 }, { "RNG", ALG_RNG } };

static int wstr_ieq(const WCHAR *w, const char *a)
{
    for (; *w && *a; w++, a++)
        if (fold(*w) != fold((unsigned char)*a)) return 0;
    return !*w && !*a;
}

/* Algorithm handles, including the Windows 10 pseudo-handles. */
static int bc_alg_of(void *h, struct bc_alg *out)
{
    static const int pseudo[] = { LSIW_HASH_MD5, LSIW_HASH_SHA1, LSIW_HASH_SHA256, LSIW_HASH_SHA384, LSIW_HASH_SHA512 };
    uintptr_t v = (uintptr_t)h;
    memset(out, 0, sizeof *out);
    if (v >= 0x21 && v <= 0x61 && (v & 0xF) == 1) { out->alg = pseudo[(v - 0x21) >> 4]; return 1; }
    if (v == 0x81) { out->alg = ALG_RNG; return 1; }
    if (v >= 0x91 && v <= 0xd1 && (v & 0xF) == 1) { out->alg = pseudo[(v - 0x91) >> 4]; out->hmac = 1; return 1; }
    if (v < 0x10000) return 0;
    const struct bc_alg *a = h;
    if (a->magic != ALG_MAGIC) return 0;
    *out = *a;
    return 1;
}

API NTSTATUS BCryptOpenAlgorithmProvider(void **ph, const WCHAR *id, const WCHAR *impl, ULONG flags)
{
    if (!ph || !id) return STATUS_INVALID_PARAMETER;
    *ph = NULL;
    if (impl && !wstr_ieq(impl, "Microsoft Primitive Provider")) return STATUS_NOT_FOUND;
    for (size_t i = 0; i < sizeof g_bc_algs / sizeof g_bc_algs[0]; i++) {
        if (!wstr_ieq(id, g_bc_algs[i].name)) continue;
        if ((flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) && g_bc_algs[i].alg == ALG_RNG) return STATUS_NOT_SUPPORTED;
        struct bc_alg *a = calloc(1, sizeof *a);
        if (!a) return STATUS_NO_MEMORY;
        a->magic = ALG_MAGIC;
        a->alg = g_bc_algs[i].alg;
        a->hmac = (flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0;
        a->reusable = (flags & BCRYPT_HASH_REUSABLE_FLAG) != 0;
        *ph = a;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_FOUND;   /* ciphers, signatures and key exchange are not provided */
}

API NTSTATUS BCryptCloseAlgorithmProvider(void *h, ULONG flags)
{
    struct bc_alg a;
    (void)flags;
    if (!bc_alg_of(h, &a)) return STATUS_INVALID_HANDLE;
    if ((uintptr_t)h >= 0x10000) { ((struct bc_alg *)h)->magic = 0; free(h); }
    return STATUS_SUCCESS;
}

API NTSTATUS BCryptGenRandom(void *h, BYTE *buf, ULONG n, ULONG flags)
{
    struct bc_alg a;
    if (flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG) { if (h) return STATUS_INVALID_PARAMETER; }
    else if (!bc_alg_of(h, &a) || a.alg != ALG_RNG) return STATUS_INVALID_HANDLE;
    if (!buf && n) return STATUS_INVALID_PARAMETER;
    fill_random(buf, n);
    return STATUS_SUCCESS;
}

/* Rust's standard library and others; documented never to fail. */
API BOOL ProcessPrng(BYTE *buf, SIZE_T n) { if (n) fill_random(buf, n); return TRUE; }

API NTSTATUS BCryptGetFipsAlgorithmMode(BYTE *enabled)
{
    if (!enabled) return STATUS_INVALID_PARAMETER;
    *enabled = 0;
    return STATUS_SUCCESS;
}

static struct bc_hash *bc_hash_of(void *h)
{
    struct bc_hash *o = h;
    return (uintptr_t)h >= 0x10000 && o->magic == HASH_MAGIC ? o : NULL;
}

static NTSTATUS put_prop(const void *val, ULONG len, BYTE *out, ULONG cb, ULONG *res)
{
    *res = len;
    if (!out) return STATUS_SUCCESS;
    if (cb < len) return STATUS_BUFFER_TOO_SMALL;
    memcpy(out, val, len);
    return STATUS_SUCCESS;
}

API NTSTATUS BCryptGetProperty(void *h, const WCHAR *prop, BYTE *out, ULONG cb, ULONG *res, ULONG flags)
{
    struct bc_alg a;
    struct bc_hash *o = bc_hash_of(h);
    (void)flags;
    if (o) { memset(&a, 0, sizeof a); a.alg = o->alg; a.hmac = o->hmac; }
    else if (!bc_alg_of(h, &a)) return STATUS_INVALID_HANDLE;
    if (!prop || !res) return STATUS_INVALID_PARAMETER;
    if (wstr_ieq(prop, "AlgorithmName")) {
        WCHAR name[8];
        for (size_t i = 0; i < sizeof g_bc_algs / sizeof g_bc_algs[0]; i++)
            if (g_bc_algs[i].alg == a.alg) wdup_ascii(g_bc_algs[i].name, name);
        return put_prop(name, (ULONG)((lsiw_wcslen(name) + 1) * sizeof(WCHAR)), out, cb, res);
    }
    if (a.alg == ALG_RNG) return STATUS_NOT_SUPPORTED;
    DWORD v;
    if (wstr_ieq(prop, "ObjectLength")) v = HASH_OBJECT_LENGTH;
    else if (wstr_ieq(prop, "HashDigestLength")) v = (DWORD)lsiw_hash_size(a.alg);
    else if (wstr_ieq(prop, "HashBlockLength")) v = (DWORD)lsiw_hash_block_size(a.alg);
    else return STATUS_NOT_SUPPORTED;   /* includes BlockLength: no block ciphers */
    return put_prop(&v, sizeof v, out, cb, res);
}

API NTSTATUS BCryptSetProperty(void *h, const WCHAR *prop, BYTE *in, ULONG cb, ULONG flags)
{
    struct bc_alg a;
    (void)prop; (void)in; (void)cb; (void)flags;
    if (!bc_hash_of(h) && !bc_alg_of(h, &a)) return STATUS_INVALID_HANDLE;
    return STATUS_NOT_SUPPORTED;   /* hash and RNG providers have no settable properties */
}

/* Places the object in the caller's buffer (aligned within it) or in
 * memory of our own. */
static struct bc_hash *bc_hash_place(BYTE *obj, ULONG cb, NTSTATUS *st)
{
    void *base = obj;
    if (obj) {
        if (cb < HASH_OBJECT_LENGTH) { *st = STATUS_BUFFER_TOO_SMALL; return NULL; }
    } else if (!(base = malloc(HASH_OBJECT_LENGTH))) {
        *st = STATUS_NO_MEMORY;
        return NULL;
    }
    struct bc_hash *o = (struct bc_hash *)(((uintptr_t)base + 15) & ~(uintptr_t)15);
    memset(o, 0, sizeof *o);
    o->base = base;
    o->owned = !obj;
    return o;
}

API NTSTATUS BCryptCreateHash(void *halg, void **ph, BYTE *obj, ULONG cb, BYTE *secret, ULONG slen, ULONG flags)
{
    struct bc_alg a;
    NTSTATUS st = STATUS_SUCCESS;
    if (!bc_alg_of(halg, &a) || a.alg == ALG_RNG) return STATUS_INVALID_HANDLE;
    if (!ph || (!secret && slen)) return STATUS_INVALID_PARAMETER;
    struct bc_hash *o = bc_hash_place(obj, cb, &st);
    if (!o) return st;
    o->magic = HASH_MAGIC;
    o->alg = a.alg;
    o->hmac = a.hmac;
    o->reusable = a.reusable || (flags & BCRYPT_HASH_REUSABLE_FLAG);
    if (a.hmac) {
        struct hmac_ctx c;
        hmac_init(&c, a.alg, secret, slen);
        o->start = c.in;
        o->outer = c.out;
    } else {
        lsiw_hash_init(&o->start, a.alg);
    }
    o->cur = o->start;
    *ph = o;
    return STATUS_SUCCESS;
}

API NTSTATUS BCryptHashData(void *h, BYTE *in, ULONG n, ULONG flags)
{
    struct bc_hash *o = bc_hash_of(h);
    (void)flags;
    if (!o || o->done) return STATUS_INVALID_HANDLE;
    if (!in && n) return STATUS_INVALID_PARAMETER;
    if (n) lsiw_hash_update(&o->cur, in, n);
    return STATUS_SUCCESS;
}

API NTSTATUS BCryptFinishHash(void *h, BYTE *out, ULONG n, ULONG flags)
{
    struct bc_hash *o = bc_hash_of(h);
    (void)flags;
    if (!o || o->done) return STATUS_INVALID_HANDLE;
    if (!out || n != lsiw_hash_size(o->alg)) return STATUS_INVALID_PARAMETER;
    if (o->hmac) {
        uint8_t inner[LSIW_HASH_MAX_DIGEST];
        struct lsiw_hash outer = o->outer;
        size_t k = lsiw_hash_final(&o->cur, inner);
        lsiw_hash_update(&outer, inner, k);
        lsiw_hash_final(&outer, out);
    } else {
        lsiw_hash_final(&o->cur, out);
    }
    if (o->reusable) o->cur = o->start; else o->done = 1;
    return STATUS_SUCCESS;
}

API NTSTATUS BCryptDestroyHash(void *h)
{
    struct bc_hash *o = bc_hash_of(h);
    if (!o) return STATUS_INVALID_HANDLE;
    o->magic = 0;
    if (o->owned) free(o->base);
    return STATUS_SUCCESS;
}

API NTSTATUS BCryptDuplicateHash(void *h, void **ph, BYTE *obj, ULONG cb, ULONG flags)
{
    struct bc_hash *o = bc_hash_of(h);
    NTSTATUS st = STATUS_SUCCESS;
    (void)flags;
    if (!o || o->done) return STATUS_INVALID_HANDLE;
    if (!ph) return STATUS_INVALID_PARAMETER;
    struct bc_hash *n = bc_hash_place(obj, cb, &st);
    if (!n) return st;
    void *base = n->base;
    int owned = n->owned;
    *n = *o;
    n->base = base;
    n->owned = owned;
    *ph = n;
    return STATUS_SUCCESS;
}

API NTSTATUS BCryptHash(void *halg, BYTE *secret, ULONG slen, BYTE *in, ULONG n, BYTE *out, ULONG outn)
{
    void *h;
    NTSTATUS st = BCryptCreateHash(halg, &h, NULL, 0, secret, slen, 0);
    if (st) return st;
    if (!(st = BCryptHashData(h, in, n, 0))) st = BCryptFinishHash(h, out, outn, 0);
    BCryptDestroyHash(h);
    return st;
}

/* PBKDF2 (RFC 8018) with HMAC over the provider's hash. */
API NTSTATUS BCryptDeriveKeyPBKDF2(void *hprf, BYTE *pw, ULONG pwlen, BYTE *salt, ULONG slen, ULONGLONG iters,
                                   BYTE *out, ULONG outlen, ULONG flags)
{
    struct bc_alg a;
    (void)flags;
    if (!bc_alg_of(hprf, &a) || a.alg == ALG_RNG) return STATUS_INVALID_HANDLE;
    if (!iters || !out || (!pw && pwlen) || (!salt && slen)) return STATUS_INVALID_PARAMETER;
    struct hmac_ctx base;
    hmac_init(&base, a.alg, pw, pwlen);
    size_t hl = lsiw_hash_size(a.alg);
    uint8_t u[LSIW_HASH_MAX_DIGEST], t[LSIW_HASH_MAX_DIGEST];
    for (uint32_t blk = 1; (size_t)(blk - 1) * hl < outlen; blk++) {
        uint8_t be[4] = { (uint8_t)(blk >> 24), (uint8_t)(blk >> 16), (uint8_t)(blk >> 8), (uint8_t)blk };
        struct hmac_ctx c = base;
        hmac_update(&c, salt, slen);
        hmac_update(&c, be, 4);
        hmac_final(&c, u);
        memcpy(t, u, hl);
        for (ULONGLONG i = 1; i < iters; i++) {
            c = base;
            hmac_update(&c, u, hl);
            hmac_final(&c, u);
            for (size_t j = 0; j < hl; j++) t[j] ^= u[j];
        }
        size_t off = (size_t)(blk - 1) * hl, take = outlen - off < hl ? outlen - off : hl;
        memcpy(out + off, t, take);
    }
    return STATUS_SUCCESS;
}

/* No symmetric ciphers: no provider can be opened for one, so these
 * never get a usable key handle. */
API NTSTATUS BCryptGenerateSymmetricKey(void *halg, void **hkey, BYTE *obj, ULONG cb, BYTE *secret, ULONG slen, ULONG flags)
{
    (void)halg; (void)obj; (void)cb; (void)secret; (void)slen; (void)flags;
    if (hkey) *hkey = NULL;
    return STATUS_NOT_SUPPORTED;
}
API NTSTATUS BCryptEncrypt(void *key, BYTE *in, ULONG n, void *pad, BYTE *iv, ULONG ivlen, BYTE *out, ULONG outn,
                           ULONG *res, ULONG flags)
{
    (void)key; (void)in; (void)n; (void)pad; (void)iv; (void)ivlen; (void)out; (void)outn; (void)flags;
    if (res) *res = 0;
    return STATUS_NOT_SUPPORTED;
}
API NTSTATUS BCryptDecrypt(void *key, BYTE *in, ULONG n, void *pad, BYTE *iv, ULONG ivlen, BYTE *out, ULONG outn,
                           ULONG *res, ULONG flags)
{
    return BCryptEncrypt(key, in, n, pad, iv, ivlen, out, outn, res, flags);
}

/* ------------------------------------------------------------------ */
/* Export table                                                        */
/* ------------------------------------------------------------------ */

/* "#n" entries are oleaut32's export ordinals, for programs that import
 * it by ordinal. */
const struct lsiw_export lsiw_ole_exports[] = {
    /* ole32 / combase */
    E(GetRunningObjectTable), E(CoInitialize), E(CoInitializeEx), E(CoUninitialize), E(OleInitialize), E(OleUninitialize),
    E(CoGetApartmentType), E(CoIncrementMTAUsage), E(CoDecrementMTAUsage), E(CoGetCurrentProcess),
    E(CoTaskMemAlloc), E(CoTaskMemRealloc), E(CoTaskMemFree), E(CoGetMalloc),
    E(CoCreateGuid), E(StringFromGUID2), E(StringFromCLSID), E(StringFromIID), E(CLSIDFromString),
    E(IIDFromString), E(CLSIDFromProgID), E(CLSIDFromProgIDEx), E(ProgIDFromCLSID),
    E(CoCreateInstance), E(CoCreateInstanceEx), E(CoGetClassObject), E(CoRegisterClassObject),
    E(CoRevokeClassObject), E(CoResumeClassObjects), E(CoSuspendClassObjects),
    E(CoInitializeSecurity), E(CoSetProxyBlanket), E(CoQueryProxyBlanket), E(CoImpersonateClient),
    E(CoRevertToSelf), E(CoGetCallContext), E(CoFreeUnusedLibraries), E(CoFreeUnusedLibrariesEx),
    E(CoAddRefServerProcess), E(CoReleaseServerProcess), E(CoWaitForMultipleHandles),
    E(CoGetObjectContext), E(CoCreateFreeThreadedMarshaler), E(CoMarshalInterface), E(CoUnmarshalInterface),
    E(CreateStreamOnHGlobal), E(GetHGlobalFromStream), E(CreateBindCtx), E(PropVariantClear), E(PropVariantCopy),
    E(OleRun), E(RegisterDragDrop), E(RevokeDragDrop), E(OleGetClipboard), E(CoRegisterMessageFilter),
    E(RoInitialize), E(RoUninitialize), E(RoGetActivationFactory), E(RoActivateInstance),
    E(WindowsCreateString), E(WindowsCreateStringReference), E(WindowsDeleteString), E(WindowsDuplicateString),
    E(WindowsGetStringRawBuffer), E(WindowsGetStringLen), E(WindowsIsStringEmpty), E(WindowsCompareStringOrdinal),
    E(GetErrorInfo), E(SetErrorInfo), E(CreateErrorInfo),
    /* oleaut32 */
    E(SysAllocString), E(SysAllocStringLen), E(SysAllocStringByteLen), E(SysReAllocString),
    E(SysReAllocStringLen), E(SysFreeString), E(SysStringLen), E(SysStringByteLen), E(SysAddRefString),
    E(SysReleaseString), E(VariantInit), E(VariantClear), E(VariantCopy), E(VariantCopyInd),
    E(VariantChangeType), E(VariantChangeTypeEx), E(VarBstrCmp), E(VarBstrCat),
    E(VarI2FromStr), E(VarI4FromStr), E(VarI8FromStr), E(VarUI1FromStr), E(VarUI2FromStr), E(VarUI4FromStr),
    E(VarUI8FromStr), E(VarR4FromStr), E(VarR8FromStr), E(VarBoolFromStr), E(VarDateFromStr), E(VarCyFromStr),
    E(VarBstrFromI2), E(VarBstrFromI4), E(VarBstrFromI8), E(VarBstrFromUI1), E(VarBstrFromUI2), E(VarBstrFromUI4),
    E(VarBstrFromUI8), E(VarBstrFromR4), E(VarBstrFromR8), E(VarBstrFromDate), E(VarBstrFromCy), E(VarBstrFromBool),
    E(VariantTimeToSystemTime), E(SystemTimeToVariantTime), E(DosDateTimeToVariantTime), E(VariantTimeToDosDateTime),
    E(SafeArrayCreate), E(SafeArrayCreateVector), E(SafeArrayDestroy), E(SafeArrayDestroyData),
    E(SafeArrayDestroyDescriptor), E(SafeArrayAllocDescriptor), E(SafeArrayAllocDescriptorEx), E(SafeArrayAllocData),
    E(SafeArrayGetDim), E(SafeArrayGetElemsize), E(SafeArrayGetLBound), E(SafeArrayGetUBound),
    E(SafeArrayAccessData), E(SafeArrayUnaccessData), E(SafeArrayLock), E(SafeArrayUnlock), E(SafeArrayPtrOfIndex),
    E(SafeArrayGetElement), E(SafeArrayPutElement), E(SafeArrayCopy), E(SafeArrayRedim), E(SafeArrayGetVartype),
    E(LoadTypeLib), E(LoadTypeLibEx), E(LoadRegTypeLib), E(RegisterTypeLib), E(UnRegisterTypeLib),
    E(GetActiveObject), E(DispGetParam), E(CreateDispTypeInfo), E(CreateStdDispatch), E(OleLoadPicture),
    EX("#2", SysAllocString), EX("#3", SysReAllocString), EX("#4", SysAllocStringLen), EX("#5", SysReAllocStringLen),
    EX("#6", SysFreeString), EX("#7", SysStringLen), EX("#8", VariantInit), EX("#9", VariantClear),
    EX("#10", VariantCopy), EX("#11", VariantCopyInd), EX("#12", VariantChangeType), EX("#13", VariantTimeToDosDateTime),
    EX("#14", DosDateTimeToVariantTime), EX("#15", SafeArrayCreate), EX("#16", SafeArrayDestroy),
    EX("#17", SafeArrayGetDim), EX("#18", SafeArrayGetElemsize), EX("#19", SafeArrayGetUBound),
    EX("#20", SafeArrayGetLBound), EX("#21", SafeArrayLock), EX("#22", SafeArrayUnlock), EX("#23", SafeArrayAccessData),
    EX("#24", SafeArrayUnaccessData), EX("#25", SafeArrayGetElement), EX("#26", SafeArrayPutElement),
    EX("#27", SafeArrayCopy), EX("#28", DispGetParam), EX("#31", CreateDispTypeInfo), EX("#32", CreateStdDispatch),
    EX("#35", GetActiveObject), EX("#36", SafeArrayAllocDescriptor), EX("#37", SafeArrayAllocData),
    EX("#38", SafeArrayDestroyDescriptor), EX("#39", SafeArrayDestroyData), EX("#40", SafeArrayRedim),
    EX("#41", SafeArrayAllocDescriptorEx), EX("#54", VarI2FromStr), EX("#64", VarI4FromStr), EX("#74", VarR4FromStr),
    EX("#77", SafeArrayGetVartype), EX("#84", VarR8FromStr), EX("#94", VarDateFromStr), EX("#104", VarCyFromStr),
    EX("#108", VarBstrFromUI1), EX("#109", VarBstrFromI2), EX("#110", VarBstrFromI4), EX("#111", VarBstrFromR4),
    EX("#112", VarBstrFromR8), EX("#113", VarBstrFromCy), EX("#114", VarBstrFromDate), EX("#116", VarBstrFromBool),
    EX("#125", VarBoolFromStr), EX("#136", VarUI1FromStr), EX("#147", VariantChangeTypeEx),
    EX("#148", SafeArrayPtrOfIndex), EX("#149", SysStringByteLen), EX("#150", SysAllocStringByteLen),
    EX("#161", LoadTypeLib), EX("#162", LoadRegTypeLib), EX("#163", RegisterTypeLib), EX("#183", LoadTypeLibEx),
    EX("#184", SystemTimeToVariantTime), EX("#185", VariantTimeToSystemTime), EX("#186", UnRegisterTypeLib),
    EX("#200", GetErrorInfo), EX("#201", SetErrorInfo), EX("#202", CreateErrorInfo),
    EX("#313", VarBstrCat), EX("#314", VarBstrCmp), EX("#411", SafeArrayCreateVector),
    /* rpcrt4 */
    E(UuidCreate), E(UuidCreateSequential), E(UuidCreateNil), E(UuidToStringA), E(UuidToStringW),
    E(UuidFromStringA), E(UuidFromStringW), E(RpcStringFreeA), E(RpcStringFreeW), E(UuidEqual),
    E(UuidCompare), E(UuidIsNil), E(UuidHash), E(I_RpcExceptionFilter), E(RpcRaiseException),
    /* crypt32 */
    E(CryptStringToBinaryA), E(CryptStringToBinaryW), E(CryptBinaryToStringA), E(CryptBinaryToStringW),
    E(CryptProtectData), E(CryptUnprotectData), E(CryptProtectMemory), E(CryptUnprotectMemory),
    E(CertOpenStore), E(CertOpenSystemStoreA), E(CertOpenSystemStoreW), E(CertCloseStore),
    E(CertEnumCertificatesInStore), E(CertFindCertificateInStore), E(CertFreeCertificateContext),
    E(CertDuplicateCertificateContext), E(CertGetCertificateChain), E(CertFreeCertificateChain),
    /* bcrypt / bcryptprimitives */
    E(BCryptOpenAlgorithmProvider), E(BCryptCloseAlgorithmProvider), E(BCryptGenRandom), E(BCryptGetProperty),
    E(BCryptSetProperty), E(BCryptCreateHash), E(BCryptHashData), E(BCryptFinishHash), E(BCryptDestroyHash),
    E(BCryptDuplicateHash), E(BCryptHash), E(BCryptDeriveKeyPBKDF2), E(BCryptGenerateSymmetricKey),
    E(BCryptEncrypt), E(BCryptDecrypt), E(BCryptGetFipsAlgorithmMode), E(ProcessPrng),
    { NULL, NULL }
};
