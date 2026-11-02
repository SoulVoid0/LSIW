/* ws2_32 / wsock32 / mswsock / iphlpapi: Windows Sockets 2 and the IP Helper
 * API on top of Linux BSD sockets.
 *
 * A SOCKET is a kernel32 HANDLE made with lsiw_handle_from_fd() and cast to
 * UINT_PTR, so ReadFile, WriteFile, CloseHandle and DuplicateHandle keep
 * working on it. Everything that differs between the two systems is
 * translated at the boundary: address family numbers (AF_INET6 is 23 on
 * Windows), option levels and names, message flags, poll bits, error codes
 * (WSAE*), and the Windows layouts of fd_set, timeval, hostent, servent,
 * addrinfo, linger, WSAPROTOCOL_INFO and the IP Helper tables.
 *
 * Overlapped operations are carried out synchronously and then completed the
 * Windows way (Internal/InternalHigh, hEvent, completion routine).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/errqueue.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Windows types and constants                                          */
/* ------------------------------------------------------------------ */

typedef ULONG_PTR SOCKET;
typedef int16_t   SHORT;
typedef uint16_t  USHORT;
typedef uint8_t   UCHAR;
typedef int       INT;
typedef uint64_t  ULONG64;
typedef int64_t   LONG_PTR;
typedef void     *HWND;
typedef struct { DWORD Data1; WORD Data2; WORD Data3; BYTE Data4[8]; } GUID;

#define INVALID_SOCKET (~(SOCKET)0)
#define SOCKET_ERROR   (-1)

/* Winsock error codes (winerror.h). */
#define WSAEINTR            10004
#define WSAEBADF            10009
#define WSAEACCES           10013
#define WSAEFAULT           10014
#define WSAEINVAL           10022
#define WSAEMFILE           10024
#define WSAEWOULDBLOCK      10035
#define WSAEINPROGRESS      10036
#define WSAEALREADY         10037
#define WSAENOTSOCK         10038
#define WSAEDESTADDRREQ     10039
#define WSAEMSGSIZE         10040
#define WSAEPROTOTYPE       10041
#define WSAENOPROTOOPT      10042
#define WSAEPROTONOSUPPORT  10043
#define WSAESOCKTNOSUPPORT  10044
#define WSAEOPNOTSUPP       10045
#define WSAEPFNOSUPPORT     10046
#define WSAEAFNOSUPPORT     10047
#define WSAEADDRINUSE       10048
#define WSAEADDRNOTAVAIL    10049
#define WSAENETDOWN         10050
#define WSAENETUNREACH      10051
#define WSAENETRESET        10052
#define WSAECONNABORTED     10053
#define WSAECONNRESET       10054
#define WSAENOBUFS          10055
#define WSAEISCONN          10056
#define WSAENOTCONN         10057
#define WSAESHUTDOWN        10058
#define WSAETIMEDOUT        10060
#define WSAECONNREFUSED     10061
#define WSAELOOP            10062
#define WSAENAMETOOLONG     10063
#define WSAEHOSTDOWN        10064
#define WSAEHOSTUNREACH     10065
#define WSAENOTEMPTY        10066
#define WSAEDQUOT           10069
#define WSAESTALE           10070
#define WSASYSNOTREADY      10091
#define WSAVERNOTSUPPORTED  10092
#define WSANOTINITIALISED   10093
#define WSASYSCALLFAILURE   10107
#define WSASERVICE_NOT_FOUND 10108
#define WSATYPE_NOT_FOUND   10109
#define WSA_E_NO_MORE       10110
#define WSAHOST_NOT_FOUND   11001
#define WSATRY_AGAIN        11002
#define WSANO_RECOVERY      11003
#define WSANO_DATA          11004
#define WSA_INVALID_HANDLE     6
#define WSA_NOT_ENOUGH_MEMORY  8
#define WSA_INVALID_PARAMETER  87
#define WSA_OPERATION_ABORTED  995
#define WSA_IO_INCOMPLETE      996
#define WSA_IO_PENDING         997
#define WSA_WAIT_FAILED        0xFFFFFFFFu
#define WSA_MAXIMUM_WAIT_EVENTS 64

#define ERROR_BUFFER_OVERFLOW     111
#define ERROR_NO_DATA             232
#define ERROR_OPERATION_ABORTED_  995
#define ERROR_IO_PENDING_         997
#define ERROR_CAN_NOT_COMPLETE    1003
#define ERROR_NOT_FOUND           1168
#define NO_ERROR                  0

/* Address families as Windows numbers them. */
#define WS_AF_UNSPEC 0
#define WS_AF_UNIX   1
#define WS_AF_INET   2
#define WS_AF_INET6  23

#define WS_SOL_SOCKET 0xFFFF
#define WS_IPPROTO_IP   0
#define WS_IPPROTO_TCP  6
#define WS_IPPROTO_UDP  17
#define WS_IPPROTO_IPV6 41
#define FROM_PROTOCOL_INFO (-1)

/* send/recv flags */
#define WS_MSG_OOB       0x1
#define WS_MSG_PEEK      0x2
#define WS_MSG_DONTROUTE 0x4
#define WS_MSG_WAITALL   0x8
#define WS_MSG_TRUNC     0x0100
#define WS_MSG_CTRUNC    0x0200
#define WS_MSG_PARTIAL   0x8000

/* ioctlsocket / WSAIoctl codes */
#define WS_FIONBIO     0x8004667Eu
#define WS_FIONREAD    0x4004667Fu
#define WS_SIOCATMARK  0x40047307u
#define SIO_GET_EXTENSION_FUNCTION_POINTER 0xC8000006u
#define SIO_KEEPALIVE_VALS                 0x98000004u
#define SIO_ADDRESS_LIST_QUERY             0x48000016u
#define SIO_ADDRESS_LIST_CHANGE            0x28000017u
#define SIO_GET_INTERFACE_LIST             0x4004747Fu
#define SIO_ROUTING_INTERFACE_QUERY        0xC8000014u
#define SIO_UDP_CONNRESET                  0x9800000Cu
#define SIO_UDP_NETRESET                   0x9800000Fu
#define SIO_LOOPBACK_FAST_PATH             0x98000010u
#define SIO_TCP_INITIAL_RTO                0x98000011u
#define SIO_SET_COMPATIBILITY_MODE         0x9800012Cu
#define SIO_BSP_HANDLE                     0x4800001Bu
#define SIO_BSP_HANDLE_SELECT              0x4800001Cu
#define SIO_BSP_HANDLE_POLL                0x4800001Du
#define SIO_BASE_HANDLE                    0x48000022u
#define SIO_IDEAL_SEND_BACKLOG_QUERY       0x4004747Bu

/* WSAEventSelect network events */
#define FD_READ    0x01
#define FD_WRITE   0x02
#define FD_OOB     0x04
#define FD_ACCEPT  0x08
#define FD_CONNECT 0x10
#define FD_CLOSE   0x20
#define FD_ALL     0x3F
#define FD_MAX_EVENTS 10

/* NTSTATUS values stored in OVERLAPPED.Internal */
#define STATUS_PENDING_             0x00000103u
#define STATUS_BUFFER_OVERFLOW_     0x80000005u
#define STATUS_UNSUCCESSFUL_        0xC0000001u
#define STATUS_INVALID_PARAMETER_   0xC000000Du
#define STATUS_NO_MEMORY_           0xC0000017u
#define STATUS_ACCESS_DENIED_       0xC0000022u
#define STATUS_PIPE_DISCONNECTED_   0xC00000B0u
#define STATUS_IO_TIMEOUT_          0xC00000B5u
#define STATUS_NOT_SUPPORTED_       0xC00000BBu
#define STATUS_CANCELLED_           0xC0000120u
#define STATUS_INVALID_CONNECTION_  0xC0000140u
#define STATUS_INVALID_ADDRESS_COMPONENT_ 0xC0000207u
#define STATUS_ADDRESS_ALREADY_EXISTS_ 0xC000020Au
#define STATUS_CONNECTION_RESET_    0xC000020Du
#define STATUS_CONNECTION_REFUSED_  0xC0000236u
#define STATUS_CONNECTION_ABORTED_  0xC0000241u
#define STATUS_NETWORK_UNREACHABLE_ 0xC000023Cu
#define STATUS_HOST_UNREACHABLE_    0xC000023Du

typedef struct {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union { struct { DWORD Offset; DWORD OffsetHigh; } s; void *Pointer; } u;
    HANDLE hEvent;
} WSAOVERLAPPED;
typedef void (WINAPI *LPWSAOVERLAPPED_COMPLETION_ROUTINE)(DWORD, DWORD, WSAOVERLAPPED *, DWORD);

typedef struct { ULONG len; char *buf; } WSABUF;
struct ws_timeval { LONG tv_sec; LONG tv_usec; };   /* Windows long is 32-bit */
typedef struct { void *lpSockaddr; INT iSockaddrLength; } SOCKET_ADDRESS;

_Static_assert(sizeof(WSAOVERLAPPED) == 32, "OVERLAPPED layout");
_Static_assert(sizeof(WSABUF) == 16, "WSABUF layout");

/* ------------------------------------------------------------------ */
/* Errors                                                               */
/* ------------------------------------------------------------------ */

static void set_wsa(DWORD e) { lsiw_set_last_error(e); }

static DWORD wsa_err(int e)
{
    switch (e) {
    case 0:               return 0;
    case EINTR:           return WSAEINTR;
    case EBADF:
    case ENOTSOCK:        return WSAENOTSOCK;
    case EACCES:
    case EPERM:           return WSAEACCES;
    case EFAULT:          return WSAEFAULT;
    case EINVAL:
    case ENOENT:          return WSAEINVAL;
    case EMFILE:
    case ENFILE:          return WSAEMFILE;
    case EAGAIN:
    case EINPROGRESS:     return WSAEWOULDBLOCK;   /* a non-blocking connect says "would block" */
    case EALREADY:        return WSAEALREADY;
    case EDESTADDRREQ:    return WSAEDESTADDRREQ;
    case EMSGSIZE:        return WSAEMSGSIZE;
    case EPROTOTYPE:      return WSAEPROTOTYPE;
    case ENOPROTOOPT:     return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT;
    case EOPNOTSUPP:      return WSAEOPNOTSUPP;
    case EPFNOSUPPORT:    return WSAEPFNOSUPPORT;
    case EAFNOSUPPORT:    return WSAEAFNOSUPPORT;
    case EADDRINUSE:      return WSAEADDRINUSE;
    case EADDRNOTAVAIL:   return WSAEADDRNOTAVAIL;
    case ENETDOWN:        return WSAENETDOWN;
    case ENETUNREACH:     return WSAENETUNREACH;
    case ENETRESET:       return WSAENETRESET;
    case ECONNABORTED:
    case EPROTO:          return WSAECONNABORTED;
    case ECONNRESET:
    case EPIPE:           return WSAECONNRESET;
    case ENOBUFS:
    case ENOMEM:          return WSAENOBUFS;
    case EISCONN:         return WSAEISCONN;
    case ENOTCONN:        return WSAENOTCONN;
    case ESHUTDOWN:       return WSAESHUTDOWN;
    case ETIMEDOUT:       return WSAETIMEDOUT;
    case ECONNREFUSED:    return WSAECONNREFUSED;
    case ELOOP:           return WSAELOOP;
    case ENAMETOOLONG:    return WSAENAMETOOLONG;
    case EHOSTDOWN:       return WSAEHOSTDOWN;
    case EHOSTUNREACH:    return WSAEHOSTUNREACH;
    case ENOTEMPTY:       return WSAENOTEMPTY;
    case EDQUOT:          return WSAEDQUOT;
    case ESTALE:          return WSAESTALE;
    default:              return WSASYSCALLFAILURE;
    }
}

static void set_wsa_errno(int e) { set_wsa(wsa_err(e)); }

/* WSA error <-> NTSTATUS, for OVERLAPPED.Internal. */
static const struct { DWORD wsa; DWORD st; } g_status_map[] = {
    { WSAECONNRESET, STATUS_CONNECTION_RESET_ }, { WSAECONNABORTED, STATUS_CONNECTION_ABORTED_ },
    { WSAECONNREFUSED, STATUS_CONNECTION_REFUSED_ }, { WSAETIMEDOUT, STATUS_IO_TIMEOUT_ },
    { WSAENETUNREACH, STATUS_NETWORK_UNREACHABLE_ }, { WSAEHOSTUNREACH, STATUS_HOST_UNREACHABLE_ },
    { WSAEMSGSIZE, STATUS_BUFFER_OVERFLOW_ }, { WSA_OPERATION_ABORTED, STATUS_CANCELLED_ },
    { WSAEINVAL, STATUS_INVALID_PARAMETER_ }, { WSAEADDRINUSE, STATUS_ADDRESS_ALREADY_EXISTS_ },
    { WSAEADDRNOTAVAIL, STATUS_INVALID_ADDRESS_COMPONENT_ }, { WSAESHUTDOWN, STATUS_PIPE_DISCONNECTED_ },
    { WSAENOTCONN, STATUS_INVALID_CONNECTION_ }, { WSAENOBUFS, STATUS_NO_MEMORY_ },
    { WSAEACCES, STATUS_ACCESS_DENIED_ }, { WSAEOPNOTSUPP, STATUS_NOT_SUPPORTED_ },
};

static DWORD wsa_to_status(DWORD wsa)
{
    if (!wsa) return 0;
    for (size_t i = 0; i < sizeof g_status_map / sizeof g_status_map[0]; i++)
        if (g_status_map[i].wsa == wsa) return g_status_map[i].st;
    return STATUS_UNSUCCESSFUL_;
}

static DWORD status_to_wsa(DWORD st)
{
    if (!st) return 0;
    for (size_t i = 0; i < sizeof g_status_map / sizeof g_status_map[0]; i++)
        if (g_status_map[i].st == st) return g_status_map[i].wsa;
    return WSAEINVAL;
}

/* getaddrinfo/getnameinfo results: the EAI_* names are WSA codes on Windows. */
static DWORD wsa_from_eai(int r, int err)
{
    switch (r) {
    case 0:            return 0;
    case EAI_AGAIN:    return WSATRY_AGAIN;
    case EAI_BADFLAGS: return WSAEINVAL;
    case EAI_FAIL:     return WSANO_RECOVERY;
    case EAI_FAMILY:   return WSAEAFNOSUPPORT;
    case EAI_MEMORY:   return WSA_NOT_ENOUGH_MEMORY;
    case EAI_NONAME:   return WSAHOST_NOT_FOUND;
    case EAI_SERVICE:  return WSATYPE_NOT_FOUND;
    case EAI_SOCKTYPE: return WSAESOCKTNOSUPPORT;
    case EAI_OVERFLOW: return WSAEFAULT;
#ifdef EAI_NODATA
    case EAI_NODATA:   return WSANO_DATA;
#endif
#ifdef EAI_ADDRFAMILY
    case EAI_ADDRFAMILY: return WSANO_DATA;
#endif
    case EAI_SYSTEM:   return wsa_err(err);
    default:           return WSANO_RECOVERY;
    }
}

/* Winsock must be started before almost anything else works. */
static int g_started;
#define NEED_INIT(ret) do { if (!__atomic_load_n(&g_started, __ATOMIC_ACQUIRE)) { \
    set_wsa(WSANOTINITIALISED); return ret; } } while (0)

/* ------------------------------------------------------------------ */
/* Other builtin DLLs, looked up at run time                            */
/* ------------------------------------------------------------------ */

typedef HANDLE (WINAPI *pf_CreateEventW)(void *, BOOL, BOOL, const WCHAR *);
typedef BOOL   (WINAPI *pf_handle)(HANDLE);
typedef DWORD  (WINAPI *pf_WaitMultiEx)(DWORD, const HANDLE *, BOOL, DWORD, BOOL);
typedef DWORD  (WINAPI *pf_thread_start)(void *);
typedef HANDLE (WINAPI *pf_CreateThread)(void *, SIZE_T, pf_thread_start, void *, DWORD, DWORD *);
typedef BOOL   (WINAPI *pf_PostMessageW)(HWND, UINT, ULONG_PTR, LONG_PTR);
typedef BOOL   (WINAPI *pf_IsWindow)(HWND);

static void *dll_sym(const char *dll, const char *sym)
{
    const struct lsiw_builtin_dll *d = lsiw_builtin_find(dll);
    return d ? lsiw_builtin_lookup(d, sym) : NULL;
}

/* Lazily resolved pointers; a race only stores the same value twice. */
#define K32_FN(type, var, name) static type var; if (!var) var = (type)dll_sym("kernel32.dll", name)

static HANDLE ev_create(void) { K32_FN(pf_CreateEventW, f, "CreateEventW"); return f ? f(NULL, TRUE, FALSE, NULL) : NULL; }
static BOOL ev_set(HANDLE h)   { K32_FN(pf_handle, f, "SetEvent");   return f ? f(h) : FALSE; }
static BOOL ev_reset(HANDLE h) { K32_FN(pf_handle, f, "ResetEvent"); return f ? f(h) : FALSE; }
static BOOL h_close(HANDLE h)  { K32_FN(pf_handle, f, "CloseHandle"); return f ? f(h) : (BOOL)lsiw_handle_close(h); }
static DWORD wait_multi(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable)
{
    K32_FN(pf_WaitMultiEx, f, "WaitForMultipleObjectsEx");
    if (!f) { set_wsa(ERROR_CALL_NOT_IMPLEMENTED); return WSA_WAIT_FAILED; }
    return f(n, hs, all, ms, alertable);
}
/* Run Windows code (a notification callback) on a proper Windows thread. */
static BOOL win_thread(pf_thread_start fn, void *arg)
{
    K32_FN(pf_CreateThread, f, "CreateThread");
    if (!f) return FALSE;
    HANDLE h = f(NULL, 0, fn, arg, 0, NULL);
    if (!h) return FALSE;
    h_close(h);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Overlapped completion                                                */
/* ------------------------------------------------------------------ */

/* LSIW has no APC delivery, so a completion routine runs as soon as the
 * operation is done. Routines that start another operation are queued and
 * run in turn instead of nesting, so a recv chain does not grow the stack. */
struct cr_call { LPWSAOVERLAPPED_COMPLETION_ROUTINE fn; DWORD err, bytes, flags; WSAOVERLAPPED *ov; };
static __thread struct cr_call t_crq[32];
static __thread int t_crq_head, t_crq_len, t_in_cr;

static void run_completion(LPWSAOVERLAPPED_COMPLETION_ROUTINE fn, DWORD err, DWORD bytes, WSAOVERLAPPED *ov, DWORD flags)
{
    if (t_in_cr) {
        if (t_crq_len < 32) {
            t_crq[(t_crq_head + t_crq_len++) % 32] = (struct cr_call){ fn, err, bytes, flags, ov };
            return;
        }
        fn(err, bytes, ov, flags);
        return;
    }
    t_in_cr = 1;
    fn(err, bytes, ov, flags);
    while (t_crq_len) {
        struct cr_call c = t_crq[t_crq_head];
        t_crq_head = (t_crq_head + 1) % 32;
        t_crq_len--;
        c.fn(c.err, c.bytes, c.ov, c.flags);
    }
    t_in_cr = 0;
}

/* Complete an operation that finished: the result goes into Internal and
 * InternalHigh (read by WSAGetOverlappedResult / GetOverlappedResult), the
 * recv flags into Offset, then the completion routine runs or hEvent is set
 * (its low bit only means "do not queue to a completion port"). */
void lsiw_iocp_io_done(HANDLE file, void *overlapped, DWORD bytes, uint32_t ntstatus);   /* k32_extra.c */

static void ov_complete(SOCKET s, WSAOVERLAPPED *ov, DWORD wsaerr, DWORD bytes, DWORD flags, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if (!ov) return;
    ov->Internal = wsa_to_status(wsaerr);
    ov->InternalHigh = bytes;
    ov->u.s.Offset = flags;
    if (cr) run_completion(cr, wsaerr, bytes, ov, flags);
    else {
        if (ov->hEvent) ev_set((HANDLE)((uintptr_t)ov->hEvent & ~(uintptr_t)1));
        /* Queue to the socket's completion port unless hEvent's low bit opts out. */
        if (!((uintptr_t)ov->hEvent & 1)) lsiw_iocp_io_done((HANDLE)s, ov, bytes, (uint32_t)ov->Internal);
    }
}

/* ------------------------------------------------------------------ */
/* Per-socket state                                                     */
/* ------------------------------------------------------------------ */

enum { ST_NEW, ST_CONNECTING, ST_CONNECTED, ST_LISTENING };

struct ws_sock {
    SOCKET s;
    struct lsiw_handle_obj *obj;   /* to notice a recycled handle value       */
    int fd;
    int refs;                      /* registry + callers; under g_lock        */
    int af, type, protocol;        /* Linux values                            */
    int state;
    int connecting;                /* non-blocking connect in progress        */
    int nonblock;
    int conn_failed;               /* last non-blocking connect failed        */
    DWORD conn_err;                /* its WSA code, reported once by SO_ERROR */
    int reuse, exclusive;          /* SO_REUSEADDR / SO_EXCLUSIVEADDRUSE      */
    int udp_connreset;             /* SIO_UDP_CONNRESET, on by default        */
    int shut_send;
    DWORD timeo[2];                /* SO_SNDTIMEO / SO_RCVTIMEO as set, in ms */
    int timeo_set[2];
    int busy;                      /* threads blocked in a call               */
    int closed;
    struct timespec since;         /* connection established (SO_CONNECT_TIME)*/
    /* WSAEventSelect / WSAAsyncSelect */
    LONG mask, armed, pending;
    INT errs[FD_MAX_EVENTS];
    HANDLE event;
    HWND hwnd; UINT msg;
    int eof, quiet, selected;
    struct ws_sock *sel_next;
};

#define MAX_SOCKS 65536
static struct ws_sock *g_socks[MAX_SOCKS];
static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static struct ws_sock *g_sel;          /* sockets with an event selection */

static long sock_index(SOCKET s)
{
    if (!s || (s & 3) || s / 4 > MAX_SOCKS) return -1;
    return (long)(s / 4) - 1;
}

static void sock_put(struct ws_sock *st)
{
    if (!st) return;
    pthread_mutex_lock(&g_lock);
    int last = --st->refs == 0;
    pthread_mutex_unlock(&g_lock);
    if (last) free(st);
}

static void sel_unlink_locked(struct ws_sock *st)
{
    if (!st->selected) return;
    for (struct ws_sock **pp = &g_sel; *pp; pp = &(*pp)->sel_next)
        if (*pp == st) { *pp = st->sel_next; break; }
    st->selected = 0;
    st->sel_next = NULL;
}

/* Remove from the registry; returns the reference the caller must drop. */
static struct ws_sock *sock_detach_locked(long i)
{
    struct ws_sock *st = g_socks[i];
    if (!st) return NULL;
    g_socks[i] = NULL;
    st->closed = 1;
    sel_unlink_locked(st);
    return st;
}

static void now_mono(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }

static void sock_fill(struct ws_sock *st, SOCKET s, struct lsiw_handle_obj *o, int af, int type, int proto)
{
    st->s = s; st->obj = o; st->fd = o->fd;
    st->af = af; st->type = type; st->protocol = proto;
    st->udp_connreset = 1;
}

/* A socket handle LSIW did not create itself (inherited, duplicated). */
static struct ws_sock *sock_adopt_locked(SOCKET s, long i, struct lsiw_handle_obj *o)
{
    struct stat sb;
    if (fstat(o->fd, &sb) < 0 || !S_ISSOCK(sb.st_mode)) return NULL;
    struct ws_sock *st = calloc(1, sizeof *st);
    if (!st) return NULL;
    int af = 0, type = 0, proto = 0, acc = 0;
    socklen_t l = sizeof af;
    getsockopt(o->fd, SOL_SOCKET, SO_DOMAIN, &af, &l); l = sizeof type;
    getsockopt(o->fd, SOL_SOCKET, SO_TYPE, &type, &l); l = sizeof proto;
    getsockopt(o->fd, SOL_SOCKET, SO_PROTOCOL, &proto, &l); l = sizeof acc;
    getsockopt(o->fd, SOL_SOCKET, SO_ACCEPTCONN, &acc, &l);
    sock_fill(st, s, o, af, type, proto);
    int fl = fcntl(o->fd, F_GETFL);
    st->nonblock = fl >= 0 && (fl & O_NONBLOCK);
    struct sockaddr_storage ss; socklen_t sl = sizeof ss;
    if (acc) st->state = ST_LISTENING;
    else if (getpeername(o->fd, (struct sockaddr *)&ss, &sl) == 0) { st->state = ST_CONNECTED; now_mono(&st->since); }
    st->refs = 1;
    g_socks[i] = st;
    return st;
}

/* Look up a SOCKET; returns a referenced state (drop with sock_put) or NULL
 * with WSAENOTSOCK set. */
static struct ws_sock *sock_get(SOCKET s)
{
    long i = sock_index(s);
    struct lsiw_handle_obj *o = i >= 0 ? lsiw_handle_get((HANDLE)(uintptr_t)s, H_FILE) : NULL;
    if (!o || o->fd < 0) { set_wsa(WSAENOTSOCK); return NULL; }
    struct ws_sock *st, *stale = NULL;
    pthread_mutex_lock(&g_lock);
    st = g_socks[i];
    if (st && (st->obj != o || st->fd != o->fd)) { stale = sock_detach_locked(i); st = NULL; }
    if (!st) st = sock_adopt_locked(s, i, o);
    if (st) st->refs++;
    pthread_mutex_unlock(&g_lock);
    if (stale) sock_put(stale);
    if (!st) set_wsa(WSAENOTSOCK);
    return st;
}

/* Wrap a new Linux socket in a HANDLE. Takes ownership of fd. */
static SOCKET sock_new(int fd, int af, int type, int proto, struct ws_sock **out)
{
    HANDLE h = lsiw_handle_from_fd(fd, 0);
    if (!h) { close(fd); set_wsa(WSAEMFILE); return INVALID_SOCKET; }
    SOCKET s = (SOCKET)(uintptr_t)h;
    long i = sock_index(s);
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    struct ws_sock *st = calloc(1, sizeof *st), *stale = NULL;
    if (!st || i < 0 || !o) { free(st); lsiw_handle_close(h); set_wsa(WSAENOBUFS); return INVALID_SOCKET; }
    sock_fill(st, s, o, af, type, proto);
    st->refs = out ? 2 : 1;
    pthread_mutex_lock(&g_lock);
    stale = sock_detach_locked(i);
    g_socks[i] = st;
    pthread_mutex_unlock(&g_lock);
    if (stale) sock_put(stale);
    if (out) *out = st;
    return s;
}

static void set_nonblock(struct ws_sock *st, int on)
{
    int fl = fcntl(st->fd, F_GETFL);
    if (fl >= 0) fcntl(st->fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
    st->nonblock = on;
}

/* Wait until fd is ready for `ev` (used where Windows semantics require a
 * blocking operation on a socket that is in non-blocking mode). */
static int wait_fd(int fd, short ev, int ms)
{
    struct pollfd p = { fd, ev, 0 };
    for (;;) {
        int r = poll(&p, 1, ms);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

/* ------------------------------------------------------------------ */
/* Socket addresses                                                     */
/* ------------------------------------------------------------------ */

union ws_sa {
    struct sockaddr sa;
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
    struct sockaddr_un un;
    struct sockaddr_storage ss;
};

static int af_to_linux(int af)
{
    switch (af) {
    case WS_AF_UNSPEC: return AF_UNSPEC;
    case WS_AF_UNIX:   return AF_UNIX;
    case WS_AF_INET:   return AF_INET;
    case WS_AF_INET6:  return AF_INET6;
    }
    return -1;
}

static int af_to_win(int af)
{
    switch (af) {
    case AF_UNSPEC: return WS_AF_UNSPEC;
    case AF_UNIX:   return WS_AF_UNIX;
    case AF_INET:   return WS_AF_INET;
    case AF_INET6:  return WS_AF_INET6;
    }
    return af;
}

/* Smallest Windows address buffer for a family (accept/recvfrom check it). */
static int win_sa_min(int af) { return af == AF_INET6 ? 28 : af == AF_INET ? 16 : 2; }

/* Windows sockaddr -> Linux. Returns the Linux length, or -1 with the WSA
 * error set. AF_UNIX paths are Windows paths and are mapped to Linux ones. */
static int sa_from_win(const void *name, int namelen, union ws_sa *out)
{
    memset(out, 0, sizeof *out);
    if (!name || namelen < 2) { set_wsa(WSAEFAULT); return -1; }
    WORD fam;
    memcpy(&fam, name, 2);
    switch (fam) {
    case WS_AF_INET:
        if (namelen < 16) { set_wsa(WSAEFAULT); return -1; }
        memcpy(&out->in, name, 16);
        out->in.sin_family = AF_INET;
        return sizeof(struct sockaddr_in);
    case WS_AF_INET6:
        /* 24 bytes is the old sockaddr_in6 without sin6_scope_id */
        if (namelen < 24) { set_wsa(WSAEFAULT); return -1; }
        memcpy(&out->in6, name, namelen >= 28 ? 28 : 24);
        out->in6.sin6_family = AF_INET6;
        return sizeof(struct sockaddr_in6);
    case WS_AF_UNIX: {
        out->un.sun_family = AF_UNIX;
        const char *p = (const char *)name + 2;
        size_t max = (size_t)namelen - 2;
        if (max > sizeof out->un.sun_path) max = sizeof out->un.sun_path;
        if (max == 0) return 2;
        if (p[0] == 0) {                    /* Linux abstract name: pass as is */
            memcpy(out->un.sun_path, p, max);
            return (int)(2 + max);
        }
        char path[sizeof out->un.sun_path + 1];
        size_t n = strnlen(p, max);
        memcpy(path, p, n); path[n] = 0;
        char *lp = lsiw_path_a(path);
        const char *src = lp ? lp : path;
        size_t ln = strlen(src);
        if (ln >= sizeof out->un.sun_path) { free(lp); set_wsa(WSAEINVAL); return -1; }
        memcpy(out->un.sun_path, src, ln + 1);
        free(lp);
        return (int)(offsetof(struct sockaddr_un, sun_path) + ln + 1);
    }
    case WS_AF_UNSPEC:
        out->sa.sa_family = AF_UNSPEC;
        return sizeof(struct sockaddr);
    }
    set_wsa(WSAEAFNOSUPPORT);
    return -1;
}

/* Linux address -> Windows form in `w`; returns the Windows length. */
static int sa_to_win_buf(const union ws_sa *a, socklen_t len, union ws_sa *w)
{
    memset(w, 0, sizeof *w);
    switch (a->sa.sa_family) {
    case AF_INET:
        memcpy(w, a, 16);
        w->sa.sa_family = WS_AF_INET;
        return 16;
    case AF_INET6:
        memcpy(w, a, 28);
        w->sa.sa_family = WS_AF_INET6;
        return 28;
    case AF_UNIX: {
        w->sa.sa_family = WS_AF_UNIX;
        size_t off = offsetof(struct sockaddr_un, sun_path);
        size_t plen = len > off ? len - off : 0;
        if (plen > sizeof a->un.sun_path) plen = sizeof a->un.sun_path;
        if (plen == 0) return 2;
        if (a->un.sun_path[0] == 0) {
            memcpy(w->un.sun_path, a->un.sun_path, plen);
            return (int)(off + plen);
        }
        char path[sizeof a->un.sun_path + 1];
        size_t n = strnlen(a->un.sun_path, plen);
        memcpy(path, a->un.sun_path, n); path[n] = 0;
        char *wp = lsiw_linux_to_win(path);
        const char *src = wp ? wp : path;
        size_t m = strlen(src);
        if (m >= sizeof w->un.sun_path) m = sizeof w->un.sun_path - 1;
        memcpy(w->un.sun_path, src, m);
        free(wp);
        return (int)(off + m + 1);
    }
    }
    size_t n = len > sizeof *w ? sizeof *w : len;
    memcpy(w, a, n);
    return (int)n;
}

/* Store an address into a Windows (name, *namelen) pair; WSAEFAULT if the
 * buffer is too small, as Windows does (it never truncates). */
static int sa_out(const union ws_sa *a, socklen_t len, void *name, INT *namelen)
{
    if (!name || !namelen) { set_wsa(WSAEFAULT); return -1; }
    union ws_sa w;
    int wl = sa_to_win_buf(a, len, &w);
    if (*namelen < wl) { set_wsa(WSAEFAULT); return -1; }
    memcpy(name, &w, (size_t)wl);
    *namelen = wl;
    return 0;
}

static int sa_is_any(const union ws_sa *a)
{
    if (a->sa.sa_family == AF_INET) return a->in.sin_addr.s_addr == 0;
    if (a->sa.sa_family == AF_INET6) return IN6_IS_ADDR_UNSPECIFIED(&a->in6.sin6_addr);
    return 0;
}

static int sa_port(const union ws_sa *a)
{
    if (a->sa.sa_family == AF_INET) return ntohs(a->in.sin_port);
    if (a->sa.sa_family == AF_INET6) return ntohs(a->in6.sin6_port);
    return -1;
}

/* ------------------------------------------------------------------ */
/* WSAEventSelect / WSAAsyncSelect: one background poll thread          */
/* ------------------------------------------------------------------ */

/* Windows records network events with "re-enabling" semantics: FD_READ is
 * recorded once and not again until a recv; FD_ACCEPT until accept; FD_WRITE
 * after a connect/accept and then only after a send failed with
 * WSAEWOULDBLOCK; FD_CONNECT and FD_CLOSE once. `armed` holds the events
 * that may be recorded now. The thread polls only for armed events, so a
 * level-triggered condition never spins. */
static int g_wake_fd = -1;
static pthread_once_t g_sel_once = PTHREAD_ONCE_INIT;
static int g_sel_thread_ok;

static void sel_wake(void)
{
    if (g_wake_fd >= 0) { uint64_t one = 1; ssize_t r = write(g_wake_fd, &one, sizeof one); (void)r; }
}

static void sel_rearm(struct ws_sock *st, LONG bits)
{
    if (!__atomic_load_n(&st->mask, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&g_lock);
    if (st->mask) {
        st->armed |= bits & st->mask;
        st->quiet = 0;
        sel_wake();
    }
    pthread_mutex_unlock(&g_lock);
}

static int bit_of(LONG ev) { int b = 0; while (ev > 1) { ev >>= 1; b++; } return b; }

static void sel_record_locked(struct ws_sock *st, LONG ev, INT err, LONG *fired)
{
    if (!(st->armed & ev)) return;
    st->armed &= ~ev;
    st->pending |= ev;
    st->errs[bit_of(ev)] = err;
    *fired |= ev;
}

/* Resolve a non-blocking connect that poll says has finished. g_lock held. */
static DWORD resolve_connect_locked(struct ws_sock *st)
{
    int err = 0; socklen_t l = sizeof err;
    if (getsockopt(st->fd, SOL_SOCKET, SO_ERROR, &err, &l) < 0) err = errno;
    if (!err) {
        /* POLLOUT can come before the handshake completes for some sockets */
        struct sockaddr_storage ss; socklen_t sl = sizeof ss;
        if (getpeername(st->fd, (struct sockaddr *)&ss, &sl) < 0 && errno == ENOTCONN) return (DWORD)-1;
    }
    st->connecting = 0;
    if (err) {
        st->state = ST_NEW;
        st->conn_failed = 1;
        st->conn_err = wsa_err(err);
        return st->conn_err;
    }
    st->state = ST_CONNECTED;
    st->conn_failed = 0;
    now_mono(&st->since);
    return 0;
}

/* Work out which events a poll result means for one socket. g_lock held. */
static LONG sel_process_locked(struct ws_sock *st, short rev)
{
    LONG fired = 0;
    if (st->state == ST_CONNECTING) {
        if (!(rev & (POLLOUT | POLLERR | POLLHUP))) return 0;
        DWORD e = resolve_connect_locked(st);
        if (e == (DWORD)-1) return 0;
        st->armed |= FD_CONNECT & st->mask;
        sel_record_locked(st, FD_CONNECT, (INT)e, &fired);
        if (e) st->quiet = 1;
        return fired;
    }
    if (st->state == ST_LISTENING) {
        if (rev & POLLIN) sel_record_locked(st, FD_ACCEPT, 0, &fired);
    } else if (st->type == SOCK_STREAM) {
        if (rev & POLLPRI) sel_record_locked(st, FD_OOB, 0, &fired);
        if (rev & (POLLIN | POLLRDHUP | POLLHUP | POLLERR)) {
            int avail = 0;
            if (ioctl(st->fd, FIONREAD, &avail) < 0) avail = 0;
            if (avail > 0) sel_record_locked(st, FD_READ, 0, &fired);
            if ((rev & (POLLRDHUP | POLLHUP | POLLERR)) || avail == 0) {
                INT err = 0;
                if (rev & POLLERR) {
                    int e = 0; socklen_t l = sizeof e;
                    getsockopt(st->fd, SOL_SOCKET, SO_ERROR, &e, &l);
                    err = e == ECONNABORTED ? WSAECONNABORTED : e ? WSAECONNRESET : 0;
                }
                st->eof = 1;
                sel_record_locked(st, FD_CLOSE, err, &fired);
            }
        }
        if ((rev & POLLOUT) && !(rev & POLLHUP)) sel_record_locked(st, FD_WRITE, 0, &fired);
    } else {
        if (rev & (POLLIN | POLLERR)) sel_record_locked(st, FD_READ, 0, &fired);
        if (rev & POLLOUT) sel_record_locked(st, FD_WRITE, 0, &fired);
    }
    if (rev & (POLLERR | POLLHUP | POLLNVAL)) st->quiet = 1;
    return fired;
}

static short sel_wanted_locked(struct ws_sock *st)
{
    if (st->quiet) return 0;
    switch (st->state) {
    case ST_CONNECTING: return (st->mask & (FD_CONNECT | FD_WRITE)) ? POLLOUT : 0;
    case ST_LISTENING:  return (st->armed & FD_ACCEPT) ? POLLIN : 0;
    case ST_NEW:        if (st->type == SOCK_STREAM) return 0;   /* unconnected TCP: nothing yet */
                        break;
    }
    short ev = 0;
    if ((st->armed & FD_READ) && !st->eof) ev |= POLLIN;
    if (st->armed & FD_OOB) ev |= POLLPRI;
    if (st->armed & FD_WRITE) ev |= POLLOUT;
    if ((st->armed & FD_CLOSE) && st->type == SOCK_STREAM) ev |= POLLRDHUP;
    return ev;
}

struct sel_note { HANDLE event; HWND hwnd; UINT msg; SOCKET s; LONG fired; INT errs[FD_MAX_EVENTS]; };

static void sel_notify(const struct sel_note *n)
{
    if (n->event) ev_set(n->event);
    if (n->hwnd) {
        static pf_PostMessageW post;
        if (!post) post = (pf_PostMessageW)dll_sym("user32.dll", "PostMessageW");
        if (!post) return;
        for (int b = 0; b < 6; b++)
            if (n->fired & (1L << b))   /* WSAMAKESELECTREPLY(event, error) */
                post(n->hwnd, n->msg, n->s, (LONG_PTR)(((uint32_t)(WORD)n->errs[b] << 16) | (1u << b)));
    }
}

static void *sel_thread(void *arg)
{
    (void)arg;
    size_t cap = 0;
    struct pollfd *pfd = NULL;
    struct ws_sock **sts = NULL;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        size_t n = 1;
        for (struct ws_sock *st = g_sel; st; st = st->sel_next) n++;
        if (n > cap) {
            cap = n * 2;
            pfd = realloc(pfd, cap * sizeof *pfd);
            sts = realloc(sts, cap * sizeof *sts);
        }
        n = 1;
        pfd[0] = (struct pollfd){ g_wake_fd, POLLIN, 0 };
        for (struct ws_sock *st = g_sel; st; st = st->sel_next) {
            short ev = sel_wanted_locked(st);
            if (!ev) continue;
            st->refs++;
            sts[n] = st;
            pfd[n++] = (struct pollfd){ st->fd, ev, 0 };
        }
        pthread_mutex_unlock(&g_lock);

        int r = poll(pfd, (nfds_t)n, -1);
        if (pfd[0].revents & POLLIN) { uint64_t v; ssize_t rr = read(g_wake_fd, &v, sizeof v); (void)rr; }

        struct sel_note *notes = calloc(n, sizeof *notes);
        size_t nn = 0;
        pthread_mutex_lock(&g_lock);
        for (size_t i = 1; i < n; i++) {
            struct ws_sock *st = sts[i];
            if (r <= 0 || st->closed || !st->selected || !pfd[i].revents) continue;
            LONG fired = sel_process_locked(st, pfd[i].revents);
            if (fired && notes) {
                notes[nn] = (struct sel_note){ st->event, st->hwnd, st->msg, st->s, fired, { 0 } };
                memcpy(notes[nn].errs, st->errs, sizeof st->errs);
                nn++;
            }
        }
        pthread_mutex_unlock(&g_lock);
        for (size_t i = 0; i < nn; i++) sel_notify(&notes[i]);
        free(notes);
        for (size_t i = 1; i < n; i++) sock_put(sts[i]);
    }
    return NULL;
}

static void sel_start(void)
{
    g_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_wake_fd < 0) return;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    g_sel_thread_ok = pthread_create(&t, &at, sel_thread, NULL) == 0;
    pthread_attr_destroy(&at);
}

/* Shared by WSAEventSelect and WSAAsyncSelect. */
static int sel_set(SOCKET s, HANDLE event, HWND hwnd, UINT msg, LONG events)
{
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    if (events) {
        pthread_once(&g_sel_once, sel_start);
        if (!g_sel_thread_ok) { sock_put(st); set_wsa(WSAENOBUFS); return SOCKET_ERROR; }
    }
    pthread_mutex_lock(&g_lock);
    /* a new selection cancels the previous one and clears the record */
    st->pending = 0;
    memset(st->errs, 0, sizeof st->errs);
    st->mask = events & FD_ALL;
    st->event = events ? event : NULL;
    st->hwnd = events ? hwnd : NULL;
    st->msg = msg;
    st->armed = st->mask;
    st->quiet = 0;
    if (events && !st->selected && !st->closed) { st->sel_next = g_sel; g_sel = st; st->selected = 1; }
    if (!events) sel_unlink_locked(st);
    pthread_mutex_unlock(&g_lock);
    if (events) set_nonblock(st, 1);   /* Windows makes the socket non-blocking */
    sel_wake();
    sock_put(st);
    return 0;
}

API int WSAEventSelect(SOCKET s, HANDLE hEventObject, LONG lNetworkEvents)
{
    NEED_INIT(SOCKET_ERROR);
    return sel_set(s, hEventObject, NULL, 0, lNetworkEvents);
}

/* Window-message notification works when user32 provides real windows;
 * without them Windows' answer for a bad hWnd is WSAEINVAL. */
API int WSAAsyncSelect(SOCKET s, HWND hWnd, UINT wMsg, LONG lEvent)
{
    NEED_INIT(SOCKET_ERROR);
    if (lEvent) {
        pf_IsWindow isw = (pf_IsWindow)dll_sym("user32.dll", "IsWindow");
        if (!isw || !dll_sym("user32.dll", "PostMessageW") || !isw(hWnd)) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
    }
    return sel_set(s, NULL, hWnd, wMsg, lEvent);
}

typedef struct { LONG lNetworkEvents; INT iErrorCode[FD_MAX_EVENTS]; } WSANETWORKEVENTS;

API int WSAEnumNetworkEvents(SOCKET s, HANDLE hEventObject, WSANETWORKEVENTS *ne)
{
    NEED_INIT(SOCKET_ERROR);
    if (!ne) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    pthread_mutex_lock(&g_lock);
    ne->lNetworkEvents = st->pending;
    memcpy(ne->iErrorCode, st->errs, sizeof ne->iErrorCode);
    st->pending = 0;
    memset(st->errs, 0, sizeof st->errs);
    if (hEventObject) ev_reset(hEventObject);
    pthread_mutex_unlock(&g_lock);
    sock_put(st);
    return 0;
}

API HANDLE WSACreateEvent(void)
{
    HANDLE h = ev_create();
    if (!h) set_wsa(WSA_NOT_ENOUGH_MEMORY);
    return h;
}
API BOOL WSACloseEvent(HANDLE h) { if (!h_close(h)) { set_wsa(WSA_INVALID_HANDLE); return FALSE; } return TRUE; }
API BOOL WSASetEvent(HANDLE h)   { if (!ev_set(h)) { set_wsa(WSA_INVALID_HANDLE); return FALSE; } return TRUE; }
API BOOL WSAResetEvent(HANDLE h) { if (!ev_reset(h)) { set_wsa(WSA_INVALID_HANDLE); return FALSE; } return TRUE; }

API DWORD WSAWaitForMultipleEvents(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable)
{
    if (!n || n > WSA_MAXIMUM_WAIT_EVENTS || !hs) { set_wsa(WSA_INVALID_PARAMETER); return WSA_WAIT_FAILED; }
    return wait_multi(n, hs, all, ms, alertable);
}

/* ------------------------------------------------------------------ */
/* Startup, sockets, connections                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    WORD  wVersion;
    WORD  wHighVersion;
    USHORT iMaxSockets;        /* _WIN64 order: these come before the strings */
    USHORT iMaxUdpDg;
    char *lpVendorInfo;
    char  szDescription[257];
    char  szSystemStatus[129];
} WSADATA;
_Static_assert(sizeof(WSADATA) == 408, "WSADATA layout");

API int WSAStartup(WORD req, WSADATA *data)
{
    BYTE major = req & 0xFF, minor = req >> 8;
    if (!data) return WSAEFAULT;
    memset(data, 0, sizeof *data);
    data->wHighVersion = 0x0202;
    strcpy(data->szDescription, "WinSock 2.0");
    strcpy(data->szSystemStatus, "Running");
    if (major < 1) { data->wVersion = 0x0202; return WSAVERNOTSUPPORTED; }
    /* versions 1.0 to 2.2 are granted as asked; anything newer gets 2.2 */
    if (major > 2 || (major == 2 && minor > 2)) data->wVersion = 0x0202;
    else data->wVersion = req;
    if (major == 1) { data->iMaxSockets = 32767; data->iMaxUdpDg = 65467; }
    __atomic_add_fetch(&g_started, 1, __ATOMIC_ACQ_REL);
    return 0;
}

static int close_socket(SOCKET s);

API int WSACleanup(void)
{
    int n = __atomic_load_n(&g_started, __ATOMIC_ACQUIRE);
    if (n <= 0) { set_wsa(WSANOTINITIALISED); return SOCKET_ERROR; }
    if (__atomic_sub_fetch(&g_started, 1, __ATOMIC_ACQ_REL) == 0) {
        /* the last cleanup resets and frees the sockets still open */
        SOCKET *list = NULL; size_t cnt = 0, cap = 0;
        pthread_mutex_lock(&g_lock);
        for (long i = 0; i < MAX_SOCKS; i++) {
            if (!g_socks[i]) continue;
            if (cnt == cap) { cap = cap ? cap * 2 : 64; SOCKET *nl = realloc(list, cap * sizeof *list); if (!nl) break; list = nl; }
            list[cnt++] = g_socks[i]->s;
        }
        pthread_mutex_unlock(&g_lock);
        for (size_t i = 0; i < cnt; i++) close_socket(list[i]);
        free(list);
    }
    return 0;
}

API int WSAGetLastError(void) { return (int)lsiw_get_last_error(); }
API void WSASetLastError(int e) { lsiw_set_last_error((uint32_t)e); }

/* WSAPROTOCOL_INFO (exact Windows layout) */
#define WSAPROTOCOL_LEN 255
typedef struct { int ChainLen; DWORD ChainEntries[7]; } WSAPROTOCOLCHAIN;
typedef struct {
    DWORD dwServiceFlags1, dwServiceFlags2, dwServiceFlags3, dwServiceFlags4;
    DWORD dwProviderFlags;
    GUID  ProviderId;
    DWORD dwCatalogEntryId;
    WSAPROTOCOLCHAIN ProtocolChain;
    int   iVersion, iAddressFamily, iMaxSockAddr, iMinSockAddr, iSocketType, iProtocol;
    int   iProtocolMaxOffset, iNetworkByteOrder, iSecurityScheme;
    DWORD dwMessageSize, dwProviderReserved;
    WCHAR szProtocol[WSAPROTOCOL_LEN + 1];
} WSAPROTOCOL_INFOW;
typedef struct {
    DWORD dwServiceFlags1, dwServiceFlags2, dwServiceFlags3, dwServiceFlags4;
    DWORD dwProviderFlags;
    GUID  ProviderId;
    DWORD dwCatalogEntryId;
    WSAPROTOCOLCHAIN ProtocolChain;
    int   iVersion, iAddressFamily, iMaxSockAddr, iMinSockAddr, iSocketType, iProtocol;
    int   iProtocolMaxOffset, iNetworkByteOrder, iSecurityScheme;
    DWORD dwMessageSize, dwProviderReserved;
    char  szProtocol[WSAPROTOCOL_LEN + 1];
} WSAPROTOCOL_INFOA;
_Static_assert(sizeof(WSAPROTOCOL_INFOW) == 628, "WSAPROTOCOL_INFOW layout");
_Static_assert(sizeof(WSAPROTOCOL_INFOA) == 372, "WSAPROTOCOL_INFOA layout");

#define WSA_FLAG_OVERLAPPED         0x01
#define WSA_FLAG_NO_HANDLE_INHERIT  0x80

static SOCKET make_socket(int af, int type, int proto)
{
    int laf = af_to_linux(af);
    if (laf < 0 || laf == AF_UNSPEC) {
        if (af != WS_AF_UNSPEC || !proto) { set_wsa(WSAEAFNOSUPPORT); return INVALID_SOCKET; }
        laf = AF_INET;               /* AF_UNSPEC with a protocol: Windows picks IPv4 */
    }
    if (type < 1 || type > 5) { set_wsa(WSAESOCKTNOSUPPORT); return INVALID_SOCKET; }
    int fd = socket(laf, type | SOCK_CLOEXEC, proto);
    if (fd < 0) { set_wsa_errno(errno); return INVALID_SOCKET; }
    int one = 1;
    /* Windows defaults: IPv6 sockets are IPv6-only, and a TCP listener can
     * rebind a port that still has connections in TIME_WAIT. */
    if (laf == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    if (type == SOCK_STREAM && (laf == AF_INET || laf == AF_INET6))
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (!proto) {
        socklen_t l = sizeof proto;
        getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &proto, &l);
    }
    return sock_new(fd, laf, type, proto, NULL);
}

API SOCKET ws_socket(int af, int type, int protocol)
{
    NEED_INIT(INVALID_SOCKET);
    return make_socket(af, type, protocol);
}

API SOCKET WSASocketW(int af, int type, int protocol, const WSAPROTOCOL_INFOW *info, DWORD group, DWORD flags)
{
    (void)group; (void)flags;      /* overlapped is always available; handles never inherit */
    NEED_INIT(INVALID_SOCKET);
    if (info) {
        if (af == FROM_PROTOCOL_INFO) af = info->iAddressFamily;
        if (type == FROM_PROTOCOL_INFO) type = info->iSocketType;
        if (protocol == FROM_PROTOCOL_INFO) protocol = info->iProtocol;
    }
    return make_socket(af, type, protocol);
}

API SOCKET WSASocketA(int af, int type, int protocol, const WSAPROTOCOL_INFOA *info, DWORD group, DWORD flags)
{
    (void)group; (void)flags;
    NEED_INIT(INVALID_SOCKET);
    if (info) {
        if (af == FROM_PROTOCOL_INFO) af = info->iAddressFamily;
        if (type == FROM_PROTOCOL_INFO) type = info->iSocketType;
        if (protocol == FROM_PROTOCOL_INFO) protocol = info->iProtocol;
    }
    return make_socket(af, type, protocol);
}

static int close_socket(SOCKET s)
{
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    long i = sock_index(s);
    struct ws_sock *reg = NULL;
    pthread_mutex_lock(&g_lock);
    if (g_socks[i] == st) reg = sock_detach_locked(i);
    pthread_mutex_unlock(&g_lock);
    sel_wake();
    /* Linux does not wake a thread blocked on an fd that gets closed;
     * Windows aborts such calls, so shut the socket down first. */
    if (__atomic_load_n(&st->busy, __ATOMIC_ACQUIRE) > 0) shutdown(st->fd, SHUT_RDWR);
    lsiw_handle_close((HANDLE)(uintptr_t)s);
    if (reg) sock_put(reg);
    sock_put(st);
    return 0;
}

API int ws_closesocket(SOCKET s)
{
    NEED_INIT(SOCKET_ERROR);
    return close_socket(s);
}

API int ws_bind(SOCKET s, const void *name, int namelen)
{
    NEED_INIT(SOCKET_ERROR);
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    union ws_sa a;
    int len = sa_from_win(name, namelen, &a), r = SOCKET_ERROR;
    if (len >= 0) {
        if (a.sa.sa_family != st->af && !(st->af == AF_INET6 && a.sa.sa_family == AF_INET)) set_wsa(WSAEFAULT);
        else if (bind(st->fd, &a.sa, (socklen_t)len) == 0) r = 0;
        else set_wsa(errno == EINVAL ? WSAEINVAL : wsa_err(errno));
    }
    sock_put(st);
    return r;
}

API int ws_listen(SOCKET s, int backlog)
{
    NEED_INIT(SOCKET_ERROR);
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    int r = SOCKET_ERROR;
    union ws_sa a; socklen_t al = sizeof a;
    if (st->state == ST_CONNECTED) { set_wsa(WSAEISCONN); goto out; }
    /* Linux would bind an unbound socket to a random port; Windows refuses */
    if (getsockname(st->fd, &a.sa, &al) == 0 &&
        ((a.sa.sa_family == AF_UNIX && al <= sizeof(sa_family_t)) ||
         ((a.sa.sa_family == AF_INET || a.sa.sa_family == AF_INET6) && sa_port(&a) == 0))) {
        set_wsa(WSAEINVAL); goto out;
    }
    if (backlog < 0) backlog = backlog == INT_MIN ? SOMAXCONN : -backlog;   /* SOMAXCONN_HINT(n) is -n */
    if (backlog > 4096) backlog = SOMAXCONN;
    if (listen(st->fd, backlog) == 0) {
        r = 0;
        pthread_mutex_lock(&g_lock);
        st->state = ST_LISTENING;
        pthread_mutex_unlock(&g_lock);
        sel_rearm(st, FD_ACCEPT);
    } else set_wsa_errno(errno);
out:
    sock_put(st);
    return r;
}

/* Accept a connection; with force_block the call waits even on a
 * non-blocking listener (used by the overlapped AcceptEx). Returns the new fd. */
static int accept_fd(struct ws_sock *st, union ws_sa *a, socklen_t *al, int force_block)
{
    for (;;) {
        __atomic_add_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
        int fd = accept4(st->fd, &a->sa, al, SOCK_CLOEXEC);
        __atomic_sub_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
        if (fd >= 0) { sel_rearm(st, FD_ACCEPT); return fd; }
        int e = errno;
        if (e == EINTR) continue;
        if (e == EAGAIN && force_block) { wait_fd(st->fd, POLLIN, -1); continue; }
        if (e == ECONNABORTED && (force_block || !st->nonblock)) continue;
        sel_rearm(st, FD_ACCEPT);
        set_wsa(e == ECONNABORTED ? WSAECONNRESET : e == EAGAIN && !st->nonblock ? WSAETIMEDOUT : wsa_err(e));
        return -1;
    }
}

/* The accepted socket takes the listener's properties, including its
 * non-blocking mode and its WSAEventSelect / WSAAsyncSelect selection. */
static void inherit_listener(struct ws_sock *ls, struct ws_sock *ns)
{
    ns->state = ST_CONNECTED;
    now_mono(&ns->since);
    if (ls->nonblock) set_nonblock(ns, 1);
    pthread_mutex_lock(&g_lock);
    if (ls->mask && !ns->closed) {
        ns->mask = ls->mask; ns->armed = ls->mask;
        ns->event = ls->event; ns->hwnd = ls->hwnd; ns->msg = ls->msg;
        if (!ns->selected) { ns->sel_next = g_sel; g_sel = ns; ns->selected = 1; }
    }
    pthread_mutex_unlock(&g_lock);
    if (ns->mask) sel_wake();
}

static SOCKET do_accept(SOCKET s, void *addr, INT *addrlen)
{
    struct ws_sock *st = sock_get(s), *ns = NULL;
    if (!st) return INVALID_SOCKET;
    SOCKET r = INVALID_SOCKET;
    if (addr && (!addrlen || *addrlen < win_sa_min(st->af))) { set_wsa(WSAEFAULT); goto out; }
    union ws_sa a; socklen_t al = sizeof a;
    int fd = accept_fd(st, &a, &al, 0);
    if (fd < 0) goto out;
    r = sock_new(fd, st->af, st->type, st->protocol, &ns);
    if (r == INVALID_SOCKET) goto out;
    inherit_listener(st, ns);
    if (addr) sa_out(&a, al, addr, addrlen);
    else if (addrlen) { union ws_sa w; *addrlen = sa_to_win_buf(&a, al, &w); }
    sock_put(ns);
out:
    sock_put(st);
    return r;
}

API SOCKET ws_accept(SOCKET s, void *addr, INT *addrlen)
{
    NEED_INIT(INVALID_SOCKET);
    return do_accept(s, addr, addrlen);
}

/* WSAAccept's condition function decides after the connection is taken
 * from the queue: CF_REJECT closes it (WSAECONNREFUSED) and CF_DEFER, which
 * cannot be honoured afterwards, reports WSATRY_AGAIN. */
typedef int (WINAPI *LPCONDITIONPROC)(WSABUF *, WSABUF *, void *, void *, WSABUF *, WSABUF *, UINT *, ULONG_PTR);

API SOCKET WSAAccept(SOCKET s, void *addr, INT *addrlen, LPCONDITIONPROC cond, ULONG_PTR data)
{
    NEED_INIT(INVALID_SOCKET);
    union ws_sa peer; INT plen = sizeof peer;
    SOCKET ns = do_accept(s, &peer, &plen);
    if (ns == INVALID_SOCKET) return ns;
    if (cond) {
        union ws_sa local; INT llen = sizeof local;
        union ws_sa la; socklen_t lal = sizeof la;
        struct ws_sock *nst = sock_get(ns);
        if (nst && getsockname(nst->fd, &la.sa, &lal) == 0) sa_out(&la, lal, &local, &llen); else llen = 0;
        sock_put(nst);
        WSABUF caller = { (ULONG)plen, (char *)&peer }, callee = { (ULONG)llen, (char *)&local };
        UINT g = 0;
        int v = cond(&caller, NULL, NULL, NULL, &callee, NULL, &g, data);
        if (v != 0) {
            close_socket(ns);
            set_wsa(v == 2 ? WSATRY_AGAIN : WSAECONNREFUSED);
            return INVALID_SOCKET;
        }
    }
    if (addr) {
        if (!addrlen || *addrlen < plen) { close_socket(ns); set_wsa(WSAEFAULT); return INVALID_SOCKET; }
        memcpy(addr, &peer, (size_t)plen);
        *addrlen = plen;
    } else if (addrlen) *addrlen = plen;
    return ns;
}

/* Wait for a connect in progress; ms < 0 waits forever. Returns 0 or a WSA code. */
static DWORD connect_wait(struct ws_sock *st, int ms)
{
    __atomic_add_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
    int r = wait_fd(st->fd, POLLOUT, ms);
    __atomic_sub_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
    if (r == 0) return WSAETIMEDOUT;
    pthread_mutex_lock(&g_lock);
    st->connecting = 1;
    DWORD e = resolve_connect_locked(st);
    pthread_mutex_unlock(&g_lock);
    if (e == (DWORD)-1) return connect_wait(st, ms);
    if (e) { st->conn_failed = 0; st->conn_err = 0; }   /* reported directly */
    return e;
}

/* connect with Windows rules; force_block waits even in non-blocking mode. */
static int do_connect(struct ws_sock *st, const void *name, int namelen, int force_block, int timeout_ms)
{
    union ws_sa a;
    int len = sa_from_win(name, namelen, &a);
    if (len < 0) return -1;
    if (a.sa.sa_family == AF_INET || a.sa.sa_family == AF_INET6) {
        if (st->type != SOCK_STREAM && sa_is_any(&a) && sa_port(&a) == 0) {
            a.sa.sa_family = AF_UNSPEC;                /* all zeros dissolves a UDP association */
        } else if (sa_port(&a) == 0 || (st->type == SOCK_STREAM && sa_is_any(&a))) {
            set_wsa(WSAEADDRNOTAVAIL);
            return -1;
        }
    }
    if (st->state == ST_LISTENING) { set_wsa(WSAEINVAL); return -1; }
    st->conn_failed = 0;
    st->conn_err = 0;
    __atomic_add_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
    int r = connect(st->fd, &a.sa, (socklen_t)len);
    int e = errno;
    __atomic_sub_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
    if (r == 0) {
        if (st->type == SOCK_STREAM || a.sa.sa_family != AF_UNSPEC) {
            pthread_mutex_lock(&g_lock);
            st->state = st->type == SOCK_STREAM ? ST_CONNECTED : st->state;
            now_mono(&st->since);
            pthread_mutex_unlock(&g_lock);
        }
        return 0;
    }
    if (e == EINPROGRESS || e == EINTR) {
        if (e == EINPROGRESS && st->nonblock && !force_block) {
            pthread_mutex_lock(&g_lock);
            st->connecting = 1;
            st->state = ST_CONNECTING;
            pthread_mutex_unlock(&g_lock);
            sel_wake();
            set_wsa(WSAEWOULDBLOCK);
            return -1;
        }
        DWORD w = connect_wait(st, timeout_ms);
        if (w) { set_wsa(w); return -1; }
        return 0;
    }
    if (e == ENOENT && st->af == AF_UNIX) { set_wsa(WSAECONNREFUSED); return -1; }
    set_wsa_errno(e);
    return -1;
}

API int ws_connect(SOCKET s, const void *name, int namelen)
{
    NEED_INIT(SOCKET_ERROR);
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    int r = do_connect(st, name, namelen, 0, -1) == 0 ? 0 : SOCKET_ERROR;
    sock_put(st);
    return r;
}

/* Caller/callee data and QoS are not supported by the TCP/IP provider either. */
API int WSAConnect(SOCKET s, const void *name, int namelen, WSABUF *caller, WSABUF *callee, void *sqos, void *gqos)
{
    (void)caller; (void)sqos; (void)gqos;
    if (callee) callee->len = 0;
    return ws_connect(s, name, namelen);
}

API int ws_shutdown(SOCKET s, int how)
{
    NEED_INIT(SOCKET_ERROR);
    if (how < 0 || how > 2) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    int r = 0;
    if (shutdown(st->fd, how) < 0) { set_wsa_errno(errno); r = SOCKET_ERROR; }   /* SD_* == SHUT_* */
    else if (how != 0) st->shut_send = 1;
    sock_put(st);
    return r;
}

API int WSASendDisconnect(SOCKET s, WSABUF *data) { (void)data; return ws_shutdown(s, 1); }
API int WSARecvDisconnect(SOCKET s, WSABUF *data) { (void)data; return ws_shutdown(s, 0); }

API int ws_getsockname(SOCKET s, void *name, INT *namelen)
{
    NEED_INIT(SOCKET_ERROR);
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    union ws_sa a; socklen_t al = sizeof a;
    int r = SOCKET_ERROR;
    if (getsockname(st->fd, &a.sa, &al) < 0) set_wsa_errno(errno);
    /* an unbound socket has no name on Windows */
    else if ((a.sa.sa_family == AF_INET || a.sa.sa_family == AF_INET6) && sa_port(&a) == 0 && sa_is_any(&a)
             && st->state != ST_CONNECTED) set_wsa(WSAEINVAL);
    else if (sa_out(&a, al, name, namelen) == 0) r = 0;
    sock_put(st);
    return r;
}

API int ws_getpeername(SOCKET s, void *name, INT *namelen)
{
    NEED_INIT(SOCKET_ERROR);
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    union ws_sa a; socklen_t al = sizeof a;
    int r = SOCKET_ERROR;
    if (getpeername(st->fd, &a.sa, &al) < 0) set_wsa_errno(errno);
    else if (sa_out(&a, al, name, namelen) == 0) r = 0;
    sock_put(st);
    return r;
}

/* ------------------------------------------------------------------ */
/* Sending and receiving                                                */
/* ------------------------------------------------------------------ */

static int flags_in(DWORD f)
{
    int r = 0;
    if (f & WS_MSG_OOB)       r |= MSG_OOB;
    if (f & WS_MSG_PEEK)      r |= MSG_PEEK;
    if (f & WS_MSG_DONTROUTE) r |= MSG_DONTROUTE;
    if (f & WS_MSG_WAITALL)   r |= MSG_WAITALL;
    return r;
}

static DWORD flags_out(int f)
{
    DWORD r = 0;
    if (f & MSG_TRUNC)  r |= WS_MSG_TRUNC;
    if (f & MSG_CTRUNC) r |= WS_MSG_CTRUNC;
    if (f & MSG_OOB)    r |= WS_MSG_OOB;
    return r;
}

/* recvmsg with Windows error semantics. force_block waits for data even on
 * a non-blocking socket (overlapped operations never "would block").
 * Returns bytes, or -1 with the WSA error set. */
static ssize_t do_recvmsg(struct ws_sock *st, struct msghdr *mh, int flags, int force_block)
{
    for (;;) {
        __atomic_add_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
        ssize_t r = recvmsg(st->fd, mh, flags | MSG_CMSG_CLOEXEC);
        int e = errno;
        __atomic_sub_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
        if (r >= 0) { sel_rearm(st, FD_READ | FD_OOB); return r; }
        if (e == EINTR) continue;
        if (e == EAGAIN && force_block) { wait_fd(st->fd, POLLIN, -1); continue; }
        /* an ICMP port-unreachable on UDP: WSAECONNRESET unless disabled by SIO_UDP_CONNRESET */
        if (e == ECONNREFUSED && st->type != SOCK_STREAM) {
            if (!st->udp_connreset) continue;
            e = ECONNRESET;
        }
        sel_rearm(st, FD_READ | FD_OOB);
        if (e == EAGAIN && !st->nonblock) set_wsa(WSAETIMEDOUT);   /* SO_RCVTIMEO expired */
        else if (e == ENOTCONN && st->type == SOCK_STREAM) set_wsa(WSAENOTCONN);
        else set_wsa_errno(e);
        return -1;
    }
}

static ssize_t do_sendmsg(struct ws_sock *st, struct msghdr *mh, int flags, int force_block)
{
    for (;;) {
        __atomic_add_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
        ssize_t r = sendmsg(st->fd, mh, flags | MSG_NOSIGNAL);
        int e = errno;
        __atomic_sub_fetch(&st->busy, 1, __ATOMIC_ACQ_REL);
        if (r >= 0) return r;
        if (e == EINTR) continue;
        if (e == EAGAIN && force_block) { wait_fd(st->fd, POLLOUT, -1); continue; }
        if (e == EAGAIN) {
            sel_rearm(st, FD_WRITE);       /* FD_WRITE comes back once there is room */
            set_wsa(st->nonblock ? WSAEWOULDBLOCK : WSAETIMEDOUT);
        } else if (e == EPIPE && st->shut_send) set_wsa(WSAESHUTDOWN);
        else if (e == ECONNREFUSED && st->type != SOCK_STREAM) set_wsa(WSAECONNRESET);
        else set_wsa_errno(e);
        return -1;
    }
}

/* Build an iovec array from WSABUFs. */
static struct iovec *iov_from(const WSABUF *b, DWORD n, struct iovec *stack, size_t nstack)
{
    struct iovec *v = n <= nstack ? stack : malloc(n * sizeof *v);
    if (!v) return NULL;
    for (DWORD i = 0; i < n; i++) { v[i].iov_base = b[i].buf; v[i].iov_len = b[i].len; }
    return v;
}

/* Common receive: returns bytes or -1. `from`/`fromlen` are Windows. */
static int recv_common(SOCKET s, const WSABUF *bufs, DWORD nbufs, DWORD *flagsp, void *from, INT *fromlen,
                       int force_block, int partial_ok, DWORD *outflags)
{
    struct ws_sock *st = sock_get(s);
    if (!st) return -1;
    int ret = -1;
    if (from && (!fromlen || *fromlen < win_sa_min(st->af))) { set_wsa(WSAEFAULT); goto out; }
    struct iovec stack[16];
    struct iovec *iov = iov_from(bufs, nbufs, stack, 16);
    if (!iov) { set_wsa(WSAENOBUFS); goto out; }
    union ws_sa a; memset(&a, 0, sizeof a);
    struct msghdr mh = { 0 };
    mh.msg_name = from ? &a : NULL;
    mh.msg_namelen = from ? sizeof a : 0;
    mh.msg_iov = iov;
    mh.msg_iovlen = nbufs;
    ssize_t r = do_recvmsg(st, &mh, flags_in(flagsp ? *flagsp : 0), force_block);
    if (iov != stack) free(iov);
    if (r < 0) goto out;
    if (from) {
        if (mh.msg_namelen) sa_out(&a, mh.msg_namelen, from, fromlen);
        else *fromlen = 0;
    }
    if (outflags) *outflags = flags_out(mh.msg_flags);
    ret = (int)r;
    /* a datagram larger than the buffer: Windows keeps the first part and fails */
    if ((mh.msg_flags & MSG_TRUNC) && st->type != SOCK_STREAM) {
        if (partial_ok) { if (outflags) *outflags |= WS_MSG_PARTIAL; }
        else { set_wsa(WSAEMSGSIZE); ret = -2 - ret; }
    }
out:
    sock_put(st);
    return ret;
}

static int send_common(SOCKET s, const WSABUF *bufs, DWORD nbufs, DWORD flags, const void *to, int tolen, int force_block)
{
    struct ws_sock *st = sock_get(s);
    if (!st) return -1;
    int ret = -1;
    union ws_sa a;
    int alen = 0;
    /* for a connection-oriented socket the destination is ignored */
    if (to && st->type != SOCK_STREAM) {
        alen = sa_from_win(to, tolen, &a);
        if (alen < 0) goto out;
    }
    struct iovec stack[16];
    struct iovec *iov = iov_from(bufs, nbufs, stack, 16);
    if (!iov) { set_wsa(WSAENOBUFS); goto out; }
    struct msghdr mh = { 0 };
    mh.msg_name = alen ? &a : NULL;
    mh.msg_namelen = (socklen_t)alen;
    mh.msg_iov = iov;
    mh.msg_iovlen = nbufs;
    ssize_t r = do_sendmsg(st, &mh, flags_in(flags) & ~MSG_WAITALL, force_block);
    if (iov != stack) free(iov);
    if (r >= 0) ret = (int)r;
out:
    sock_put(st);
    return ret;
}

API int ws_recv(SOCKET s, char *buf, int len, int flags)
{
    NEED_INIT(SOCKET_ERROR);
    WSABUF b = { (ULONG)(len < 0 ? 0 : len), buf };
    DWORD f = (DWORD)flags;
    int r = recv_common(s, &b, 1, &f, NULL, NULL, 0, 0, NULL);
    return r < 0 ? SOCKET_ERROR : r;
}

API int ws_recvfrom(SOCKET s, char *buf, int len, int flags, void *from, INT *fromlen)
{
    NEED_INIT(SOCKET_ERROR);
    WSABUF b = { (ULONG)(len < 0 ? 0 : len), buf };
    DWORD f = (DWORD)flags;
    int r = recv_common(s, &b, 1, &f, from, fromlen, 0, 0, NULL);
    return r < 0 ? SOCKET_ERROR : r;
}

/* WSARecvEx (mswsock/wsock32): a truncated datagram returns the part that
 * fitted and sets MSG_PARTIAL instead of failing. */
API int WSARecvEx(SOCKET s, char *buf, int len, int *flags)
{
    NEED_INIT(SOCKET_ERROR);
    WSABUF b = { (ULONG)(len < 0 ? 0 : len), buf };
    DWORD f = flags ? (DWORD)*flags : 0, of = 0;
    int r = recv_common(s, &b, 1, &f, NULL, NULL, 0, 1, &of);
    if (r < 0) return SOCKET_ERROR;
    if (flags) *flags = (int)(of & WS_MSG_PARTIAL);
    return r;
}

API int ws_send(SOCKET s, const char *buf, int len, int flags)
{
    NEED_INIT(SOCKET_ERROR);
    WSABUF b = { (ULONG)(len < 0 ? 0 : len), (char *)buf };
    int r = send_common(s, &b, 1, (DWORD)flags, NULL, 0, 0);
    return r < 0 ? SOCKET_ERROR : r;
}

API int ws_sendto(SOCKET s, const char *buf, int len, int flags, const void *to, int tolen)
{
    NEED_INIT(SOCKET_ERROR);
    WSABUF b = { (ULONG)(len < 0 ? 0 : len), (char *)buf };
    int r = send_common(s, &b, 1, (DWORD)flags, to, tolen, 0);
    return r < 0 ? SOCKET_ERROR : r;
}

static int wsarecv_impl(SOCKET s, WSABUF *bufs, DWORD n, DWORD *got, DWORD *flags, void *from, INT *fromlen,
                        WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    NEED_INIT(SOCKET_ERROR);
    if (!bufs && n) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    DWORD of = 0;
    int r = recv_common(s, bufs, n, flags, from, fromlen, ov != NULL, 0, &of);
    if (r == -1) return SOCKET_ERROR;
    DWORD bytes = r >= 0 ? (DWORD)r : (DWORD)(-2 - r);
    if (got) *got = bytes;
    if (flags) *flags = of;
    if (r < -1) return SOCKET_ERROR;          /* WSAEMSGSIZE, part of the datagram delivered */
    ov_complete(s, ov, 0, bytes, of, cr);
    return 0;
}

API int WSARecv(SOCKET s, WSABUF *bufs, DWORD n, DWORD *got, DWORD *flags, WSAOVERLAPPED *ov,
                LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    return wsarecv_impl(s, bufs, n, got, flags, NULL, NULL, ov, cr);
}

API int WSARecvFrom(SOCKET s, WSABUF *bufs, DWORD n, DWORD *got, DWORD *flags, void *from, INT *fromlen,
                    WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    return wsarecv_impl(s, bufs, n, got, flags, from, fromlen, ov, cr);
}

static int wsasend_impl(SOCKET s, WSABUF *bufs, DWORD n, DWORD *sent, DWORD flags, const void *to, int tolen,
                        WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    NEED_INIT(SOCKET_ERROR);
    if (!bufs && n) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    int r = send_common(s, bufs, n, flags, to, tolen, ov != NULL);
    if (r < 0) return SOCKET_ERROR;
    if (sent) *sent = (DWORD)r;
    ov_complete(s, ov, 0, (DWORD)r, 0, cr);
    return 0;
}

API int WSASend(SOCKET s, WSABUF *bufs, DWORD n, DWORD *sent, DWORD flags, WSAOVERLAPPED *ov,
                LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    return wsasend_impl(s, bufs, n, sent, flags, NULL, 0, ov, cr);
}

API int WSASendTo(SOCKET s, WSABUF *bufs, DWORD n, DWORD *sent, DWORD flags, const void *to, int tolen,
                  WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    return wsasend_impl(s, bufs, n, sent, flags, to, tolen, ov, cr);
}

API BOOL WSAGetOverlappedResult(SOCKET s, WSAOVERLAPPED *ov, DWORD *bytes, BOOL wait, DWORD *flags)
{
    (void)wait;
    NEED_INIT(FALSE);
    struct ws_sock *st = sock_get(s);
    if (!st) return FALSE;
    sock_put(st);
    if (!ov) { set_wsa(WSAEFAULT); return FALSE; }
    if (ov->Internal == STATUS_PENDING_) { set_wsa(WSA_IO_INCOMPLETE); return FALSE; }
    if (bytes) *bytes = (DWORD)ov->InternalHigh;
    if (flags) *flags = ov->u.s.Offset;
    if (ov->Internal) { set_wsa(status_to_wsa((DWORD)ov->Internal)); return FALSE; }
    return TRUE;
}

/* ---- WSARecvMsg / WSASendMsg and control-message translation ---- */

typedef struct {
    void   *name;
    INT     namelen;
    WSABUF *lpBuffers;
    ULONG   dwBufferCount;
    WSABUF  Control;
    ULONG   dwFlags;
} WSAMSG;
_Static_assert(sizeof(WSAMSG) == 56, "WSAMSG layout");

/* WSACMSGHDR has the same layout and alignment as the Linux cmsghdr on
 * x86-64 (size_t length, two ints, data aligned to 8); only the levels,
 * types and payloads differ. */
#define WS_IP_PKTINFO     19
#define WS_IPV6_PKTINFO   19
#define WS_IP_HOPLIMIT    21
#define WS_IPV6_HOPLIMIT  21
#define WS_IPV6_TCLASS    39

struct ws_in_pktinfo  { struct in_addr ipi_addr; ULONG ipi_ifindex; };
struct ws_in6_pktinfo { struct in6_addr ipi6_addr; ULONG ipi6_ifindex; };

static size_t cmsg_put(char *buf, size_t cap, size_t off, int level, int type, const void *data, size_t len, int *trunc)
{
    size_t space = CMSG_SPACE(len);
    if (off + space > cap) { *trunc = 1; return off; }
    struct cmsghdr *h = (struct cmsghdr *)(buf + off);
    h->cmsg_len = CMSG_LEN(len);
    h->cmsg_level = level;
    h->cmsg_type = type;
    memcpy(CMSG_DATA(h), data, len);
    return off + space;
}

static size_t cmsg_to_win(struct msghdr *mh, char *out, size_t cap, int *trunc)
{
    size_t off = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo pi; memcpy(&pi, CMSG_DATA(c), sizeof pi);
            struct ws_in_pktinfo w = { pi.ipi_addr, (ULONG)pi.ipi_ifindex };
            off = cmsg_put(out, cap, off, WS_IPPROTO_IP, WS_IP_PKTINFO, &w, sizeof w, trunc);
        } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo pi; memcpy(&pi, CMSG_DATA(c), sizeof pi);
            struct ws_in6_pktinfo w = { pi.ipi6_addr, pi.ipi6_ifindex };
            off = cmsg_put(out, cap, off, WS_IPPROTO_IPV6, WS_IPV6_PKTINFO, &w, sizeof w, trunc);
        } else if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL) {
            int v; memcpy(&v, CMSG_DATA(c), sizeof v);
            off = cmsg_put(out, cap, off, WS_IPPROTO_IP, WS_IP_HOPLIMIT, &v, sizeof v, trunc);
        } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_HOPLIMIT) {
            int v; memcpy(&v, CMSG_DATA(c), sizeof v);
            off = cmsg_put(out, cap, off, WS_IPPROTO_IPV6, WS_IPV6_HOPLIMIT, &v, sizeof v, trunc);
        } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS) {
            int v; memcpy(&v, CMSG_DATA(c), sizeof v);
            off = cmsg_put(out, cap, off, WS_IPPROTO_IPV6, WS_IPV6_TCLASS, &v, sizeof v, trunc);
        }
    }
    return off;
}

/* Windows control data for sending -> Linux; only source selection is
 * meaningful (IP_PKTINFO / IPV6_PKTINFO). Returns the Linux length. */
static size_t cmsg_from_win(const WSABUF *ctl, char *out, size_t cap)
{
    if (!ctl || !ctl->buf || ctl->len < sizeof(struct cmsghdr)) return 0;
    struct msghdr wm = { 0 };
    wm.msg_control = ctl->buf;
    wm.msg_controllen = ctl->len;
    size_t off = 0;
    int trunc = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&wm); c; c = CMSG_NXTHDR(&wm, c)) {
        if (c->cmsg_level == WS_IPPROTO_IP && c->cmsg_type == WS_IP_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(struct ws_in_pktinfo))) {
            struct ws_in_pktinfo w; memcpy(&w, CMSG_DATA(c), sizeof w);
            struct in_pktinfo pi = { (int)w.ipi_ifindex, w.ipi_addr, { 0 } };
            off = cmsg_put(out, cap, off, IPPROTO_IP, IP_PKTINFO, &pi, sizeof pi, &trunc);
        } else if (c->cmsg_level == WS_IPPROTO_IPV6 && c->cmsg_type == WS_IPV6_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(struct ws_in6_pktinfo))) {
            struct ws_in6_pktinfo w; memcpy(&w, CMSG_DATA(c), sizeof w);
            struct in6_pktinfo pi = { w.ipi6_addr, w.ipi6_ifindex };
            off = cmsg_put(out, cap, off, IPPROTO_IPV6, IPV6_PKTINFO, &pi, sizeof pi, &trunc);
        }
    }
    return off;
}

API int WSARecvMsg(SOCKET s, WSAMSG *msg, DWORD *got, WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    NEED_INIT(SOCKET_ERROR);
    if (!msg) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    int ret = SOCKET_ERROR;
    struct iovec stack[16];
    struct iovec *iov = iov_from(msg->lpBuffers, msg->dwBufferCount, stack, 16);
    size_t ccap = (size_t)msg->Control.len * 2 + 256;
    char *ctl = msg->Control.buf ? calloc(1, ccap) : NULL;
    if (!iov) { set_wsa(WSAENOBUFS); goto out; }
    union ws_sa a; memset(&a, 0, sizeof a);
    struct msghdr mh = { 0 };
    mh.msg_name = msg->name ? &a : NULL;
    mh.msg_namelen = msg->name ? sizeof a : 0;
    mh.msg_iov = iov;
    mh.msg_iovlen = msg->dwBufferCount;
    mh.msg_control = ctl;
    mh.msg_controllen = ctl ? ccap : 0;
    ssize_t r = do_recvmsg(st, &mh, flags_in(msg->dwFlags & WS_MSG_PEEK), ov != NULL);
    if (r < 0) goto out;
    if (msg->name) {
        INT nl = msg->namelen;
        if (mh.msg_namelen && sa_out(&a, mh.msg_namelen, msg->name, &nl) == 0) msg->namelen = nl;
        else msg->namelen = 0;
    }
    int trunc = 0;
    if (ctl) msg->Control.len = (ULONG)cmsg_to_win(&mh, msg->Control.buf, msg->Control.len, &trunc);
    else msg->Control.len = 0;
    msg->dwFlags = flags_out(mh.msg_flags) | (trunc ? WS_MSG_CTRUNC : 0);
    if (got) *got = (DWORD)r;
    if ((mh.msg_flags & MSG_TRUNC) && st->type != SOCK_STREAM) { set_wsa(WSAEMSGSIZE); goto out; }
    ov_complete(s, ov, 0, (DWORD)r, msg->dwFlags, cr);
    ret = 0;
out:
    if (iov && iov != stack) free(iov);
    free(ctl);
    sock_put(st);
    return ret;
}

API int WSASendMsg(SOCKET s, WSAMSG *msg, DWORD flags, DWORD *sent, WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    NEED_INIT(SOCKET_ERROR);
    if (!msg) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    int ret = SOCKET_ERROR;
    union ws_sa a;
    int alen = 0;
    char ctl[256];
    struct iovec stack[16];
    struct iovec *iov = NULL;
    if (msg->name && st->type != SOCK_STREAM && (alen = sa_from_win(msg->name, msg->namelen, &a)) < 0) goto out;
    iov = iov_from(msg->lpBuffers, msg->dwBufferCount, stack, 16);
    if (!iov) { set_wsa(WSAENOBUFS); goto out; }
    struct msghdr mh = { 0 };
    mh.msg_name = alen ? &a : NULL;
    mh.msg_namelen = (socklen_t)alen;
    mh.msg_iov = iov;
    mh.msg_iovlen = msg->dwBufferCount;
    size_t cl = cmsg_from_win(&msg->Control, ctl, sizeof ctl);
    mh.msg_control = cl ? ctl : NULL;
    mh.msg_controllen = cl;
    ssize_t r = do_sendmsg(st, &mh, flags_in(flags), ov != NULL);
    if (r < 0) goto out;
    if (sent) *sent = (DWORD)r;
    ov_complete(s, ov, 0, (DWORD)r, 0, cr);
    ret = 0;
out:
    if (iov && iov != stack) free(iov);
    sock_put(st);
    return ret;
}

/* ------------------------------------------------------------------ */
/* select, WSAPoll                                                      */
/* ------------------------------------------------------------------ */

/* Windows fd_set is { u_int fd_count; SOCKET fd_array[FD_SETSIZE]; } where a
 * program may define FD_SETSIZE larger than 64, so fd_count is trusted. */
typedef struct { UINT fd_count; SOCKET fd_array[64]; } WS_FD_SET;

static SOCKET *fds_of(WS_FD_SET *set) { return set->fd_array; }

API int ws___WSAFDIsSet(SOCKET s, WS_FD_SET *set)
{
    if (!set) return 0;
    SOCKET *a = fds_of(set);
    for (UINT i = 0; i < set->fd_count; i++) if (a[i] == s) return 1;
    return 0;
}

static int ms_until(const struct timespec *deadline)
{
    if (!deadline) return -1;
    struct timespec now; now_mono(&now);
    long long ms = (deadline->tv_sec - now.tv_sec) * 1000LL + (deadline->tv_nsec - now.tv_nsec) / 1000000;
    return ms < 0 ? 0 : ms > INT_MAX ? INT_MAX : (int)ms;
}

/* Settle a finished non-blocking connect seen by select/WSAPoll. */
static void poll_connect(struct ws_sock *st, short rev)
{
    if (!(rev & (POLLOUT | POLLERR | POLLHUP))) return;
    pthread_mutex_lock(&g_lock);
    if (st->connecting) resolve_connect_locked(st);
    pthread_mutex_unlock(&g_lock);
}

API int ws_select(int nfds, WS_FD_SET *rd, WS_FD_SET *wr, WS_FD_SET *ex, const struct ws_timeval *tv)
{
    (void)nfds;
    NEED_INIT(SOCKET_ERROR);
    WS_FD_SET *sets[3] = { rd, wr, ex };
    size_t n = 0;
    for (int k = 0; k < 3; k++) if (sets[k]) n += sets[k]->fd_count;
    if (!n && !tv) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
    if (!n) {                           /* Windows rejects a select with no sockets */
        set_wsa(WSAEINVAL);
        return SOCKET_ERROR;
    }
    struct pollfd *p = calloc(n, sizeof *p);
    struct ws_sock **sts = calloc(n, sizeof *sts);
    int ret = SOCKET_ERROR;
    size_t m = 0;
    if (!p || !sts) { set_wsa(WSAENOBUFS); goto out; }
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        SOCKET *a = fds_of(sets[k]);
        for (UINT i = 0; i < sets[k]->fd_count; i++) {
            struct ws_sock *st = sock_get(a[i]);
            if (!st) goto out;
            sts[m] = st;
            short ev = k == 0 ? POLLIN : k == 1 ? POLLOUT : POLLPRI;
            if (st->connecting) ev |= POLLOUT;
            p[m++] = (struct pollfd){ st->fd, ev, 0 };
        }
    }
    struct timespec dl, *dlp = NULL;
    if (tv) {
        long long us = (long long)(tv->tv_sec < 0 ? 0 : tv->tv_sec) * 1000000LL + (tv->tv_usec < 0 ? 0 : tv->tv_usec);
        now_mono(&dl);
        dl.tv_sec += us / 1000000;
        dl.tv_nsec += (us % 1000000) * 1000;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        dlp = &dl;
    }
    int r;
    for (;;) {
        for (size_t i = 0; i < m; i++) __atomic_add_fetch(&sts[i]->busy, 1, __ATOMIC_ACQ_REL);
        r = poll(p, (nfds_t)m, ms_until(dlp));
        int e = errno;
        for (size_t i = 0; i < m; i++) __atomic_sub_fetch(&sts[i]->busy, 1, __ATOMIC_ACQ_REL);
        if (r >= 0) break;
        if (e != EINTR) { set_wsa_errno(e); goto out; }
    }
    for (size_t i = 0; i < m; i++) if (sts[i]->connecting) poll_connect(sts[i], p[i].revents);
    int total = 0;
    size_t idx = 0;
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        SOCKET *a = fds_of(sets[k]);
        UINT kept = 0, cnt = sets[k]->fd_count;
        for (UINT i = 0; i < cnt; i++, idx++) {
            struct ws_sock *st = sts[idx];
            short rev = p[idx].revents;
            int ready = 0;
            int idle = st->type == SOCK_STREAM && st->state == ST_NEW;   /* unconnected TCP */
            if (k == 0) ready = !st->conn_failed && !idle && st->state != ST_CONNECTING &&
                                (rev & (POLLIN | POLLHUP | POLLERR));
            else if (k == 1) ready = !st->conn_failed && !idle && st->state != ST_CONNECTING && (rev & POLLOUT);
            else ready = (rev & POLLPRI) || st->conn_failed;
            if (ready) a[kept++] = a[i];
        }
        sets[k]->fd_count = kept;
        total += (int)kept;
    }
    ret = total;
out:
    for (size_t i = 0; i < m; i++) sock_put(sts[i]);
    free(p);
    free(sts);
    return ret;
}

typedef struct { SOCKET fd; SHORT events; SHORT revents; } WSAPOLLFD;
_Static_assert(sizeof(WSAPOLLFD) == 16, "WSAPOLLFD layout");
#define WS_POLLRDNORM 0x0100
#define WS_POLLRDBAND 0x0200
#define WS_POLLPRI    0x0400
#define WS_POLLWRNORM 0x0010
#define WS_POLLWRBAND 0x0020
#define WS_POLLERR    0x0001
#define WS_POLLHUP    0x0002
#define WS_POLLNVAL   0x0004

static short poll_in(SHORT w)
{
    short l = 0;
    if (w & WS_POLLRDNORM) l |= POLLIN | POLLRDNORM;
    if (w & WS_POLLRDBAND) l |= POLLRDBAND;
    if (w & WS_POLLPRI)    l |= POLLPRI;
    if (w & WS_POLLWRNORM) l |= POLLOUT;
    if (w & WS_POLLWRBAND) l |= POLLWRBAND;
    return l;
}

static SHORT poll_out(short l, SHORT want)
{
    SHORT w = 0;
    if (l & (POLLIN | POLLRDNORM)) w |= WS_POLLRDNORM;
    if (l & POLLRDBAND) w |= WS_POLLRDBAND;
    if (l & POLLPRI)    w |= WS_POLLPRI;
    if (l & POLLOUT)    w |= WS_POLLWRNORM;
    if (l & POLLWRBAND) w |= WS_POLLWRBAND;
    w &= want;
    if (l & POLLERR)  w |= WS_POLLERR;
    if (l & POLLHUP)  w |= WS_POLLHUP;
    if (l & POLLNVAL) w |= WS_POLLNVAL;
    return w;
}

API int WSAPoll(WSAPOLLFD *fds, ULONG n, INT timeout)
{
    NEED_INIT(SOCKET_ERROR);
    if (!fds) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    if (!n) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
    const SHORT valid = WS_POLLRDNORM | WS_POLLRDBAND | WS_POLLPRI | WS_POLLWRNORM | WS_POLLWRBAND;
    for (ULONG i = 0; i < n; i++)
        if (fds[i].events & ~valid) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }   /* as Windows */
    struct pollfd *p = calloc(n, sizeof *p);
    struct ws_sock **sts = calloc(n, sizeof *sts);
    if (!p || !sts) { free(p); free(sts); set_wsa(WSAENOBUFS); return SOCKET_ERROR; }
    int nvalid = 0;
    for (ULONG i = 0; i < n; i++) {
        p[i].fd = -1;
        fds[i].revents = 0;
        if ((LONG_PTR)fds[i].fd < 0) continue;      /* negative entries are ignored */
        sts[i] = sock_get(fds[i].fd);
        if (!sts[i]) { fds[i].revents = WS_POLLNVAL; continue; }
        p[i].fd = sts[i]->fd;
        p[i].events = poll_in(fds[i].events);
        nvalid++;
    }
    int r = 0, ret;
    if (nvalid) {
        struct timespec dl, *dlp = NULL;
        if (timeout >= 0) {
            now_mono(&dl);
            dl.tv_sec += timeout / 1000;
            dl.tv_nsec += (long)(timeout % 1000) * 1000000L;
            if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
            dlp = &dl;
        }
        do r = poll(p, n, ms_until(dlp)); while (r < 0 && errno == EINTR);
    }
    if (r < 0) { set_wsa_errno(errno); ret = SOCKET_ERROR; }
    else {
        ret = 0;
        for (ULONG i = 0; i < n; i++) {
            if (sts[i]) {
                if (sts[i]->connecting) poll_connect(sts[i], p[i].revents);
                fds[i].revents = poll_out(p[i].revents, fds[i].events);
            }
            if (fds[i].revents) ret++;
        }
    }
    for (ULONG i = 0; i < n; i++) sock_put(sts[i]);
    free(p);
    free(sts);
    return ret;
}

/* ------------------------------------------------------------------ */
/* ioctlsocket                                                          */
/* ------------------------------------------------------------------ */

static int ioctl_common(struct ws_sock *st, DWORD cmd, ULONG *argp)
{
    if (!argp) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    if (cmd == WS_FIONBIO) {
        /* while WSAEventSelect is active the socket must stay non-blocking */
        if (!*argp && st->mask) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
        set_nonblock(st, *argp != 0);
        return 0;
    }
    if (cmd == WS_FIONREAD) {
        int n = 0;
        if (st->state != ST_LISTENING && ioctl(st->fd, FIONREAD, &n) < 0) n = 0;
        *argp = (ULONG)(n < 0 ? 0 : n);
        return 0;
    }
    if (cmd == WS_SIOCATMARK) {
        /* Windows answers TRUE when no urgent data is waiting */
        int at = 0;
        if (ioctl(st->fd, SIOCATMARK, &at) < 0) at = 0;
        *argp = !at;
        return 0;
    }
    set_wsa(WSAEINVAL);
    return SOCKET_ERROR;
}

API int ioctlsocket(SOCKET s, LONG cmd, ULONG *argp)
{
    NEED_INIT(SOCKET_ERROR);
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    int r = ioctl_common(st, (DWORD)cmd, argp);
    sock_put(st);
    return r;
}

/* ------------------------------------------------------------------ */
/* Protocol information                                                 */
/* ------------------------------------------------------------------ */

#define XP1_CONNECTIONLESS       0x00000001
#define XP1_GUARANTEED_DELIVERY  0x00000002
#define XP1_GUARANTEED_ORDER     0x00000004
#define XP1_MESSAGE_ORIENTED     0x00000008
#define XP1_GRACEFUL_CLOSE       0x00000020
#define XP1_EXPEDITED_DATA       0x00000040
#define XP1_SUPPORT_BROADCAST    0x00000200
#define XP1_SUPPORT_MULTIPOINT   0x00000400
#define XP1_IFS_HANDLES          0x00020000
#define PFL_MATCHES_PROTOCOL_ZERO 0x00000008

/* The TCP/IP provider: TCP and UDP over IPv4 and IPv6. */
static const struct proto_desc { int af, type, proto; DWORD catalog; const char *name; } g_protos[] = {
    { WS_AF_INET,  SOCK_STREAM, IPPROTO_TCP, 1001, "MSAFD Tcpip [TCP/IP]" },
    { WS_AF_INET,  SOCK_DGRAM,  IPPROTO_UDP, 1002, "MSAFD Tcpip [UDP/IP]" },
    { WS_AF_INET6, SOCK_STREAM, IPPROTO_TCP, 1003, "MSAFD Tcpip [TCP/IPv6]" },
    { WS_AF_INET6, SOCK_DGRAM,  IPPROTO_UDP, 1004, "MSAFD Tcpip [UDP/IPv6]" },
};
static const GUID g_tcpip_provider = { 0xE70F1AA0, 0xAB8B, 0x11CF, { 0x8C, 0xA3, 0x00, 0x80, 0x5F, 0x48, 0xA1, 0x92 } };

/* Fill the fields shared by the A and W structures (everything before
 * szProtocol, identical in both). */
static void proto_fill(WSAPROTOCOL_INFOW *pi, const struct proto_desc *d)
{
    memset(pi, 0, offsetof(WSAPROTOCOL_INFOW, szProtocol));
    if (d->type == SOCK_STREAM)
        pi->dwServiceFlags1 = XP1_GUARANTEED_DELIVERY | XP1_GUARANTEED_ORDER | XP1_GRACEFUL_CLOSE |
                              XP1_EXPEDITED_DATA | XP1_IFS_HANDLES;
    else
        pi->dwServiceFlags1 = XP1_CONNECTIONLESS | XP1_MESSAGE_ORIENTED | XP1_SUPPORT_BROADCAST |
                              XP1_SUPPORT_MULTIPOINT | XP1_IFS_HANDLES;
    pi->dwProviderFlags = PFL_MATCHES_PROTOCOL_ZERO;
    pi->ProviderId = g_tcpip_provider;
    pi->dwCatalogEntryId = d->catalog;
    pi->ProtocolChain.ChainLen = 1;                 /* BASE_PROTOCOL */
    pi->iVersion = 2;
    pi->iAddressFamily = d->af;
    pi->iMaxSockAddr = pi->iMinSockAddr = d->af == WS_AF_INET6 ? 28 : 16;
    pi->iSocketType = d->type;
    pi->iProtocol = d->proto;
    pi->dwMessageSize = d->type == SOCK_STREAM ? 0 : 0xFFBB;
}

static void proto_fill_w(WSAPROTOCOL_INFOW *pi, const struct proto_desc *d)
{
    proto_fill(pi, d);
    memset(pi->szProtocol, 0, sizeof pi->szProtocol);
    lsiw_utf8_to_utf16(d->name, -1, pi->szProtocol, WSAPROTOCOL_LEN + 1);
}

static void proto_fill_a(WSAPROTOCOL_INFOA *pa, const struct proto_desc *d)
{
    proto_fill((WSAPROTOCOL_INFOW *)pa, d);
    memset(pa->szProtocol, 0, sizeof pa->szProtocol);
    strcpy(pa->szProtocol, d->name);
}

static const struct proto_desc *proto_for(int linux_af, int type)
{
    int af = af_to_win(linux_af);
    for (size_t i = 0; i < sizeof g_protos / sizeof g_protos[0]; i++)
        if (g_protos[i].af == af && g_protos[i].type == type) return &g_protos[i];
    return &g_protos[0];
}

static int enum_protocols(const INT *filter, void *buf, DWORD *len, int wide)
{
    NEED_INIT(SOCKET_ERROR);
    if (!len) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    const struct proto_desc *sel[4];
    int n = 0;
    for (size_t i = 0; i < 4; i++) {
        int ok = !filter;
        for (const INT *f = filter; f && *f; f++) if (*f == g_protos[i].proto) ok = 1;
        if (ok) sel[n++] = &g_protos[i];
    }
    size_t one = wide ? sizeof(WSAPROTOCOL_INFOW) : sizeof(WSAPROTOCOL_INFOA);
    if (!buf || *len < n * one) { *len = (DWORD)(n * one); set_wsa(WSAENOBUFS); return SOCKET_ERROR; }
    for (int i = 0; i < n; i++) {
        if (wide) proto_fill_w((WSAPROTOCOL_INFOW *)buf + i, sel[i]);
        else proto_fill_a((WSAPROTOCOL_INFOA *)buf + i, sel[i]);
    }
    return n;
}

API int WSAEnumProtocolsW(const INT *protocols, WSAPROTOCOL_INFOW *buf, DWORD *len) { return enum_protocols(protocols, buf, len, 1); }
API int WSAEnumProtocolsA(const INT *protocols, WSAPROTOCOL_INFOA *buf, DWORD *len) { return enum_protocols(protocols, buf, len, 0); }

/* Sharing a socket with another process needs the duplicated descriptor to
 * cross processes, which LSIW's handle table cannot do. */
API int WSADuplicateSocketW(SOCKET s, DWORD pid, WSAPROTOCOL_INFOW *info) { (void)s; (void)pid; (void)info; set_wsa(WSAEINVAL); return SOCKET_ERROR; }
API int WSADuplicateSocketA(SOCKET s, DWORD pid, WSAPROTOCOL_INFOA *info) { (void)s; (void)pid; (void)info; set_wsa(WSAEINVAL); return SOCKET_ERROR; }

/* ------------------------------------------------------------------ */
/* getsockopt / setsockopt                                              */
/* ------------------------------------------------------------------ */

enum {
    K_INT, K_BOOL, K_BUF, K_LINGER, K_DONTLINGER, K_TIMEO, K_ERROR, K_TYPE, K_REUSE, K_EXCL,
    K_IGNORE, K_ZERO, K_CONNTIME, K_MAXMSG, K_MAXDG, K_PIA, K_PIW, K_MAXRT, K_DONTFRAG,
    K_MCAST_IF4, K_MREQ_SRC, K_RAW, K_GETONLY_INT
};

#define WS_SO_DONTLINGER       (int)(~0x0080u)
#define WS_SO_EXCLUSIVEADDRUSE (int)(~0x0004u)
#define WS_SO_OPENTYPE         0x7008

static const struct optmap { int wl, wn, ll, ln, kind; } g_opts[] = {
    { WS_SOL_SOCKET, 0x0001, SOL_SOCKET, SO_DEBUG, K_BOOL },
    { WS_SOL_SOCKET, 0x0002, SOL_SOCKET, SO_ACCEPTCONN, K_BOOL },
    { WS_SOL_SOCKET, 0x0004, SOL_SOCKET, SO_REUSEADDR, K_REUSE },
    { WS_SOL_SOCKET, 0x0008, SOL_SOCKET, SO_KEEPALIVE, K_BOOL },
    { WS_SOL_SOCKET, 0x0010, SOL_SOCKET, SO_DONTROUTE, K_BOOL },
    { WS_SOL_SOCKET, 0x0020, SOL_SOCKET, SO_BROADCAST, K_BOOL },
    { WS_SOL_SOCKET, 0x0080, SOL_SOCKET, SO_LINGER, K_LINGER },
    { WS_SOL_SOCKET, 0x0100, SOL_SOCKET, SO_OOBINLINE, K_BOOL },
    { WS_SOL_SOCKET, WS_SO_DONTLINGER, SOL_SOCKET, SO_LINGER, K_DONTLINGER },
    { WS_SOL_SOCKET, WS_SO_EXCLUSIVEADDRUSE, SOL_SOCKET, SO_REUSEADDR, K_EXCL },
    { WS_SOL_SOCKET, 0x1001, SOL_SOCKET, SO_SNDBUF, K_BUF },
    { WS_SOL_SOCKET, 0x1002, SOL_SOCKET, SO_RCVBUF, K_BUF },
    { WS_SOL_SOCKET, 0x1003, SOL_SOCKET, SO_SNDLOWAT, K_INT },
    { WS_SOL_SOCKET, 0x1004, SOL_SOCKET, SO_RCVLOWAT, K_INT },
    { WS_SOL_SOCKET, 0x1005, SOL_SOCKET, SO_SNDTIMEO, K_TIMEO },
    { WS_SOL_SOCKET, 0x1006, SOL_SOCKET, SO_RCVTIMEO, K_TIMEO },
    { WS_SOL_SOCKET, 0x1007, SOL_SOCKET, SO_ERROR, K_ERROR },
    { WS_SOL_SOCKET, 0x1008, SOL_SOCKET, SO_TYPE, K_TYPE },
    { WS_SOL_SOCKET, 0x2001, 0, 0, K_ZERO },        /* SO_GROUP_ID */
    { WS_SOL_SOCKET, 0x2002, 0, 0, K_ZERO },        /* SO_GROUP_PRIORITY */
    { WS_SOL_SOCKET, 0x2003, 0, 0, K_MAXMSG },      /* SO_MAX_MSG_SIZE */
    { WS_SOL_SOCKET, 0x2004, 0, 0, K_PIA },         /* SO_PROTOCOL_INFOA */
    { WS_SOL_SOCKET, 0x2005, 0, 0, K_PIW },         /* SO_PROTOCOL_INFOW */
    { WS_SOL_SOCKET, 0x3002, 0, 0, K_IGNORE },      /* SO_CONDITIONAL_ACCEPT */
    { WS_SOL_SOCKET, 0x3005, 0, 0, K_IGNORE },      /* SO_RANDOMIZE_PORT */
    { WS_SOL_SOCKET, 0x3006, 0, 0, K_IGNORE },      /* SO_PORT_SCALABILITY */
    { WS_SOL_SOCKET, 0x3007, 0, 0, K_IGNORE },      /* SO_REUSE_UNICASTPORT */
    { WS_SOL_SOCKET, 0x3008, 0, 0, K_IGNORE },      /* SO_REUSE_MULTICASTPORT */
    { WS_SOL_SOCKET, 0x7009, 0, 0, K_MAXDG },       /* SO_MAXDG */
    { WS_SOL_SOCKET, 0x700A, 0, 0, K_MAXDG },       /* SO_MAXPATHDG */
    { WS_SOL_SOCKET, 0x700B, 0, 0, K_IGNORE },      /* SO_UPDATE_ACCEPT_CONTEXT */
    { WS_SOL_SOCKET, 0x700C, 0, 0, K_CONNTIME },    /* SO_CONNECT_TIME */
    { WS_SOL_SOCKET, 0x7010, 0, 0, K_IGNORE },      /* SO_UPDATE_CONNECT_CONTEXT */
    { WS_IPPROTO_TCP, 1,  IPPROTO_TCP, TCP_NODELAY, K_BOOL },
    { WS_IPPROTO_TCP, 2,  0, 0, K_IGNORE },          /* TCP_EXPEDITED_1122 */
    { WS_IPPROTO_TCP, 3,  IPPROTO_TCP, TCP_KEEPIDLE, K_INT },   /* TCP_KEEPALIVE, seconds */
    { WS_IPPROTO_TCP, 4,  IPPROTO_TCP, TCP_MAXSEG, K_INT },
    { WS_IPPROTO_TCP, 5,  IPPROTO_TCP, TCP_USER_TIMEOUT, K_MAXRT },
    { WS_IPPROTO_TCP, 6,  0, 0, K_IGNORE },          /* TCP_STDURG */
    { WS_IPPROTO_TCP, 7,  0, 0, K_IGNORE },          /* TCP_NOURG */
    { WS_IPPROTO_TCP, 9,  0, 0, K_IGNORE },          /* TCP_NOSYNRETRIES */
    { WS_IPPROTO_TCP, 10, 0, 0, K_IGNORE },          /* TCP_TIMESTAMPS */
    { WS_IPPROTO_TCP, 12, 0, 0, K_IGNORE },          /* TCP_CONGESTION_ALGORITHM */
    { WS_IPPROTO_TCP, 13, 0, 0, K_IGNORE },          /* TCP_DELAY_FIN_ACK */
    { WS_IPPROTO_TCP, 14, IPPROTO_TCP, TCP_USER_TIMEOUT, K_INT },   /* TCP_MAXRTMS */
    { WS_IPPROTO_TCP, 15, 0, 0, K_IGNORE },          /* TCP_FASTOPEN */
    { WS_IPPROTO_TCP, 16, IPPROTO_TCP, TCP_KEEPCNT, K_INT },
    { WS_IPPROTO_TCP, 17, IPPROTO_TCP, TCP_KEEPINTVL, K_INT },
    { WS_IPPROTO_TCP, 18, 0, 0, K_IGNORE },          /* TCP_FAIL_CONNECT_ON_ICMP_ERROR */
    { WS_IPPROTO_IP, 1,  IPPROTO_IP, IP_OPTIONS, K_RAW },
    { WS_IPPROTO_IP, 2,  IPPROTO_IP, IP_HDRINCL, K_BOOL },
    { WS_IPPROTO_IP, 3,  IPPROTO_IP, IP_TOS, K_INT },
    { WS_IPPROTO_IP, 4,  IPPROTO_IP, IP_TTL, K_INT },
    { WS_IPPROTO_IP, 9,  IPPROTO_IP, IP_MULTICAST_IF, K_MCAST_IF4 },
    { WS_IPPROTO_IP, 10, IPPROTO_IP, IP_MULTICAST_TTL, K_INT },
    { WS_IPPROTO_IP, 11, IPPROTO_IP, IP_MULTICAST_LOOP, K_BOOL },
    { WS_IPPROTO_IP, 12, IPPROTO_IP, IP_ADD_MEMBERSHIP, K_RAW },
    { WS_IPPROTO_IP, 13, IPPROTO_IP, IP_DROP_MEMBERSHIP, K_RAW },
    { WS_IPPROTO_IP, 14, IPPROTO_IP, IP_MTU_DISCOVER, K_DONTFRAG },
    { WS_IPPROTO_IP, 15, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, K_MREQ_SRC },
    { WS_IPPROTO_IP, 16, IPPROTO_IP, IP_DROP_SOURCE_MEMBERSHIP, K_MREQ_SRC },
    { WS_IPPROTO_IP, 17, IPPROTO_IP, IP_BLOCK_SOURCE, K_MREQ_SRC },
    { WS_IPPROTO_IP, 18, IPPROTO_IP, IP_UNBLOCK_SOURCE, K_MREQ_SRC },
    { WS_IPPROTO_IP, 19, IPPROTO_IP, IP_PKTINFO, K_BOOL },
    { WS_IPPROTO_IP, 21, IPPROTO_IP, IP_RECVTTL, K_BOOL },    /* IP_HOPLIMIT */
    { WS_IPPROTO_IP, 22, 0, 0, K_IGNORE },                     /* IP_RECEIVE_BROADCAST */
    { WS_IPPROTO_IP, 40, IPPROTO_IP, IP_RECVTOS, K_BOOL },
    { WS_IPPROTO_IP, 73, IPPROTO_IP, IP_MTU, K_GETONLY_INT },
    { WS_IPPROTO_IPV6, 4,  IPPROTO_IPV6, IPV6_UNICAST_HOPS, K_INT },
    { WS_IPPROTO_IPV6, 9,  IPPROTO_IPV6, IPV6_MULTICAST_IF, K_INT },
    { WS_IPPROTO_IPV6, 10, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, K_INT },
    { WS_IPPROTO_IPV6, 11, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, K_BOOL },
    { WS_IPPROTO_IPV6, 12, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, K_RAW },
    { WS_IPPROTO_IPV6, 13, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, K_RAW },
    { WS_IPPROTO_IPV6, 14, IPPROTO_IPV6, IPV6_DONTFRAG, K_BOOL },
    { WS_IPPROTO_IPV6, 19, IPPROTO_IPV6, IPV6_RECVPKTINFO, K_BOOL },
    { WS_IPPROTO_IPV6, 21, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, K_BOOL },
    { WS_IPPROTO_IPV6, 23, 0, 0, K_IGNORE },                   /* IPV6_PROTECTION_LEVEL */
    { WS_IPPROTO_IPV6, 27, IPPROTO_IPV6, IPV6_V6ONLY, K_BOOL },
    { WS_IPPROTO_IPV6, 40, IPPROTO_IPV6, IPV6_RECVTCLASS, K_BOOL },
    { WS_IPPROTO_IPV6, 72, IPPROTO_IPV6, IPV6_MTU, K_GETONLY_INT },
    { WS_IPPROTO_UDP, 1, SOL_SOCKET, SO_NO_CHECK, K_BOOL },    /* UDP_NOCHECKSUM */
};

static const struct optmap *opt_find(int level, int name)
{
    for (size_t i = 0; i < sizeof g_opts / sizeof g_opts[0]; i++)
        if (g_opts[i].wl == level && g_opts[i].wn == name) return &g_opts[i];
    return NULL;
}

static int known_level(int level)
{
    return level == WS_SOL_SOCKET || level == WS_IPPROTO_IP || level == WS_IPPROTO_TCP ||
           level == WS_IPPROTO_IPV6 || level == WS_IPPROTO_UDP;
}

/* SO_OPENTYPE is a per-thread setting made on INVALID_SOCKET. */
static __thread int t_opentype;

static int opt_ret_int(char *val, INT *len, int v)
{
    if (!val || !len || *len < 1) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    if (*len >= 4) { memcpy(val, &v, 4); *len = 4; }
    else { *val = (char)v; *len = 1; }     /* old programs pass a one-byte BOOL */
    return 0;
}

static int lget_int(struct ws_sock *st, const struct optmap *m, int *v)
{
    socklen_t l = sizeof *v;
    *v = 0;
    if (getsockopt(st->fd, m->ll, m->ln, v, &l) < 0) { set_wsa_errno(errno); return -1; }
    return 0;
}

API int ws_getsockopt(SOCKET s, int level, int name, char *val, INT *len)
{
    NEED_INIT(SOCKET_ERROR);
    if (level == WS_SOL_SOCKET && name == WS_SO_OPENTYPE) return opt_ret_int(val, len, t_opentype);
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    int r = SOCKET_ERROR, v = 0;
    const struct optmap *m = opt_find(level, name);
    if (!val || !len) { set_wsa(WSAEFAULT); goto out; }
    if (!m) { set_wsa(known_level(level) ? WSAENOPROTOOPT : WSAEINVAL); goto out; }
    switch (m->kind) {
    case K_INT: case K_BOOL: case K_GETONLY_INT: case K_TYPE:
        if (lget_int(st, m, &v) == 0) r = opt_ret_int(val, len, v);
        break;
    case K_BUF:
        if (lget_int(st, m, &v) == 0) r = opt_ret_int(val, len, v / 2);   /* Linux reports twice the size set */
        break;
    case K_MAXRT:
        if (lget_int(st, m, &v) == 0) r = opt_ret_int(val, len, v / 1000);
        break;
    case K_DONTFRAG:
        if (lget_int(st, m, &v) == 0) r = opt_ret_int(val, len, v == IP_PMTUDISC_DO || v == IP_PMTUDISC_PROBE);
        break;
    case K_REUSE: r = opt_ret_int(val, len, st->type == SOCK_STREAM ? st->reuse : (lget_int(st, m, &v), v)); break;
    case K_EXCL:  r = opt_ret_int(val, len, st->exclusive); break;
    case K_IGNORE: case K_ZERO: r = opt_ret_int(val, len, 0); break;
    case K_MAXMSG:
        r = opt_ret_int(val, len, st->type == SOCK_STREAM ? 0x7FFFFFFF : st->af == AF_INET6 ? 65527 : 65507);
        break;
    case K_MAXDG: r = opt_ret_int(val, len, 65467); break;
    case K_CONNTIME: {
        int secs = -1;   /* 0xFFFFFFFF: not connected */
        if (st->state == ST_CONNECTED) { struct timespec now; now_mono(&now); secs = (int)(now.tv_sec - st->since.tv_sec); }
        r = opt_ret_int(val, len, secs);
        break;
    }
    case K_ERROR:
        if (st->conn_err) { v = (int)st->conn_err; st->conn_err = 0; }
        else if (lget_int(st, m, &v) == 0) v = (int)wsa_err(v);
        else break;
        r = opt_ret_int(val, len, v);
        break;
    case K_LINGER: case K_DONTLINGER: {
        struct linger lg = { 0, 0 }; socklen_t l = sizeof lg;
        if (getsockopt(st->fd, SOL_SOCKET, SO_LINGER, &lg, &l) < 0) { set_wsa_errno(errno); break; }
        if (m->kind == K_DONTLINGER) { r = opt_ret_int(val, len, !lg.l_onoff); break; }
        if (*len < 4) { set_wsa(WSAEFAULT); break; }
        USHORT wl[2] = { (USHORT)(lg.l_onoff != 0), (USHORT)lg.l_linger };   /* Windows: two u_shorts */
        memcpy(val, wl, 4); *len = 4; r = 0;
        break;
    }
    case K_TIMEO: {
        struct timeval tv = { 0, 0 }; socklen_t l = sizeof tv;
        if (getsockopt(st->fd, m->ll, m->ln, &tv, &l) < 0) { set_wsa_errno(errno); break; }
        if (*len < 4) { set_wsa(WSAEFAULT); break; }
        DWORD ms = (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);    /* Windows: a DWORD of milliseconds */
        int k = m->ln == SO_RCVTIMEO;
        if (st->timeo_set[k]) ms = st->timeo[k];   /* Linux rounds to timer ticks; report what was set */
        memcpy(val, &ms, 4); *len = 4; r = 0;
        break;
    }
    case K_PIA: case K_PIW: {
        size_t need = m->kind == K_PIW ? sizeof(WSAPROTOCOL_INFOW) : sizeof(WSAPROTOCOL_INFOA);
        if ((size_t)*len < need) { set_wsa(WSAEFAULT); break; }
        const struct proto_desc *d = proto_for(st->af, st->type);
        if (m->kind == K_PIW) proto_fill_w((WSAPROTOCOL_INFOW *)(void *)val, d);
        else proto_fill_a((WSAPROTOCOL_INFOA *)(void *)val, d);
        *len = (INT)need; r = 0;
        break;
    }
    case K_MCAST_IF4: case K_RAW: case K_MREQ_SRC: {
        socklen_t l = (socklen_t)*len;
        if (getsockopt(st->fd, m->ll, m->ln, val, &l) < 0) { set_wsa_errno(errno); break; }
        *len = (INT)l; r = 0;
        break;
    }
    }
out:
    sock_put(st);
    return r;
}

static int opt_get_val(const char *val, int len, int *v)
{
    if (!val || len < 1) { set_wsa(WSAEFAULT); return -1; }
    if (len >= 4) memcpy(v, val, 4);
    else *v = (unsigned char)*val;
    return 0;
}

static int lset(struct ws_sock *st, int lvl, int name, const void *v, socklen_t l)
{
    if (setsockopt(st->fd, lvl, name, v, l) < 0) { set_wsa_errno(errno); return SOCKET_ERROR; }
    return 0;
}

API int ws_setsockopt(SOCKET s, int level, int name, const char *val, int len)
{
    NEED_INIT(SOCKET_ERROR);
    int v = 0;
    if (level == WS_SOL_SOCKET && name == WS_SO_OPENTYPE) {
        if (opt_get_val(val, len, &v) < 0) return SOCKET_ERROR;
        t_opentype = v;
        return 0;
    }
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    int r = SOCKET_ERROR;
    const struct optmap *m = opt_find(level, name);
    if (!m) { set_wsa(known_level(level) ? WSAENOPROTOOPT : WSAEINVAL); goto out; }
    switch (m->kind) {
    case K_INT: case K_BOOL: case K_BUF:
        if (opt_get_val(val, len, &v) == 0) r = lset(st, m->ll, m->ln, &v, sizeof v);
        break;
    case K_MAXRT:
        if (opt_get_val(val, len, &v) == 0) { v = v < 0 ? 0 : v * 1000; r = lset(st, m->ll, m->ln, &v, sizeof v); }
        break;
    case K_DONTFRAG:
        if (opt_get_val(val, len, &v) == 0) { v = v ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT; r = lset(st, m->ll, m->ln, &v, sizeof v); }
        break;
    case K_REUSE:
        if (opt_get_val(val, len, &v) < 0) break;
        if (v && st->exclusive) { set_wsa(WSAEINVAL); break; }
        /* TCP sockets keep Linux SO_REUSEADDR on (see make_socket) */
        if (st->type == SOCK_STREAM) { st->reuse = v != 0; r = 0; }
        else r = lset(st, SOL_SOCKET, SO_REUSEADDR, &v, sizeof v);
        break;
    case K_EXCL: {
        if (opt_get_val(val, len, &v) < 0) break;
        if (v && st->reuse) { set_wsa(WSAEINVAL); break; }
        st->exclusive = v != 0;
        int lr = !st->exclusive && st->type == SOCK_STREAM;
        r = lset(st, SOL_SOCKET, SO_REUSEADDR, &lr, sizeof lr);
        break;
    }
    case K_IGNORE: case K_ZERO: r = 0; break;
    case K_LINGER: {
        if (!val || len < 4) { set_wsa(WSAEFAULT); break; }
        USHORT wl[2]; memcpy(wl, val, 4);
        struct linger lg = { wl[0] != 0, wl[1] };
        r = lset(st, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        break;
    }
    case K_DONTLINGER: {
        if (opt_get_val(val, len, &v) < 0) break;
        struct linger lg = { 0, 0 }; socklen_t l = sizeof lg;
        getsockopt(st->fd, SOL_SOCKET, SO_LINGER, &lg, &l);
        lg.l_onoff = !v;
        r = lset(st, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        break;
    }
    case K_TIMEO: {
        if (!val || len < 4) { set_wsa(WSAEFAULT); break; }
        DWORD ms; memcpy(&ms, val, 4);
        struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000 };
        r = lset(st, m->ll, m->ln, &tv, sizeof tv);
        if (r == 0) { int k = m->ln == SO_RCVTIMEO; st->timeo[k] = ms; st->timeo_set[k] = 1; }
        break;
    }
    case K_MCAST_IF4: {
        if (!val || len < 4) { set_wsa(WSAEFAULT); break; }
        uint32_t a; memcpy(&a, val, 4);
        /* 0.x.x.x is an interface index (network order) rather than an address */
        if (a && ((const unsigned char *)val)[0] == 0) {
            struct ip_mreqn mr; memset(&mr, 0, sizeof mr);
            mr.imr_ifindex = (int)ntohl(a);
            r = lset(st, IPPROTO_IP, IP_MULTICAST_IF, &mr, sizeof mr);
        } else r = lset(st, IPPROTO_IP, IP_MULTICAST_IF, val, 4);
        break;
    }
    case K_MREQ_SRC: {
        /* Windows order: group, source, interface; Linux: group, interface, source */
        if (!val || len < 12) { set_wsa(WSAEFAULT); break; }
        struct in_addr w[3]; memcpy(w, val, 12);
        struct ip_mreq_source ms = { w[0], w[2], w[1] };
        r = lset(st, m->ll, m->ln, &ms, sizeof ms);
        break;
    }
    case K_RAW:
        if (!val && len) { set_wsa(WSAEFAULT); break; }
        r = lset(st, m->ll, m->ln, val, (socklen_t)(len < 0 ? 0 : len));
        break;
    default:            /* read-only options */
        set_wsa(WSAENOPROTOOPT);
        break;
    }
out:
    sock_put(st);
    return r;
}

/* wsock32 (Winsock 1.1) numbered the IPPROTO_IP options differently. */
static int ip11_to_ws2(int level, int name)
{
    static const int map[] = { 0, 1, 9, 10, 11, 12, 13, 4, 3, 14 };   /* index: 1.1 value */
    if (level == WS_IPPROTO_IP && name >= 1 && name <= 9) return map[name];
    return name;
}
API int wsock32_getsockopt(SOCKET s, int level, int name, char *val, INT *len) { return ws_getsockopt(s, level, ip11_to_ws2(level, name), val, len); }
API int wsock32_setsockopt(SOCKET s, int level, int name, const char *val, int len) { return ws_setsockopt(s, level, ip11_to_ws2(level, name), val, len); }

/* ------------------------------------------------------------------ */
/* Extension functions (mswsock)                                        */
/* ------------------------------------------------------------------ */

/* AcceptEx stores each address in its block of lpOutputBuffer as an INT
 * length, padding, and the Windows sockaddr at offset 8 (the block is at
 * least the address size + 16); GetAcceptExSockaddrs reads it back.
 * Programs are documented to use only GetAcceptExSockaddrs. */
static void put_acceptex_addr(char *blk, DWORD blklen, const union ws_sa *a, socklen_t al)
{
    union ws_sa w;
    INT wl = sa_to_win_buf(a, al, &w);
    if ((DWORD)wl + 8 > blklen) wl = blklen >= 8 ? (INT)(blklen - 8) : 0;
    memset(blk, 0, blklen);
    if (blklen < 8) return;
    memcpy(blk, &wl, 4);
    memcpy(blk + 8, &w, (size_t)wl);
}

API BOOL AcceptEx(SOCKET ls, SOCKET as, void *out, DWORD rxlen, DWORD llen, DWORD rlen, DWORD *got, WSAOVERLAPPED *ov)
{
    NEED_INIT(FALSE);
    struct ws_sock *lst = sock_get(ls), *ast = lst ? sock_get(as) : NULL;
    BOOL ret = FALSE;
    if (!lst || !ast) goto out;
    int minl = win_sa_min(lst->af) + 16;
    if (!out || llen < (DWORD)minl || rlen < (DWORD)minl) { set_wsa(WSAEINVAL); goto out; }
    union ws_sa peer; socklen_t pl = sizeof peer;
    int fd = accept_fd(lst, &peer, &pl, 1);
    if (fd < 0) goto out;
    /* the connection takes over the descriptor of the prepared socket */
    if (dup3(fd, ast->fd, O_CLOEXEC) < 0) { set_wsa_errno(errno); close(fd); goto out; }
    close(fd);
    ast->af = lst->af; ast->type = lst->type; ast->protocol = lst->protocol;
    ast->state = ST_CONNECTED;
    now_mono(&ast->since);
    if (ast->nonblock) set_nonblock(ast, 1);
    DWORD bytes = 0;
    if (rxlen) {
        WSABUF b = { rxlen, out };
        struct iovec v = { b.buf, b.len };
        struct msghdr mh = { 0 };
        mh.msg_iov = &v; mh.msg_iovlen = 1;
        ssize_t r = do_recvmsg(ast, &mh, 0, 1);   /* completes with the first data block */
        if (r < 0) goto out;
        bytes = (DWORD)r;
    }
    union ws_sa loc; socklen_t ll = sizeof loc;
    if (getsockname(ast->fd, &loc.sa, &ll) < 0) { memset(&loc, 0, sizeof loc); loc.sa.sa_family = (sa_family_t)ast->af; ll = sizeof loc.sa; }
    put_acceptex_addr((char *)out + rxlen, llen, &loc, ll);
    put_acceptex_addr((char *)out + rxlen + llen, rlen, &peer, pl);
    if (got) *got = bytes;
    ov_complete(ls, ov, 0, bytes, 0, NULL);
    ret = TRUE;
out:
    sock_put(ast);
    sock_put(lst);
    return ret;
}

API void GetAcceptExSockaddrs(void *out, DWORD rxlen, DWORD llen, DWORD rlen, void **lsa, INT *lsalen, void **rsa, INT *rsalen)
{
    char *l = (char *)out + rxlen, *r = l + llen;
    INT n;
    if (lsa)    *lsa = l + 8;
    if (lsalen) { memcpy(&n, l, 4); *lsalen = n; }
    if (rsa)    *rsa = r + 8;
    if (rsalen) { memcpy(&n, r, 4); *rsalen = n; }
    (void)rlen;
}

API BOOL ConnectEx(SOCKET s, const void *name, int namelen, void *sendbuf, DWORD sendlen, DWORD *sent, WSAOVERLAPPED *ov)
{
    NEED_INIT(FALSE);
    struct ws_sock *st = sock_get(s);
    if (!st) return FALSE;
    BOOL ret = FALSE;
    union ws_sa a; socklen_t al = sizeof a;
    /* ConnectEx needs a bound socket */
    if (getsockname(st->fd, &a.sa, &al) < 0 ||
        ((a.sa.sa_family == AF_INET || a.sa.sa_family == AF_INET6) && sa_port(&a) == 0)) { set_wsa(WSAEINVAL); goto out; }
    if (do_connect(st, name, namelen, 1, -1) < 0) goto out;
    DWORD bytes = 0;
    if (sendbuf && sendlen) {
        struct iovec v = { sendbuf, sendlen };
        struct msghdr mh = { 0 };
        mh.msg_iov = &v; mh.msg_iovlen = 1;
        ssize_t r = do_sendmsg(st, &mh, 0, 1);
        if (r < 0) goto out;
        bytes = (DWORD)r;
    }
    if (sent) *sent = bytes;
    ov_complete(s, ov, 0, bytes, 0, NULL);
    ret = TRUE;
out:
    sock_put(st);
    return ret;
}

#define TF_DISCONNECT   0x01
#define TF_REUSE_SOCKET 0x02

/* Close the connection; with TF_REUSE_SOCKET the handle gets a fresh
 * unconnected socket of the same kind so it can be used with AcceptEx or
 * ConnectEx again. */
static int disconnect_sock(struct ws_sock *st, DWORD flags)
{
    shutdown(st->fd, SHUT_RDWR);
    if (flags & TF_REUSE_SOCKET) {
        int nfd = socket(st->af, st->type | SOCK_CLOEXEC, st->protocol);
        if (nfd < 0) { set_wsa_errno(errno); return -1; }
        int one = 1;
        if (st->type == SOCK_STREAM) setsockopt(nfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (st->af == AF_INET6) setsockopt(nfd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
        if (dup3(nfd, st->fd, O_CLOEXEC) < 0) { close(nfd); set_wsa_errno(errno); return -1; }
        close(nfd);
        pthread_mutex_lock(&g_lock);
        st->state = ST_NEW; st->eof = 0; st->quiet = 0; st->shut_send = 0;
        pthread_mutex_unlock(&g_lock);
        if (st->nonblock) set_nonblock(st, 1);
    }
    return 0;
}

API BOOL DisconnectEx(SOCKET s, WSAOVERLAPPED *ov, DWORD flags, DWORD reserved)
{
    (void)reserved;
    NEED_INIT(FALSE);
    struct ws_sock *st = sock_get(s);
    if (!st) return FALSE;
    BOOL ret = disconnect_sock(st, flags) == 0;
    if (ret) ov_complete(s, ov, 0, 0, 0, NULL);
    sock_put(st);
    return ret;
}

typedef struct { void *Head; DWORD HeadLength; void *Tail; DWORD TailLength; } TRANSMIT_FILE_BUFFERS;

static int send_all(struct ws_sock *st, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        struct iovec v = { (void *)p, len };
        struct msghdr mh = { 0 };
        mh.msg_iov = &v; mh.msg_iovlen = 1;
        ssize_t r = do_sendmsg(st, &mh, 0, 1);
        if (r < 0) return -1;
        p += r; len -= (size_t)r;
    }
    return 0;
}

API BOOL TransmitFile(SOCKET s, HANDLE file, DWORD count, DWORD per_send, WSAOVERLAPPED *ov,
                      TRANSMIT_FILE_BUFFERS *tb, DWORD flags)
{
    (void)per_send;
    NEED_INIT(FALSE);
    struct ws_sock *st = sock_get(s);
    if (!st) return FALSE;
    BOOL ret = FALSE;
    uint64_t total = 0;
    if (tb && tb->Head && tb->HeadLength) { if (send_all(st, tb->Head, tb->HeadLength) < 0) goto out; total += tb->HeadLength; }
    if (file) {
        int ffd = lsiw_fd_from_handle(file);
        if (ffd < 0) { set_wsa(WSAEINVAL); goto out; }
        /* with an OVERLAPPED the offset comes from it, otherwise from the file pointer */
        off_t off = ov ? (off_t)(((uint64_t)ov->u.s.OffsetHigh << 32) | ov->u.s.Offset) : 0;
        uint64_t left = count ? count : UINT64_MAX;
        while (left) {
            size_t chunk = left > (1u << 30) ? (1u << 30) : (size_t)left;
            ssize_t r = sendfile(st->fd, ffd, ov ? &off : NULL, chunk);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && errno == EAGAIN) { wait_fd(st->fd, POLLOUT, -1); continue; }
            if (r < 0) { set_wsa_errno(errno); goto out; }
            if (r == 0) break;
            left -= (uint64_t)r; total += (uint64_t)r;
        }
    }
    if (tb && tb->Tail && tb->TailLength) { if (send_all(st, tb->Tail, tb->TailLength) < 0) goto out; total += tb->TailLength; }
    if ((flags & (TF_DISCONNECT | TF_REUSE_SOCKET)) && disconnect_sock(st, flags) < 0) goto out;
    ov_complete(s, ov, 0, (DWORD)total, 0, NULL);
    ret = TRUE;
out:
    sock_put(st);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Waiting for address / route changes (netlink)                        */
/* ------------------------------------------------------------------ */

static int nl_open(unsigned groups)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct sockaddr_nl sn = { 0 };
    sn.nl_family = AF_NETLINK;
    sn.nl_groups = groups;
    if (bind(fd, (struct sockaddr *)&sn, sizeof sn) < 0) { close(fd); return -1; }
    return fd;
}

/* An outstanding asynchronous change notification. */
struct nl_wait {
    WSAOVERLAPPED *ov;
    LPWSAOVERLAPPED_COMPLETION_ROUTINE cr;
    int nlfd, cancelfd;
    struct nl_wait *next;
};
static struct nl_wait *g_nl_waits;
static pthread_mutex_t g_nl_lock = PTHREAD_MUTEX_INITIALIZER;

static void *nl_wait_thread(void *arg)
{
    struct nl_wait *w = arg;
    struct pollfd p[2] = { { w->nlfd, POLLIN, 0 }, { w->cancelfd, POLLIN, 0 } };
    int r;
    do r = poll(p, 2, -1); while (r < 0 && errno == EINTR);
    int cancelled = r <= 0 || (p[1].revents & POLLIN);
    pthread_mutex_lock(&g_nl_lock);
    for (struct nl_wait **pp = &g_nl_waits; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; break; }
    pthread_mutex_unlock(&g_nl_lock);
    close(w->nlfd);
    close(w->cancelfd);
    /* no APCs here: a completion routine cannot run on this thread, so only
     * the OVERLAPPED status and its event report the change */
    w->ov->Internal = cancelled ? STATUS_CANCELLED_ : 0;
    w->ov->InternalHigh = 0;
    if (w->ov->hEvent) ev_set((HANDLE)((uintptr_t)w->ov->hEvent & ~(uintptr_t)1));
    free(w);
    return NULL;
}

/* Returns 0 when started (the change completes ov later), or an errno. */
static int nl_wait_async(unsigned groups, WSAOVERLAPPED *ov)
{
    struct nl_wait *w = calloc(1, sizeof *w);
    if (!w) return ENOMEM;
    w->ov = ov;
    w->nlfd = nl_open(groups);
    w->cancelfd = eventfd(0, EFD_CLOEXEC);
    if (w->nlfd < 0 || w->cancelfd < 0) {
        int e = errno;
        if (w->nlfd >= 0) close(w->nlfd);
        if (w->cancelfd >= 0) close(w->cancelfd);
        free(w);
        return e ? e : EIO;
    }
    ov->Internal = STATUS_PENDING_;
    pthread_mutex_lock(&g_nl_lock);
    w->next = g_nl_waits;
    g_nl_waits = w;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &at, nl_wait_thread, w);
    pthread_attr_destroy(&at);
    if (r) g_nl_waits = w->next;
    pthread_mutex_unlock(&g_nl_lock);
    if (r) { close(w->nlfd); close(w->cancelfd); free(w); return r; }
    return 0;
}

static int nl_cancel(WSAOVERLAPPED *ov)
{
    int found = 0;
    pthread_mutex_lock(&g_nl_lock);
    for (struct nl_wait *w = g_nl_waits; w; w = w->next)
        if (w->ov == ov) { uint64_t one = 1; ssize_t r = write(w->cancelfd, &one, sizeof one); (void)r; found = 1; }
    pthread_mutex_unlock(&g_nl_lock);
    return found;
}

/* Block until the kernel reports a change in `groups`. */
static int nl_wait_sync(unsigned groups)
{
    int fd = nl_open(groups);
    if (fd < 0) return errno;
    char buf[4096];
    ssize_t r;
    do r = recv(fd, buf, sizeof buf, 0); while (r < 0 && errno == EINTR);
    close(fd);
    return 0;
}

#define NL_ADDR_GROUPS  (RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR)
#define NL_ROUTE_GROUPS (RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE)

/* ------------------------------------------------------------------ */
/* WSAIoctl                                                             */
/* ------------------------------------------------------------------ */

static const GUID WSAID_ACCEPTEX            = { 0xb5367df1, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } };
static const GUID WSAID_GETACCEPTEXSOCKADDRS = { 0xb5367df2, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } };
static const GUID WSAID_TRANSMITFILE        = { 0xb5367df0, 0xcbac, 0x11cf, { 0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92 } };
static const GUID WSAID_CONNECTEX           = { 0x25a207b9, 0xddf3, 0x4660, { 0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e } };
static const GUID WSAID_DISCONNECTEX        = { 0x7fda2e11, 0x8630, 0x436f, { 0xa0, 0x31, 0xf5, 0x36, 0xa6, 0xee, 0xc1, 0x57 } };
static const GUID WSAID_WSARECVMSG          = { 0xf689d7c8, 0x6f1f, 0x436b, { 0x8a, 0x53, 0xe5, 0x4f, 0xe3, 0x51, 0xc3, 0x22 } };
static const GUID WSAID_WSASENDMSG          = { 0xa441e712, 0x754f, 0x43ca, { 0x84, 0xa7, 0x0d, 0xee, 0x44, 0xcf, 0x60, 0x6d } };

/* Local addresses of one family, for SIO_ADDRESS_LIST_QUERY and friends. */
static int local_addrs(int family, union ws_sa **out)
{
    struct ifaddrs *ifa = NULL;
    *out = NULL;
    if (getifaddrs(&ifa) < 0) return -1;
    int n = 0, cap = 0;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != family || !(i->ifa_flags & IFF_UP)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; union ws_sa *nb = realloc(*out, (size_t)cap * sizeof **out); if (!nb) break; *out = nb; }
        memset(&(*out)[n], 0, sizeof **out);
        memcpy(&(*out)[n], i->ifa_addr, family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
        n++;
    }
    freeifaddrs(ifa);
    return n;
}

/* INTERFACE_INFO for SIO_GET_INTERFACE_LIST: flags and three 24-byte
 * sockaddr_gen unions. */
typedef struct { ULONG iiFlags; BYTE iiAddress[24]; BYTE iiBroadcastAddress[24]; BYTE iiNetmask[24]; } INTERFACE_INFO;
#define WS_IFF_UP 1
#define WS_IFF_BROADCAST 2
#define WS_IFF_LOOPBACK 4
#define WS_IFF_POINTTOPOINT 8
#define WS_IFF_MULTICAST 16

static void put_sin(BYTE *dst, const struct sockaddr *sa)
{
    memset(dst, 0, 24);
    if (!sa || sa->sa_family != AF_INET) return;
    memcpy(dst, sa, 16);
    WORD f = WS_AF_INET; memcpy(dst, &f, 2);
}

static int ioctl_iflist(void *out, DWORD outlen, DWORD *ret)
{
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) < 0) { set_wsa_errno(errno); return SOCKET_ERROR; }
    DWORD n = 0, need = 0;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET) need += sizeof(INTERFACE_INFO);
    if (!out || outlen < need) { freeifaddrs(ifa); if (ret) *ret = need; set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    INTERFACE_INFO *ii = out;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        ULONG f = 0;
        if (i->ifa_flags & IFF_UP) f |= WS_IFF_UP;
        if (i->ifa_flags & IFF_BROADCAST) f |= WS_IFF_BROADCAST;
        if (i->ifa_flags & IFF_LOOPBACK) f |= WS_IFF_LOOPBACK;
        if (i->ifa_flags & IFF_POINTOPOINT) f |= WS_IFF_POINTTOPOINT;
        if (i->ifa_flags & IFF_MULTICAST) f |= WS_IFF_MULTICAST;
        ii[n].iiFlags = f;
        put_sin(ii[n].iiAddress, i->ifa_addr);
        put_sin(ii[n].iiBroadcastAddress, (i->ifa_flags & IFF_BROADCAST) ? i->ifa_broadaddr : NULL);
        put_sin(ii[n].iiNetmask, i->ifa_netmask);
        n++;
    }
    freeifaddrs(ifa);
    if (ret) *ret = n * (DWORD)sizeof(INTERFACE_INFO);
    return 0;
}

/* SOCKET_ADDRESS_LIST { INT iAddressCount; SOCKET_ADDRESS Address[]; }
 * followed by the sockaddrs themselves. */
static int ioctl_addrlist(struct ws_sock *st, void *out, DWORD outlen, DWORD *ret)
{
    union ws_sa *as;
    int n = local_addrs(st->af == AF_INET6 ? AF_INET6 : AF_INET, &as);
    if (n < 0) { set_wsa_errno(errno); return SOCKET_ERROR; }
    int wl = st->af == AF_INET6 ? 28 : 16;
    DWORD need = (DWORD)(8 + n * (int)sizeof(SOCKET_ADDRESS) + n * wl);
    if (!out || outlen < need) { free(as); if (ret) *ret = need; set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    char *base = out;
    INT cnt = n;
    memcpy(base, &cnt, 4);
    memset(base + 4, 0, 4);
    SOCKET_ADDRESS *sa = (SOCKET_ADDRESS *)(base + 8);
    char *data = base + 8 + n * sizeof(SOCKET_ADDRESS);
    for (int i = 0; i < n; i++) {
        union ws_sa w;
        sa_to_win_buf(&as[i], (socklen_t)wl, &w);
        memcpy(data + i * wl, &w, (size_t)wl);
        sa[i].lpSockaddr = data + i * wl;
        sa[i].iSockaddrLength = wl;
    }
    free(as);
    if (ret) *ret = need;
    return 0;
}

/* Which local address the system would use to reach `dst`. */
static int route_source(const union ws_sa *dst, socklen_t dl, union ws_sa *src, socklen_t *sl)
{
    int fd = socket(dst->sa.sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    union ws_sa d = *dst;
    if (d.sa.sa_family == AF_INET && !d.in.sin_port) d.in.sin_port = htons(9);
    if (d.sa.sa_family == AF_INET6 && !d.in6.sin6_port) d.in6.sin6_port = htons(9);
    int r = connect(fd, &d.sa, dl);
    if (r == 0) r = getsockname(fd, &src->sa, sl);
    int e = errno;
    close(fd);
    errno = e;
    return r;
}

struct tcp_keepalive { ULONG onoff; ULONG keepalivetime; ULONG keepaliveinterval; };

API int WSAIoctl(SOCKET s, DWORD code, void *in, DWORD inlen, void *out, DWORD outlen, DWORD *ret,
                 WSAOVERLAPPED *ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    NEED_INIT(SOCKET_ERROR);
    struct ws_sock *st = sock_get(s);
    if (!st) return SOCKET_ERROR;
    int r = SOCKET_ERROR;
    DWORD bytes = 0;
    switch (code) {
    case WS_FIONBIO:
        if (!in || inlen < 4) { set_wsa(WSAEFAULT); break; }
        r = ioctl_common(st, code, in);
        break;
    case WS_FIONREAD: case WS_SIOCATMARK:
        if (!out || outlen < 4) { set_wsa(WSAEFAULT); break; }
        r = ioctl_common(st, code, out);
        bytes = 4;
        break;
    case SIO_GET_EXTENSION_FUNCTION_POINTER: {
        if (!in || inlen < sizeof(GUID) || !out || outlen < sizeof(void *)) { set_wsa(WSAEFAULT); break; }
        GUID g; memcpy(&g, in, sizeof g);
        void *fn = NULL;
        if (!memcmp(&g, &WSAID_ACCEPTEX, sizeof g)) fn = (void *)AcceptEx;
        else if (!memcmp(&g, &WSAID_GETACCEPTEXSOCKADDRS, sizeof g)) fn = (void *)GetAcceptExSockaddrs;
        else if (!memcmp(&g, &WSAID_TRANSMITFILE, sizeof g)) fn = (void *)TransmitFile;
        else if (!memcmp(&g, &WSAID_CONNECTEX, sizeof g)) fn = (void *)ConnectEx;
        else if (!memcmp(&g, &WSAID_DISCONNECTEX, sizeof g)) fn = (void *)DisconnectEx;
        else if (!memcmp(&g, &WSAID_WSARECVMSG, sizeof g)) fn = (void *)WSARecvMsg;
        else if (!memcmp(&g, &WSAID_WSASENDMSG, sizeof g)) fn = (void *)WSASendMsg;
        if (!fn) { set_wsa(WSAEINVAL); break; }   /* TransmitPackets and others are not provided */
        memcpy(out, &fn, sizeof fn);
        bytes = sizeof fn;
        r = 0;
        break;
    }
    case SIO_KEEPALIVE_VALS: {
        if (!in || inlen < sizeof(struct tcp_keepalive)) { set_wsa(WSAEFAULT); break; }
        struct tcp_keepalive ka; memcpy(&ka, in, sizeof ka);
        int on = ka.onoff != 0;
        int idle = (int)((ka.keepalivetime + 999) / 1000), intv = (int)((ka.keepaliveinterval + 999) / 1000);
        if (idle < 1) idle = 1;
        if (intv < 1) intv = 1;
        if (setsockopt(st->fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on) < 0) { set_wsa_errno(errno); break; }
        if (on) {
            setsockopt(st->fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
            setsockopt(st->fd, IPPROTO_TCP, TCP_KEEPINTVL, &intv, sizeof intv);
        }
        r = 0;
        break;
    }
    case SIO_ADDRESS_LIST_QUERY:
        r = ioctl_addrlist(st, out, outlen, &bytes);
        break;
    case SIO_ADDRESS_LIST_CHANGE:
        if (ov) {
            int e = nl_wait_async(NL_ADDR_GROUPS, ov);
            if (e) { set_wsa_errno(e); break; }
            set_wsa(WSA_IO_PENDING);   /* completes when an address changes */
            sock_put(st);
            return SOCKET_ERROR;
        }
        { int e = nl_wait_sync(NL_ADDR_GROUPS); if (e) { set_wsa_errno(e); break; } }
        r = 0;
        break;
    case SIO_GET_INTERFACE_LIST:
        r = ioctl_iflist(out, outlen, &bytes);
        break;
    case SIO_ROUTING_INTERFACE_QUERY: {
        union ws_sa d, src; socklen_t sl = sizeof src;
        int dl = sa_from_win(in, (int)inlen, &d);
        if (dl < 0) break;
        if (route_source(&d, (socklen_t)dl, &src, &sl) < 0) { set_wsa_errno(errno); break; }
        INT ol = (INT)outlen;
        if (sa_out(&src, sl, out, &ol) < 0) break;
        bytes = (DWORD)ol;
        r = 0;
        break;
    }
    case SIO_UDP_CONNRESET:
        if (!in || inlen < 4) { set_wsa(WSAEFAULT); break; }
        { BOOL b; memcpy(&b, in, 4); st->udp_connreset = b != 0; }
        r = 0;
        break;
    case SIO_UDP_NETRESET: case SIO_LOOPBACK_FAST_PATH: case SIO_TCP_INITIAL_RTO: case SIO_SET_COMPATIBILITY_MODE:
        r = 0;                     /* tuning hints with no Linux counterpart */
        break;
    case SIO_BASE_HANDLE: case SIO_BSP_HANDLE: case SIO_BSP_HANDLE_SELECT: case SIO_BSP_HANDLE_POLL:
        if (!out || outlen < sizeof(SOCKET)) { set_wsa(WSAEFAULT); break; }
        memcpy(out, &s, sizeof s);  /* there are no layered providers: the socket is its own base */
        bytes = sizeof s;
        r = 0;
        break;
    case SIO_IDEAL_SEND_BACKLOG_QUERY: {
        if (!out || outlen < 4) { set_wsa(WSAEFAULT); break; }
        int v = 0; socklen_t l = sizeof v;
        getsockopt(st->fd, SOL_SOCKET, SO_SNDBUF, &v, &l);
        ULONG u = (ULONG)(v > 0 ? v / 2 : 65536);
        memcpy(out, &u, 4);
        bytes = 4;
        r = 0;
        break;
    }
    default:
        set_wsa(WSAEINVAL);
        break;
    }
    if (ret) *ret = bytes;
    if (r == 0) ov_complete(s, ov, 0, bytes, 0, cr);
    sock_put(st);
    return r;
}

/* ------------------------------------------------------------------ */
/* Byte order and address text                                          */
/* ------------------------------------------------------------------ */

API USHORT ws_htons(USHORT v) { return (USHORT)((v >> 8) | (v << 8)); }
API USHORT ws_ntohs(USHORT v) { return (USHORT)((v >> 8) | (v << 8)); }
API ULONG  ws_htonl(ULONG v)  { return __builtin_bswap32(v); }
API ULONG  ws_ntohl(ULONG v)  { return __builtin_bswap32(v); }

static int check_sock(SOCKET s)
{
    struct ws_sock *st = sock_get(s);
    if (!st) return -1;
    sock_put(st);
    return 0;
}
API int WSAHtons(SOCKET s, USHORT v, USHORT *out) { NEED_INIT(SOCKET_ERROR); if (check_sock(s)) return SOCKET_ERROR; if (!out) { set_wsa(WSAEFAULT); return SOCKET_ERROR; } *out = ws_htons(v); return 0; }
API int WSANtohs(SOCKET s, USHORT v, USHORT *out) { NEED_INIT(SOCKET_ERROR); if (check_sock(s)) return SOCKET_ERROR; if (!out) { set_wsa(WSAEFAULT); return SOCKET_ERROR; } *out = ws_ntohs(v); return 0; }
API int WSAHtonl(SOCKET s, ULONG v, ULONG *out)   { NEED_INIT(SOCKET_ERROR); if (check_sock(s)) return SOCKET_ERROR; if (!out) { set_wsa(WSAEFAULT); return SOCKET_ERROR; } *out = ws_htonl(v); return 0; }
API int WSANtohl(SOCKET s, ULONG v, ULONG *out)   { NEED_INIT(SOCKET_ERROR); if (check_sock(s)) return SOCKET_ERROR; if (!out) { set_wsa(WSAEFAULT); return SOCKET_ERROR; } *out = ws_ntohl(v); return 0; }

/* inet_addr accepts the classic forms (a.b.c.d, a.b.c, a.b, a; octal and
 * hex parts) and returns INADDR_NONE (0xFFFFFFFF) for anything else. */
API ULONG ws_inet_addr(const char *cp)
{
    struct in_addr a;
    if (!cp || !*cp) return 0xFFFFFFFFu;
    size_t n = strlen(cp);
    while (n && cp[n - 1] == ' ') n--;          /* trailing blanks are tolerated */
    char buf[64];
    if (n >= sizeof buf) return 0xFFFFFFFFu;
    memcpy(buf, cp, n); buf[n] = 0;
    if (!inet_aton(buf, &a)) return 0xFFFFFFFFu;
    return a.s_addr;
}

/* in_addr is a 4-byte struct passed in a register; per-thread result buffer. */
API char *ws_inet_ntoa(ULONG in)
{
    static __thread char buf[16];
    const unsigned char *b = (const unsigned char *)&in;
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

API INT ws_inet_pton(INT family, const char *src, void *dst)
{
    if (!src || !dst) { set_wsa(WSAEFAULT); return -1; }
    int laf = family == WS_AF_INET ? AF_INET : family == WS_AF_INET6 ? AF_INET6 : -1;
    if (laf < 0) { set_wsa(WSAEAFNOSUPPORT); return -1; }
    return inet_pton(laf, src, dst) == 1 ? 1 : 0;
}

API INT InetPtonW(INT family, const WCHAR *src, void *dst)
{
    if (!src) { set_wsa(WSAEFAULT); return -1; }
    char *u = lsiw_w2u(src);
    INT r = ws_inet_pton(family, u, dst);
    free(u);
    return r;
}

API const char *ws_inet_ntop(INT family, const void *addr, char *buf, size_t size)
{
    if (!addr || !buf) { set_wsa(WSAEFAULT); return NULL; }
    int laf = family == WS_AF_INET ? AF_INET : family == WS_AF_INET6 ? AF_INET6 : -1;
    if (laf < 0) { set_wsa(WSAEAFNOSUPPORT); return NULL; }
    char tmp[INET6_ADDRSTRLEN];
    if (!inet_ntop(laf, addr, tmp, sizeof tmp)) { set_wsa(WSAEINVAL); return NULL; }
    if (strlen(tmp) + 1 > size) { set_wsa(WSAEINVAL); return NULL; }    /* buffer too small */
    strcpy(buf, tmp);
    return buf;
}

API const WCHAR *InetNtopW(INT family, const void *addr, WCHAR *buf, size_t size)
{
    char tmp[INET6_ADDRSTRLEN];
    if (!buf) { set_wsa(WSAEFAULT); return NULL; }
    if (!ws_inet_ntop(family, addr, tmp, sizeof tmp)) return NULL;
    size_t n = strlen(tmp);
    if (n + 1 > size) { set_wsa(WSAEINVAL); return NULL; }
    for (size_t i = 0; i <= n; i++) buf[i] = (WCHAR)(unsigned char)tmp[i];
    return buf;
}

/* "a.b.c.d[:port]" or "[v6%scope]:port" / "v6%scope" -> Windows sockaddr. */
static int str_to_addr(const char *s, INT family, void *out, INT *outlen)
{
    if (!s || !outlen) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    union ws_sa a; memset(&a, 0, sizeof a);
    char buf[128];
    if (strlen(s) >= sizeof buf) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
    strcpy(buf, s);
    int wl;
    if (family == WS_AF_INET) {
        char *colon = strchr(buf, ':');
        unsigned long port = 0;
        if (colon) {
            char *end; *colon = 0;
            port = strtoul(colon + 1, &end, 10);
            if (*end || port > 65535 || !colon[1]) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
        }
        if (!inet_aton(buf, &a.in.sin_addr)) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
        a.in.sin_family = AF_INET;
        a.in.sin_port = htons((uint16_t)port);
        wl = 16;
    } else if (family == WS_AF_INET6) {
        char *host = buf, *pct;
        unsigned long port = 0;
        if (buf[0] == '[') {
            char *close = strchr(buf, ']'), *end;
            if (!close) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
            *close = 0;
            host = buf + 1;
            if (close[1] == ':') {
                port = strtoul(close + 2, &end, 10);
                if (*end || port > 65535 || !close[2]) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
            } else if (close[1]) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
        }
        if ((pct = strchr(host, '%'))) {
            *pct = 0;
            char *end;
            unsigned long sc = strtoul(pct + 1, &end, 10);
            if (*end) sc = if_nametoindex(pct + 1);
            if (!sc && pct[1] != '0') { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
            a.in6.sin6_scope_id = (uint32_t)sc;
        }
        if (inet_pton(AF_INET6, host, &a.in6.sin6_addr) != 1) { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
        a.in6.sin6_family = AF_INET6;
        a.in6.sin6_port = htons((uint16_t)port);
        wl = 28;
    } else { set_wsa(WSAEINVAL); return SOCKET_ERROR; }
    if (!out || *outlen < wl) { *outlen = wl; set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    union ws_sa w;
    sa_to_win_buf(&a, (socklen_t)wl, &w);
    memcpy(out, &w, (size_t)wl);
    *outlen = wl;
    return 0;
}

API INT WSAStringToAddressA(char *s, INT family, WSAPROTOCOL_INFOA *pi, void *out, INT *outlen)
{
    (void)pi;
    NEED_INIT(SOCKET_ERROR);
    return str_to_addr(s, family, out, outlen);
}

API INT WSAStringToAddressW(WCHAR *s, INT family, WSAPROTOCOL_INFOW *pi, void *out, INT *outlen)
{
    (void)pi;
    NEED_INIT(SOCKET_ERROR);
    if (!s) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    char *u = lsiw_w2u(s);
    INT r = str_to_addr(u, family, out, outlen);
    free(u);
    return r;
}

/* Windows sockaddr -> "a.b.c.d[:port]" / "[v6%scope]:port". */
static int addr_to_str(const void *sa, DWORD salen, char *out, size_t cap)
{
    if (!sa || salen < 2) { set_wsa(WSAEFAULT); return -1; }
    WORD fam; memcpy(&fam, sa, 2);
    char host[INET6_ADDRSTRLEN];
    if (fam == WS_AF_INET) {
        if (salen < 16) { set_wsa(WSAEFAULT); return -1; }
        struct sockaddr_in in; memcpy(&in, sa, 16);
        inet_ntop(AF_INET, &in.sin_addr, host, sizeof host);
        if (in.sin_port) snprintf(out, cap, "%s:%u", host, ntohs(in.sin_port));
        else snprintf(out, cap, "%s", host);
    } else if (fam == WS_AF_INET6) {
        if (salen < 24) { set_wsa(WSAEFAULT); return -1; }
        struct sockaddr_in6 in6; memset(&in6, 0, sizeof in6);
        memcpy(&in6, sa, salen >= 28 ? 28 : 24);
        inet_ntop(AF_INET6, &in6.sin6_addr, host, sizeof host);
        char scope[16] = "";
        if (in6.sin6_scope_id) snprintf(scope, sizeof scope, "%%%u", in6.sin6_scope_id);
        if (in6.sin6_port) snprintf(out, cap, "[%s%s]:%u", host, scope, ntohs(in6.sin6_port));
        else snprintf(out, cap, "%s%s", host, scope);
    } else { set_wsa(WSAEINVAL); return -1; }
    return 0;
}

API INT WSAAddressToStringA(void *sa, DWORD salen, WSAPROTOCOL_INFOA *pi, char *out, DWORD *outlen)
{
    (void)pi;
    NEED_INIT(SOCKET_ERROR);
    char tmp[80];
    if (!outlen) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    if (addr_to_str(sa, salen, tmp, sizeof tmp) < 0) return SOCKET_ERROR;
    DWORD need = (DWORD)strlen(tmp) + 1;
    if (!out || *outlen < need) { *outlen = need; set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    memcpy(out, tmp, need);
    *outlen = need;
    return 0;
}

API INT WSAAddressToStringW(void *sa, DWORD salen, WSAPROTOCOL_INFOW *pi, WCHAR *out, DWORD *outlen)
{
    (void)pi;
    NEED_INIT(SOCKET_ERROR);
    char tmp[80];
    if (!outlen) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    if (addr_to_str(sa, salen, tmp, sizeof tmp) < 0) return SOCKET_ERROR;
    DWORD need = (DWORD)strlen(tmp) + 1;
    if (!out || *outlen < need) { *outlen = need; set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    for (DWORD i = 0; i < need; i++) out[i] = (WCHAR)(unsigned char)tmp[i];
    *outlen = need;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Host names                                                           */
/* ------------------------------------------------------------------ */

/* Windows' gethostname is the DNS host label, without the domain. */
static void local_hostname(char *buf, size_t cap)
{
    if (gethostname(buf, cap) < 0 || !*buf) snprintf(buf, cap, "localhost");
    buf[cap - 1] = 0;
    char *dot = strchr(buf, '.');
    if (dot && dot != buf) *dot = 0;
}

API int ws_gethostname(char *name, int namelen)
{
    NEED_INIT(SOCKET_ERROR);
    char h[256];
    local_hostname(h, sizeof h);
    if (!name || namelen < 0 || strlen(h) + 1 > (size_t)namelen) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    strcpy(name, h);
    return 0;
}

API int GetHostNameW(WCHAR *name, int namelen)
{
    NEED_INIT(SOCKET_ERROR);
    char h[256];
    local_hostname(h, sizeof h);
    int need = lsiw_utf8_to_utf16(h, -1, NULL, 0);
    if (!name || namelen < need) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    lsiw_utf8_to_utf16(h, -1, name, namelen);
    return 0;
}

/* hostent in the Windows layout; one per thread, as on Windows. */
struct ws_hostent { char *h_name; char **h_aliases; SHORT h_addrtype; SHORT h_length; char **h_addr_list; };
_Static_assert(sizeof(struct ws_hostent) == 32, "hostent layout");
#define MAX_HOST_ADDRS 35
static __thread struct {
    struct ws_hostent h;
    char *aliases[1];
    char *addrs[MAX_HOST_ADDRS + 1];
    char data[MAX_HOST_ADDRS][16];
    char name[256];
} t_host;

static struct ws_hostent *host_begin(const char *name, int type, int len)
{
    memset(&t_host, 0, sizeof t_host);
    snprintf(t_host.name, sizeof t_host.name, "%.255s", name);
    t_host.h.h_name = t_host.name;
    t_host.h.h_aliases = t_host.aliases;
    t_host.h.h_addrtype = (SHORT)type;
    t_host.h.h_length = (SHORT)len;
    t_host.h.h_addr_list = t_host.addrs;
    return &t_host.h;
}

static void host_add(const void *addr, int len)
{
    int n = 0;
    while (n < MAX_HOST_ADDRS && t_host.addrs[n]) {
        if (!memcmp(t_host.addrs[n], addr, (size_t)len)) return;
        n++;
    }
    if (n == MAX_HOST_ADDRS) return;
    memcpy(t_host.data[n], addr, (size_t)len);
    t_host.addrs[n] = t_host.data[n];
}

/* The machine's own IPv4 addresses (non-loopback first), which is what
 * Windows returns when a program resolves its own host name. */
static int host_add_local_v4(void)
{
    struct ifaddrs *ifa = NULL;
    int n = 0;
    if (getifaddrs(&ifa) < 0) return 0;
    for (int pass = 0; pass < 2 && !n; pass++)
        for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            if (!pass && (i->ifa_flags & IFF_LOOPBACK)) continue;
            host_add(&((struct sockaddr_in *)(void *)i->ifa_addr)->sin_addr, 4);
            n++;
        }
    freeifaddrs(ifa);
    return n;
}

API struct ws_hostent *ws_gethostbyname(const char *name)
{
    NEED_INIT(NULL);
    char self[256];
    local_hostname(self, sizeof self);
    if (!name || !*name) name = self;
    struct in_addr a;
    if (inet_pton(AF_INET, name, &a) == 1) {
        struct ws_hostent *h = host_begin(name, WS_AF_INET, 4);
        host_add(&a, 4);
        return h;
    }
    if (!strcasecmp(name, self)) {
        struct ws_hostent *h = host_begin(self, WS_AF_INET, 4);
        if (host_add_local_v4()) return h;
    }
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    int r = getaddrinfo(name, NULL, &hints, &res);
    if (r) { set_wsa(wsa_from_eai(r, errno)); return NULL; }
    struct ws_hostent *h = host_begin(res->ai_canonname ? res->ai_canonname : name, WS_AF_INET, 4);
    for (struct addrinfo *i = res; i; i = i->ai_next)
        host_add(&((struct sockaddr_in *)(void *)i->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return h;
}

API struct ws_hostent *ws_gethostbyaddr(const char *addr, int len, int type)
{
    NEED_INIT(NULL);
    if (!addr) { set_wsa(WSAEFAULT); return NULL; }
    union ws_sa a; memset(&a, 0, sizeof a);
    socklen_t sl;
    int alen;
    if (type == WS_AF_INET) {
        if (len < 4) { set_wsa(WSAEFAULT); return NULL; }
        a.in.sin_family = AF_INET; memcpy(&a.in.sin_addr, addr, 4); sl = sizeof a.in; alen = 4;
    } else if (type == WS_AF_INET6) {
        if (len < 16) { set_wsa(WSAEFAULT); return NULL; }
        a.in6.sin6_family = AF_INET6; memcpy(&a.in6.sin6_addr, addr, 16); sl = sizeof a.in6; alen = 16;
    } else { set_wsa(WSAEAFNOSUPPORT); return NULL; }
    char host[NI_MAXHOST];
    int r = getnameinfo(&a.sa, sl, host, sizeof host, NULL, 0, NI_NAMEREQD);
    if (r) { set_wsa(wsa_from_eai(r, errno)); return NULL; }
    struct ws_hostent *h = host_begin(host, type, alen);
    host_add(addr, alen);
    return h;
}

/* ------------------------------------------------------------------ */
/* Services and protocols                                               */
/* ------------------------------------------------------------------ */

/* Windows x64 servent puts s_proto before s_port. */
struct ws_servent { char *s_name; char **s_aliases; char *s_proto; SHORT s_port; };
struct ws_protoent { char *p_name; char **p_aliases; SHORT p_proto; };
_Static_assert(sizeof(struct ws_servent) == 32, "servent layout");
_Static_assert(sizeof(struct ws_protoent) == 24, "protoent layout");

/* Used only when the system has no /etc/services or /etc/protocols entry. */
static const struct { const char *name; int port; const char *proto; } g_services[] = {
    { "echo", 7, NULL }, { "discard", 9, NULL }, { "daytime", 13, NULL }, { "ftp-data", 20, "tcp" },
    { "ftp", 21, "tcp" }, { "ssh", 22, NULL }, { "telnet", 23, "tcp" }, { "smtp", 25, "tcp" },
    { "time", 37, NULL }, { "domain", 53, NULL }, { "tftp", 69, "udp" }, { "gopher", 70, "tcp" },
    { "finger", 79, "tcp" }, { "http", 80, NULL }, { "www", 80, NULL }, { "kerberos", 88, NULL },
    { "pop3", 110, "tcp" }, { "sunrpc", 111, NULL }, { "nntp", 119, "tcp" }, { "ntp", 123, "udp" },
    { "netbios-ns", 137, NULL }, { "netbios-dgm", 138, "udp" }, { "netbios-ssn", 139, "tcp" },
    { "imap", 143, "tcp" }, { "snmp", 161, "udp" }, { "ldap", 389, NULL }, { "https", 443, NULL },
    { "microsoft-ds", 445, NULL }, { "syslog", 514, "udp" }, { "submission", 587, "tcp" },
    { "ldaps", 636, "tcp" }, { "imaps", 993, "tcp" }, { "pop3s", 995, "tcp" }, { "ms-sql-s", 1433, "tcp" },
    { "mysql", 3306, "tcp" }, { "ms-wbt-server", 3389, "tcp" }, { "postgresql", 5432, "tcp" },
    { "http-alt", 8080, "tcp" },
};
static const struct { const char *name; int num; const char *alias; } g_protocols[] = {
    { "ip", 0, "IP" }, { "icmp", 1, "ICMP" }, { "igmp", 2, "IGMP" }, { "tcp", 6, "TCP" },
    { "udp", 17, "UDP" }, { "ipv6", 41, "IPv6" }, { "ipv6-icmp", 58, "IPv6-ICMP" }, { "raw", 255, "RAW" },
};

static __thread struct { struct ws_servent se; char *aliases[17]; char buf[1024]; } t_serv;
static __thread struct { struct ws_protoent pe; char *aliases[17]; char buf[512]; } t_proto;

/* Copy a NULL-terminated alias list into buf; returns the bytes used. */
static size_t copy_aliases(char **src, char **dst, int maxn, char *buf, size_t cap)
{
    size_t off = 0;
    int n = 0;
    for (; src && src[n] && n < maxn; n++) {
        size_t l = strlen(src[n]) + 1;
        if (off + l > cap) break;
        memcpy(buf + off, src[n], l);
        dst[n] = buf + off;
        off += l;
    }
    dst[n] = NULL;
    return off;
}

static struct ws_servent *serv_make(const char *name, char **aliases, int port_net, const char *proto)
{
    memset(&t_serv, 0, sizeof t_serv);
    size_t off = 0, cap = sizeof t_serv.buf;
    size_t nl = strlen(name) + 1, pl = strlen(proto) + 1;
    if (nl + pl > cap) return NULL;
    memcpy(t_serv.buf, name, nl); off += nl;
    memcpy(t_serv.buf + off, proto, pl);
    t_serv.se.s_name = t_serv.buf;
    t_serv.se.s_proto = t_serv.buf + off;
    off += pl;
    copy_aliases(aliases, t_serv.aliases, 16, t_serv.buf + off, cap - off);
    t_serv.se.s_aliases = t_serv.aliases;
    t_serv.se.s_port = (SHORT)port_net;
    return &t_serv.se;
}

static struct ws_servent *serv_lookup(const char *name, int port_net, const char *proto)
{
    struct servent se, *res = NULL;
    char buf[2048];
    if (name) getservbyname_r(name, proto, &se, buf, sizeof buf, &res);
    else getservbyport_r(port_net, proto, &se, buf, sizeof buf, &res);
    if (res) return serv_make(res->s_name, res->s_aliases, res->s_port, res->s_proto);
    for (size_t i = 0; i < sizeof g_services / sizeof g_services[0]; i++) {
        const char *p = g_services[i].proto;
        if (proto && p && strcasecmp(proto, p)) continue;
        if (name ? !strcasecmp(name, g_services[i].name) : ntohs((uint16_t)port_net) == g_services[i].port)
            return serv_make(g_services[i].name, NULL, htons((uint16_t)g_services[i].port), proto ? proto : p ? p : "tcp");
    }
    set_wsa(WSANO_DATA);
    return NULL;
}

API struct ws_servent *ws_getservbyname(const char *name, const char *proto)
{
    NEED_INIT(NULL);
    if (!name) { set_wsa(WSAEFAULT); return NULL; }
    return serv_lookup(name, 0, proto);
}

API struct ws_servent *ws_getservbyport(int port, const char *proto)
{
    NEED_INIT(NULL);
    return serv_lookup(NULL, port & 0xFFFF, proto);
}

static struct ws_protoent *proto_make(const char *name, char **aliases, const char *one_alias, int num)
{
    memset(&t_proto, 0, sizeof t_proto);
    size_t nl = strlen(name) + 1;
    if (nl > sizeof t_proto.buf) return NULL;
    memcpy(t_proto.buf, name, nl);
    t_proto.pe.p_name = t_proto.buf;
    char *one[2] = { (char *)one_alias, NULL };
    copy_aliases(aliases ? aliases : one_alias ? one : NULL, t_proto.aliases, 16, t_proto.buf + nl, sizeof t_proto.buf - nl);
    t_proto.pe.p_aliases = t_proto.aliases;
    t_proto.pe.p_proto = (SHORT)num;
    return &t_proto.pe;
}

static struct ws_protoent *proto_lookup(const char *name, int num)
{
    struct protoent pe, *res = NULL;
    char buf[1024];
    if (name) getprotobyname_r(name, &pe, buf, sizeof buf, &res);
    else getprotobynumber_r(num, &pe, buf, sizeof buf, &res);
    if (res) return proto_make(res->p_name, res->p_aliases, NULL, res->p_proto);
    for (size_t i = 0; i < sizeof g_protocols / sizeof g_protocols[0]; i++)
        if (name ? !strcasecmp(name, g_protocols[i].name) || !strcasecmp(name, g_protocols[i].alias) : num == g_protocols[i].num)
            return proto_make(g_protocols[i].name, NULL, g_protocols[i].alias, g_protocols[i].num);
    set_wsa(WSANO_DATA);
    return NULL;
}

API struct ws_protoent *ws_getprotobyname(const char *name) { NEED_INIT(NULL); if (!name) { set_wsa(WSAEFAULT); return NULL; } return proto_lookup(name, 0); }
API struct ws_protoent *ws_getprotobynumber(int num) { NEED_INIT(NULL); return proto_lookup(NULL, num); }

/* ------------------------------------------------------------------ */
/* getaddrinfo family                                                   */
/* ------------------------------------------------------------------ */

/* Windows addrinfo: ai_canonname comes BEFORE ai_addr (Linux has the
 * opposite order). The W and Ex variants share the prefix. */
struct ws_addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    size_t ai_addrlen;
    void *ai_canonname;        /* char* or WCHAR* */
    void *ai_addr;
    struct ws_addrinfo *ai_next;
};
struct ws_addrinfoex {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    size_t ai_addrlen;
    void *ai_canonname;
    void *ai_addr;
    void *ai_blob;
    size_t ai_bloblen;
    GUID *ai_provider;
    struct ws_addrinfoex *ai_next;
};
_Static_assert(sizeof(struct ws_addrinfo) == 48, "ADDRINFOA layout");
_Static_assert(sizeof(struct ws_addrinfoex) == 72, "ADDRINFOEX layout");

#define WS_AI_PASSIVE      0x01
#define WS_AI_CANONNAME    0x02
#define WS_AI_NUMERICHOST  0x04
#define WS_AI_NUMERICSERV  0x08
#define WS_AI_ALL          0x0100
#define WS_AI_ADDRCONFIG   0x0400
#define WS_AI_V4MAPPED     0x0800
#define WS_AI_DISABLE_IDN_ENCODING 0x80000
#define WS_AI_EXTENDED     0x80000000u

struct gai_hints { int flags, family, socktype, protocol; };

static int has_non_ascii(const char *s) { for (; s && *s; s++) if ((unsigned char)*s >= 0x80) return 1; return 0; }

/* Resolve with Windows rules; returns the Linux list or a WSA error. */
static DWORD resolve(const char *node, const char *service, const struct gai_hints *wh, int wide, struct addrinfo **res)
{
    struct addrinfo h = { 0 };
    char self[256];
    *res = NULL;
    if (!node && !service) return WSAHOST_NOT_FOUND;
    if (wh) {
        if ((unsigned)wh->flags & WS_AI_EXTENDED) return WSAEINVAL;   /* ADDRINFOEX2+ results are not produced */
        int laf = af_to_linux(wh->family);
        if (laf < 0) return WSAEAFNOSUPPORT;
        h.ai_family = laf;
        h.ai_socktype = wh->socktype;
        h.ai_protocol = wh->protocol;
        if (wh->flags & WS_AI_PASSIVE)     h.ai_flags |= AI_PASSIVE;
        if (wh->flags & WS_AI_CANONNAME)   h.ai_flags |= AI_CANONNAME;
        if (wh->flags & WS_AI_NUMERICHOST) h.ai_flags |= AI_NUMERICHOST;
        if (wh->flags & WS_AI_NUMERICSERV) h.ai_flags |= AI_NUMERICSERV;
        if (wh->flags & WS_AI_ALL)         h.ai_flags |= AI_ALL;
        if (wh->flags & WS_AI_ADDRCONFIG)  h.ai_flags |= AI_ADDRCONFIG;
        if (wh->flags & WS_AI_V4MAPPED)    h.ai_flags |= AI_V4MAPPED;
        if (wide && !(wh->flags & WS_AI_DISABLE_IDN_ENCODING) && has_non_ascii(node)) h.ai_flags |= AI_IDN | AI_CANONIDN;
    } else if (wide && has_non_ascii(node)) h.ai_flags |= AI_IDN;
    /* an empty host name means this computer on Windows */
    if (node && !*node) { local_hostname(self, sizeof self); node = self; }
    int r = getaddrinfo(node, service, &h, res);
    if (r == EAI_SERVICE && service) {
        /* no /etc/services entry: fall back to the well-known ports */
        for (size_t i = 0; i < sizeof g_services / sizeof g_services[0]; i++)
            if (!strcasecmp(service, g_services[i].name)) {
                char num[8];
                snprintf(num, sizeof num, "%d", g_services[i].port);
                r = getaddrinfo(node, num, &h, res);
                break;
            }
    }
    return r ? wsa_from_eai(r, errno) : 0;
}

/* Build the Windows list in one allocation per node (node + sockaddr +
 * canonical name); kind 0 = ADDRINFOA, 1 = ADDRINFOW, 2 = ADDRINFOEXA,
 * 3 = ADDRINFOEXW. Raw-socket duplicates are dropped unless asked for, as
 * Windows returns one entry per address and socket type requested. */
static void *build_ai(struct addrinfo *res, const struct gai_hints *wh, int kind)
{
    void *head = NULL, **link = &head;
    int first = 1;
    size_t nodesz = kind >= 2 ? sizeof(struct ws_addrinfoex) : sizeof(struct ws_addrinfo);
    for (struct addrinfo *i = res; i; i = i->ai_next) {
        if (i->ai_socktype == SOCK_RAW && (!wh || wh->socktype != SOCK_RAW)) continue;
        union ws_sa w;
        int wl = sa_to_win_buf((const union ws_sa *)(void *)i->ai_addr, i->ai_addrlen, &w);
        const char *cn = first ? i->ai_canonname : NULL;
        size_t cnsz = 0;
        if (cn) cnsz = (kind & 1) ? (size_t)lsiw_utf8_to_utf16(cn, -1, NULL, 0) * sizeof(WCHAR) : strlen(cn) + 1;
        char *mem = calloc(1, nodesz + 32 + cnsz);
        if (!mem) break;
        struct ws_addrinfo *n = (struct ws_addrinfo *)mem;     /* common prefix */
        n->ai_flags = 0;
        n->ai_family = af_to_win(i->ai_family);
        n->ai_socktype = i->ai_socktype;
        n->ai_protocol = i->ai_protocol;
        n->ai_addrlen = (size_t)wl;
        n->ai_addr = mem + nodesz;
        memcpy(n->ai_addr, &w, (size_t)wl);
        if (cn) {
            char *cp = mem + nodesz + 32;
            if (kind & 1) lsiw_utf8_to_utf16(cn, -1, (WCHAR *)(void *)cp, (int)(cnsz / sizeof(WCHAR)));
            else memcpy(cp, cn, cnsz);
            n->ai_canonname = cp;
        }
        *link = n;
        link = kind >= 2 ? (void **)&((struct ws_addrinfoex *)n)->ai_next : (void **)&n->ai_next;
        first = 0;
    }
    return head;
}

static void free_ai(void *p, int ex)
{
    while (p) {
        void *next = ex ? (void *)((struct ws_addrinfoex *)p)->ai_next : (void *)((struct ws_addrinfo *)p)->ai_next;
        free(p);
        p = next;
    }
}

static INT gai_common(const char *node, const char *service, const void *hints, int kind, void **out)
{
    if (!out) { set_wsa(WSAEINVAL); return WSAEINVAL; }
    *out = NULL;
    struct gai_hints gh, *ghp = NULL;
    if (hints) {
        const struct ws_addrinfo *wh = hints;    /* the first four ints are common to all kinds */
        gh = (struct gai_hints){ wh->ai_flags, wh->ai_family, wh->ai_socktype, wh->ai_protocol };
        ghp = &gh;
    }
    struct addrinfo *res = NULL;
    DWORD e = resolve(node, service, ghp, kind & 1, &res);
    if (!e) {
        *out = build_ai(res, ghp, kind);
        freeaddrinfo(res);
        if (!*out) e = WSAHOST_NOT_FOUND;
    }
    set_wsa(e);
    return (INT)e;
}

API INT ws_getaddrinfo(const char *node, const char *service, const struct ws_addrinfo *hints, struct ws_addrinfo **res)
{
    NEED_INIT(WSANOTINITIALISED);
    return gai_common(node, service, hints, 0, (void **)res);
}

API INT GetAddrInfoW(const WCHAR *node, const WCHAR *service, const struct ws_addrinfo *hints, struct ws_addrinfo **res)
{
    NEED_INIT(WSANOTINITIALISED);
    char *n = lsiw_w2u(node), *s = lsiw_w2u(service);
    INT r = gai_common(n, s, hints, 1, (void **)res);
    free(n); free(s);
    return r;
}

API void ws_freeaddrinfo(struct ws_addrinfo *ai) { free_ai(ai, 0); }
API void FreeAddrInfoW(struct ws_addrinfo *ai)   { free_ai(ai, 0); }
API void FreeAddrInfoEx(struct ws_addrinfoex *ai)  { free_ai(ai, 1); }
API void FreeAddrInfoExW(struct ws_addrinfoex *ai) { free_ai(ai, 1); }

/* GetAddrInfoEx is answered synchronously: with an OVERLAPPED the result is
 * returned directly (no WSA_IO_PENDING), which callers must accept as an
 * immediate completion. Namespaces other than DNS have nothing to offer. */
static INT gaiex(const char *node, const char *service, DWORD ns, const struct ws_addrinfoex *hints,
                 struct ws_addrinfoex **res, HANDLE *name_handle, int wide)
{
    if (name_handle) *name_handle = NULL;
    if (ns != 0 && ns != 12) { set_wsa(WSAEINVAL); return WSAEINVAL; }   /* NS_ALL, NS_DNS */
    return gai_common(node, service, hints, wide ? 3 : 2, (void **)res);
}

API INT GetAddrInfoExA(const char *node, const char *service, DWORD ns, GUID *nsid, const struct ws_addrinfoex *hints,
                       struct ws_addrinfoex **res, struct ws_timeval *timeout, WSAOVERLAPPED *ov,
                       void *routine, HANDLE *name_handle)
{
    (void)nsid; (void)timeout; (void)ov; (void)routine;
    NEED_INIT(WSANOTINITIALISED);
    return gaiex(node, service, ns, hints, res, name_handle, 0);
}

API INT GetAddrInfoExW(const WCHAR *node, const WCHAR *service, DWORD ns, GUID *nsid, const struct ws_addrinfoex *hints,
                       struct ws_addrinfoex **res, struct ws_timeval *timeout, WSAOVERLAPPED *ov,
                       void *routine, HANDLE *name_handle)
{
    (void)nsid; (void)timeout; (void)ov; (void)routine;
    NEED_INIT(WSANOTINITIALISED);
    char *n = lsiw_w2u(node), *s = lsiw_w2u(service);
    INT r = gaiex(n, s, ns, hints, res, name_handle, 1);
    free(n); free(s);
    return r;
}

/* Nothing is ever pending, so there is nothing to cancel. */
API INT GetAddrInfoExCancel(HANDLE *h) { (void)h; set_wsa(WSA_INVALID_HANDLE); return WSA_INVALID_HANDLE; }
API INT GetAddrInfoExOverlappedResult(WSAOVERLAPPED *ov)
{
    if (!ov) return WSAEINVAL;
    return (INT)status_to_wsa((DWORD)ov->Internal);
}

#define WS_NI_NOFQDN      0x01
#define WS_NI_NUMERICHOST 0x02
#define WS_NI_NAMEREQD    0x04
#define WS_NI_NUMERICSERV 0x08
#define WS_NI_DGRAM       0x10

static INT gni_common(const void *sa, INT salen, char *host, DWORD hostlen, char *serv, DWORD servlen, INT flags)
{
    union ws_sa a;
    if (!sa || salen < 2) { set_wsa(WSAEFAULT); return WSAEFAULT; }
    WORD fam; memcpy(&fam, sa, 2);
    if ((fam == WS_AF_INET && salen < 16) || (fam == WS_AF_INET6 && salen < 28)) { set_wsa(WSAEFAULT); return WSAEFAULT; }
    if (fam != WS_AF_INET && fam != WS_AF_INET6) { set_wsa(WSAEAFNOSUPPORT); return WSAEAFNOSUPPORT; }
    int len = sa_from_win(sa, salen, &a);
    if (len < 0) return (INT)lsiw_get_last_error();
    int lf = 0;
    if (flags & WS_NI_NOFQDN)      lf |= NI_NOFQDN;
    if (flags & WS_NI_NUMERICHOST) lf |= NI_NUMERICHOST;
    if (flags & WS_NI_NAMEREQD)    lf |= NI_NAMEREQD;
    if (flags & WS_NI_NUMERICSERV) lf |= NI_NUMERICSERV;
    if (flags & WS_NI_DGRAM)       lf |= NI_DGRAM;
    int r = getnameinfo(&a.sa, (socklen_t)len, host, host ? hostlen : 0, serv, serv ? servlen : 0, lf);
    DWORD e = wsa_from_eai(r, errno);
    set_wsa(e);
    return (INT)e;
}

API INT ws_getnameinfo(const void *sa, INT salen, char *host, DWORD hostlen, char *serv, DWORD servlen, INT flags)
{
    NEED_INIT(WSANOTINITIALISED);
    return gni_common(sa, salen, host, hostlen, serv, servlen, flags);
}

API INT GetNameInfoW(const void *sa, INT salen, WCHAR *host, DWORD hostlen, WCHAR *serv, DWORD servlen, INT flags)
{
    NEED_INIT(WSANOTINITIALISED);
    char h[NI_MAXHOST], s[NI_MAXSERV];
    INT r = gni_common(sa, salen, host ? h : NULL, sizeof h, serv ? s : NULL, sizeof s, flags);
    if (r) return r;
    if ((host && lsiw_u2w_buf(h, host, (int)hostlen) > (int)hostlen) ||
        (serv && lsiw_u2w_buf(s, serv, (int)servlen) > (int)servlen)) { set_wsa(WSAEFAULT); return WSAEFAULT; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* WSAConnectByName                                                     */
/* ------------------------------------------------------------------ */

static BOOL connect_by_name(SOCKET s, const char *node, const char *service, DWORD *llen, void *laddr,
                            DWORD *rlen, void *raddr, const struct ws_timeval *tv, void *reserved)
{
    NEED_INIT(FALSE);
    if (reserved || !node || !service) { set_wsa(WSAEINVAL); return FALSE; }
    struct ws_sock *st = sock_get(s);
    if (!st) return FALSE;
    BOOL ok = FALSE;
    int ms = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    struct addrinfo h = { 0 }, *res = NULL;
    h.ai_socktype = st->type;
    if (st->af == AF_INET6) {
        /* an IPv6 socket becomes dual-stack so IPv4 results can be used */
        int off = 0;
        setsockopt(st->fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        h.ai_family = AF_INET6;
        h.ai_flags = AI_V4MAPPED | AI_ALL;
    } else h.ai_family = st->af;
    int r = getaddrinfo(node, service, &h, &res);
    if (r) { set_wsa(wsa_from_eai(r, errno)); goto out; }
    DWORD last = WSAEHOSTUNREACH;
    for (struct addrinfo *i = res; i && !ok; i = i->ai_next) {
        union ws_sa w;
        int wl = sa_to_win_buf((const union ws_sa *)(void *)i->ai_addr, i->ai_addrlen, &w);
        if (do_connect(st, &w, wl, 1, ms) == 0) ok = TRUE;
        else last = lsiw_get_last_error();
    }
    freeaddrinfo(res);
    if (!ok) { set_wsa(last); goto out; }
    union ws_sa a; socklen_t al = sizeof a;
    if (laddr && llen && getsockname(st->fd, &a.sa, &al) == 0) { INT l = (INT)*llen; if (sa_out(&a, al, laddr, &l) == 0) *llen = (DWORD)l; }
    al = sizeof a;
    if (raddr && rlen && getpeername(st->fd, &a.sa, &al) == 0) { INT l = (INT)*rlen; if (sa_out(&a, al, raddr, &l) == 0) *rlen = (DWORD)l; }
out:
    sock_put(st);
    return ok;
}

API BOOL WSAConnectByNameA(SOCKET s, const char *node, const char *service, DWORD *llen, void *laddr,
                           DWORD *rlen, void *raddr, const struct ws_timeval *tv, void *reserved)
{
    return connect_by_name(s, node, service, llen, laddr, rlen, raddr, tv, reserved);
}

API BOOL WSAConnectByNameW(SOCKET s, const WCHAR *node, const WCHAR *service, DWORD *llen, void *laddr,
                           DWORD *rlen, void *raddr, const struct ws_timeval *tv, void *reserved)
{
    char *n = lsiw_w2u(node), *sv = lsiw_w2u(service);
    BOOL r = connect_by_name(s, n, sv, llen, laddr, rlen, raddr, tv, reserved);
    free(n); free(sv);
    return r;
}

/* ------------------------------------------------------------------ */
/* Name-space lookups and Winsock 1 leftovers                           */
/* ------------------------------------------------------------------ */

/* No name-space providers are registered: a lookup starts and finds nothing. */
API INT WSALookupServiceBeginA(void *restrictions, DWORD flags, HANDLE *h)
{
    (void)restrictions; (void)flags;
    NEED_INIT(SOCKET_ERROR);
    if (!h) { set_wsa(WSAEFAULT); return SOCKET_ERROR; }
    *h = malloc(1);
    return 0;
}
API INT WSALookupServiceBeginW(void *restrictions, DWORD flags, HANDLE *h) { return WSALookupServiceBeginA(restrictions, flags, h); }
API INT WSALookupServiceNextA(HANDLE h, DWORD flags, DWORD *len, void *results)
{
    (void)flags; (void)len; (void)results;
    NEED_INIT(SOCKET_ERROR);
    set_wsa(h ? WSA_E_NO_MORE : WSA_INVALID_HANDLE);
    return SOCKET_ERROR;
}
API INT WSALookupServiceNextW(HANDLE h, DWORD flags, DWORD *len, void *results) { return WSALookupServiceNextA(h, flags, len, results); }
API INT WSALookupServiceEnd(HANDLE h)
{
    NEED_INIT(SOCKET_ERROR);
    if (!h) { set_wsa(WSA_INVALID_HANDLE); return SOCKET_ERROR; }
    free(h);
    return 0;
}

API INT WSAProviderConfigChange(HANDLE *h, WSAOVERLAPPED *ov, void *cr) { (void)h; (void)ov; (void)cr; set_wsa(WSAEOPNOTSUPP); return SOCKET_ERROR; }
API INT WSANSPIoctl(HANDLE h, DWORD code, void *in, DWORD inlen, void *out, DWORD outlen, DWORD *ret, void *completion)
{
    (void)h; (void)code; (void)in; (void)inlen; (void)out; (void)outlen; (void)ret; (void)completion;
    set_wsa(WSAEOPNOTSUPP);
    return SOCKET_ERROR;
}

/* Winsock 2 has no blocking hooks: calls never "block" in the 1.1 sense. */
API BOOL WSAIsBlocking(void) { return FALSE; }
API int WSACancelBlockingCall(void) { return 0; }
API void *WSASetBlockingHook(void *fn) { (void)fn; set_wsa(WSAEOPNOTSUPP); return NULL; }
API int WSAUnhookBlockingHook(void) { return 0; }

/* ================================================================== */
/* iphlpapi                                                             */
/* ================================================================== */

/* ---- the interface model, gathered once per call ---- */

#define IF_TYPE_OTHER              1
#define IF_TYPE_ETHERNET_CSMACD    6
#define IF_TYPE_PPP                23
#define IF_TYPE_SOFTWARE_LOOPBACK  24
#define IF_TYPE_IEEE80211          71
#define IF_TYPE_TUNNEL             131

#define IfOperStatusUp             1
#define IfOperStatusDown           2
#define IfOperStatusTesting        3
#define IfOperStatusUnknown        4
#define IfOperStatusDormant        5
#define IfOperStatusNotPresent     6
#define IfOperStatusLowerLayerDown 7

struct if_ip { int fam; BYTE a[16]; int prefix; };
struct if_gw { int fam; BYTE a[16]; };

struct ifinfo {
    char name[IF_NAMESIZE];
    unsigned index, flags, mtu, arptype, maclen;
    int wireless, oper;
    DWORD iftype;
    BYTE mac[8];
    uint64_t speed;           /* bits per second, 0 if unknown */
    char descr[128];
    GUID guid;
    struct if_ip ips[32];
    int nip;
    struct if_gw gws[8];
    int ngw;
    uint64_t rx_bytes, rx_packets, rx_mcast, rx_drop, rx_err, tx_bytes, tx_packets, tx_drop, tx_err;
};

static int read_sysfs(const char *ifn, const char *file, char *buf, size_t cap)
{
    char path[256];
    snprintf(path, sizeof path, "/sys/class/net/%s/%s", ifn, file);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) n--;
    buf[n] = 0;
    return (int)n;
}

static uint64_t sysfs_u64(const char *ifn, const char *file, uint64_t def)
{
    char b[64];
    if (read_sysfs(ifn, file, b, sizeof b) <= 0) return def;
    char *end;
    long long v = strtoll(b, &end, 0);
    return v < 0 ? def : (uint64_t)v;
}

static uint64_t fnv1a(uint64_t h, const char *s)
{
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001b3ULL; }
    return h;
}

/* A stable GUID for an interface, derived from its name (Windows adapter
 * names are GUIDs; Linux has none). */
static void name_guid(const char *name, GUID *g)
{
    uint64_t a = fnv1a(fnv1a(0xcbf29ce484222325ULL, "lsiw-netif:"), name);
    uint64_t b = fnv1a(fnv1a(0x84222325cbf29ce4ULL, "lsiw-netif2:"), name);
    BYTE raw[16];
    memcpy(raw, &a, 8); memcpy(raw + 8, &b, 8);
    raw[7] = (BYTE)((raw[7] & 0x0F) | 0x50);    /* version 5 (name based) */
    raw[8] = (BYTE)((raw[8] & 0x3F) | 0x80);    /* RFC 4122 variant */
    memcpy(&g->Data1, raw, 4); memcpy(&g->Data2, raw + 4, 2); memcpy(&g->Data3, raw + 6, 2);
    memcpy(g->Data4, raw + 8, 8);
}

static void guid_str(const GUID *g, char *out, size_t cap)
{
    snprintf(out, cap, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static int prefix_of(const BYTE *mask, int len)
{
    int n = 0;
    for (int i = 0; i < len; i++) n += __builtin_popcount(mask[i]);
    return n;
}

static void fill_link(struct ifinfo *f)
{
    char b[256];
    f->flags = (unsigned)sysfs_u64(f->name, "flags", 0);
    f->mtu = (unsigned)sysfs_u64(f->name, "mtu", 0);
    f->arptype = (unsigned)sysfs_u64(f->name, "type", 0);
    if (!f->flags || !f->mtu) {           /* no sysfs (some containers): ask the kernel */
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            struct ifreq ir; memset(&ir, 0, sizeof ir);
            snprintf(ir.ifr_name, sizeof ir.ifr_name, "%s", f->name);
            if (!f->flags && ioctl(fd, SIOCGIFFLAGS, &ir) == 0) f->flags = (unsigned)(unsigned short)ir.ifr_flags;
            if (!f->mtu && ioctl(fd, SIOCGIFMTU, &ir) == 0) f->mtu = (unsigned)ir.ifr_mtu;
            if (ioctl(fd, SIOCGIFHWADDR, &ir) == 0 && !f->arptype) f->arptype = ir.ifr_hwaddr.sa_family;
            close(fd);
        }
    }
    if (f->flags & IFF_LOOPBACK) f->arptype = 772;
    snprintf(b, sizeof b, "/sys/class/net/%s/wireless", f->name);
    struct stat sb;
    f->wireless = stat(b, &sb) == 0;
    if (!f->wireless) { snprintf(b, sizeof b, "/sys/class/net/%s/phy80211", f->name); f->wireless = stat(b, &sb) == 0; }
    switch (f->arptype) {
    case 1:   f->iftype = f->wireless ? IF_TYPE_IEEE80211 : IF_TYPE_ETHERNET_CSMACD; break;
    case 772: f->iftype = IF_TYPE_SOFTWARE_LOOPBACK; break;
    case 512: f->iftype = IF_TYPE_PPP; break;
    case 768: case 769: case 776: case 778: case 823: case 65534: f->iftype = IF_TYPE_TUNNEL; break;
    default:  f->iftype = IF_TYPE_OTHER; break;
    }
    /* MAC address; the loopback and tunnels have none on Windows */
    if (f->iftype != IF_TYPE_SOFTWARE_LOOPBACK && read_sysfs(f->name, "address", b, sizeof b) > 0) {
        unsigned v[8]; int n = sscanf(b, "%x:%x:%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        unsigned al = (unsigned)sysfs_u64(f->name, "addr_len", (uint64_t)n);
        if (n > 0 && al <= 8) {
            int nz = 0;
            for (int i = 0; i < n; i++) { f->mac[i] = (BYTE)v[i]; nz |= v[i]; }
            f->maclen = nz ? (unsigned)n : 0;
        }
    }
    /* sysfs flags lack IFF_RUNNING; the carrier file says whether the link runs */
    int running = (f->flags & IFF_RUNNING) || sysfs_u64(f->name, "carrier", 0) == 1;
    int up = (f->flags & IFF_UP) && running;
    f->oper = up ? IfOperStatusUp : IfOperStatusDown;
    if (read_sysfs(f->name, "operstate", b, sizeof b) > 0) {
        /* "unknown" is what drivers without link reporting (the loopback) say */
        if (!strcmp(b, "up") || (!strcmp(b, "unknown") && (f->flags & IFF_UP))) f->oper = IfOperStatusUp;
        else if (!strcmp(b, "down")) f->oper = IfOperStatusDown;
        else if (!strcmp(b, "dormant")) f->oper = IfOperStatusDormant;
        else if (!strcmp(b, "notpresent")) f->oper = IfOperStatusNotPresent;
        else if (!strcmp(b, "lowerlayerdown")) f->oper = IfOperStatusLowerLayerDown;
        else if (!strcmp(b, "testing")) f->oper = IfOperStatusTesting;
    }
    uint64_t mbps = sysfs_u64(f->name, "speed", 0);
    f->speed = mbps && mbps < 10000000 ? mbps * 1000000ULL : f->iftype == IF_TYPE_SOFTWARE_LOOPBACK ? 1073741824ULL : 0;
    /* the description names the driver, which is the closest Linux has to an adapter model */
    char link[256], target[256];
    snprintf(link, sizeof link, "/sys/class/net/%s/device/driver", f->name);
    ssize_t tl = readlink(link, target, sizeof target - 1);
    if (f->iftype == IF_TYPE_SOFTWARE_LOOPBACK) snprintf(f->descr, sizeof f->descr, "Software Loopback Interface");
    else if (tl > 0) {
        target[tl] = 0;
        const char *drv = strrchr(target, '/');
        snprintf(f->descr, sizeof f->descr, "%.15s (%.100s)", f->name, drv ? drv + 1 : target);
    } else snprintf(f->descr, sizeof f->descr, "%s", f->name);
    name_guid(f->name, &f->guid);
    f->rx_bytes = sysfs_u64(f->name, "statistics/rx_bytes", 0);
    f->rx_packets = sysfs_u64(f->name, "statistics/rx_packets", 0);
    f->rx_mcast = sysfs_u64(f->name, "statistics/multicast", 0);
    f->rx_drop = sysfs_u64(f->name, "statistics/rx_dropped", 0);
    f->rx_err = sysfs_u64(f->name, "statistics/rx_errors", 0);
    f->tx_bytes = sysfs_u64(f->name, "statistics/tx_bytes", 0);
    f->tx_packets = sysfs_u64(f->name, "statistics/tx_packets", 0);
    f->tx_drop = sysfs_u64(f->name, "statistics/tx_dropped", 0);
    f->tx_err = sysfs_u64(f->name, "statistics/tx_errors", 0);
}

static void hex_to_bytes(const char *h, BYTE *out, int n)
{
    for (int i = 0; i < n; i++) { unsigned v = 0; sscanf(h + 2 * i, "%2x", &v); out[i] = (BYTE)v; }
}

static void add_gateways(struct ifinfo *ifs, int n)
{
    char line[512];
    FILE *fp = fopen("/proc/net/route", "re");
    if (fp) {
        if (fgets(line, sizeof line, fp)) {
            while (fgets(line, sizeof line, fp)) {
                char ifn[IF_NAMESIZE + 1]; unsigned long dest, gw, mask; unsigned flags;
                if (sscanf(line, "%16s %lx %lx %x %*s %*s %*s %lx", ifn, &dest, &gw, &flags, &mask) != 5) continue;
                if (dest || mask || !gw || !(flags & 2)) continue;     /* default route via a gateway */
                for (int i = 0; i < n; i++)
                    if (!strcmp(ifs[i].name, ifn) && ifs[i].ngw < 8) {
                        uint32_t g = (uint32_t)gw;
                        ifs[i].gws[ifs[i].ngw].fam = AF_INET;
                        memcpy(ifs[i].gws[ifs[i].ngw++].a, &g, 4);
                    }
            }
        }
        fclose(fp);
    }
    fp = fopen("/proc/net/ipv6_route", "re");
    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            char dst[33], src[33], nh[33], ifn[IF_NAMESIZE + 1]; unsigned dl, sl;
            if (sscanf(line, "%32s %x %32s %x %32s %*s %*s %*s %*s %16s", dst, &dl, src, &sl, nh, ifn) != 6) continue;
            if (dl != 0 || !strcmp(nh, "00000000000000000000000000000000")) continue;
            for (int i = 0; i < n; i++)
                if (!strcmp(ifs[i].name, ifn) && ifs[i].ngw < 8) {
                    ifs[i].gws[ifs[i].ngw].fam = AF_INET6;
                    hex_to_bytes(nh, ifs[i].gws[ifs[i].ngw++].a, 16);
                }
        }
        fclose(fp);
    }
}

/* All interfaces, physical ones first and the loopback last, as Windows
 * lists them. Returns the count; *out is malloc'd. */
static int collect_ifaces(struct ifinfo **out)
{
    *out = NULL;
    struct if_nameindex *ni = if_nameindex();
    if (!ni) return -1;
    int n = 0;
    while (ni[n].if_index) n++;
    struct ifinfo *ifs = calloc(n ? (size_t)n : 1, sizeof *ifs);
    if (!ifs) { if_freenameindex(ni); return -1; }
    for (int i = 0; i < n; i++) {
        snprintf(ifs[i].name, sizeof ifs[i].name, "%s", ni[i].if_name);
        ifs[i].index = ni[i].if_index;
        fill_link(&ifs[i]);
    }
    if_freenameindex(ni);
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *a = ifa; a; a = a->ifa_next) {
            if (!a->ifa_addr || (a->ifa_addr->sa_family != AF_INET && a->ifa_addr->sa_family != AF_INET6)) continue;
            for (int i = 0; i < n; i++) {
                if (strcmp(ifs[i].name, a->ifa_name) || ifs[i].nip >= 32) continue;
                struct if_ip *ip = &ifs[i].ips[ifs[i].nip++];
                ip->fam = a->ifa_addr->sa_family;
                if (ip->fam == AF_INET) {
                    memcpy(ip->a, &((struct sockaddr_in *)(void *)a->ifa_addr)->sin_addr, 4);
                    ip->prefix = a->ifa_netmask ? prefix_of((BYTE *)&((struct sockaddr_in *)(void *)a->ifa_netmask)->sin_addr, 4) : 32;
                } else {
                    memcpy(ip->a, &((struct sockaddr_in6 *)(void *)a->ifa_addr)->sin6_addr, 16);
                    ip->prefix = a->ifa_netmask ? prefix_of((BYTE *)&((struct sockaddr_in6 *)(void *)a->ifa_netmask)->sin6_addr, 16) : 128;
                }
                break;
            }
        }
        freeifaddrs(ifa);
    }
    add_gateways(ifs, n);
    /* stable order: by index, loopback last */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            int li = ifs[i].iftype == IF_TYPE_SOFTWARE_LOOPBACK, lj = ifs[j].iftype == IF_TYPE_SOFTWARE_LOOPBACK;
            if (li > lj || (li == lj && ifs[j].index < ifs[i].index)) { struct ifinfo t = ifs[i]; ifs[i] = ifs[j]; ifs[j] = t; }
        }
    *out = ifs;
    return n;
}

static struct ifinfo *find_if(struct ifinfo *ifs, int n, unsigned index)
{
    for (int i = 0; i < n; i++) if (ifs[i].index == index) return &ifs[i];
    return NULL;
}

/* DNS servers and search domain. With systemd-resolved (Fedora) the real
 * upstream servers are in /run/systemd/resolve/resolv.conf. */
struct dnsinfo { int n4, n6; struct in_addr v4[8]; struct in6_addr v6[8]; char domain[128]; };

static void read_dns(struct dnsinfo *d)
{
    memset(d, 0, sizeof *d);
    FILE *fp = fopen("/run/systemd/resolve/resolv.conf", "re");
    if (!fp) fp = fopen("/etc/resolv.conf", "re");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof line, fp)) {
        char key[32], val[256];
        if (sscanf(line, "%31s %255s", key, val) != 2) continue;
        if (!strcmp(key, "nameserver")) {
            char *pct = strchr(val, '%');
            if (pct) *pct = 0;
            if (d->n4 < 8 && inet_pton(AF_INET, val, &d->v4[d->n4]) == 1) d->n4++;
            else if (d->n6 < 8 && inet_pton(AF_INET6, val, &d->v6[d->n6]) == 1) d->n6++;
        } else if ((!strcmp(key, "domain") || !strcmp(key, "search")) && !d->domain[0] && strcmp(val, ".")) {
            snprintf(d->domain, sizeof d->domain, "%.127s", val);
        }
    }
    fclose(fp);
}

static uint64_t luid_of(const struct ifinfo *f) { return ((uint64_t)(f->iftype & 0xFFFF) << 48) | ((uint64_t)(f->index & 0xFFFFFF) << 24); }
static unsigned luid_index(uint64_t v) { return (unsigned)((v >> 24) & 0xFFFFFF); }

/* ---- a two-pass writer for structures packed into one caller buffer ---- */

struct bld { uint8_t *base; size_t off; int measure; _Alignas(16) uint8_t sink[2048]; };

static void *bal(struct bld *b, size_t n, size_t align)
{
    b->off = (b->off + align - 1) & ~(align - 1);
    void *p = b->measure ? b->sink : b->base + b->off;
    memset(p, 0, n > sizeof b->sink && b->measure ? sizeof b->sink : n);
    b->off += n;
    return p;
}

static WCHAR *bstr_w(struct bld *b, const char *s)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", s);
    int n = lsiw_utf8_to_utf16(tmp, -1, NULL, 0);
    WCHAR *w = bal(b, (size_t)n * sizeof(WCHAR), 2);
    if (!b->measure) lsiw_utf8_to_utf16(tmp, -1, w, n);
    return w;
}

static char *bstr_a(struct bld *b, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = bal(b, n, 1);
    if (!b->measure) memcpy(p, s, n);
    return p;
}

static void bsa(struct bld *b, int fam, const BYTE *a, uint32_t scope, SOCKET_ADDRESS *out)
{
    int wl = fam == AF_INET6 ? 28 : 16;
    BYTE *p = bal(b, (size_t)wl, 4);
    union ws_sa s; memset(&s, 0, sizeof s);
    if (fam == AF_INET6) {
        s.in6.sin6_family = WS_AF_INET6;
        memcpy(&s.in6.sin6_addr, a, 16);
        s.in6.sin6_scope_id = scope;
    } else {
        s.in.sin_family = WS_AF_INET;
        memcpy(&s.in.sin_addr, a, 4);
    }
    if (!b->measure) memcpy(p, &s, (size_t)wl);
    out->lpSockaddr = p;
    out->iSockaddrLength = wl;
}

/* ---- GetAdaptersAddresses ---- */

typedef union { ULONG64 Alignment; struct { ULONG Length; DWORD Flags; } s; } HDR8;

typedef struct ws_unicast {
    HDR8 h; struct ws_unicast *Next; SOCKET_ADDRESS Address;
    INT PrefixOrigin, SuffixOrigin, DadState;
    ULONG ValidLifetime, PreferredLifetime, LeaseLifetime;
    UCHAR OnLinkPrefixLength;
} WS_UNICAST;
typedef struct ws_simple_addr { HDR8 h; struct ws_simple_addr *Next; SOCKET_ADDRESS Address; } WS_SIMPLE_ADDR;
typedef struct ws_prefix { HDR8 h; struct ws_prefix *Next; SOCKET_ADDRESS Address; ULONG PrefixLength; } WS_PREFIX;
typedef struct ws_dns_suffix { struct ws_dns_suffix *Next; WCHAR String[256]; } WS_DNS_SUFFIX;

typedef struct ws_adapter {
    union { ULONG64 Alignment; struct { ULONG Length; DWORD IfIndex; } s; } h;
    struct ws_adapter *Next;
    char *AdapterName;
    WS_UNICAST *FirstUnicastAddress;
    WS_SIMPLE_ADDR *FirstAnycastAddress;
    WS_SIMPLE_ADDR *FirstMulticastAddress;
    WS_SIMPLE_ADDR *FirstDnsServerAddress;
    WCHAR *DnsSuffix;
    WCHAR *Description;
    WCHAR *FriendlyName;
    BYTE  PhysicalAddress[8];
    ULONG PhysicalAddressLength;
    ULONG Flags;
    ULONG Mtu;
    DWORD IfType;
    INT   OperStatus;
    DWORD Ipv6IfIndex;
    ULONG ZoneIndices[16];
    WS_PREFIX *FirstPrefix;
    ULONG64 TransmitLinkSpeed;
    ULONG64 ReceiveLinkSpeed;
    WS_SIMPLE_ADDR *FirstWinsServerAddress;
    WS_SIMPLE_ADDR *FirstGatewayAddress;
    ULONG Ipv4Metric, Ipv6Metric;
    ULONG64 Luid;
    SOCKET_ADDRESS Dhcpv4Server;
    uint32_t CompartmentId;
    GUID  NetworkGuid;
    INT   ConnectionType;
    INT   TunnelType;
    SOCKET_ADDRESS Dhcpv6Server;
    BYTE  Dhcpv6ClientDuid[130];
    ULONG Dhcpv6ClientDuidLength;
    ULONG Dhcpv6Iaid;
    WS_DNS_SUFFIX *FirstDnsSuffix;
} WS_ADAPTER;
_Static_assert(sizeof(WS_UNICAST) == 64, "IP_ADAPTER_UNICAST_ADDRESS_LH layout");
_Static_assert(sizeof(WS_SIMPLE_ADDR) == 32, "IP_ADAPTER_*_ADDRESS layout");
_Static_assert(sizeof(WS_PREFIX) == 40, "IP_ADAPTER_PREFIX layout");
_Static_assert(sizeof(WS_ADAPTER) == 448, "IP_ADAPTER_ADDRESSES_LH layout");
_Static_assert(offsetof(WS_ADAPTER, ZoneIndices) == 112 && offsetof(WS_ADAPTER, Luid) == 224 &&
               offsetof(WS_ADAPTER, Dhcpv6Server) == 280 && offsetof(WS_ADAPTER, FirstDnsSuffix) == 440,
               "IP_ADAPTER_ADDRESSES_LH offsets");

#define GAA_FLAG_SKIP_UNICAST       0x0001
#define GAA_FLAG_SKIP_ANYCAST       0x0002
#define GAA_FLAG_SKIP_MULTICAST     0x0004
#define GAA_FLAG_SKIP_DNS_SERVER    0x0008
#define GAA_FLAG_INCLUDE_PREFIX     0x0010
#define GAA_FLAG_SKIP_FRIENDLY_NAME 0x0020
#define GAA_FLAG_INCLUDE_GATEWAYS   0x0080

#define IP_ADAPTER_DHCP_ENABLED 0x04
#define IP_ADAPTER_NO_MULTICAST 0x10
#define IP_ADAPTER_IPV4_ENABLED 0x80
#define IP_ADAPTER_IPV6_ENABLED 0x100
#define IP_ADAPTER_ADDRESS_DNS_ELIGIBLE 0x01

static int is_ll6(const BYTE *a) { return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; }

static void gaa_build(struct bld *b, struct ifinfo *ifs, int n, int lfam, ULONG flags, const struct dnsinfo *dns)
{
    WS_ADAPTER *prev = NULL;
    for (int i = 0; i < n; i++) {
        struct ifinfo *f = &ifs[i];
        int loop = f->iftype == IF_TYPE_SOFTWARE_LOOPBACK;
        WS_ADAPTER *ad = bal(b, sizeof *ad, 8);
        if (prev) prev->Next = ad;
        prev = ad;
        ad->h.s.Length = sizeof *ad;
        ad->h.s.IfIndex = f->index;
        char gs[40];
        guid_str(&f->guid, gs, sizeof gs);
        ad->AdapterName = bstr_a(b, gs);
        int has4 = 0, has6 = 0;
        for (int k = 0; k < f->nip; k++) { if (f->ips[k].fam == AF_INET) has4 = 1; else has6 = 1; }
        if (!(flags & GAA_FLAG_SKIP_UNICAST)) {
            WS_UNICAST *up = NULL;
            for (int k = 0; k < f->nip; k++) {
                struct if_ip *ip = &f->ips[k];
                if (lfam != AF_UNSPEC && ip->fam != lfam) continue;
                WS_UNICAST *u = bal(b, sizeof *u, 8);
                if (up) up->Next = u; else ad->FirstUnicastAddress = u;
                up = u;
                u->h.s.Length = sizeof *u;
                int ll = ip->fam == AF_INET6 && is_ll6(ip->a);
                u->h.s.Flags = loop || ll ? 0 : IP_ADAPTER_ADDRESS_DNS_ELIGIBLE;
                bsa(b, ip->fam, ip->a, ll ? f->index : 0, &u->Address);
                /* origins: loopback and link-local are well known, the rest manual / router advertised */
                if (loop) { u->PrefixOrigin = 2; u->SuffixOrigin = 2; }
                else if (ll) { u->PrefixOrigin = 2; u->SuffixOrigin = 4; }
                else if (ip->fam == AF_INET6) { u->PrefixOrigin = 4; u->SuffixOrigin = 4; }
                else { u->PrefixOrigin = 1; u->SuffixOrigin = 1; }
                u->DadState = 4;                    /* IpDadStatePreferred */
                u->ValidLifetime = u->PreferredLifetime = u->LeaseLifetime = 0xFFFFFFFFu;
                u->OnLinkPrefixLength = (UCHAR)ip->prefix;
            }
        }
        if (!(flags & GAA_FLAG_SKIP_DNS_SERVER) && !loop && f->oper == IfOperStatusUp) {
            WS_SIMPLE_ADDR *dp = NULL;
            for (int k = 0; k < dns->n4 + dns->n6; k++) {
                int v6 = k >= dns->n4;
                if (lfam != AF_UNSPEC && lfam != (v6 ? AF_INET6 : AF_INET)) continue;
                WS_SIMPLE_ADDR *d = bal(b, sizeof *d, 8);
                if (dp) dp->Next = d; else ad->FirstDnsServerAddress = d;
                dp = d;
                d->h.s.Length = sizeof *d;
                bsa(b, v6 ? AF_INET6 : AF_INET, v6 ? (const BYTE *)&dns->v6[k - dns->n4] : (const BYTE *)&dns->v4[k], 0, &d->Address);
            }
        }
        ad->DnsSuffix = bstr_w(b, loop ? "" : dns->domain);
        ad->Description = bstr_w(b, f->descr);
        ad->FriendlyName = bstr_w(b, (flags & GAA_FLAG_SKIP_FRIENDLY_NAME) ? "" : f->name);
        memcpy(ad->PhysicalAddress, f->mac, 8);
        ad->PhysicalAddressLength = f->maclen;
        ad->Flags = (has4 ? IP_ADAPTER_IPV4_ENABLED : 0) | (has6 ? IP_ADAPTER_IPV6_ENABLED : 0) |
                    ((f->flags & IFF_MULTICAST) ? 0 : IP_ADAPTER_NO_MULTICAST);
        ad->Mtu = f->mtu;
        ad->IfType = f->iftype;
        ad->OperStatus = f->oper;
        ad->Ipv6IfIndex = has6 || loop ? f->index : 0;
        for (int z = 1; z < 16; z++) ad->ZoneIndices[z] = z <= 4 ? f->index : z < 14 ? 1 : 0;
        if (flags & GAA_FLAG_INCLUDE_PREFIX) {
            WS_PREFIX *pp = NULL;
            for (int k = 0; k < f->nip; k++) {
                struct if_ip *ip = &f->ips[k];
                if (lfam != AF_UNSPEC && ip->fam != lfam) continue;
                BYTE net[16] = { 0 };
                int len = ip->fam == AF_INET6 ? 16 : 4;
                for (int x = 0; x < len; x++) {
                    int bits = ip->prefix - x * 8;
                    BYTE m = bits >= 8 ? 0xFF : bits <= 0 ? 0 : (BYTE)(0xFF << (8 - bits));
                    net[x] = ip->a[x] & m;
                }
                WS_PREFIX *p = bal(b, sizeof *p, 8);
                if (pp) pp->Next = p; else ad->FirstPrefix = p;
                pp = p;
                p->h.s.Length = sizeof *p;
                bsa(b, ip->fam, net, 0, &p->Address);
                p->PrefixLength = (ULONG)ip->prefix;
            }
        }
        ad->TransmitLinkSpeed = ad->ReceiveLinkSpeed = f->speed;
        if (flags & GAA_FLAG_INCLUDE_GATEWAYS) {
            WS_SIMPLE_ADDR *gp = NULL;
            for (int k = 0; k < f->ngw; k++) {
                if (lfam != AF_UNSPEC && f->gws[k].fam != lfam) continue;
                WS_SIMPLE_ADDR *g = bal(b, sizeof *g, 8);
                if (gp) gp->Next = g; else ad->FirstGatewayAddress = g;
                gp = g;
                g->h.s.Length = sizeof *g;
                bsa(b, f->gws[k].fam, f->gws[k].a, f->gws[k].fam == AF_INET6 && is_ll6(f->gws[k].a) ? f->index : 0, &g->Address);
            }
        }
        ad->Ipv4Metric = ad->Ipv6Metric = loop ? 75 : 25;
        ad->Luid = luid_of(f);
        ad->CompartmentId = 1;
        ad->ConnectionType = 1;                     /* NET_IF_CONNECTION_DEDICATED */
        ad->TunnelType = f->iftype == IF_TYPE_TUNNEL ? 1 : 0;
        if (!loop && dns->domain[0]) {
            WS_DNS_SUFFIX *s = bal(b, sizeof *s, 8);
            if (!b->measure) lsiw_utf8_to_utf16(dns->domain, -1, s->String, 255);
            ad->FirstDnsSuffix = s;
        }
    }
}

API ULONG GetAdaptersAddresses(ULONG family, ULONG flags, void *reserved, void *buf, ULONG *size)
{
    if (!size || reserved) return ERROR_INVALID_PARAMETER;
    int lfam = family == WS_AF_UNSPEC ? AF_UNSPEC : family == WS_AF_INET ? AF_INET : family == WS_AF_INET6 ? AF_INET6 : -1;
    if (lfam < 0) return ERROR_INVALID_PARAMETER;
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs);
    if (n <= 0) { free(ifs); return ERROR_NO_DATA; }
    struct dnsinfo dns;
    read_dns(&dns);
    struct bld *b = calloc(1, sizeof *b);
    if (!b) { free(ifs); return ERROR_NOT_ENOUGH_MEMORY; }
    b->measure = 1;
    gaa_build(b, ifs, n, lfam, flags, &dns);
    size_t need = b->off;
    ULONG r = ERROR_SUCCESS;
    if (!buf || *size < need) { *size = (ULONG)need; r = ERROR_BUFFER_OVERFLOW; }
    else {
        b->measure = 0; b->base = buf; b->off = 0;
        gaa_build(b, ifs, n, lfam, flags, &dns);
    }
    free(b);
    free(ifs);
    return r;
}

/* ---- GetAdaptersInfo / GetNetworkParams ---- */

typedef struct ws_ip_addr_string { struct ws_ip_addr_string *Next; char IpAddress[16]; char IpMask[16]; DWORD Context; } WS_IP_ADDR_STRING;
typedef struct ws_adapter_info {
    struct ws_adapter_info *Next;
    DWORD ComboIndex;
    char  AdapterName[260];
    char  Description[132];
    UINT  AddressLength;
    BYTE  Address[8];
    DWORD Index;
    UINT  Type;
    UINT  DhcpEnabled;
    WS_IP_ADDR_STRING *CurrentIpAddress;
    WS_IP_ADDR_STRING IpAddressList, GatewayList, DhcpServer;
    BOOL  HaveWins;
    WS_IP_ADDR_STRING PrimaryWinsServer, SecondaryWinsServer;
    int64_t LeaseObtained, LeaseExpires;
} WS_ADAPTER_INFO;
_Static_assert(sizeof(WS_IP_ADDR_STRING) == 48, "IP_ADDR_STRING layout");
_Static_assert(sizeof(WS_ADAPTER_INFO) == 704, "IP_ADAPTER_INFO layout");

static void ip4_str(const BYTE *a, char *out)
{
    snprintf(out, 16, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

static void mask4(int prefix, BYTE *m)
{
    uint32_t v = prefix <= 0 ? 0 : prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
    v = htonl(v);
    memcpy(m, &v, 4);
}

static void gai_build(struct bld *b, struct ifinfo *ifs, int n)
{
    WS_ADAPTER_INFO *prev = NULL, *ads[64];
    int na = 0;
    for (int i = 0; i < n && na < 64; i++) {
        if (ifs[i].iftype == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        WS_ADAPTER_INFO *ai = bal(b, sizeof *ai, 8);
        if (prev) prev->Next = ai;
        prev = ai;
        ads[na++] = ai;
    }
    na = 0;
    for (int i = 0; i < n && na < 64; i++) {
        struct ifinfo *f = &ifs[i];
        if (f->iftype == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        WS_ADAPTER_INFO *ai = ads[na++];
        if (b->measure) ai = (WS_ADAPTER_INFO *)(void *)b->sink;
        ai->ComboIndex = f->index;
        guid_str(&f->guid, ai->AdapterName, sizeof ai->AdapterName);
        snprintf(ai->Description, sizeof ai->Description, "%s", f->descr);
        ai->AddressLength = f->maclen;
        memcpy(ai->Address, f->mac, 8);
        ai->Index = f->index;
        ai->Type = f->iftype == IF_TYPE_IEEE80211 ? IF_TYPE_IEEE80211 : f->iftype == IF_TYPE_PPP ? IF_TYPE_PPP :
                   f->iftype == IF_TYPE_TUNNEL ? IF_TYPE_TUNNEL : f->iftype == IF_TYPE_ETHERNET_CSMACD ? 6 : IF_TYPE_OTHER;
        strcpy(ai->IpAddressList.IpAddress, "0.0.0.0");
        strcpy(ai->IpAddressList.IpMask, "0.0.0.0");
        strcpy(ai->GatewayList.IpAddress, "0.0.0.0");
        strcpy(ai->GatewayList.IpMask, "0.0.0.0");
        WS_IP_ADDR_STRING *last = &ai->IpAddressList;
        int first = 1;
        for (int k = 0; k < f->nip; k++) {
            if (f->ips[k].fam != AF_INET) continue;
            WS_IP_ADDR_STRING *s = first ? &ai->IpAddressList : bal(b, sizeof *s, 8);
            if (!first) { last->Next = s; last = s; }
            if (b->measure && !first) continue;
            BYTE m[4];
            mask4(f->ips[k].prefix, m);
            ip4_str(f->ips[k].a, s->IpAddress);
            ip4_str(m, s->IpMask);
            s->Context = f->index;
            first = 0;
        }
        if (!first) ai->CurrentIpAddress = &ai->IpAddressList;
        for (int k = 0; k < f->ngw; k++)
            if (f->gws[k].fam == AF_INET) { ip4_str(f->gws[k].a, ai->GatewayList.IpAddress); strcpy(ai->GatewayList.IpMask, "255.255.255.255"); break; }
    }
}

API DWORD GetAdaptersInfo(void *buf, ULONG *size)
{
    if (!size) return ERROR_INVALID_PARAMETER;
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs), phys = 0;
    for (int i = 0; i < n; i++) if (ifs[i].iftype != IF_TYPE_SOFTWARE_LOOPBACK) phys++;
    if (n <= 0 || !phys) { free(ifs); return ERROR_NO_DATA; }
    struct bld *b = calloc(1, sizeof *b);
    if (!b) { free(ifs); return ERROR_NOT_ENOUGH_MEMORY; }
    b->measure = 1;
    gai_build(b, ifs, n);
    size_t need = b->off;
    DWORD r = ERROR_SUCCESS;
    if (!buf || *size < need) { *size = (ULONG)need; r = ERROR_BUFFER_OVERFLOW; }
    else { b->measure = 0; b->base = buf; b->off = 0; gai_build(b, ifs, n); }
    free(b);
    free(ifs);
    return r;
}

typedef struct {
    char HostName[132];
    char DomainName[132];
    WS_IP_ADDR_STRING *CurrentDnsServer;
    WS_IP_ADDR_STRING DnsServerList;
    UINT NodeType;
    char ScopeId[260];
    UINT EnableRouting, EnableProxy, EnableDns;
} WS_FIXED_INFO;
_Static_assert(sizeof(WS_FIXED_INFO) == 600, "FIXED_INFO layout");

API DWORD GetNetworkParams(void *buf, ULONG *size)
{
    if (!size) return ERROR_INVALID_PARAMETER;
    struct dnsinfo dns;
    read_dns(&dns);
    ULONG need = (ULONG)(sizeof(WS_FIXED_INFO) + (dns.n4 > 1 ? (size_t)(dns.n4 - 1) * sizeof(WS_IP_ADDR_STRING) : 0));
    if (!buf || *size < need) { *size = need; return ERROR_BUFFER_OVERFLOW; }
    WS_FIXED_INFO *fi = buf;
    memset(fi, 0, need);
    char full[256];
    if (gethostname(full, sizeof full) < 0) strcpy(full, "localhost");
    full[sizeof full - 1] = 0;
    char *dot = strchr(full, '.');
    if (dot) *dot = 0;
    snprintf(fi->HostName, sizeof fi->HostName, "%.128s", full);
    snprintf(fi->DomainName, sizeof fi->DomainName, "%.128s", dot ? dot + 1 : dns.domain);
    WS_IP_ADDR_STRING *s = &fi->DnsServerList, *extra = (WS_IP_ADDR_STRING *)(void *)(fi + 1);
    for (int i = 0; i < dns.n4; i++) {
        if (i) { s->Next = extra; s = extra++; }
        inet_ntop(AF_INET, &dns.v4[i], s->IpAddress, sizeof s->IpAddress);
        strcpy(s->IpMask, "255.255.255.255");
    }
    fi->NodeType = 1;                     /* BROADCAST_NODETYPE: no WINS */
    FILE *fw = fopen("/proc/sys/net/ipv4/ip_forward", "re");
    if (fw) { int v = 0; if (fscanf(fw, "%d", &v) == 1) fi->EnableRouting = v != 0; fclose(fw); }
    return ERROR_SUCCESS;
}

/* ---- best interface / route ---- */

static int if_of_local(const union ws_sa *src)
{
    struct ifaddrs *ifa = NULL;
    int idx = 0;
    if (getifaddrs(&ifa) < 0) return 0;
    for (struct ifaddrs *i = ifa; i && !idx; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != src->sa.sa_family) continue;
        if (src->sa.sa_family == AF_INET ? ((struct sockaddr_in *)(void *)i->ifa_addr)->sin_addr.s_addr == src->in.sin_addr.s_addr
                                         : !memcmp(&((struct sockaddr_in6 *)(void *)i->ifa_addr)->sin6_addr, &src->in6.sin6_addr, 16))
            idx = (int)if_nametoindex(i->ifa_name);
    }
    freeifaddrs(ifa);
    return idx;
}

static DWORD best_if(const union ws_sa *dst, socklen_t dl, DWORD *out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    union ws_sa src; socklen_t sl = sizeof src;
    if (route_source(dst, dl, &src, &sl) < 0) return ERROR_CAN_NOT_COMPLETE;
    int idx = if_of_local(&src);
    if (!idx && sa_is_any(dst) == 0) {
        /* loopback destinations resolve to the loopback interface */
        idx = (int)if_nametoindex("lo");
    }
    if (!idx) return ERROR_CAN_NOT_COMPLETE;
    *out = (DWORD)idx;
    return NO_ERROR;
}

API DWORD GetBestInterface(DWORD dest, DWORD *index)
{
    union ws_sa d; memset(&d, 0, sizeof d);
    d.in.sin_family = AF_INET;
    d.in.sin_addr.s_addr = dest;
    return best_if(&d, sizeof d.in, index);
}

API DWORD GetBestInterfaceEx(void *dest, DWORD *index)
{
    union ws_sa d;
    if (!dest) return ERROR_INVALID_PARAMETER;
    WORD fam; memcpy(&fam, dest, 2);
    int len = sa_from_win(dest, fam == WS_AF_INET6 ? 28 : 16, &d);
    if (len < 0 || (d.sa.sa_family != AF_INET && d.sa.sa_family != AF_INET6)) return ERROR_INVALID_PARAMETER;
    return best_if(&d, (socklen_t)len, index);
}

typedef struct {
    DWORD dwForwardDest, dwForwardMask, dwForwardPolicy, dwForwardNextHop, dwForwardIfIndex;
    DWORD dwForwardType, dwForwardProto, dwForwardAge, dwForwardNextHopAS;
    DWORD dwForwardMetric1, dwForwardMetric2, dwForwardMetric3, dwForwardMetric4, dwForwardMetric5;
} WS_MIB_IPFORWARDROW;
_Static_assert(sizeof(WS_MIB_IPFORWARDROW) == 56, "MIB_IPFORWARDROW layout");

/* IPv4 routes from /proc/net/route (values there are already in network order). */
static int read_routes(WS_MIB_IPFORWARDROW **out)
{
    *out = NULL;
    FILE *fp = fopen("/proc/net/route", "re");
    if (!fp) return 0;
    char line[512];
    int n = 0, cap = 0;
    if (!fgets(line, sizeof line, fp)) { fclose(fp); return 0; }
    while (fgets(line, sizeof line, fp)) {
        char ifn[IF_NAMESIZE + 1]; unsigned long dest, gw, mask; unsigned flags, metric;
        if (sscanf(line, "%16s %lx %lx %x %*s %*s %u %lx", ifn, &dest, &gw, &flags, &metric, &mask) != 6) continue;
        if (!(flags & 1)) continue;                     /* RTF_UP */
        if (n == cap) { cap = cap ? cap * 2 : 16; WS_MIB_IPFORWARDROW *nr = realloc(*out, (size_t)cap * sizeof **out); if (!nr) break; *out = nr; }
        WS_MIB_IPFORWARDROW *r = &(*out)[n++];
        memset(r, 0, sizeof *r);
        r->dwForwardDest = (DWORD)dest;
        r->dwForwardMask = (DWORD)mask;
        r->dwForwardNextHop = (DWORD)gw;
        r->dwForwardIfIndex = if_nametoindex(ifn);
        r->dwForwardType = (flags & 2) ? 4 : 3;          /* MIB_IPROUTE_TYPE_INDIRECT / DIRECT */
        r->dwForwardProto = 3;                          /* MIB_IPPROTO_NETMGMT */
        r->dwForwardMetric1 = metric;
        r->dwForwardMetric2 = r->dwForwardMetric3 = r->dwForwardMetric4 = r->dwForwardMetric5 = 0xFFFFFFFFu;
    }
    fclose(fp);
    return n;
}

static int fwd_cmp(const void *a, const void *b)
{
    const WS_MIB_IPFORWARDROW *x = a, *y = b;
    uint32_t xd = ntohl(x->dwForwardDest), yd = ntohl(y->dwForwardDest);
    if (xd != yd) return xd < yd ? -1 : 1;
    uint32_t xm = ntohl(x->dwForwardMask), ym = ntohl(y->dwForwardMask);
    if (xm != ym) return xm < ym ? -1 : 1;
    uint32_t xn = ntohl(x->dwForwardNextHop), yn = ntohl(y->dwForwardNextHop);
    return xn < yn ? -1 : xn > yn;
}

API DWORD GetIpForwardTable(void *table, ULONG *size, BOOL order)
{
    if (!size) return ERROR_INVALID_PARAMETER;
    WS_MIB_IPFORWARDROW *rows;
    int n = read_routes(&rows);
    ULONG need = (ULONG)(4 + (size_t)n * sizeof *rows);
    if (n && need < 4 + sizeof *rows) need = 4 + sizeof *rows;
    if (!table || *size < need) { *size = need; free(rows); return ERROR_INSUFFICIENT_BUFFER; }
    if (order && n) qsort(rows, (size_t)n, sizeof *rows, fwd_cmp);
    DWORD cnt = (DWORD)n;
    memcpy(table, &cnt, 4);
    if (n) memcpy((char *)table + 4, rows, (size_t)n * sizeof *rows);
    free(rows);
    return n ? NO_ERROR : ERROR_NO_DATA;
}

API DWORD GetBestRoute(DWORD dest, DWORD src, WS_MIB_IPFORWARDROW *best)
{
    (void)src;
    if (!best) return ERROR_INVALID_PARAMETER;
    WS_MIB_IPFORWARDROW *rows;
    int n = read_routes(&rows), pick = -1;
    for (int i = 0; i < n; i++) {
        if ((dest & rows[i].dwForwardMask) != rows[i].dwForwardDest) continue;
        if (pick < 0) { pick = i; continue; }
        uint32_t m = ntohl(rows[i].dwForwardMask), pm = ntohl(rows[pick].dwForwardMask);
        if (m > pm || (m == pm && rows[i].dwForwardMetric1 < rows[pick].dwForwardMetric1)) pick = i;
    }
    if (pick >= 0) *best = rows[pick];
    free(rows);
    return pick >= 0 ? NO_ERROR : ERROR_CAN_NOT_COMPLETE;
}

/* ---- interface tables ---- */

typedef struct {
    WCHAR wszName[256];
    DWORD dwIndex, dwType, dwMtu, dwSpeed, dwPhysAddrLen;
    BYTE  bPhysAddr[8];
    DWORD dwAdminStatus, dwOperStatus, dwLastChange;
    DWORD dwInOctets, dwInUcastPkts, dwInNUcastPkts, dwInDiscards, dwInErrors, dwInUnknownProtos;
    DWORD dwOutOctets, dwOutUcastPkts, dwOutNUcastPkts, dwOutDiscards, dwOutErrors, dwOutQLen;
    DWORD dwDescrLen;
    BYTE  bDescr[256];
} WS_MIB_IFROW;
_Static_assert(sizeof(WS_MIB_IFROW) == 860, "MIB_IFROW layout");

static void fill_ifrow(WS_MIB_IFROW *r, const struct ifinfo *f)
{
    memset(r, 0, sizeof *r);
    char nm[80], gs[40];
    guid_str(&f->guid, gs, sizeof gs);
    snprintf(nm, sizeof nm, "\\DEVICE\\TCPIP_%s", gs);
    lsiw_utf8_to_utf16(nm, -1, r->wszName, 256);
    r->dwIndex = f->index;
    r->dwType = f->iftype;
    r->dwMtu = f->mtu;
    r->dwSpeed = f->speed > 0xFFFFFFFFull ? 0xFFFFFFFFu : (DWORD)f->speed;
    r->dwPhysAddrLen = f->maclen;
    memcpy(r->bPhysAddr, f->mac, 8);
    r->dwAdminStatus = (f->flags & IFF_UP) ? 1 : 2;
    r->dwOperStatus = f->oper == IfOperStatusUp ? 5 : 0;   /* IF_OPER_STATUS_OPERATIONAL / NON_OPERATIONAL */
    r->dwInOctets = (DWORD)f->rx_bytes;
    r->dwInUcastPkts = (DWORD)(f->rx_packets - (f->rx_mcast < f->rx_packets ? f->rx_mcast : f->rx_packets));
    r->dwInNUcastPkts = (DWORD)f->rx_mcast;
    r->dwInDiscards = (DWORD)f->rx_drop;
    r->dwInErrors = (DWORD)f->rx_err;
    r->dwOutOctets = (DWORD)f->tx_bytes;
    r->dwOutUcastPkts = (DWORD)f->tx_packets;
    r->dwOutDiscards = (DWORD)f->tx_drop;
    r->dwOutErrors = (DWORD)f->tx_err;
    size_t dl = strlen(f->descr);
    if (dl > 255) dl = 255;
    memcpy(r->bDescr, f->descr, dl);
    r->dwDescrLen = (DWORD)dl + 1;
}

API DWORD GetIfEntry(WS_MIB_IFROW *row)
{
    if (!row) return ERROR_INVALID_PARAMETER;
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs);
    struct ifinfo *f = n > 0 ? find_if(ifs, n, row->dwIndex) : NULL;
    if (f) fill_ifrow(row, f);
    free(ifs);
    return f ? NO_ERROR : ERROR_INVALID_DATA;
}

static int ifrow_cmp(const void *a, const void *b)
{
    DWORD x = ((const WS_MIB_IFROW *)a)->dwIndex, y = ((const WS_MIB_IFROW *)b)->dwIndex;
    return x < y ? -1 : x > y;
}

API DWORD GetIfTable(void *table, ULONG *size, BOOL order)
{
    if (!size) return ERROR_INVALID_PARAMETER;
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs);
    if (n < 0) n = 0;
    ULONG need = (ULONG)(4 + (size_t)(n ? n : 1) * sizeof(WS_MIB_IFROW));
    if (!table || *size < need) { *size = need; free(ifs); return ERROR_INSUFFICIENT_BUFFER; }
    DWORD cnt = (DWORD)n;
    memcpy(table, &cnt, 4);
    WS_MIB_IFROW *rows = (WS_MIB_IFROW *)(void *)((char *)table + 4);
    for (int i = 0; i < n; i++) fill_ifrow(&rows[i], &ifs[i]);
    if (order) qsort(rows, (size_t)n, sizeof *rows, ifrow_cmp);
    free(ifs);
    return NO_ERROR;
}

typedef struct {
    ULONG64 InterfaceLuid;
    DWORD InterfaceIndex;
    GUID  InterfaceGuid;
    WCHAR Alias[257];
    WCHAR Description[257];
    ULONG PhysicalAddressLength;
    BYTE  PhysicalAddress[32];
    BYTE  PermanentPhysicalAddress[32];
    ULONG Mtu;
    DWORD Type;
    INT   TunnelType, MediaType, PhysicalMediumType, AccessType, DirectionType;
    BYTE  InterfaceAndOperStatusFlags;
    INT   OperStatus, AdminStatus, MediaConnectState;
    GUID  NetworkGuid;
    INT   ConnectionType;
    ULONG64 TransmitLinkSpeed, ReceiveLinkSpeed;
    ULONG64 InOctets, InUcastPkts, InNUcastPkts, InDiscards, InErrors, InUnknownProtos;
    ULONG64 InUcastOctets, InMulticastOctets, InBroadcastOctets;
    ULONG64 OutOctets, OutUcastPkts, OutNUcastPkts, OutDiscards, OutErrors;
    ULONG64 OutUcastOctets, OutMulticastOctets, OutBroadcastOctets, OutQLen;
} WS_MIB_IF_ROW2;
_Static_assert(sizeof(WS_MIB_IF_ROW2) == 1352, "MIB_IF_ROW2 layout");
_Static_assert(offsetof(WS_MIB_IF_ROW2, OperStatus) == 1156 && offsetof(WS_MIB_IF_ROW2, TransmitLinkSpeed) == 1192,
               "MIB_IF_ROW2 offsets");

static void fill_ifrow2(WS_MIB_IF_ROW2 *r, const struct ifinfo *f)
{
    memset(r, 0, sizeof *r);
    int loop = f->iftype == IF_TYPE_SOFTWARE_LOOPBACK;
    r->InterfaceLuid = luid_of(f);
    r->InterfaceIndex = f->index;
    r->InterfaceGuid = f->guid;
    lsiw_utf8_to_utf16(f->name, -1, r->Alias, 257);
    lsiw_utf8_to_utf16(f->descr, -1, r->Description, 257);
    r->PhysicalAddressLength = f->maclen;
    memcpy(r->PhysicalAddress, f->mac, 8);
    memcpy(r->PermanentPhysicalAddress, f->mac, 8);
    r->Mtu = f->mtu;
    r->Type = f->iftype;
    r->TunnelType = f->iftype == IF_TYPE_TUNNEL ? 1 : 0;
    r->MediaType = loop ? 11 : f->iftype == IF_TYPE_TUNNEL ? 19 : 0;     /* NdisMediumLoopback / IP / 802_3 */
    r->PhysicalMediumType = f->iftype == IF_TYPE_IEEE80211 ? 9 : f->iftype == IF_TYPE_ETHERNET_CSMACD ? 14 : 0;
    r->AccessType = loop ? 1 : (f->flags & IFF_POINTOPOINT) ? 3 : 2;
    r->DirectionType = 0;
    /* HardwareInterface (bit 0) and ConnectorPresent (bit 2) for real devices */
    if (!loop && f->iftype != IF_TYPE_TUNNEL && f->maclen) r->InterfaceAndOperStatusFlags = 0x05;
    if (f->oper != IfOperStatusUp) r->InterfaceAndOperStatusFlags |= 0x10;   /* NotMediaConnected */
    r->OperStatus = f->oper;
    r->AdminStatus = (f->flags & IFF_UP) ? 1 : 2;
    r->MediaConnectState = f->oper == IfOperStatusUp ? 1 : 2;
    r->ConnectionType = 1;
    r->TransmitLinkSpeed = r->ReceiveLinkSpeed = f->speed;
    r->InOctets = f->rx_bytes;
    r->InNUcastPkts = f->rx_mcast;
    r->InUcastPkts = f->rx_packets - (f->rx_mcast < f->rx_packets ? f->rx_mcast : f->rx_packets);
    r->InDiscards = f->rx_drop;
    r->InErrors = f->rx_err;
    r->OutOctets = f->tx_bytes;
    r->OutUcastPkts = f->tx_packets;
    r->OutDiscards = f->tx_drop;
    r->OutErrors = f->tx_err;
}

API DWORD GetIfEntry2(WS_MIB_IF_ROW2 *row)
{
    if (!row) return ERROR_INVALID_PARAMETER;
    unsigned idx = row->InterfaceLuid ? luid_index(row->InterfaceLuid) : row->InterfaceIndex;
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs);
    struct ifinfo *f = n > 0 ? find_if(ifs, n, idx) : NULL;
    if (f) fill_ifrow2(row, f);
    free(ifs);
    return f ? NO_ERROR : ERROR_FILE_NOT_FOUND;
}

API DWORD GetIfEntry2Ex(INT level, WS_MIB_IF_ROW2 *row) { (void)level; return GetIfEntry2(row); }

/* Tables from the *2 functions are freed with FreeMibTable: process heap. */
API DWORD GetIfTable2(void **table)
{
    if (!table) return ERROR_INVALID_PARAMETER;
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs);
    if (n < 0) n = 0;
    char *t = lsiw_heap_alloc(8 + (size_t)(n ? n : 1) * sizeof(WS_MIB_IF_ROW2), 1);
    if (!t) { free(ifs); return ERROR_NOT_ENOUGH_MEMORY; }
    ULONG cnt = (ULONG)n;
    memcpy(t, &cnt, 4);
    for (int i = 0; i < n; i++) fill_ifrow2((WS_MIB_IF_ROW2 *)(void *)(t + 8) + i, &ifs[i]);
    free(ifs);
    *table = t;
    return NO_ERROR;
}

API DWORD GetIfTable2Ex(INT level, void **table) { (void)level; return GetIfTable2(table); }
API void FreeMibTable(void *p) { if (p) lsiw_heap_free(p); }

typedef struct { DWORD dwAddr, dwIndex, dwMask, dwBCastAddr, dwReasmSize; USHORT unused1, wType; } WS_MIB_IPADDRROW;
_Static_assert(sizeof(WS_MIB_IPADDRROW) == 24, "MIB_IPADDRROW layout");

static int ipaddr_cmp(const void *a, const void *b)
{
    uint32_t x = ntohl(((const WS_MIB_IPADDRROW *)a)->dwAddr), y = ntohl(((const WS_MIB_IPADDRROW *)b)->dwAddr);
    return x < y ? -1 : x > y;
}

API DWORD GetIpAddrTable(void *table, ULONG *size, BOOL order)
{
    if (!size) return ERROR_INVALID_PARAMETER;
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs), cnt = 0;
    for (int i = 0; i < n; i++) for (int k = 0; k < ifs[i].nip; k++) if (ifs[i].ips[k].fam == AF_INET) cnt++;
    ULONG need = (ULONG)(4 + (size_t)(cnt ? cnt : 1) * sizeof(WS_MIB_IPADDRROW));
    if (!table || *size < need) { *size = need; free(ifs); return ERROR_INSUFFICIENT_BUFFER; }
    WS_MIB_IPADDRROW *rows = (WS_MIB_IPADDRROW *)(void *)((char *)table + 4);
    int j = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < ifs[i].nip; k++) {
            if (ifs[i].ips[k].fam != AF_INET) continue;
            WS_MIB_IPADDRROW *r = &rows[j++];
            memset(r, 0, sizeof *r);
            memcpy(&r->dwAddr, ifs[i].ips[k].a, 4);
            r->dwIndex = ifs[i].index;
            BYTE m[4]; mask4(ifs[i].ips[k].prefix, m); memcpy(&r->dwMask, m, 4);
            r->dwBCastAddr = 1;          /* Windows reports 1 here, not a real broadcast address */
            r->dwReasmSize = 65535;
            r->wType = 0x0001;           /* MIB_IPADDR_PRIMARY */
        }
    DWORD c = (DWORD)cnt;
    memcpy(table, &c, 4);
    if (order) qsort(rows, (size_t)cnt, sizeof *rows, ipaddr_cmp);
    free(ifs);
    return NO_ERROR;
}

typedef struct { DWORD dwIndex, dwPhysAddrLen; BYTE bPhysAddr[8]; DWORD dwAddr, dwType; } WS_MIB_IPNETROW;
_Static_assert(sizeof(WS_MIB_IPNETROW) == 24, "MIB_IPNETROW layout");

API DWORD GetIpNetTable(void *table, ULONG *size, BOOL order)
{
    (void)order;
    if (!size) return ERROR_INVALID_PARAMETER;
    WS_MIB_IPNETROW rows[256];
    int n = 0;
    FILE *fp = fopen("/proc/net/arp", "re");
    char line[256];
    if (fp && fgets(line, sizeof line, fp)) {
        while (n < 256 && fgets(line, sizeof line, fp)) {
            char ip[64], hw[64], dev[IF_NAMESIZE + 1]; unsigned type, flags;
            if (sscanf(line, "%63s 0x%x 0x%x %63s %*s %16s", ip, &type, &flags, hw, dev) != 5) continue;
            WS_MIB_IPNETROW *r = &rows[n];
            memset(r, 0, sizeof *r);
            r->dwIndex = if_nametoindex(dev);
            unsigned v[6];
            if (sscanf(hw, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                for (int i = 0; i < 6; i++) r->bPhysAddr[i] = (BYTE)v[i];
                r->dwPhysAddrLen = 6;
            }
            struct in_addr a;
            if (inet_pton(AF_INET, ip, &a) != 1) continue;
            r->dwAddr = a.s_addr;
            r->dwType = (flags & 4) ? 4 : (flags & 2) ? 3 : 2;    /* STATIC / DYNAMIC / INVALID */
            n++;
        }
    }
    if (fp) fclose(fp);
    ULONG need = (ULONG)(4 + (size_t)(n ? n : 1) * sizeof(WS_MIB_IPNETROW));
    if (!table || *size < need) { *size = need; return ERROR_INSUFFICIENT_BUFFER; }
    DWORD c = (DWORD)n;
    memcpy(table, &c, 4);
    memcpy((char *)table + 4, rows, (size_t)n * sizeof rows[0]);
    return n ? NO_ERROR : ERROR_NO_DATA;
}

/* IP_INTERFACE_INFO: physical adapters by index and \DEVICE\TCPIP_{GUID} name. */
typedef struct { ULONG Index; WCHAR Name[128]; } WS_IP_ADAPTER_INDEX_MAP;

API DWORD GetInterfaceInfo(void *buf, ULONG *size)
{
    if (!size) return ERROR_INVALID_PARAMETER;
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs), cnt = 0;
    for (int i = 0; i < n; i++) if (ifs[i].iftype != IF_TYPE_SOFTWARE_LOOPBACK) cnt++;
    ULONG need = (ULONG)(4 + (size_t)(cnt ? cnt : 1) * sizeof(WS_IP_ADAPTER_INDEX_MAP));
    if (!buf || *size < need) { *size = need; free(ifs); return ERROR_INSUFFICIENT_BUFFER; }
    LONG c = cnt;
    memcpy(buf, &c, 4);
    WS_IP_ADAPTER_INDEX_MAP *m = (WS_IP_ADAPTER_INDEX_MAP *)(void *)((char *)buf + 4);
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (ifs[i].iftype == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        char nm[80], gs[40];
        guid_str(&ifs[i].guid, gs, sizeof gs);
        snprintf(nm, sizeof nm, "\\DEVICE\\TCPIP_%s", gs);
        m[j].Index = ifs[i].index;
        memset(m[j].Name, 0, sizeof m[j].Name);
        lsiw_utf8_to_utf16(nm, -1, m[j].Name, 128);
        j++;
    }
    free(ifs);
    return cnt ? NO_ERROR : ERROR_NO_DATA;
}

API DWORD GetAdapterIndex(const WCHAR *name, ULONG *index)
{
    if (!name || !index) return ERROR_INVALID_PARAMETER;
    char *u = lsiw_w2u(name);
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs);
    DWORD r = ERROR_INVALID_PARAMETER;
    for (int i = 0; i < n; i++) {
        char gs[40];
        guid_str(&ifs[i].guid, gs, sizeof gs);
        const char *tail = strrchr(u, '{');
        if ((tail && !strcasecmp(tail, gs)) || !strcmp(u, ifs[i].name)) { *index = ifs[i].index; r = NO_ERROR; break; }
    }
    free(ifs);
    free(u);
    return r;
}

API DWORD GetFriendlyIfIndex(DWORD index) { return index & 0xFFFFFF; }

API DWORD GetNumberOfInterfaces(DWORD *n)
{
    if (!n) return ERROR_INVALID_PARAMETER;
    struct if_nameindex *ni = if_nameindex();
    DWORD c = 0;
    if (ni) { while (ni[c].if_index) c++; if_freenameindex(ni); }
    *n = c;
    return NO_ERROR;
}

/* ---- TCP / UDP tables from /proc/net ---- */

struct sockrow { BYTE la[16], ra[16]; uint32_t lscope, rscope; unsigned lport, rport, state; unsigned long inode; DWORD pid; };

static int read_proc_net(const char *file, int v6, struct sockrow **out)
{
    *out = NULL;
    FILE *fp = fopen(file, "re");
    if (!fp) return 0;
    char line[512];
    int n = 0, cap = 0;
    if (!fgets(line, sizeof line, fp)) { fclose(fp); return 0; }
    while (fgets(line, sizeof line, fp)) {
        char la[40], ra[40]; unsigned lp, rp, st; unsigned long inode;
        if (sscanf(line, "%*s %39[0-9A-Fa-f]:%x %39[0-9A-Fa-f]:%x %x %*s %*s %*s %*s %*s %lu", la, &lp, ra, &rp, &st, &inode) != 6) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; struct sockrow *nr = realloc(*out, (size_t)cap * sizeof **out); if (!nr) break; *out = nr; }
        struct sockrow *r = &(*out)[n++];
        memset(r, 0, sizeof *r);
        /* each 32-bit group is the address bytes as a host-order number */
        for (int g = 0; g < (v6 ? 4 : 1); g++) {
            char tmp[9];
            memcpy(tmp, la + 8 * g, 8); tmp[8] = 0; uint32_t w = (uint32_t)strtoul(tmp, NULL, 16); memcpy(r->la + 4 * g, &w, 4);
            memcpy(tmp, ra + 8 * g, 8); tmp[8] = 0; w = (uint32_t)strtoul(tmp, NULL, 16); memcpy(r->ra + 4 * g, &w, 4);
        }
        r->lport = lp; r->rport = rp; r->state = st; r->inode = inode;
    }
    fclose(fp);
    return n;
}

/* Owning processes: match socket inodes against /proc/<pid>/fd links. */
static void fill_pids(struct sockrow *rows, int n)
{
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    char path[300], link[64];
    while ((e = readdir(d))) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        snprintf(path, sizeof path, "/proc/%ld/fd", pid);
        DIR *fd = opendir(path);
        if (!fd) continue;
        struct dirent *f;
        while ((f = readdir(fd))) {
            snprintf(path, sizeof path, "/proc/%ld/fd/%s", pid, f->d_name);
            ssize_t l = readlink(path, link, sizeof link - 1);
            if (l <= 0) continue;
            link[l] = 0;
            unsigned long ino;
            if (sscanf(link, "socket:[%lu]", &ino) != 1) continue;
            for (int i = 0; i < n; i++) if (rows[i].inode == ino && !rows[i].pid) rows[i].pid = (DWORD)pid;
        }
        closedir(fd);
    }
    closedir(d);
}

static DWORD tcp_state(unsigned s)
{
    static const DWORD map[] = { 0, 5, 3, 4, 6, 7, 11, 1, 8, 10, 2, 9 };   /* Linux TCP_* -> MIB_TCP_STATE_* */
    return s < sizeof map / sizeof map[0] ? map[s] : 1;
}

#define TCP_TABLE_BASIC_LISTENER        0
#define TCP_TABLE_BASIC_CONNECTIONS     1
#define TCP_TABLE_BASIC_ALL             2
#define TCP_TABLE_OWNER_PID_LISTENER    3
#define TCP_TABLE_OWNER_PID_CONNECTIONS 4
#define TCP_TABLE_OWNER_PID_ALL         5

static int sockrow_cmp(const void *a, const void *b)
{
    const struct sockrow *x = a, *y = b;
    int c = memcmp(x->la, y->la, 16);
    if (c) return c;
    if (x->lport != y->lport) return x->lport < y->lport ? -1 : 1;
    c = memcmp(x->ra, y->ra, 16);
    if (c) return c;
    return x->rport < y->rport ? -1 : x->rport > y->rport;
}

/* Rows are written as DWORD sequences in the Windows field order. */
static DWORD ip_table(void *table, DWORD *size, BOOL order, ULONG af, int cls, int tcp)
{
    if (!size) return ERROR_INVALID_PARAMETER;
    int v6 = af == WS_AF_INET6;
    if (af != WS_AF_INET && !v6) return ERROR_INVALID_PARAMETER;
    int owner = tcp ? (cls >= 3 && cls <= 5) : cls == 1;
    if ((tcp && cls > 5) || (!tcp && cls > 1) || (v6 && !owner)) return ERROR_NOT_SUPPORTED;
    struct sockrow *rows;
    int n = read_proc_net(tcp ? (v6 ? "/proc/net/tcp6" : "/proc/net/tcp") : (v6 ? "/proc/net/udp6" : "/proc/net/udp"), v6, &rows);
    int want_listen = !tcp || cls == 0 || cls == 3 || cls == 2 || cls == 5;
    int want_conn = !tcp || cls == 1 || cls == 4 || cls == 2 || cls == 5;
    int m = 0;
    for (int i = 0; i < n; i++) {
        int listening = rows[i].state == 10;
        if (tcp && ((listening && !want_listen) || (!listening && !want_conn))) continue;
        rows[m++] = rows[i];
    }
    if (owner) fill_pids(rows, m);
    if (order && m) qsort(rows, (size_t)m, sizeof *rows, sockrow_cmp);
    size_t rsz = tcp ? (v6 ? 56 : owner ? 24 : 20) : (v6 ? 28 : owner ? 12 : 8);
    ULONG need = (ULONG)(4 + (size_t)(m ? m : 1) * rsz);
    if (!table || *size < need) { *size = need; free(rows); return ERROR_INSUFFICIENT_BUFFER; }
    DWORD cnt = (DWORD)m;
    memcpy(table, &cnt, 4);
    char *p = (char *)table + 4;
    for (int i = 0; i < m; i++, p += rsz) {
        struct sockrow *r = &rows[i];
        DWORD lp = htons((uint16_t)r->lport), rp = htons((uint16_t)r->rport);
        DWORD st = tcp_state(r->state);
        if (!v6) {
            DWORD la, ra; memcpy(&la, r->la, 4); memcpy(&ra, r->ra, 4);
            DWORD v[6];
            int k = 0;
            if (tcp) { v[k++] = st; v[k++] = la; v[k++] = lp; v[k++] = ra; v[k++] = rp; }
            else { v[k++] = la; v[k++] = lp; }
            if (owner) v[k++] = r->pid;
            memcpy(p, v, (size_t)k * 4);
        } else if (tcp) {           /* MIB_TCP6ROW_OWNER_PID */
            memcpy(p, r->la, 16); memset(p + 16, 0, 4); memcpy(p + 20, &lp, 4);
            memcpy(p + 24, r->ra, 16); memset(p + 40, 0, 4); memcpy(p + 44, &rp, 4);
            memcpy(p + 48, &st, 4); memcpy(p + 52, &r->pid, 4);
        } else {                    /* MIB_UDP6ROW_OWNER_PID */
            memcpy(p, r->la, 16); memset(p + 16, 0, 4); memcpy(p + 20, &lp, 4); memcpy(p + 24, &r->pid, 4);
        }
    }
    free(rows);
    return NO_ERROR;
}

API DWORD GetTcpTable(void *t, DWORD *size, BOOL order) { return ip_table(t, size, order, WS_AF_INET, TCP_TABLE_BASIC_ALL, 1); }
API DWORD GetUdpTable(void *t, DWORD *size, BOOL order) { return ip_table(t, size, order, WS_AF_INET, 0, 0); }
API DWORD GetExtendedTcpTable(void *t, DWORD *size, BOOL order, ULONG af, INT cls, ULONG reserved)
{
    (void)reserved;
    return ip_table(t, size, order, af, cls, 1);
}
API DWORD GetExtendedUdpTable(void *t, DWORD *size, BOOL order, ULONG af, INT cls, ULONG reserved)
{
    (void)reserved;
    return ip_table(t, size, order, af, cls, 0);
}

/* ---- names, indexes, LUIDs ---- */

API ULONG ws_if_nametoindex(const char *name) { return name ? if_nametoindex(name) : 0; }
API char *ws_if_indextoname(ULONG index, char *name)
{
    char b[IF_NAMESIZE];
    if (!name || !if_indextoname(index, b)) return NULL;
    strcpy(name, b);
    return name;
}

static int if_by_index(unsigned idx, struct ifinfo *out)
{
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs);
    struct ifinfo *f = n > 0 ? find_if(ifs, n, idx) : NULL;
    if (f) *out = *f;
    free(ifs);
    return f != NULL;
}

API DWORD ConvertInterfaceIndexToLuid(DWORD index, ULONG64 *luid)
{
    if (!luid) return ERROR_INVALID_PARAMETER;
    struct ifinfo f;
    *luid = 0;
    if (!if_by_index(index, &f)) return ERROR_FILE_NOT_FOUND;
    *luid = luid_of(&f);
    return NO_ERROR;
}

API DWORD ConvertInterfaceLuidToIndex(const ULONG64 *luid, DWORD *index)
{
    if (!luid || !index) return ERROR_INVALID_PARAMETER;
    char b[IF_NAMESIZE];
    unsigned idx = luid_index(*luid);
    if (!idx || !if_indextoname(idx, b)) { *index = 0; return ERROR_FILE_NOT_FOUND; }
    *index = idx;
    return NO_ERROR;
}

API DWORD ConvertInterfaceLuidToNameA(const ULONG64 *luid, char *name, SIZE_T len)
{
    if (!luid || !name) return ERROR_INVALID_PARAMETER;
    char b[IF_NAMESIZE];
    if (!if_indextoname(luid_index(*luid), b)) return ERROR_FILE_NOT_FOUND;
    if (strlen(b) + 1 > len) return ERROR_NOT_ENOUGH_MEMORY;
    strcpy(name, b);
    return NO_ERROR;
}

API DWORD ConvertInterfaceLuidToNameW(const ULONG64 *luid, WCHAR *name, SIZE_T len)
{
    char b[IF_NAMESIZE];
    DWORD r = ConvertInterfaceLuidToNameA(luid, b, sizeof b);
    if (r) return r;
    if (!name) return ERROR_INVALID_PARAMETER;
    if (lsiw_u2w_buf(b, name, (int)(len > INT_MAX ? INT_MAX : len)) > (int)len) return ERROR_NOT_ENOUGH_MEMORY;
    return NO_ERROR;
}

API DWORD ConvertInterfaceNameToLuidA(const char *name, ULONG64 *luid)
{
    if (!name || !luid) return ERROR_INVALID_PARAMETER;
    unsigned idx = if_nametoindex(name);
    if (!idx) return ERROR_INVALID_NAME;
    return ConvertInterfaceIndexToLuid(idx, luid);
}

API DWORD ConvertInterfaceNameToLuidW(const WCHAR *name, ULONG64 *luid)
{
    if (!name) return ERROR_INVALID_PARAMETER;
    char *u = lsiw_w2u(name);
    DWORD r = ConvertInterfaceNameToLuidA(u, luid);
    free(u);
    return r;
}

/* The alias is the friendly name, which LSIW makes the Linux name. */
API DWORD ConvertInterfaceLuidToAlias(const ULONG64 *luid, WCHAR *alias, SIZE_T len) { return ConvertInterfaceLuidToNameW(luid, alias, len); }
API DWORD ConvertInterfaceAliasToLuid(const WCHAR *alias, ULONG64 *luid) { return ConvertInterfaceNameToLuidW(alias, luid); }

API DWORD ConvertInterfaceLuidToGuid(const ULONG64 *luid, GUID *guid)
{
    if (!luid || !guid) return ERROR_INVALID_PARAMETER;
    char b[IF_NAMESIZE];
    if (!if_indextoname(luid_index(*luid), b)) return ERROR_FILE_NOT_FOUND;
    name_guid(b, guid);
    return NO_ERROR;
}

API DWORD ConvertInterfaceGuidToLuid(const GUID *guid, ULONG64 *luid)
{
    if (!guid || !luid) return ERROR_INVALID_PARAMETER;
    struct ifinfo *ifs;
    int n = collect_ifaces(&ifs);
    DWORD r = ERROR_FILE_NOT_FOUND;
    for (int i = 0; i < n; i++)
        if (!memcmp(&ifs[i].guid, guid, sizeof *guid)) { *luid = luid_of(&ifs[i]); r = NO_ERROR; break; }
    free(ifs);
    return r;
}

API DWORD ConvertLengthToIpv4Mask(ULONG len, ULONG *mask)
{
    if (!mask) return ERROR_INVALID_PARAMETER;
    if (len > 32) { *mask = 0xFFFFFFFFu; return ERROR_INVALID_PARAMETER; }
    BYTE m[4]; mask4((int)len, m); memcpy(mask, m, 4);
    return NO_ERROR;
}

API DWORD ConvertIpv4MaskToLength(ULONG mask, UCHAR *len)
{
    if (!len) return ERROR_INVALID_PARAMETER;
    uint32_t h = ntohl(mask);
    int n = __builtin_popcount(h);
    uint32_t canon = n ? ~0u << (32 - n) : 0;      /* contiguous leading ones only */
    if (h != canon) return ERROR_INVALID_PARAMETER;
    *len = (UCHAR)n;
    return NO_ERROR;
}

/* ---- change notifications ---- */

/* The handle given out for overlapped notifications: Windows says not to
 * close it, and GetOverlappedResult only reads the OVERLAPPED. */
static HANDLE notify_handle(void)
{
    static HANDLE h;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lk);
    if (!h) { int fd = nl_open(0); if (fd >= 0) h = lsiw_handle_from_fd(fd, 0); }
    pthread_mutex_unlock(&lk);
    return h;
}

static DWORD notify_change(HANDLE *handle, WSAOVERLAPPED *ov, unsigned groups)
{
    if (!handle && !ov) {                      /* synchronous: block until a change */
        int e = nl_wait_sync(groups);
        return e ? ERROR_NOT_SUPPORTED : NO_ERROR;
    }
    if (!handle || !ov) return ERROR_INVALID_PARAMETER;
    *handle = notify_handle();
    if (nl_wait_async(groups, ov)) return ERROR_NOT_SUPPORTED;
    return ERROR_IO_PENDING_;
}

API DWORD NotifyAddrChange(HANDLE *handle, WSAOVERLAPPED *ov) { return notify_change(handle, ov, NL_ADDR_GROUPS); }
API DWORD NotifyRouteChange(HANDLE *handle, WSAOVERLAPPED *ov) { return notify_change(handle, ov, NL_ROUTE_GROUPS); }
API BOOL CancelIPChangeNotify(WSAOVERLAPPED *ov) { return ov && nl_cancel(ov); }

/* Callback registrations (NotifyIpInterfaceChange and friends) are accepted
 * but later changes are not reported; the optional initial notification
 * (MibInitialNotification, no row) is delivered from a Windows thread. */
typedef void (WINAPI *pf_mib_cb)(void *ctx, void *row, INT type);
struct mib_reg { pf_mib_cb cb; void *ctx; };

static DWORD WINAPI mib_initial_thread(void *arg)
{
    struct mib_reg r = *(struct mib_reg *)arg;
    free(arg);
    r.cb(r.ctx, NULL, 3);                      /* MibInitialNotification */
    return 0;
}

static DWORD mib_register(USHORT family, pf_mib_cb cb, void *ctx, BOOL initial, HANDLE *handle)
{
    if (!cb || !handle || (family != WS_AF_UNSPEC && family != WS_AF_INET && family != WS_AF_INET6)) return ERROR_INVALID_PARAMETER;
    struct mib_reg *reg = malloc(sizeof *reg);
    if (!reg) return ERROR_NOT_ENOUGH_MEMORY;
    reg->cb = cb; reg->ctx = ctx;
    *handle = reg;
    if (initial) {
        struct mib_reg *copy = malloc(sizeof *copy);
        if (copy) { *copy = *reg; if (!win_thread(mib_initial_thread, copy)) free(copy); }
    }
    return NO_ERROR;
}

API DWORD NotifyIpInterfaceChange(USHORT family, pf_mib_cb cb, void *ctx, BOOL initial, HANDLE *handle) { return mib_register(family, cb, ctx, initial, handle); }
API DWORD NotifyUnicastIpAddressChange(USHORT family, pf_mib_cb cb, void *ctx, BOOL initial, HANDLE *handle) { return mib_register(family, cb, ctx, initial, handle); }
API DWORD NotifyRouteChange2(USHORT family, pf_mib_cb cb, void *ctx, BOOL initial, HANDLE *handle) { return mib_register(family, cb, ctx, initial, handle); }
API DWORD CancelMibChangeNotify2(HANDLE handle)
{
    if (!handle) return ERROR_INVALID_HANDLE;
    free(handle);
    return NO_ERROR;
}

/* ---- ICMP echo (ping) ---- */

/* Unprivileged ICMP datagram sockets are allowed by net.ipv4.ping_group_range
 * (open to all users on Fedora). Where they are not, IcmpCreateFile fails
 * with ERROR_ACCESS_DENIED. */
static HANDLE icmp_create(int af)
{
    int fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC, af == AF_INET6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP);
    if (fd < 0) { lsiw_set_last_error(errno == EACCES || errno == EPERM ? ERROR_ACCESS_DENIED : lsiw_errno_to_win(errno)); return INVALID_HANDLE_VALUE; }
    int one = 1;
    if (af == AF_INET) { setsockopt(fd, IPPROTO_IP, IP_RECVTTL, &one, sizeof one); setsockopt(fd, IPPROTO_IP, IP_RECVERR, &one, sizeof one); }
    SOCKET s = sock_new(fd, af, SOCK_DGRAM, af == AF_INET6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP, NULL);
    if (s == INVALID_SOCKET) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    return (HANDLE)(uintptr_t)s;
}

API HANDLE IcmpCreateFile(void)  { return icmp_create(AF_INET); }
API HANDLE Icmp6CreateFile(void) { return icmp_create(AF_INET6); }

API BOOL IcmpCloseHandle(HANDLE h)
{
    if (close_socket((SOCKET)(uintptr_t)h) != 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    return TRUE;
}

typedef struct { UCHAR Ttl, Tos, Flags, OptionsSize; UCHAR *OptionsData; } WS_IP_OPTION_INFORMATION;
typedef struct {
    DWORD Address; ULONG Status; ULONG RoundTripTime; USHORT DataSize; USHORT Reserved;
    void *Data; WS_IP_OPTION_INFORMATION Options;
} WS_ICMP_ECHO_REPLY;
_Static_assert(sizeof(WS_ICMP_ECHO_REPLY) == 40, "ICMP_ECHO_REPLY layout");

#define IP_SUCCESS               0
#define IP_BUF_TOO_SMALL         11001
#define IP_DEST_NET_UNREACHABLE  11002
#define IP_DEST_HOST_UNREACHABLE 11003
#define IP_DEST_PROT_UNREACHABLE 11004
#define IP_DEST_PORT_UNREACHABLE 11005
#define IP_REQ_TIMED_OUT         11010
#define IP_TTL_EXPIRED_TRANSIT   11013
#define IP_GENERAL_FAILURE       11050

struct icmp_hdr { BYTE type, code; uint16_t cksum, id, seq; };



static DWORD icmp_echo(HANDLE h, DWORD dst, const void *data, WORD size, const WS_IP_OPTION_INFORMATION *opt,
                       void *reply, DWORD replysize, DWORD timeout)
{
    struct ws_sock *st = sock_get((SOCKET)(uintptr_t)h);
    if (!st || st->protocol != IPPROTO_ICMP) { sock_put(st); lsiw_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    DWORD ret = 0;
    if (!reply || replysize < sizeof(WS_ICMP_ECHO_REPLY) + size + 8) { lsiw_set_last_error(IP_BUF_TOO_SMALL); goto out; }
    WS_ICMP_ECHO_REPLY *rp = reply;
    memset(rp, 0, sizeof *rp);
    static uint16_t seq_counter;
    uint16_t seq = __atomic_add_fetch(&seq_counter, 1, __ATOMIC_RELAXED);
    size_t plen = sizeof(struct icmp_hdr) + size;
    BYTE *pkt = calloc(1, plen);
    if (!pkt) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); goto out; }
    struct icmp_hdr ih = { 8, 0, 0, 0, htons(seq) };           /* echo request; the kernel sets id and checksum */
    memcpy(pkt, &ih, sizeof ih);
    if (size && data) memcpy(pkt + sizeof ih, data, size);
    if (opt && opt->Ttl) { int t = opt->Ttl; setsockopt(st->fd, IPPROTO_IP, IP_TTL, &t, sizeof t); }
    if (opt) { int tos = opt->Tos; setsockopt(st->fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos); }
    struct sockaddr_in to = { 0 };
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = dst;
    struct timespec t0, t1;
    now_mono(&t0);
    if (sendto(st->fd, pkt, plen, 0, (struct sockaddr *)&to, sizeof to) < 0) {
        free(pkt);
        rp->Status = errno == ENETUNREACH ? IP_DEST_NET_UNREACHABLE : errno == EHOSTUNREACH ? IP_DEST_HOST_UNREACHABLE : IP_GENERAL_FAILURE;
        lsiw_set_last_error(rp->Status);
        goto out;
    }
    free(pkt);
    BYTE buf[65536];
    struct timespec dl = t0;
    dl.tv_sec += timeout / 1000;
    dl.tv_nsec += (long)(timeout % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    for (;;) {
        int left = ms_until(&dl);
        struct pollfd p = { st->fd, POLLIN, 0 };
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { rp->Status = IP_REQ_TIMED_OUT; rp->Address = dst; lsiw_set_last_error(IP_REQ_TIMED_OUT); goto out; }
        char ctl[512];
        struct sockaddr_in from;
        struct iovec v = { buf, sizeof buf };
        struct msghdr mh = { 0 };
        mh.msg_name = &from; mh.msg_namelen = sizeof from;
        mh.msg_iov = &v; mh.msg_iovlen = 1;
        mh.msg_control = ctl; mh.msg_controllen = sizeof ctl;
        if (p.revents & POLLERR) {
            /* an ICMP error about our request (unreachable, TTL exceeded) */
            ssize_t n = recvmsg(st->fd, &mh, MSG_ERRQUEUE);
            if (n < 0) continue;
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
                if (c->cmsg_level != IPPROTO_IP || c->cmsg_type != IP_RECVERR) continue;
                struct sock_extended_err *ee = (struct sock_extended_err *)(void *)CMSG_DATA(c);
                struct sockaddr_in *off = (struct sockaddr_in *)(void *)SO_EE_OFFENDER(ee);
                rp->Address = off->sin_family == AF_INET ? off->sin_addr.s_addr : dst;
                if (ee->ee_type == 11) rp->Status = IP_TTL_EXPIRED_TRANSIT;
                else if (ee->ee_type == 3)
                    rp->Status = ee->ee_code == 0 ? IP_DEST_NET_UNREACHABLE : ee->ee_code == 2 ? IP_DEST_PROT_UNREACHABLE :
                                 ee->ee_code == 3 ? IP_DEST_PORT_UNREACHABLE : IP_DEST_HOST_UNREACHABLE;
                else rp->Status = IP_GENERAL_FAILURE;
                now_mono(&t1);
                rp->RoundTripTime = (ULONG)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
                rp->Reserved = 1;
                ret = 1;     /* Windows counts an ICMP error as a reply with a non-success status */
                goto out;
            }
            continue;
        }
        ssize_t n = recvmsg(st->fd, &mh, 0);
        if (n < (ssize_t)sizeof(struct icmp_hdr)) continue;
        struct icmp_hdr rh; memcpy(&rh, buf, sizeof rh);
        if (rh.type != 0 || ntohs(rh.seq) != seq) continue;    /* not our echo reply */
        now_mono(&t1);
        rp->Address = from.sin_addr.s_addr;
        rp->Status = IP_SUCCESS;
        rp->RoundTripTime = (ULONG)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        size_t dlen = (size_t)n - sizeof rh;
        if (dlen > size) dlen = size;
        rp->DataSize = (USHORT)dlen;
        rp->Data = (char *)reply + sizeof *rp;
        memcpy(rp->Data, buf + sizeof rh, dlen);
        rp->Reserved = 1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL) { int t; memcpy(&t, CMSG_DATA(c), sizeof t); rp->Options.Ttl = (UCHAR)t; }
        ret = 1;
        goto out;
    }
out:
    sock_put(st);
    return ret;
}

API DWORD IcmpSendEcho(HANDLE h, DWORD dst, void *data, WORD size, WS_IP_OPTION_INFORMATION *opt, void *reply, DWORD replysize, DWORD timeout)
{
    return icmp_echo(h, dst, data, size, opt, reply, replysize, timeout);
}

/* With an event the request is still answered synchronously; the event is
 * then set and the call returns 0 / ERROR_IO_PENDING as Windows does for
 * asynchronous use, so the caller reads the reply with IcmpParseReplies.
 * APC routines cannot be delivered and are refused. */
API DWORD IcmpSendEcho2(HANDLE h, HANDLE event, void *apc, void *apcctx, DWORD dst, void *data, WORD size,
                        WS_IP_OPTION_INFORMATION *opt, void *reply, DWORD replysize, DWORD timeout)
{
    (void)apcctx;
    if (apc) { lsiw_set_last_error(ERROR_NOT_SUPPORTED); return 0; }
    DWORD r = icmp_echo(h, dst, data, size, opt, reply, replysize, timeout);
    if (event) {
        ev_set(event);
        lsiw_set_last_error(ERROR_IO_PENDING_);
        return 0;
    }
    return r;
}

API DWORD IcmpParseReplies(void *reply, DWORD size)
{
    if (!reply || size < sizeof(WS_ICMP_ECHO_REPLY)) return 0;
    WS_ICMP_ECHO_REPLY *rp = reply;
    if (!rp->Reserved) lsiw_set_last_error(rp->Status ? rp->Status : IP_REQ_TIMED_OUT);
    return rp->Reserved;
}

/* ------------------------------------------------------------------ */
/* Export tables                                                        */
/* ------------------------------------------------------------------ */

/* ws2_32 (also wsock32, mswsock and iphlpapi). Old programs import the
 * classic functions by ordinal; those numbers are listed as "#n". */
const struct lsiw_export lsiw_ws2_exports[] = {
    E(WSAStartup), E(WSACleanup), E(WSAGetLastError), E(WSASetLastError),
    EX("socket", ws_socket), E(WSASocketA), E(WSASocketW), EX("closesocket", ws_closesocket),
    EX("bind", ws_bind), EX("listen", ws_listen), EX("accept", ws_accept), E(WSAAccept),
    EX("connect", ws_connect), E(WSAConnect), E(WSAConnectByNameA), E(WSAConnectByNameW),
    EX("shutdown", ws_shutdown), E(WSASendDisconnect), E(WSARecvDisconnect),
    EX("getsockname", ws_getsockname), EX("getpeername", ws_getpeername),
    EX("recv", ws_recv), EX("recvfrom", ws_recvfrom), E(WSARecvEx), EX("send", ws_send), EX("sendto", ws_sendto),
    E(WSARecv), E(WSARecvFrom), E(WSASend), E(WSASendTo), E(WSASendMsg), E(WSAGetOverlappedResult),
    EX("__WSAFDIsSet", ws___WSAFDIsSet), EX("select", ws_select), E(WSAPoll),
    E(ioctlsocket), E(WSAIoctl), EX("getsockopt", ws_getsockopt), EX("setsockopt", ws_setsockopt),
    E(WSAEnumProtocolsA), E(WSAEnumProtocolsW), E(WSADuplicateSocketA), E(WSADuplicateSocketW),
    E(AcceptEx), E(GetAcceptExSockaddrs), E(TransmitFile),
    E(WSAEventSelect), E(WSAAsyncSelect), E(WSAEnumNetworkEvents), E(WSACreateEvent), E(WSACloseEvent),
    E(WSASetEvent), E(WSAResetEvent), E(WSAWaitForMultipleEvents),
    EX("htons", ws_htons), EX("ntohs", ws_ntohs), EX("htonl", ws_htonl), EX("ntohl", ws_ntohl),
    E(WSAHtons), E(WSANtohs), E(WSAHtonl), E(WSANtohl),
    EX("inet_addr", ws_inet_addr), EX("inet_ntoa", ws_inet_ntoa),
    EX("inet_pton", ws_inet_pton), EX("InetPtonA", ws_inet_pton), E(InetPtonW),
    EX("inet_ntop", ws_inet_ntop), EX("InetNtopA", ws_inet_ntop), E(InetNtopW),
    E(WSAStringToAddressA), E(WSAStringToAddressW), E(WSAAddressToStringA), E(WSAAddressToStringW),
    EX("gethostname", ws_gethostname), E(GetHostNameW), EX("gethostbyname", ws_gethostbyname),
    EX("gethostbyaddr", ws_gethostbyaddr), EX("getservbyname", ws_getservbyname),
    EX("getservbyport", ws_getservbyport), EX("getprotobyname", ws_getprotobyname),
    EX("getprotobynumber", ws_getprotobynumber),
    EX("getaddrinfo", ws_getaddrinfo), E(GetAddrInfoW), E(GetAddrInfoExA), E(GetAddrInfoExW),
    E(GetAddrInfoExCancel), E(GetAddrInfoExOverlappedResult),
    EX("freeaddrinfo", ws_freeaddrinfo), E(FreeAddrInfoW), E(FreeAddrInfoEx), E(FreeAddrInfoExW),
    EX("getnameinfo", ws_getnameinfo), E(GetNameInfoW),
    E(WSALookupServiceBeginA), E(WSALookupServiceBeginW), E(WSALookupServiceNextA), E(WSALookupServiceNextW),
    E(WSALookupServiceEnd), E(WSAProviderConfigChange), E(WSANSPIoctl),
    E(WSAIsBlocking), E(WSACancelBlockingCall), E(WSASetBlockingHook), E(WSAUnhookBlockingHook),
    /* ws2_32 ordinals */
    EX("#1", ws_accept), EX("#2", ws_bind), EX("#3", ws_closesocket), EX("#4", ws_connect),
    EX("#5", ws_getpeername), EX("#6", ws_getsockname), EX("#7", ws_getsockopt), EX("#8", ws_htonl),
    EX("#9", ws_htons), EX("#10", ioctlsocket), EX("#11", ws_inet_addr), EX("#12", ws_inet_ntoa),
    EX("#13", ws_listen), EX("#14", ws_ntohl), EX("#15", ws_ntohs), EX("#16", ws_recv),
    EX("#17", ws_recvfrom), EX("#18", ws_select), EX("#19", ws_send), EX("#20", ws_sendto),
    EX("#21", ws_setsockopt), EX("#22", ws_shutdown), EX("#23", ws_socket),
    EX("#51", ws_gethostbyaddr), EX("#52", ws_gethostbyname), EX("#53", ws_getprotobyname),
    EX("#54", ws_getprotobynumber), EX("#55", ws_getservbyname), EX("#56", ws_getservbyport),
    EX("#57", ws_gethostname), EX("#101", WSAAsyncSelect), EX("#109", WSASetBlockingHook),
    EX("#110", WSAUnhookBlockingHook), EX("#111", WSAGetLastError), EX("#112", WSASetLastError),
    EX("#113", WSACancelBlockingCall), EX("#114", WSAIsBlocking), EX("#115", WSAStartup),
    EX("#116", WSACleanup), EX("#151", ws___WSAFDIsSet),
    /* iphlpapi */
    E(GetAdaptersAddresses), E(GetAdaptersInfo), E(GetNetworkParams), E(GetBestInterface), E(GetBestInterfaceEx),
    E(GetBestRoute), E(GetIfEntry), E(GetIfEntry2), E(GetIfEntry2Ex), E(GetIfTable), E(GetIfTable2), E(GetIfTable2Ex),
    E(FreeMibTable), E(GetIpAddrTable), E(GetIpForwardTable), E(GetIpNetTable), E(GetInterfaceInfo),
    E(GetAdapterIndex), E(GetFriendlyIfIndex), E(GetNumberOfInterfaces),
    E(GetTcpTable), E(GetExtendedTcpTable), E(GetUdpTable), E(GetExtendedUdpTable),
    EX("if_nametoindex", ws_if_nametoindex), EX("if_indextoname", ws_if_indextoname),
    E(ConvertInterfaceIndexToLuid), E(ConvertInterfaceLuidToIndex), E(ConvertInterfaceLuidToNameA),
    E(ConvertInterfaceLuidToNameW), E(ConvertInterfaceNameToLuidA), E(ConvertInterfaceNameToLuidW),
    E(ConvertInterfaceLuidToAlias), E(ConvertInterfaceAliasToLuid), E(ConvertInterfaceLuidToGuid),
    E(ConvertInterfaceGuidToLuid), E(ConvertLengthToIpv4Mask), E(ConvertIpv4MaskToLength),
    E(NotifyAddrChange), E(NotifyRouteChange), E(CancelIPChangeNotify), E(NotifyIpInterfaceChange),
    E(NotifyUnicastIpAddressChange), E(NotifyRouteChange2), E(CancelMibChangeNotify2),
    E(IcmpCreateFile), E(Icmp6CreateFile), E(IcmpCloseHandle), E(IcmpSendEcho), E(IcmpSendEcho2), E(IcmpParseReplies),
    { NULL, NULL }
};

/* wsock32-only behaviour, searched before lsiw_ws2_exports for wsock32.dll:
 * Winsock 1.1 numbering of the IPPROTO_IP options, and the Microsoft
 * extension functions wsock32 exports by ordinal. */
const struct lsiw_export lsiw_wsock32_exports[] = {
    EX("getsockopt", wsock32_getsockopt), EX("setsockopt", wsock32_setsockopt),
    EX("#7", wsock32_getsockopt), EX("#21", wsock32_setsockopt),
    EX("#1107", WSARecvEx), EX("#1140", TransmitFile), EX("#1141", AcceptEx), EX("#1142", GetAcceptExSockaddrs),
    { NULL, NULL }
};
