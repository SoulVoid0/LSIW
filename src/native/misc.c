/* Small Windows DLLs implemented by the native engine:
 *   winmm     multimedia timers, PlaySound, empty device lists, MCI/joystick errors
 *   version   VS_VERSIONINFO reading straight from PE files (PE32 and PE32+)
 *   userenv   profile folders and environment blocks
 *   secur32   GetUserNameEx and an SSPI surface with no security packages
 *   psapi     process list, image/mapped file names, memory counters, working sets
 *   dbghelp   export-based symbols, AMD64 StackWalk64, image directory helpers
 *   powrprof  processor/battery information, the Balanced power scheme
 *   netapi32  workgroup machine with one local user
 *   mpr       no network drives
 *   wtsapi32  one active console session
 *   dwmapi, shcore   always-composited desktop at 96 DPI
 *   dnsapi    A/AAAA queries through the system resolver
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native.h"
#include "pe.h"
#include "seh.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

typedef int32_t  HRESULT;
typedef ULONG_PTR DWORD_PTR;
typedef uint8_t  BOOLEAN;
typedef uint16_t USHORT;
typedef ULONG_PTR UINT_PTR;
typedef uint64_t ULONG64;
typedef uint64_t DWORD64;

#define S_OK            ((HRESULT)0)
#define E_ACCESSDENIED  ((HRESULT)0x80070005)
#define E_HANDLE        ((HRESULT)0x80070006)
#define E_INVALIDARG    ((HRESULT)0x80070057)

/* Win32 error codes this file needs beyond native.h. */
#define ERROR_BAD_FORMAT                 11
#define ERROR_BAD_LENGTH                 24
#define ERROR_BAD_NETPATH                53
#define ERROR_INVALID_LEVEL              124
#define ERROR_INVALID_ADDRESS            487
#define ERROR_FILE_INVALID               1006
#define ERROR_INVALID_FLAGS              1004
#define ERROR_BAD_DEVICE                 1200
#define ERROR_NO_NETWORK                 1222
#define ERROR_PRIVILEGE_NOT_HELD         1314
#define ERROR_NONE_MAPPED                1332
#define ERROR_CANT_ACCESS_DOMAIN_INFO    1351
#define ERROR_NO_SUCH_DOMAIN             1355
#define ERROR_RESOURCE_TYPE_NOT_FOUND    1813
#define ERROR_NOT_CONNECTED              2250

#define HKEY_CURRENT_USER ((HANDLE)(uintptr_t)0x80000001u)

static int is_current_process(HANDLE h)
{
    return h == (HANDLE)(intptr_t)-1;
}

/* Another builtin API, looked up at run time (kernel32 is the usual home). */
static void *k32_api(const char *name)
{
    return lsiw_builtin_lookup(lsiw_builtin_find("kernel32.dll"), name);
}

/* COMPUTERNAME exactly as kernel32's GetComputerName reports it. */
static const char *computer_name(void)
{
    const char *c = lsiw_var_windows("COMPUTERNAME");
    return c && *c ? c : "LSIW";
}

static const char *user_name(void)
{
    const char *u = lsiw_user_name();
    return u && *u ? u : "user";
}

/* Copy UTF-8 into a heap block (LocalFree/NetApiBufferFree/WTSFreeMemory
 * release it with lsiw_heap_free). */
static WCHAR *heap_wstr(const char *s)
{
    int n = lsiw_utf8_to_utf16(s, -1, NULL, 0);
    WCHAR *w = lsiw_heap_alloc((size_t)n * sizeof(WCHAR), 0);
    if (w) lsiw_utf8_to_utf16(s, -1, w, n);
    return w;
}

static char *heap_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = lsiw_heap_alloc(n, 0);
    if (p) memcpy(p, s, n);
    return p;
}

static int read_text_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return (int)n;
}

static long long read_ll_file(const char *path, long long def)
{
    char b[64];
    if (read_text_file(path, b, sizeof b) <= 0) return def;
    return strtoll(b, NULL, 10);
}

/* Windows path -> NT device path the way psapi reports it: the single C:
 * volume is \Device\HarddiskVolume1. */
static char *device_path(const char *win)
{
    if (win && isalpha((unsigned char)win[0]) && win[1] == ':')
        return lsiw_strdup_printf("\\Device\\HarddiskVolume1%s", win + 2);
    return lsiw_strdup_printf("\\Device\\HarddiskVolume1\\%s", win ? win : "");
}

/* ================================================================== */
/* winmm                                                                */
/* ================================================================== */

#define MMSYSERR_NOERROR      0
#define MMSYSERR_BADDEVICEID  2
#define MMSYSERR_NODRIVER     6
#define MMSYSERR_INVALPARAM   11
#define TIMERR_NOERROR        0
#define TIMERR_NOCANDO        97
#define TIMERR_STRUCT         129
#define MIDIERR_NODEVICE      68
#define JOYERR_PARMS          165
#define JOYERR_UNPLUGGED      167
#define MCIERR_INVALID_DEVICE_ID   257
#define MCIERR_INVALID_DEVICE_NAME 263
#define WAVE_MAPPER           0xFFFFFFFFu
#define TIME_MS               1
#define TIME_PERIODIC         0x0001
#define TIME_CALLBACK_EVENT_SET   0x0010
#define TIME_CALLBACK_EVENT_PULSE 0x0020
#define TIME_KILL_SYNCHRONOUS 0x0100

typedef struct {
    UINT wType;
    union {
        DWORD ms, sample, cb, ticks;
        struct { BYTE hour, min, sec, frame, fps, dummy, pad[2]; } smpte;
        struct { DWORD songptrpos; } midi;
    } u;
} MMTIME;

typedef struct { UINT wPeriodMin, wPeriodMax; } TIMECAPS;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Milliseconds since the system started, wrapping every ~49.7 days. */
API DWORD timeGetTime(void) { return (DWORD)mono_ms(); }

API UINT timeGetSystemTime(MMTIME *t, UINT cb)
{
    if (!t || cb < sizeof *t) return TIMERR_STRUCT;
    t->wType = TIME_MS;
    t->u.ms = timeGetTime();
    return TIMERR_NOERROR;
}

/* Linux timers are already high resolution; only the argument is checked. */
API UINT timeBeginPeriod(UINT p) { return p ? TIMERR_NOERROR : TIMERR_NOCANDO; }
API UINT timeEndPeriod(UINT p)   { return p ? TIMERR_NOERROR : TIMERR_NOCANDO; }

API UINT timeGetDevCaps(TIMECAPS *tc, UINT cb)
{
    if (!tc || cb < sizeof *tc) return TIMERR_NOCANDO;
    tc->wPeriodMin = 1;
    tc->wPeriodMax = 1000000;
    return TIMERR_NOERROR;
}

/* timeSetEvent: each timer runs on its own thread created through
 * kernel32's CreateThread, so the callback runs on a real Windows thread
 * (TEB, TLS, SEH all set up). Kills wake the thread through a condition
 * variable on CLOCK_MONOTONIC. */
typedef void (WINAPI *LPTIMECALLBACK)(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR dw1, DWORD_PTR dw2);
typedef DWORD (WINAPI *win_thread_fn)(void *);
typedef HANDLE (WINAPI *create_thread_fn)(void *, SIZE_T, win_thread_fn, void *, DWORD, DWORD *);
typedef BOOL (WINAPI *handle_fn)(HANDLE);

struct mm_timer {
    UINT id, delay, flags;
    void *proc;
    DWORD_PTR user;
    int killed;
    uint32_t tid;
    struct mm_timer *next;
};

static pthread_mutex_t g_tm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tm_cond;
static pthread_once_t g_tm_once = PTHREAD_ONCE_INIT;
static struct mm_timer *g_timers;
static UINT g_timer_next_id = 1;

static void tm_init(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g_tm_cond, &a);
    pthread_condattr_destroy(&a);
}

static void ts_add_ms(struct timespec *ts, UINT ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static int ts_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static void tm_unlink(struct mm_timer *t)
{
    for (struct mm_timer **pp = &g_timers; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; return; }
}

static DWORD WINAPI timer_thread(void *arg)
{
    struct mm_timer *t = arg;
    struct timespec next, now;
    pthread_mutex_lock(&g_tm_lock);
    t->tid = lsiw_thread()->tid;
    clock_gettime(CLOCK_MONOTONIC, &next);
    ts_add_ms(&next, t->delay);
    while (!t->killed) {
        pthread_cond_timedwait(&g_tm_cond, &g_tm_lock, &next);
        if (t->killed) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_before(&now, &next)) continue;
        pthread_mutex_unlock(&g_tm_lock);
        if (t->flags & TIME_CALLBACK_EVENT_SET) {
            handle_fn f = (handle_fn)k32_api("SetEvent");
            if (f) f((HANDLE)t->proc);
        } else if (t->flags & TIME_CALLBACK_EVENT_PULSE) {
            handle_fn f = (handle_fn)k32_api("PulseEvent");
            if (f) f((HANDLE)t->proc);
        } else {
            ((LPTIMECALLBACK)t->proc)(t->id, 0, t->user, 0, 0);
        }
        pthread_mutex_lock(&g_tm_lock);
        if (!(t->flags & TIME_PERIODIC)) break;
        ts_add_ms(&next, t->delay);
        /* A callback that overran by more than a period: skip the missed
         * ticks instead of firing them back to back. */
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_before(&next, &now)) { next = now; ts_add_ms(&next, t->delay); }
    }
    tm_unlink(t);
    pthread_cond_broadcast(&g_tm_cond);
    pthread_mutex_unlock(&g_tm_lock);
    free(t);
    return 0;
}

API UINT timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK proc, DWORD_PTR user, UINT flags)
{
    (void)resolution;
    if (delay < 1 || delay > 1000000 || !proc) return 0;
    pthread_once(&g_tm_once, tm_init);
    create_thread_fn ct = (create_thread_fn)k32_api("CreateThread");
    if (!ct) return 0;
    struct mm_timer *t = calloc(1, sizeof *t);
    if (!t) return 0;
    t->delay = delay;
    t->flags = flags;
    t->proc = (void *)proc;
    t->user = user;
    pthread_mutex_lock(&g_tm_lock);
    t->id = g_timer_next_id++;
    if (!g_timer_next_id) g_timer_next_id = 1;
    t->next = g_timers;
    g_timers = t;
    UINT id = t->id;
    pthread_mutex_unlock(&g_tm_lock);
    HANDLE h = ct(NULL, 0, timer_thread, t, 0, NULL);
    if (!h) {
        pthread_mutex_lock(&g_tm_lock);
        tm_unlink(t);
        pthread_mutex_unlock(&g_tm_lock);
        free(t);
        return 0;
    }
    handle_fn close_h = (handle_fn)k32_api("CloseHandle");
    if (close_h) close_h(h);
    return id;
}

API UINT timeKillEvent(UINT id)
{
    pthread_once(&g_tm_once, tm_init);
    pthread_mutex_lock(&g_tm_lock);
    struct mm_timer *t = g_timers;
    while (t && t->id != id) t = t->next;
    if (!t) { pthread_mutex_unlock(&g_tm_lock); return MMSYSERR_INVALPARAM; }
    t->killed = 1;
    pthread_cond_broadcast(&g_tm_cond);
    /* TIME_KILL_SYNCHRONOUS: return only once no callback can run any more
     * (not when called from the timer's own callback, which would deadlock). */
    if ((t->flags & TIME_KILL_SYNCHRONOUS) && t->tid != lsiw_thread()->tid) {
        for (;;) {
            struct mm_timer *x = g_timers;
            while (x && x != t) x = x->next;
            if (!x) break;
            pthread_cond_wait(&g_tm_cond, &g_tm_lock);
        }
    }
    pthread_mutex_unlock(&g_tm_lock);
    return TIMERR_NOERROR;
}

/* ---- PlaySound ---------------------------------------------------- */

#define SND_ASYNC      0x00000001
#define SND_NODEFAULT  0x00000002
#define SND_MEMORY     0x00000004
#define SND_LOOP       0x00000008
#define SND_NOSTOP     0x00000010
#define SND_PURGE      0x00000040
#define SND_ALIAS      0x00010000
#define SND_FILENAME   0x00020000
#define SND_RESOURCE   0x00040004
#define SND_ALIAS_ID   0x00110000

static pthread_mutex_t g_snd_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_snd_pid;       /* runner of the current asynchronous sound */

/* Full path of a program found in $PATH, or NULL. */
static char *find_program(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        const char *e = strchr(path, ':');
        size_t l = e ? (size_t)(e - path) : strlen(path);
        if (l) {
            char *p = lsiw_strdup_printf("%.*s/%s", (int)l, path, name);
            if (!access(p, X_OK)) return p;
            free(p);
        }
        path += l + (e ? 1 : 0);
    }
    return NULL;
}

/* Stop the asynchronous sound still playing, if any. Call with g_snd_lock. */
static void snd_stop_locked(void)
{
    if (g_snd_pid > 0) {
        kill(-g_snd_pid, SIGTERM);
        waitpid(g_snd_pid, NULL, 0);
    }
    g_snd_pid = 0;
}

static int snd_playing_locked(void)
{
    if (g_snd_pid <= 0) return 0;
    if (waitpid(g_snd_pid, NULL, WNOHANG) == 0) return 1;
    g_snd_pid = 0;
    return 0;
}

/* Run argv (argv[0] is a full path) in a child process group so it can be
 * stopped as a whole. The runner repeats the player for SND_LOOP and
 * deletes `tmpfile` at the end. Everything the child calls after fork() is
 * async-signal-safe. Returns FALSE if the player could not run. */
static BOOL snd_spawn(char **argv, const char *tmpfile, int async, int loop)
{
    pthread_mutex_lock(&g_snd_lock);
    snd_stop_locked();
    pid_t pid = fork();
    if (pid < 0) { pthread_mutex_unlock(&g_snd_lock); return FALSE; }
    if (pid == 0) {
        setpgid(0, 0);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        int status = 127;
        do {
            pid_t c = fork();
            if (c == 0) { execv(argv[0], argv); _exit(127); }
            if (c < 0 || waitpid(c, &status, 0) < 0) break;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) break;
        } while (loop);
        if (tmpfile) unlink(tmpfile);
        _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }
    setpgid(pid, pid);
    BOOL ok = TRUE;
    if (async) {
        g_snd_pid = pid;
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    pthread_mutex_unlock(&g_snd_lock);
    return ok;
}

static BOOL play_wav_file(const char *linux_path, const char *tmpfile, int async, int loop)
{
    static const char *const players[][3] = {
        { "paplay", NULL, NULL }, { "pw-play", NULL, NULL }, { "aplay", "-q", NULL },
    };
    for (size_t i = 0; i < sizeof players / sizeof players[0]; i++) {
        char *exe = find_program(players[i][0]);
        if (!exe) continue;
        char *argv[4] = { exe, NULL, NULL, NULL };
        int n = 1;
        if (players[i][1]) argv[n++] = (char *)players[i][1];
        argv[n] = (char *)linux_path;
        BOOL ok = snd_spawn(argv, tmpfile, async, loop);
        free(exe);
        return ok;
    }
    if (tmpfile) unlink(tmpfile);
    return FALSE;
}

/* Windows system-event aliases -> freedesktop sound theme names. */
static const struct { const char *alias; const char *xdg; } g_sound_aliases[] = {
    { "SystemAsterisk", "dialog-information" }, { "SystemExclamation", "dialog-warning" },
    { "SystemHand", "dialog-error" },           { "SystemQuestion", "dialog-question" },
    { "SystemDefault", "bell" },                { ".Default", "bell" },
    { "SystemExit", "desktop-logout" },         { "SystemStart", "desktop-login" },
    { "SystemNotification", "message" },        { "MailBeep", "message-new-email" },
    { "Notification.Default", "message" },      { "DeviceConnect", "device-added" },
    { "DeviceDisconnect", "device-removed" },   { "EmptyRecycleBin", "trash-empty" },
};

static const char *alias_to_xdg(const char *alias)
{
    for (size_t i = 0; i < sizeof g_sound_aliases / sizeof g_sound_aliases[0]; i++)
        if (!strcasecmp(alias, g_sound_aliases[i].alias)) return g_sound_aliases[i].xdg;
    return NULL;
}

/* SND_ALIAS_ID values are two characters packed in a WORD: 'S' + code. */
static const char *alias_id_to_xdg(uintptr_t id)
{
    if ((id & 0xFF) != 'S') return NULL;
    switch ((id >> 8) & 0xFF) {
    case '*': return "dialog-information";
    case '?': return "dialog-question";
    case 'H': return "dialog-error";
    case '!': return "dialog-warning";
    case 'D': return "bell";
    case 'E': return "desktop-logout";
    case 'S': return "desktop-login";
    }
    return NULL;
}

static BOOL play_xdg_sound(const char *xdg, int async)
{
    char *exe = find_program("canberra-gtk-play");
    if (!exe) return FALSE;
    char *argv[] = { exe, "-i", (char *)xdg, NULL };
    BOOL ok = snd_spawn(argv, NULL, async, 0);
    free(exe);
    return ok;
}

/* Copy an in-memory RIFF/WAVE image to a temporary .wav file. */
static char *wav_to_tempfile(const void *data)
{
    const uint8_t *p = data;
    if (!p || memcmp(p, "RIFF", 4)) return NULL;
    uint32_t len;
    memcpy(&len, p + 4, 4);
    len += 8;
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || access(dir, W_OK)) dir = "/tmp";
    char *path = lsiw_strdup_printf("%s/lsiw-sound-XXXXXX.wav", dir);
    int fd = mkstemps(path, 4);
    if (fd < 0) { free(path); return NULL; }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w <= 0) { close(fd); unlink(path); free(path); return NULL; }
        off += (size_t)w;
    }
    close(fd);
    return path;
}

typedef void *(WINAPI *find_res_fn)(HMODULE, const void *, const void *);
typedef void *(WINAPI *load_res_fn)(HMODULE, void *);
typedef void *(WINAPI *lock_res_fn)(void *);

/* Common body of PlaySoundA/W and sndPlaySoundA/W. `name` is UTF-8 unless
 * the flags make it a pointer (SND_MEMORY) or an id (SND_ALIAS_ID);
 * `raw` is the caller's original argument. */
static BOOL play_sound(const char *name, const void *raw, HMODULE mod, DWORD flags, int wide)
{
    int async = (flags & SND_ASYNC) != 0;
    int loop = (flags & SND_LOOP) && async;      /* SND_LOOP requires SND_ASYNC */
    pthread_mutex_lock(&g_snd_lock);
    if ((flags & SND_NOSTOP) && snd_playing_locked()) { pthread_mutex_unlock(&g_snd_lock); return FALSE; }
    snd_stop_locked();
    pthread_mutex_unlock(&g_snd_lock);
    if (!raw || (flags & SND_PURGE)) return TRUE;   /* stop request */

    BOOL ok = FALSE;
    if ((flags & SND_RESOURCE) == SND_RESOURCE) {
        /* A WAVE resource of `mod`. */
        find_res_fn fr = (find_res_fn)k32_api(wide ? "FindResourceW" : "FindResourceA");
        load_res_fn lr = (load_res_fn)k32_api("LoadResource");
        lock_res_fn lk = (lock_res_fn)k32_api("LockResource");
        static const WCHAR wave_w[] = { 'W', 'A', 'V', 'E', 0 };
        void *res = fr ? fr(mod, raw, wide ? (const void *)wave_w : (const void *)"WAVE") : NULL;
        void *data = res && lr && lk ? lk(lr(mod, res)) : NULL;
        char *tmp = data ? wav_to_tempfile(data) : NULL;
        if (tmp) { ok = play_wav_file(tmp, tmp, async, loop); free(tmp); }
    } else if (flags & SND_MEMORY) {
        char *tmp = wav_to_tempfile(raw);
        if (tmp) { ok = play_wav_file(tmp, tmp, async, loop); free(tmp); }
    } else if ((flags & SND_ALIAS_ID) == SND_ALIAS_ID) {
        const char *xdg = alias_id_to_xdg((uintptr_t)raw);
        if (xdg) ok = play_xdg_sound(xdg, async);
    } else {
        const char *xdg = (flags & SND_FILENAME) ? NULL : alias_to_xdg(name);
        if (xdg) {
            ok = play_xdg_sound(xdg, async);
        } else if (!(flags & SND_ALIAS)) {
            /* Not a known alias: a file name (SND_FILENAME, or no type flag). */
            char *lp = lsiw_path_a(name);
            if (lp && !access(lp, R_OK)) ok = play_wav_file(lp, NULL, async, loop);
            free(lp);
        }
    }
    /* A sound that cannot be found falls back to the default system sound
     * unless SND_NODEFAULT; the call still reports failure. */
    if (!ok && !(flags & SND_NODEFAULT) && !(flags & SND_MEMORY))
        play_xdg_sound("bell", async);
    return ok;
}

static int snd_name_is_pointer(DWORD flags)
{
    return !((flags & SND_ALIAS_ID) == SND_ALIAS_ID) && !(flags & SND_MEMORY) &&
           (flags & SND_RESOURCE) != SND_RESOURCE;
}

API BOOL PlaySoundA(const char *sound, HMODULE mod, DWORD flags)
{
    return play_sound(sound && snd_name_is_pointer(flags) ? sound : "", sound, mod, flags, 0);
}

API BOOL PlaySoundW(const WCHAR *sound, HMODULE mod, DWORD flags)
{
    char *u = sound && snd_name_is_pointer(flags) ? lsiw_w2u(sound) : NULL;
    BOOL r = play_sound(u ? u : "", sound, mod, flags, 1);
    free(u);
    return r;
}

/* sndPlaySound: the name is an alias or a file name (tried in that order). */
API BOOL sndPlaySoundA(const char *sound, UINT flags)
{
    flags &= SND_ASYNC | SND_NODEFAULT | SND_MEMORY | SND_LOOP | SND_NOSTOP;
    return PlaySoundA(sound, NULL, flags);
}

API BOOL sndPlaySoundW(const WCHAR *sound, UINT flags)
{
    flags &= SND_ASYNC | SND_NODEFAULT | SND_MEMORY | SND_LOOP | SND_NOSTOP;
    return PlaySoundW(sound, NULL, flags);
}

/* ---- audio/MIDI/mixer/joystick devices: none are exposed natively ---- */

API UINT waveOutGetNumDevs(void) { return 0; }
API UINT waveInGetNumDevs(void)  { return 0; }
API UINT midiOutGetNumDevs(void) { return 0; }
API UINT midiInGetNumDevs(void)  { return 0; }
API UINT mixerGetNumDevs(void)   { return 0; }
API UINT auxGetNumDevs(void)     { return 0; }
API UINT joyGetNumDevs(void)     { return 0; }

/* With no devices, WAVE_MAPPER has no driver to map to and any other id is
 * out of range. */
API UINT waveOutOpen(HANDLE *h, UINT id, const void *fmt, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    (void)fmt; (void)cb; (void)inst; (void)flags;
    if (h) *h = NULL;
    return id == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
}

API UINT waveInOpen(HANDLE *h, UINT id, const void *fmt, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    (void)fmt; (void)cb; (void)inst; (void)flags;
    if (h) *h = NULL;
    return id == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
}

API UINT waveOutGetDevCapsA(UINT_PTR id, void *caps, UINT cb) { (void)id; (void)caps; (void)cb; return MMSYSERR_BADDEVICEID; }
API UINT waveOutGetDevCapsW(UINT_PTR id, void *caps, UINT cb) { (void)id; (void)caps; (void)cb; return MMSYSERR_BADDEVICEID; }
API UINT waveInGetDevCapsA(UINT_PTR id, void *caps, UINT cb)  { (void)id; (void)caps; (void)cb; return MMSYSERR_BADDEVICEID; }
API UINT waveInGetDevCapsW(UINT_PTR id, void *caps, UINT cb)  { (void)id; (void)caps; (void)cb; return MMSYSERR_BADDEVICEID; }

API UINT midiOutOpen(HANDLE *h, UINT id, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    (void)id; (void)cb; (void)inst; (void)flags;
    if (h) *h = NULL;
    return MIDIERR_NODEVICE;
}

API UINT mixerOpen(HANDLE *h, UINT id, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    (void)id; (void)cb; (void)inst; (void)flags;
    if (h) *h = NULL;
    return MMSYSERR_BADDEVICEID;
}

/* MCI: there are no MCI drivers, so every device name is unknown. */
API DWORD mciSendStringA(const char *cmd, char *ret, UINT retlen, HANDLE cb)
{
    (void)cmd; (void)cb;
    if (ret && retlen) ret[0] = 0;
    return MCIERR_INVALID_DEVICE_NAME;
}

API DWORD mciSendStringW(const WCHAR *cmd, WCHAR *ret, UINT retlen, HANDLE cb)
{
    (void)cmd; (void)cb;
    if (ret && retlen) ret[0] = 0;
    return MCIERR_INVALID_DEVICE_NAME;
}

#define MCI_OPEN 0x0803
API DWORD mciSendCommandA(UINT dev, UINT msg, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)dev; (void)p1; (void)p2;
    return msg == MCI_OPEN ? MCIERR_INVALID_DEVICE_NAME : MCIERR_INVALID_DEVICE_ID;
}

API DWORD mciSendCommandW(UINT dev, UINT msg, DWORD_PTR p1, DWORD_PTR p2)
{
    return mciSendCommandA(dev, msg, p1, p2);
}

static const char *mci_error_text(DWORD err)
{
    switch (err) {
    case 0:   return "The specified command was carried out.";
    case 257: return "Invalid MCI device ID. Use the ID returned when opening the MCI device.";
    case 259: return "The driver cannot recognize the specified command parameter.";
    case 261: return "The driver cannot recognize the specified command.";
    case 262: return "There is a problem with your media device. Make sure it is working correctly or contact the device manufacturer.";
    case 263: return "The specified device is not open or is not recognized by MCI.";
    case 264: return "Not enough memory is available for this task. Quit one or more applications to increase available memory, and then try again.";
    case 266: return "There is an undetectable problem in loading the specified device driver.";
    case 274: return "The MCI device you are using does not support the specified command.";
    case 275: return "Cannot find the specified file. Make sure the path and filename are correct.";
    case 306: return "The specified device is not installed on the system. Use the Drivers option in Control Panel to install the device.";
    }
    return NULL;
}

/* Returns FALSE for an unknown error code, after storing an empty string. */
API BOOL mciGetErrorStringA(DWORD err, char *buf, UINT len)
{
    const char *t = mci_error_text(err);
    if (!buf || !len) return FALSE;
    buf[0] = 0;
    if (!t) return FALSE;
    snprintf(buf, len, "%s", t);
    return TRUE;
}

API BOOL mciGetErrorStringW(DWORD err, WCHAR *buf, UINT len)
{
    const char *t = mci_error_text(err);
    if (!buf || !len) return FALSE;
    buf[0] = 0;
    if (!t) return FALSE;
    if (lsiw_u2w_buf(t, buf, (int)len) > (int)len) {
        WCHAR *w = lsiw_u2w(t);
        memcpy(buf, w, (len - 1) * sizeof(WCHAR));
        buf[len - 1] = 0;
        free(w);
    }
    return TRUE;
}

/* Joysticks: ids 0..15 are valid but nothing is plugged in. */
API UINT joyGetPosEx(UINT id, void *info) { (void)info; return id < 16 ? JOYERR_UNPLUGGED : JOYERR_PARMS; }
API UINT joyGetPos(UINT id, void *info)   { (void)info; return id < 16 ? JOYERR_UNPLUGGED : JOYERR_PARMS; }
/* No joystick has capabilities to report: every id is an invalid parameter. */
API UINT joyGetDevCapsA(UINT_PTR id, void *caps, UINT cb) { (void)id; (void)caps; (void)cb; return JOYERR_PARMS; }
API UINT joyGetDevCapsW(UINT_PTR id, void *caps, UINT cb) { (void)id; (void)caps; (void)cb; return JOYERR_PARMS; }

/* Read memory of this process without faulting: 1 on success. */
static int safe_read(void *dst, const void *src, size_t n)
{
    struct iovec l = { dst, n }, r = { (void *)src, n };
    ssize_t got = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (got == (ssize_t)n) return 1;
    if (got < 0 && (errno == ENOSYS || errno == EPERM)) {
        struct map_info mi;
        uintptr_t a = (uintptr_t)src;
        if (lsiw_query_maps(a, &mi) || !mi.mapped || !(mi.prot & PROT_READ) || a + n > mi.end) return 0;
        memcpy(dst, src, n);
        return 1;
    }
    return 0;
}

/* ================================================================== */
/* version                                                              */
/* ================================================================== */

#define RT_VERSION                       16
#define VS_FFI_SIGNATURE                 0xFEEF04BDu
#define FILE_VER_GET_LOCALISED           0x01
#define FILE_VER_GET_NEUTRAL             0x02
#define FILE_VER_GET_PREFETCHED          0x04
#define ERROR_RESOURCE_DATA_NOT_FOUND    1812

typedef struct {
    DWORD dwSignature, dwStrucVersion;
    DWORD dwFileVersionMS, dwFileVersionLS, dwProductVersionMS, dwProductVersionLS;
    DWORD dwFileFlagsMask, dwFileFlags, dwFileOS, dwFileType, dwFileSubtype;
    DWORD dwFileDateMS, dwFileDateLS;
} VS_FIXEDFILEINFO;

static uint16_t rd16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* A PE file read from disk (not loaded): RVAs are translated through the
 * section table. Works for PE32 and PE32+ alike. */
struct pe_file {
    const uint8_t *data;
    size_t size;
    const uint8_t *sections;
    unsigned nsec;
    uint32_t rsrc_rva, rsrc_size;
};

static DWORD pe_parse(const uint8_t *d, size_t n, struct pe_file *pe)
{
    memset(pe, 0, sizeof *pe);
    if (n < 0x40 || rd16(d) != IMAGE_DOS_SIGNATURE) return ERROR_BAD_FORMAT;
    uint32_t nt = rd32(d + 0x3c);
    if ((uint64_t)nt + 24 > n || rd32(d + nt) != IMAGE_NT_SIGNATURE) return ERROR_BAD_FORMAT;
    unsigned nsec = rd16(d + nt + 6), optsize = rd16(d + nt + 20);
    if (optsize < 2 || (uint64_t)nt + 24 + optsize + nsec * 40ull > n) return ERROR_BAD_FORMAT;
    const uint8_t *opt = d + nt + 24;
    unsigned nrva_off, dd_off;
    switch (rd16(opt)) {
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC: nrva_off = 108; dd_off = 112; break;
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC: nrva_off = 92;  dd_off = 96;  break;
    default: return ERROR_BAD_FORMAT;
    }
    pe->data = d;
    pe->size = n;
    pe->sections = opt + optsize;
    pe->nsec = nsec;
    if (optsize >= dd_off + 8 * (IMAGE_DIRECTORY_ENTRY_RESOURCE + 1) &&
        rd32(opt + nrva_off) > IMAGE_DIRECTORY_ENTRY_RESOURCE) {
        pe->rsrc_rva = rd32(opt + dd_off + 8 * IMAGE_DIRECTORY_ENTRY_RESOURCE);
        pe->rsrc_size = rd32(opt + dd_off + 8 * IMAGE_DIRECTORY_ENTRY_RESOURCE + 4);
    }
    return 0;
}

/* RVA -> pointer into the file, with `len` bytes of file data behind it. */
static const uint8_t *pe_rva(const struct pe_file *pe, uint32_t rva, uint32_t len)
{
    for (unsigned i = 0; i < pe->nsec; i++) {
        const uint8_t *s = pe->sections + i * 40;
        uint32_t vsize = rd32(s + 8), va = rd32(s + 12), raw = rd32(s + 16), rawptr = rd32(s + 20);
        uint32_t span = vsize > raw ? vsize : raw;
        if (rva < va || rva - va >= span) continue;
        uint64_t off = (uint64_t)rawptr + (rva - va);
        if ((uint64_t)(rva - va) + len > raw || off + len > pe->size) return NULL;
        return pe->data + off;
    }
    return NULL;
}

/* One level of the resource tree: the entry with integer id `id`, or the
 * first entry when id < 0. Returns its OffsetToData, or -1. */
static int64_t res_entry(const uint8_t *rsrc, uint32_t rsize, uint32_t dir_off, int id)
{
    if ((uint64_t)dir_off + 16 > rsize) return -1;
    unsigned count = (unsigned)rd16(rsrc + dir_off + 12) + rd16(rsrc + dir_off + 14);
    for (unsigned i = 0; i < count; i++) {
        uint64_t e = (uint64_t)dir_off + 16 + 8ull * i;
        if (e + 8 > rsize) return -1;
        uint32_t name = rd32(rsrc + e);
        if (id < 0 || (!(name & 0x80000000u) && name == (uint32_t)id)) return rd32(rsrc + e + 4);
    }
    return -1;
}

/* Locate the RT_VERSION resource: type 16 -> first name (normally
 * VS_VERSION_INFO = 1) -> language neutral, else US English, else the first
 * language, as FindResource's neutral-language fallback does. */
static DWORD pe_version_resource(const struct pe_file *pe, const uint8_t **data, uint32_t *size)
{
    if (!pe->rsrc_rva || pe->rsrc_size < 16) return ERROR_RESOURCE_DATA_NOT_FOUND;
    const uint8_t *rsrc = pe_rva(pe, pe->rsrc_rva, 16);
    if (!rsrc) return ERROR_RESOURCE_DATA_NOT_FOUND;
    uint32_t rsize = pe->rsrc_size;
    /* The directory may be shorter on disk than its declared size. */
    while (rsize > 16 && !pe_rva(pe, pe->rsrc_rva, rsize)) rsize = rsize > 4096 ? rsize - 4096 : 16;
    int64_t t = res_entry(rsrc, rsize, 0, RT_VERSION);
    if (t < 0 || !(t & 0x80000000u)) return ERROR_RESOURCE_TYPE_NOT_FOUND;
    int64_t nm = res_entry(rsrc, rsize, (uint32_t)t & 0x7fffffffu, -1);
    if (nm < 0 || !(nm & 0x80000000u)) return 1814;       /* ERROR_RESOURCE_NAME_NOT_FOUND */
    uint32_t langdir = (uint32_t)nm & 0x7fffffffu;
    int64_t lg = res_entry(rsrc, rsize, langdir, 0);
    if (lg < 0) lg = res_entry(rsrc, rsize, langdir, 0x0409);
    if (lg < 0) lg = res_entry(rsrc, rsize, langdir, -1);
    if (lg < 0 || (lg & 0x80000000u) || (uint64_t)lg + 16 > rsize) return 1815;  /* ERROR_RESOURCE_LANG_NOT_FOUND */
    uint32_t rva = rd32(rsrc + lg), len = rd32(rsrc + lg + 4);
    const uint8_t *p = len >= 6 ? pe_rva(pe, rva, len) : NULL;
    if (!p) return ERROR_RESOURCE_DATA_NOT_FOUND;
    *data = p;
    *size = len;
    return 0;
}

/* The VS_VERSIONINFO resource of a file, as a malloc'd copy. Returns a
 * Win32 error code. */
static DWORD ver_read(const char *linux_path, uint8_t **out, DWORD *outlen)
{
    *out = NULL;
    *outlen = 0;
    if (!linux_path) return ERROR_FILE_NOT_FOUND;
    int fd = open(linux_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return lsiw_errno_to_win(errno);
    struct stat st;
    if (fstat(fd, &st)) { int e = errno; close(fd); return lsiw_errno_to_win(e); }
    if (S_ISDIR(st.st_mode)) { close(fd); return ERROR_ACCESS_DENIED; }
    if (!S_ISREG(st.st_mode) || st.st_size < 0x40) { close(fd); return ERROR_BAD_FORMAT; }
    size_t n = (size_t)st.st_size;
    uint8_t *map = mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return lsiw_errno_to_win(errno);
    struct pe_file pe;
    const uint8_t *res = NULL;
    uint32_t len = 0;
    DWORD err = pe_parse(map, n, &pe);
    if (!err) err = pe_version_resource(&pe, &res, &len);
    if (!err) {
        *out = malloc(len);
        if (*out) { memcpy(*out, res, len); *outlen = len; }
        else err = ERROR_NOT_ENOUGH_MEMORY;
    }
    munmap(map, n);
    return err;
}

/* Layout of the buffer GetFileVersionInfo fills:
 *   [VS_VERSIONINFO block, padded to 4][LSVQ magic][scratch size][scratch]
 * Both the A and W functions store the original UTF-16 block; VerQueryValueA
 * converts string values into the scratch area (Windows likewise asks for
 * a buffer several times the resource size to hold an ANSI copy). */
#define VQ_SCRATCH_MAGIC 0x5156534Cu        /* "LSVQ" */

static DWORD ver_total_size(DWORD reslen)
{
    DWORD r4 = (reslen + 3) & ~3u;
    return r4 * 3 + 8;
}

static void ver_set_handle(DWORD *handle) { if (handle) *handle = 0; }

static DWORD ver_size(const char *linux_path, DWORD *handle)
{
    ver_set_handle(handle);
    uint8_t *blk;
    DWORD len, err = ver_read(linux_path, &blk, &len);
    if (err) { lsiw_set_last_error(err); return 0; }
    free(blk);
    return ver_total_size(len);
}

static BOOL ver_get(const char *linux_path, DWORD len, void *data)
{
    if (!data) { lsiw_set_last_error(ERROR_INVALID_DATA); return FALSE; }
    uint8_t *blk;
    DWORD blen, err = ver_read(linux_path, &blk, &blen);
    if (err) { lsiw_set_last_error(err); return FALSE; }
    /* A short buffer gets a truncated copy, as on Windows. */
    DWORD n = blen < len ? blen : len;
    memcpy(data, blk, n);
    DWORD wlen = rd16(blk), r4 = ((wlen > blen ? blen : wlen) + 3) & ~3u;
    if (len >= r4 + 8 && n >= wlen) {
        uint32_t magic = VQ_SCRATCH_MAGIC, room = len - r4 - 8;
        if (n < r4) memset((uint8_t *)data + n, 0, r4 - n);
        memcpy((uint8_t *)data + r4, &magic, 4);
        memcpy((uint8_t *)data + r4 + 4, &room, 4);
    }
    free(blk);
    return TRUE;
}

static int ver_flags_ok(DWORD flags)
{
    if (flags & ~(DWORD)(FILE_VER_GET_LOCALISED | FILE_VER_GET_NEUTRAL | FILE_VER_GET_PREFETCHED)) {
        lsiw_set_last_error(ERROR_INVALID_FLAGS);
        return 0;
    }
    return 1;
}

/* There are no MUI satellite files, so the localised and the neutral
 * version information are the same: the file's own resource. */
API DWORD GetFileVersionInfoSizeW(const WCHAR *file, DWORD *handle)
{
    if (!file) { ver_set_handle(handle); lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    char *lp = lsiw_path_w(file);
    DWORD r = ver_size(lp, handle);
    free(lp);
    return r;
}

API DWORD GetFileVersionInfoSizeA(const char *file, DWORD *handle)
{
    if (!file) { ver_set_handle(handle); lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    char *lp = lsiw_path_a(file);
    DWORD r = ver_size(lp, handle);
    free(lp);
    return r;
}

API DWORD GetFileVersionInfoSizeExW(DWORD flags, const WCHAR *file, DWORD *handle)
{
    if (!ver_flags_ok(flags)) { ver_set_handle(handle); return 0; }
    return GetFileVersionInfoSizeW(file, handle);
}

API DWORD GetFileVersionInfoSizeExA(DWORD flags, const char *file, DWORD *handle)
{
    if (!ver_flags_ok(flags)) { ver_set_handle(handle); return 0; }
    return GetFileVersionInfoSizeA(file, handle);
}

API BOOL GetFileVersionInfoW(const WCHAR *file, DWORD handle, DWORD len, void *data)
{
    (void)handle;
    if (!file) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *lp = lsiw_path_w(file);
    BOOL r = ver_get(lp, len, data);
    free(lp);
    return r;
}

API BOOL GetFileVersionInfoA(const char *file, DWORD handle, DWORD len, void *data)
{
    (void)handle;
    if (!file) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *lp = lsiw_path_a(file);
    BOOL r = ver_get(lp, len, data);
    free(lp);
    return r;
}

API BOOL GetFileVersionInfoExW(DWORD flags, const WCHAR *file, DWORD handle, DWORD len, void *data)
{
    if (!ver_flags_ok(flags)) return FALSE;
    return GetFileVersionInfoW(file, handle, len, data);
}

API BOOL GetFileVersionInfoExA(DWORD flags, const char *file, DWORD handle, DWORD len, void *data)
{
    if (!ver_flags_ok(flags)) return FALSE;
    return GetFileVersionInfoA(file, handle, len, data);
}

/* A node of the VS_VERSIONINFO tree:
 *   WORD wLength, wValueLength, wType; WCHAR szKey[]; pad to 4;
 *   Value (wValueLength WCHARs if wType == 1, else bytes); pad to 4; Children
 * Offsets are relative to the root, which is 4-byte aligned in the file. */
struct vnode {
    size_t off, end, key, value, child;
    WORD vlen, type;
};

static int vnode_at(const uint8_t *b, size_t off, size_t limit, struct vnode *n)
{
    if (off + 6 > limit) return 0;
    WORD len = rd16(b + off);
    if (len < 6 || off + len > limit) return 0;
    n->off = off;
    n->end = off + len;
    n->vlen = rd16(b + off + 2);
    n->type = rd16(b + off + 4);
    n->key = off + 6;
    size_t k = n->key;
    while (k + 2 <= n->end && rd16(b + k)) k += 2;
    if (k + 2 > n->end) return 0;
    k = (k + 2 + 3) & ~(size_t)3;
    if (k > n->end) k = n->end;
    n->value = k;
    size_t vbytes = n->type == 1 ? (size_t)n->vlen * 2 : n->vlen;
    if (vbytes > n->end - k) vbytes = n->end - k;
    n->child = (k + vbytes + 3) & ~(size_t)3;
    if (n->child > n->end) n->child = n->end;
    return 1;
}

static int vkey_eq(const uint8_t *key, const WCHAR *name, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        WCHAR a = rd16(key + 2 * i), c = name[i];
        if (a < 128) a = (WCHAR)tolower(a);
        if (c < 128) c = (WCHAR)tolower(c);
        if (a != c) return 0;
    }
    return rd16(key + 2 * len) == 0;
}

/* Follow a "\Key\Key..." path from the root, matching keys without regard
 * to case. "\" (or "") names the root, whose value is VS_FIXEDFILEINFO. */
static int ver_find(const uint8_t *b, const WCHAR *sub, struct vnode *out)
{
    struct vnode n;
    if (!vnode_at(b, 0, rd16(b), &n)) return 0;
    const WCHAR *p = sub;
    for (;;) {
        while (*p == '\\') p++;
        if (!*p) break;
        const WCHAR *e = p;
        while (*e && *e != '\\') e++;
        size_t c = n.child;
        int found = 0;
        while (c < n.end) {
            struct vnode ch;
            if (!vnode_at(b, c, n.end, &ch)) break;
            if (vkey_eq(b + ch.key, p, (size_t)(e - p))) { n = ch; found = 1; break; }
            c = (ch.end + 3) & ~(size_t)3;
        }
        if (!found) return 0;
        p = e;
    }
    *out = n;
    return 1;
}

static int ver_query(const void *block, const WCHAR *sub, struct vnode *n)
{
    if (!ver_find(block, sub, n)) return 0;
    if (n->off == 0) {
        /* The root value must be a VS_FIXEDFILEINFO with its signature. */
        if (n->vlen < sizeof(VS_FIXEDFILEINFO) || n->value + sizeof(VS_FIXEDFILEINFO) > n->end ||
            rd32((const uint8_t *)block + n->value) != VS_FFI_SIGNATURE) return 0;
    }
    return 1;
}

API BOOL VerQueryValueW(const void *block, const WCHAR *sub, void **buf, UINT *len)
{
    struct vnode n;
    if (!block || !sub || !buf) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!ver_query(block, sub, &n)) {
        *buf = NULL;
        if (len) *len = 0;
        lsiw_set_last_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
        return FALSE;
    }
    *buf = (uint8_t *)block + n.value;
    /* Strings: length in characters as stored (it includes the NUL);
     * binary values: length in bytes. */
    if (len) *len = n.vlen;
    return TRUE;
}

/* Room for an ANSI string converted by VerQueryValueA: the scratch area
 * GetFileVersionInfo reserved, or a per-thread buffer for blocks that did
 * not come from it (such as a LockResource'd version resource). */
static char *vq_scratch(const uint8_t *block, size_t need)
{
    static __thread char *tbuf;
    static __thread size_t tcap;
    size_t r4 = ((size_t)rd16(block) + 3) & ~(size_t)3;
    uint32_t hdr[2];
    if (safe_read(hdr, block + r4, sizeof hdr) && hdr[0] == VQ_SCRATCH_MAGIC && hdr[1] >= need)
        return (char *)block + r4 + 8;
    if (tcap < need) {
        char *p = realloc(tbuf, need);
        if (!p) return NULL;
        tbuf = p;
        tcap = need;
    }
    return tbuf;
}

API BOOL VerQueryValueA(const void *block, const char *sub, void **buf, UINT *len)
{
    struct vnode n;
    if (!block || !sub || !buf) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    WCHAR *wsub = lsiw_u2w(sub);
    int ok = ver_query(block, wsub, &n);
    free(wsub);
    if (!ok) {
        *buf = NULL;
        if (len) *len = 0;
        lsiw_set_last_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
        return FALSE;
    }
    const uint8_t *v = (const uint8_t *)block + n.value;
    if (n.type != 1) {
        *buf = (void *)v;
        if (len) *len = n.vlen;
        return TRUE;
    }
    size_t chars = 0;
    while (chars < n.vlen && n.value + 2 * chars + 2 <= n.end && rd16(v + 2 * chars)) chars++;
    WCHAR *tmp = malloc((chars + 1) * sizeof(WCHAR));
    if (!tmp) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    memcpy(tmp, v, chars * sizeof(WCHAR));
    tmp[chars] = 0;
    char *u = lsiw_w2u(tmp);
    free(tmp);
    size_t need = strlen(u) + 1;
    char *dst = vq_scratch(block, need);
    if (!dst) { free(u); lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    memcpy(dst, u, need);
    free(u);
    *buf = dst;
    if (len) *len = n.vlen ? (UINT)need : 0;
    return TRUE;
}

/* VerLanguageName: English names of common languages, then the primary
 * language alone, then "Language Neutral". */
static const struct { WORD id; const char *name; } g_lang_names[] = {
    { 0x0000, "Language Neutral" },             { 0x007f, "Invariant Language" },
    { 0x0400, "Process Default Language" },     { 0x0800, "System Default Language" },
    { 0x0401, "Arabic (Saudi Arabia)" },        { 0x0402, "Bulgarian (Bulgaria)" },
    { 0x0403, "Catalan (Catalan)" },            { 0x0404, "Chinese (Traditional, Taiwan)" },
    { 0x0405, "Czech (Czech Republic)" },       { 0x0406, "Danish (Denmark)" },
    { 0x0407, "German (Germany)" },             { 0x0408, "Greek (Greece)" },
    { 0x0409, "English (United States)" },      { 0x040a, "Spanish (Spain, Traditional Sort)" },
    { 0x040b, "Finnish (Finland)" },            { 0x040c, "French (France)" },
    { 0x040d, "Hebrew (Israel)" },              { 0x040e, "Hungarian (Hungary)" },
    { 0x040f, "Icelandic (Iceland)" },          { 0x0410, "Italian (Italy)" },
    { 0x0411, "Japanese (Japan)" },             { 0x0412, "Korean (Korea)" },
    { 0x0413, "Dutch (Netherlands)" },          { 0x0414, "Norwegian, Bokm\xc3\xa5l (Norway)" },
    { 0x0415, "Polish (Poland)" },              { 0x0416, "Portuguese (Brazil)" },
    { 0x0418, "Romanian (Romania)" },           { 0x0419, "Russian (Russia)" },
    { 0x041a, "Croatian (Croatia)" },           { 0x041b, "Slovak (Slovakia)" },
    { 0x041d, "Swedish (Sweden)" },             { 0x041e, "Thai (Thailand)" },
    { 0x041f, "Turkish (Turkey)" },             { 0x0421, "Indonesian (Indonesia)" },
    { 0x0422, "Ukrainian (Ukraine)" },          { 0x0424, "Slovenian (Slovenia)" },
    { 0x0425, "Estonian (Estonia)" },           { 0x0426, "Latvian (Latvia)" },
    { 0x0427, "Lithuanian (Lithuania)" },       { 0x0429, "Persian (Iran)" },
    { 0x042a, "Vietnamese (Vietnam)" },         { 0x0439, "Hindi (India)" },
    { 0x0804, "Chinese (Simplified, PRC)" },    { 0x0807, "German (Switzerland)" },
    { 0x0809, "English (United Kingdom)" },     { 0x080a, "Spanish (Mexico)" },
    { 0x080c, "French (Belgium)" },             { 0x0810, "Italian (Switzerland)" },
    { 0x0813, "Dutch (Belgium)" },              { 0x0816, "Portuguese (Portugal)" },
    { 0x0c04, "Chinese (Traditional, Hong Kong S.A.R.)" }, { 0x0c07, "German (Austria)" },
    { 0x0c09, "English (Australia)" },          { 0x0c0a, "Spanish (Spain, International Sort)" },
    { 0x0c0c, "French (Canada)" },              { 0x1009, "English (Canada)" },
    { 0x100c, "French (Switzerland)" },         { 0x1409, "English (New Zealand)" },
    { 0x1809, "English (Ireland)" },            { 0x1c09, "English (South Africa)" },
};

static char *lang_name(DWORD lang)
{
    size_t n = sizeof g_lang_names / sizeof g_lang_names[0];
    for (size_t i = 0; i < n; i++)
        if (g_lang_names[i].id == lang) return strdup(g_lang_names[i].name);
    for (size_t i = 4; i < n; i++)
        if ((g_lang_names[i].id & 0x3ff) == (lang & 0x3ff)) {
            const char *s = g_lang_names[i].name, *p = strstr(s, " (");
            return strndup(s, p ? (size_t)(p - s) : strlen(s));
        }
    return strdup("Language Neutral");
}

/* Returns the full length (without NUL); copies as much as fits. */
API DWORD VerLanguageNameA(DWORD lang, char *buf, DWORD size)
{
    char *s = lang_name(lang);
    DWORD l = (DWORD)strlen(s);
    if (buf && size) { DWORD c = l < size - 1 ? l : size - 1; memcpy(buf, s, c); buf[c] = 0; }
    free(s);
    return l;
}

API DWORD VerLanguageNameW(DWORD lang, WCHAR *buf, DWORD size)
{
    char *s = lang_name(lang);
    WCHAR *w = lsiw_u2w(s);
    DWORD l = (DWORD)lsiw_wcslen(w);
    if (buf && size) { DWORD c = l < size - 1 ? l : size - 1; memcpy(buf, w, c * sizeof(WCHAR)); buf[c] = 0; }
    free(s);
    free(w);
    return l;
}

/* ---- VerFindFile / VerInstallFile --------------------------------- */

#define VFFF_ISSHAREDFILE   0x0001
#define VFF_CURNEDEST       0x0001
#define VFF_BUFFTOOSMALL    0x0004
#define VIFF_FORCEINSTALL   0x0001
#define VIFF_DONTDELETEOLD  0x0002
#define VIF_MISMATCH        0x00000002
#define VIF_SRCOLD          0x00000004
#define VIF_WRITEPROT       0x00000040
#define VIF_OUTOFSPACE      0x00000100
#define VIF_ACCESSVIOLATION 0x00000200
#define VIF_CANNOTCREATE    0x00000800
#define VIF_CANNOTDELETECUR 0x00004000
#define VIF_OUTOFMEMORY     0x00008000
#define VIF_CANNOTREADSRC   0x00010000
#define VIF_BUFFTOOSMALL    0x00040000
#define VIF_CANNOTLOADLZ32  0x00080000

static char *win_join(const char *dir, const char *file)
{
    size_t l = dir ? strlen(dir) : 0;
    if (!l) return strdup(file);
    return lsiw_strdup_printf("%s%s%s", dir, (dir[l - 1] == '\\' || dir[l - 1] == '/') ? "" : "\\", file);
}

static int win_file_exists(const char *dir, const char *file)
{
    char *w = win_join(dir, file);
    char *lp = lsiw_path_a(w);
    int r = lp && !access(lp, F_OK);
    free(w);
    free(lp);
    return r;
}

static int win_dir_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    while (la && (a[la - 1] == '\\' || a[la - 1] == '/')) la--;
    while (lb && (b[lb - 1] == '\\' || b[lb - 1] == '/')) lb--;
    return la == lb && !strncasecmp(a, b, la);
}

/* Where a file should go (system folder for shared files, else the
 * application folder) and where a copy already exists. */
static DWORD ver_find_file(DWORD flags, const char *file, const char *windir, const char *appdir,
                           const char **cur, const char **dest)
{
    const char *sys = lsiw_var_windows("SYSTEM32");
    const char *win = windir && *windir ? windir : lsiw_var_windows("WINDIR");
    if (!sys) sys = "C:\\windows\\system32";
    if (!win) win = "C:\\windows";
    if (!appdir) appdir = "";
    *dest = (flags & VFFF_ISSHAREDFILE) ? sys : appdir;
    const char *order[3];
    if (flags & VFFF_ISSHAREDFILE) { order[0] = sys; order[1] = win; order[2] = appdir; }
    else { order[0] = appdir; order[1] = sys; order[2] = win; }
    *cur = "";
    for (int i = 0; i < 3; i++)
        if (*order[i] && file && win_file_exists(order[i], file)) { *cur = order[i]; break; }
    return (**cur && !win_dir_eq(*cur, *dest)) ? VFF_CURNEDEST : 0;
}

static DWORD put_dir_a(const char *s, char *buf, UINT *len)
{
    UINT need = (UINT)strlen(s) + 1;
    if (!len) return 0;
    if (!buf || *len < need) {
        if (buf && *len) { memcpy(buf, s, *len - 1); buf[*len - 1] = 0; }
        *len = need;
        return VFF_BUFFTOOSMALL;
    }
    memcpy(buf, s, need);
    *len = need;
    return 0;
}

static DWORD put_dir_w(const char *s, WCHAR *buf, UINT *len)
{
    WCHAR *w = lsiw_u2w(s);
    UINT need = (UINT)lsiw_wcslen(w) + 1;
    DWORD r = 0;
    if (!len) { free(w); return 0; }
    if (!buf || *len < need) {
        if (buf && *len) { memcpy(buf, w, (*len - 1) * sizeof(WCHAR)); buf[*len - 1] = 0; }
        r = VFF_BUFFTOOSMALL;
    } else {
        memcpy(buf, w, need * sizeof(WCHAR));
    }
    *len = need;
    free(w);
    return r;
}

API DWORD VerFindFileA(DWORD flags, const char *file, const char *windir, const char *appdir,
                       char *curdir, UINT *curlen, char *destdir, UINT *destlen)
{
    const char *cur, *dest;
    DWORD r = ver_find_file(flags, file, windir, appdir, &cur, &dest);
    r |= put_dir_a(cur, curdir, curlen);
    r |= put_dir_a(dest, destdir, destlen);
    return r;
}

API DWORD VerFindFileW(DWORD flags, const WCHAR *file, const WCHAR *windir, const WCHAR *appdir,
                       WCHAR *curdir, UINT *curlen, WCHAR *destdir, UINT *destlen)
{
    char *f = lsiw_w2u(file), *w = lsiw_w2u(windir), *a = lsiw_w2u(appdir);
    const char *cur, *dest;
    DWORD r = ver_find_file(flags, f, w, a, &cur, &dest);
    char *c = strdup(cur), *d = strdup(dest);   /* `cur`/`dest` may point into `a` */
    free(f); free(w); free(a);
    r |= put_dir_w(c, curdir, curlen);
    r |= put_dir_w(d, destdir, destlen);
    free(c); free(d);
    return r;
}

static uint64_t file_version_of(const char *linux_path, int *have)
{
    uint8_t *blk;
    DWORD len;
    *have = 0;
    if (ver_read(linux_path, &blk, &len)) return 0;
    struct vnode n;
    static const WCHAR root[] = { '\\', 0 };
    uint64_t v = 0;
    if (ver_query(blk, root, &n)) {
        VS_FIXEDFILEINFO fi;
        memcpy(&fi, blk + n.value, sizeof fi);
        v = (uint64_t)fi.dwFileVersionMS << 32 | fi.dwFileVersionLS;
        *have = 1;
    }
    free(blk);
    return v;
}

static DWORD errno_to_vif(int e)
{
    switch (e) {
    case ENOSPC: case EDQUOT: return VIF_OUTOFSPACE;
    case EROFS: return VIF_WRITEPROT;
    case EACCES: case EPERM: return VIF_ACCESSVIOLATION;
    case ENOMEM: return VIF_OUTOFMEMORY;
    }
    return VIF_CANNOTCREATE;
}

/* Copy srcdir\src to destdir\dest. Version-checked unless VIFF_FORCEINSTALL:
 * an older source is refused with VIF_SRCOLD | VIF_MISMATCH (Windows also
 * leaves a temporary copy behind in that case; this does not). LZ-compressed
 * (SZDD) sources are not expanded. */
static DWORD ver_install(DWORD flags, const char *src, const char *dest, const char *srcdir,
                         const char *destdir, const char *curdir)
{
    char *ws = win_join(srcdir, src), *wd = win_join(destdir, dest);
    char *ls = lsiw_path_a(ws), *ld = lsiw_path_a(wd);
    free(ws); free(wd);
    DWORD r = 0;
    int in = ls ? open(ls, O_RDONLY | O_CLOEXEC) : -1;
    char magic[4];
    if (in < 0) { r = VIF_CANNOTREADSRC; goto out; }
    if (pread(in, magic, 4, 0) == 4 && !memcmp(magic, "SZDD", 4)) { r = VIF_CANNOTLOADLZ32; goto out; }
    if (!ld) { r = VIF_CANNOTCREATE; goto out; }
    if (!(flags & VIFF_FORCEINSTALL) && !access(ld, F_OK)) {
        int hs, hd;
        uint64_t vs = file_version_of(ls, &hs), vd = file_version_of(ld, &hd);
        if (hs && hd && vs < vd) { r = VIF_SRCOLD | VIF_MISMATCH; goto out; }
    }
    char *tmp = lsiw_strdup_printf("%s.lsiwXXXXXX", ld);
    int outfd = mkstemp(tmp);
    if (outfd < 0) { r = errno_to_vif(errno); free(tmp); goto out; }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t o = 0; o < n; ) {
            ssize_t w = write(outfd, buf + o, (size_t)(n - o));
            if (w <= 0) { r = errno_to_vif(errno); break; }
            o += w;
        }
        if (r) break;
    }
    if (n < 0 && !r) r = VIF_CANNOTREADSRC;
    struct stat st;
    if (!r && !fstat(in, &st)) fchmod(outfd, st.st_mode & 0777);
    close(outfd);
    if (!r && rename(tmp, ld)) r = errno_to_vif(errno);
    if (r) unlink(tmp);
    free(tmp);
    /* The old copy elsewhere is removed unless VIFF_DONTDELETEOLD. */
    if (!r && curdir && *curdir && destdir && !win_dir_eq(curdir, destdir) && !(flags & VIFF_DONTDELETEOLD)) {
        char *wc = win_join(curdir, dest), *lc = lsiw_path_a(wc);
        if (lc && !access(lc, F_OK) && unlink(lc)) r = VIF_CANNOTDELETECUR;
        free(wc); free(lc);
    }
out:
    if (in >= 0) close(in);
    free(ls); free(ld);
    return r;
}

API DWORD VerInstallFileA(DWORD flags, const char *src, const char *dest, const char *srcdir,
                          const char *destdir, const char *curdir, char *tmpfile, UINT *tmplen)
{
    (void)tmpfile; (void)tmplen;
    if (!src || !dest) return VIF_CANNOTREADSRC;
    return ver_install(flags, src, dest, srcdir, destdir, curdir);
}

API DWORD VerInstallFileW(DWORD flags, const WCHAR *src, const WCHAR *dest, const WCHAR *srcdir,
                          const WCHAR *destdir, const WCHAR *curdir, WCHAR *tmpfile, UINT *tmplen)
{
    (void)tmpfile; (void)tmplen;
    if (!src || !dest) return VIF_CANNOTREADSRC;
    char *s = lsiw_w2u(src), *d = lsiw_w2u(dest), *sd = lsiw_w2u(srcdir), *dd = lsiw_w2u(destdir), *cd = lsiw_w2u(curdir);
    DWORD r = ver_install(flags, s, d, sd, dd, cd);
    free(s); free(d); free(sd); free(dd); free(cd);
    return r;
}

/* ================================================================== */
/* userenv                                                              */
/* ================================================================== */

static const char *var_or(const char *name, const char *def)
{
    const char *v = lsiw_var_windows(name);
    return v && *v ? v : def;
}

/* The userenv sizing convention: *size is in characters including the NUL;
 * too small -> FALSE, ERROR_INSUFFICIENT_BUFFER and the needed size. */
static BOOL profile_ret_a(const char *s, char *buf, DWORD *size)
{
    if (!size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD need = (DWORD)strlen(s) + 1;
    if (!buf || *size < need) { *size = need; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(buf, s, need);
    *size = need;
    return TRUE;
}

static BOOL profile_ret_w(const char *s, WCHAR *buf, DWORD *size)
{
    if (!size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD need = (DWORD)lsiw_utf8_to_utf16(s, -1, NULL, 0);
    if (!buf || *size < need) { *size = need; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    lsiw_utf8_to_utf16(s, -1, buf, (int)need);
    *size = need;
    return TRUE;
}

static const char *profile_dir(void)    { return var_or("CSIDL_PROFILE", "C:\\users\\user"); }
static const char *profiles_dir(void)   { return var_or("PROFILESFOLDER", "C:\\users"); }
static const char *allusers_dir(void)   { return var_or("ALLUSERSPROFILE", "C:\\ProgramData"); }
static const char *default_profile(void){ return var_or("DEFAULTUSERPROFILE", "C:\\users\\Default"); }

/* The token is not examined: every token belongs to the one Linux user. */
API BOOL GetUserProfileDirectoryA(HANDLE tok, char *buf, DWORD *size)  { (void)tok; return profile_ret_a(profile_dir(), buf, size); }
API BOOL GetUserProfileDirectoryW(HANDLE tok, WCHAR *buf, DWORD *size) { (void)tok; return profile_ret_w(profile_dir(), buf, size); }
API BOOL GetProfilesDirectoryA(char *buf, DWORD *size)   { return profile_ret_a(profiles_dir(), buf, size); }
API BOOL GetProfilesDirectoryW(WCHAR *buf, DWORD *size)  { return profile_ret_w(profiles_dir(), buf, size); }
API BOOL GetAllUsersProfileDirectoryA(char *buf, DWORD *size)  { return profile_ret_a(allusers_dir(), buf, size); }
API BOOL GetAllUsersProfileDirectoryW(WCHAR *buf, DWORD *size) { return profile_ret_w(allusers_dir(), buf, size); }
API BOOL GetDefaultUserProfileDirectoryA(char *buf, DWORD *size)  { return profile_ret_a(default_profile(), buf, size); }
API BOOL GetDefaultUserProfileDirectoryW(WCHAR *buf, DWORD *size) { return profile_ret_w(default_profile(), buf, size); }

/* 0 = a local, permanent profile (no PT_TEMPORARY/ROAMING/MANDATORY bit). */
API BOOL GetProfileType(DWORD *flags)
{
    if (!flags) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *flags = 0;
    return TRUE;
}

typedef WCHAR *(WINAPI *get_env_w_fn)(void);
typedef BOOL (WINAPI *free_env_w_fn)(WCHAR *);
typedef DWORD (WINAPI *expand_w_fn)(const WCHAR *, WCHAR *, DWORD);
typedef DWORD (WINAPI *expand_a_fn)(const char *, char *, DWORD);

/* The environment of the (only) user is the process environment, whether or
 * not `inherit` asks for the current process's variables to be merged in. */
API BOOL CreateEnvironmentBlock(void **env, HANDLE tok, BOOL inherit)
{
    (void)tok; (void)inherit;
    if (!env) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    get_env_w_fn get = (get_env_w_fn)k32_api("GetEnvironmentStringsW");
    free_env_w_fn fre = (free_env_w_fn)k32_api("FreeEnvironmentStringsW");
    WCHAR *src = get ? get() : NULL;
    if (!src) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    size_t n = 0;
    while (src[n] || src[n + 1]) n++;
    n += 2;
    WCHAR *blk = lsiw_heap_alloc(n * sizeof(WCHAR), 0);
    if (blk) memcpy(blk, src, n * sizeof(WCHAR));
    if (fre) fre(src);
    if (!blk) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    *env = blk;
    return TRUE;
}

API BOOL DestroyEnvironmentBlock(void *env)
{
    if (env) lsiw_heap_free(env);
    return TRUE;
}

API BOOL ExpandEnvironmentStringsForUserW(HANDLE tok, const WCHAR *src, WCHAR *dst, DWORD size)
{
    (void)tok;
    expand_w_fn f = (expand_w_fn)k32_api("ExpandEnvironmentStringsW");
    if (!src || !f) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD n = f(src, dst, size);
    if (!n) return FALSE;
    if (n > size) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    return TRUE;
}

API BOOL ExpandEnvironmentStringsForUserA(HANDLE tok, const char *src, char *dst, DWORD size)
{
    (void)tok;
    expand_a_fn f = (expand_a_fn)k32_api("ExpandEnvironmentStringsA");
    if (!src || !f) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD n = f(src, dst, size);
    if (!n) return FALSE;
    if (n > size) { lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    return TRUE;
}

typedef struct {
    DWORD dwSize, dwFlags;
    void *lpUserName, *lpProfilePath, *lpDefaultPath, *lpServerName, *lpPolicyPath;
    HANDLE hProfile;
} PROFILEINFO;

/* The user's profile is always loaded; its registry hive is HKCU. */
API BOOL LoadUserProfileW(HANDLE tok, PROFILEINFO *pi)
{
    (void)tok;
    if (!pi || pi->dwSize != sizeof *pi || !pi->lpUserName) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    pi->hProfile = HKEY_CURRENT_USER;
    return TRUE;
}

API BOOL LoadUserProfileA(HANDLE tok, PROFILEINFO *pi) { return LoadUserProfileW(tok, pi); }
API BOOL UnloadUserProfile(HANDLE tok, HANDLE prof) { (void)tok; (void)prof; return TRUE; }

/* There are no AppContainers; no SID names one. */
API HRESULT GetAppContainerFolderPath(const WCHAR *sid, WCHAR **path)
{
    (void)sid;
    if (path) *path = NULL;
    return E_INVALIDARG;
}

/* Creating another user's profile needs administrator rights. */
API HRESULT CreateProfile(const WCHAR *sid, const WCHAR *user, WCHAR *path, DWORD cch)
{
    (void)sid; (void)user;
    if (path && cch) path[0] = 0;
    return E_ACCESSDENIED;
}

/* ================================================================== */
/* secur32 / sspicli                                                    */
/* ================================================================== */

enum {
    NameUnknown = 0, NameFullyQualifiedDN = 1, NameSamCompatible = 2, NameDisplay = 3,
    NameUniqueId = 6, NameCanonical = 7, NameUserPrincipal = 8, NameCanonicalEx = 9,
    NameServicePrincipal = 10, NameDnsDomain = 12, NameGivenName = 13, NameSurname = 14
};

/* The display name: the GECOS full name, else the login name. */
static char *display_name(void)
{
    struct passwd pw, *res = NULL;
    char buf[4096];
    if (!getpwuid_r(getuid(), &pw, buf, sizeof buf, &res) && res && res->pw_gecos && *res->pw_gecos) {
        size_t l = strcspn(res->pw_gecos, ",");
        if (l) return strndup(res->pw_gecos, l);
    }
    return strdup(user_name());
}

/* Returns the name or NULL with the Win32 error in *err. A machine outside
 * a domain has no DN, UPN, canonical or DNS-domain names. */
static char *user_name_ex(int fmt, DWORD *err)
{
    switch (fmt) {
    case NameSamCompatible: return lsiw_strdup_printf("%s\\%s", computer_name(), user_name());
    case NameDisplay: return display_name();
    case NameUniqueId:
        return lsiw_strdup_printf("{%08x-0000-0000-0000-000000000000}", (unsigned)getuid());
    case NameFullyQualifiedDN: case NameCanonical: case NameUserPrincipal: case NameCanonicalEx:
    case NameServicePrincipal: case NameDnsDomain: case NameGivenName: case NameSurname:
        *err = ERROR_NONE_MAPPED;
        return NULL;
    }
    *err = ERROR_INVALID_PARAMETER;
    return NULL;
}

/* Sizes are in characters: success stores the length without the NUL, a
 * short buffer gets ERROR_MORE_DATA and the size including it. */
API BOOLEAN GetUserNameExA(int fmt, char *buf, ULONG *size)
{
    DWORD err = 0;
    if (!size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *s = user_name_ex(fmt, &err);
    if (!s) { lsiw_set_last_error(err); return FALSE; }
    ULONG need = (ULONG)strlen(s) + 1;
    BOOLEAN ok = buf && *size >= need;
    if (ok) { memcpy(buf, s, need); *size = need - 1; }
    else { *size = need; lsiw_set_last_error(ERROR_MORE_DATA); }
    free(s);
    return ok;
}

API BOOLEAN GetUserNameExW(int fmt, WCHAR *buf, ULONG *size)
{
    DWORD err = 0;
    if (!size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *s = user_name_ex(fmt, &err);
    if (!s) { lsiw_set_last_error(err); return FALSE; }
    ULONG need = (ULONG)lsiw_utf8_to_utf16(s, -1, NULL, 0);
    BOOLEAN ok = buf && *size >= need;
    if (ok) { lsiw_utf8_to_utf16(s, -1, buf, (int)need); *size = need - 1; }
    else { *size = need; lsiw_set_last_error(ERROR_MORE_DATA); }
    free(s);
    return ok;
}

/* Not joined to a domain: no computer object exists. */
API BOOLEAN GetComputerObjectNameA(int fmt, char *buf, ULONG *size)
{
    (void)fmt; (void)buf; (void)size;
    lsiw_set_last_error(ERROR_CANT_ACCESS_DOMAIN_INFO);
    return FALSE;
}

API BOOLEAN GetComputerObjectNameW(int fmt, WCHAR *buf, ULONG *size)
{
    (void)fmt; (void)buf; (void)size;
    lsiw_set_last_error(ERROR_CANT_ACCESS_DOMAIN_INFO);
    return FALSE;
}

/* SSPI with no security packages: Negotiate, NTLM, Kerberos and Schannel
 * are not available natively. Package lookups fail with
 * SEC_E_SECPKG_NOT_FOUND; since no credential or context handle can ever be
 * issued, every call taking one fails with SEC_E_INVALID_HANDLE. */
typedef LONG SECURITY_STATUS;
#define SEC_E_OK                   ((SECURITY_STATUS)0)
#define SEC_E_INVALID_HANDLE       ((SECURITY_STATUS)0x80090301)
#define SEC_E_UNSUPPORTED_FUNCTION ((SECURITY_STATUS)0x80090302)
#define SEC_E_SECPKG_NOT_FOUND     ((SECURITY_STATUS)0x80090305)

API SECURITY_STATUS AcquireCredentialsHandleW(void *principal, void *package, ULONG use, void *logon,
                                              void *auth, void *keyfn, void *keyarg, void *cred, void *expiry)
{
    (void)principal; (void)package; (void)use; (void)logon; (void)auth; (void)keyfn; (void)keyarg; (void)expiry;
    if (cred) memset(cred, 0, 16);
    return SEC_E_SECPKG_NOT_FOUND;
}

API SECURITY_STATUS AcquireCredentialsHandleA(void *principal, void *package, ULONG use, void *logon,
                                              void *auth, void *keyfn, void *keyarg, void *cred, void *expiry)
{
    return AcquireCredentialsHandleW(principal, package, use, logon, auth, keyfn, keyarg, cred, expiry);
}

/* Shared body of the calls that take a credential or context handle; the
 * arguments are never looked at (callers clean up the MS-ABI stack). */
API SECURITY_STATUS sspi_invalid_handle(void) { return SEC_E_INVALID_HANDLE; }
API SECURITY_STATUS sspi_unsupported(void)    { return SEC_E_UNSUPPORTED_FUNCTION; }

API SECURITY_STATUS FreeContextBuffer(void *p)
{
    if (p) lsiw_heap_free(p);
    return SEC_E_OK;
}

API SECURITY_STATUS EnumerateSecurityPackagesW(ULONG *count, void **info)
{
    if (count) *count = 0;
    if (info) *info = NULL;
    return SEC_E_OK;
}

API SECURITY_STATUS EnumerateSecurityPackagesA(ULONG *count, void **info) { return EnumerateSecurityPackagesW(count, info); }

API SECURITY_STATUS QuerySecurityPackageInfoW(const void *name, void **info)
{
    (void)name;
    if (info) *info = NULL;
    return SEC_E_SECPKG_NOT_FOUND;
}

API SECURITY_STATUS QuerySecurityPackageInfoA(const void *name, void **info) { return QuerySecurityPackageInfoW(name, info); }

/* SecurityFunctionTable (version 1): dwVersion, then 27 function slots. */
struct sec_fn_table {
    ULONG dwVersion;
    void *fn[27];
};

static const struct sec_fn_table g_sspi_table_w = { 1, {
    (void *)EnumerateSecurityPackagesW, (void *)sspi_invalid_handle /* QueryCredentialsAttributes */,
    (void *)AcquireCredentialsHandleW, (void *)sspi_invalid_handle /* FreeCredentialsHandle */, NULL,
    (void *)sspi_invalid_handle /* InitializeSecurityContext */, (void *)sspi_invalid_handle /* AcceptSecurityContext */,
    (void *)sspi_invalid_handle /* CompleteAuthToken */, (void *)sspi_invalid_handle /* DeleteSecurityContext */,
    (void *)sspi_invalid_handle /* ApplyControlToken */, (void *)sspi_invalid_handle /* QueryContextAttributes */,
    (void *)sspi_invalid_handle /* ImpersonateSecurityContext */, (void *)sspi_invalid_handle /* RevertSecurityContext */,
    (void *)sspi_invalid_handle /* MakeSignature */, (void *)sspi_invalid_handle /* VerifySignature */,
    (void *)FreeContextBuffer, (void *)QuerySecurityPackageInfoW, NULL, NULL,
    (void *)sspi_invalid_handle /* ExportSecurityContext */, (void *)sspi_unsupported /* ImportSecurityContext */,
    (void *)sspi_invalid_handle /* AddCredentials */, NULL,
    (void *)sspi_invalid_handle /* QuerySecurityContextToken */, (void *)sspi_invalid_handle /* EncryptMessage */,
    (void *)sspi_invalid_handle /* DecryptMessage */, NULL } };

static const struct sec_fn_table g_sspi_table_a = { 1, {
    (void *)EnumerateSecurityPackagesA, (void *)sspi_invalid_handle, (void *)AcquireCredentialsHandleA,
    (void *)sspi_invalid_handle, NULL, (void *)sspi_invalid_handle, (void *)sspi_invalid_handle,
    (void *)sspi_invalid_handle, (void *)sspi_invalid_handle, (void *)sspi_invalid_handle,
    (void *)sspi_invalid_handle, (void *)sspi_invalid_handle, (void *)sspi_invalid_handle,
    (void *)sspi_invalid_handle, (void *)sspi_invalid_handle, (void *)FreeContextBuffer,
    (void *)QuerySecurityPackageInfoA, NULL, NULL, (void *)sspi_invalid_handle, (void *)sspi_unsupported,
    (void *)sspi_invalid_handle, NULL, (void *)sspi_invalid_handle, (void *)sspi_invalid_handle,
    (void *)sspi_invalid_handle, NULL } };

API const void *InitSecurityInterfaceW(void) { return &g_sspi_table_w; }
API const void *InitSecurityInterfaceA(void) { return &g_sspi_table_a; }

/* ================================================================== */
/* psapi                                                                */
/* ================================================================== */

#define ERROR_PARTIAL_COPY 299

/* Linux pid behind a process handle (LSIW process ids are Linux pids). */
static int handle_pid(HANDLE h)
{
    if (is_current_process(h)) return getpid();
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_PROCESS);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return -1; }
    return o->pid;
}

API BOOL EnumProcesses(DWORD *pids, DWORD cb, DWORD *needed)
{
    if (!needed || (cb && !pids)) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    DIR *d = opendir("/proc");
    if (!d) { lsiw_set_errno_error(errno); return FALSE; }
    DWORD n = 0, max = cb / sizeof(DWORD);
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        char *end;
        unsigned long v = strtoul(e->d_name, &end, 10);
        if (*end || end == e->d_name) continue;
        pids[n++] = (DWORD)v;
    }
    closedir(d);
    *needed = n * (DWORD)sizeof(DWORD);   /* bytes returned, as on Windows */
    return TRUE;
}

/* The Windows path of a process's image. For this process it is the
 * program LSIW runs (the Linux executable is LSIW itself). */
static char *process_image_win(int pid)
{
    if (pid == getpid() && lsiw_main_module && lsiw_main_module->win_path)
        return strdup(lsiw_main_module->win_path);
    char link[64], target[4096];
    snprintf(link, sizeof link, "/proc/%d/exe", pid);
    ssize_t l = readlink(link, target, sizeof target - 1);
    if (l <= 0) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return NULL; }
    target[l] = 0;
    return lsiw_linux_to_win(target);
}

static char *process_image_device(HANDLE h)
{
    int pid = handle_pid(h);
    if (pid < 0) return NULL;
    char *w = process_image_win(pid);
    if (!w) return NULL;
    char *d = device_path(w);
    free(w);
    return d;
}

API DWORD GetProcessImageFileNameA(HANDLE h, char *buf, DWORD size)
{
    char *d = process_image_device(h);
    if (!d) return 0;
    DWORD l = (DWORD)strlen(d);
    if (!buf || l + 1 > size) { free(d); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, d, l + 1);
    free(d);
    return l;
}

API DWORD GetProcessImageFileNameW(HANDLE h, WCHAR *buf, DWORD size)
{
    char *d = process_image_device(h);
    if (!d) return 0;
    DWORD need = (DWORD)lsiw_utf8_to_utf16(d, -1, NULL, 0);
    if (!buf || need > size) { free(d); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    lsiw_utf8_to_utf16(d, -1, buf, (int)size);
    free(d);
    return need - 1;
}

/* Device path of the file mapped at `addr`: a loaded PE module first (the
 * loader may copy images into anonymous memory), else /proc/<pid>/maps. */
static char *mapped_file_device(HANDLE h, const void *addr)
{
    int pid = handle_pid(h);
    if (pid < 0) return NULL;
    if (pid == getpid()) {
        struct lsiw_module *m = lsiw_module_from_address(addr);
        if (m && m->win_path) return device_path(m->win_path);
    }
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "re");
    if (!f) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return NULL; }
    char line[4352];
    uintptr_t a = (uintptr_t)addr;
    char *res = NULL;
    DWORD err = ERROR_INVALID_ADDRESS;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e;
        int pos = 0;
        if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %n", &s, &e, &pos) < 2) continue;
        if (a < s || a >= e) continue;
        char *file = line + pos;
        file[strcspn(file, "\n")] = 0;
        if (file[0] != '/') { err = ERROR_FILE_INVALID; break; }   /* private memory */
        char *w = lsiw_linux_to_win(file);
        res = device_path(w);
        free(w);
        break;
    }
    fclose(f);
    if (!res) lsiw_set_last_error(err);
    return res;
}

/* Truncates to the buffer and returns the characters copied. */
API DWORD GetMappedFileNameA(HANDLE h, void *addr, char *buf, DWORD size)
{
    char *d = mapped_file_device(h, addr);
    if (!d) return 0;
    if (!buf || !size) { free(d); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    DWORD l = (DWORD)strlen(d);
    if (l >= size) { l = size - 1; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); }
    memcpy(buf, d, l);
    buf[l] = 0;
    free(d);
    return l;
}

API DWORD GetMappedFileNameW(HANDLE h, void *addr, WCHAR *buf, DWORD size)
{
    char *d = mapped_file_device(h, addr);
    if (!d) return 0;
    if (!buf || !size) { free(d); lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    WCHAR *w = lsiw_u2w(d);
    DWORD l = (DWORD)lsiw_wcslen(w);
    if (l >= size) { l = size - 1; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER); }
    memcpy(buf, w, l * sizeof(WCHAR));
    buf[l] = 0;
    free(w);
    free(d);
    return l;
}

/* /proc/meminfo value in kB, or -1. */
static long long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "re");
    if (!f) return -1;
    char line[256];
    size_t kl = strlen(key);
    long long v = -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ':') { v = strtoll(line + kl + 1, NULL, 10); break; }
    fclose(f);
    return v;
}

static DWORD count_processes(void)
{
    DWORD n = 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) if (isdigit((unsigned char)e->d_name[0])) n++;
    closedir(d);
    return n;
}

typedef struct {
    DWORD cb;
    SIZE_T CommitTotal, CommitLimit, CommitPeak, PhysicalTotal, PhysicalAvailable, SystemCache,
           KernelTotal, KernelPaged, KernelNonpaged, PageSize;
    DWORD HandleCount, ProcessCount, ThreadCount;
} PERFORMANCE_INFORMATION;

static SIZE_T g_commit_peak;

/* All sizes are in pages. */
API BOOL GetPerformanceInfo(PERFORMANCE_INFORMATION *pi, DWORD cb)
{
    if (!pi || cb < sizeof *pi) { lsiw_set_last_error(ERROR_BAD_LENGTH); return FALSE; }
    long pg = sysconf(_SC_PAGESIZE);
    SIZE_T kpp = (SIZE_T)(pg / 1024);
    if (!kpp) kpp = 4;
    memset(pi, 0, sizeof *pi);
    pi->cb = sizeof *pi;
    long long v;
    pi->PhysicalTotal = (v = meminfo_kb("MemTotal")) > 0 ? (SIZE_T)v / kpp : 0;
    pi->PhysicalAvailable = (v = meminfo_kb("MemAvailable")) > 0 ? (SIZE_T)v / kpp : 0;
    pi->CommitTotal = (v = meminfo_kb("Committed_AS")) > 0 ? (SIZE_T)v / kpp : 0;
    long long swap = meminfo_kb("SwapTotal");
    /* Windows' commit limit is RAM plus page files. */
    pi->CommitLimit = pi->PhysicalTotal + (swap > 0 ? (SIZE_T)swap / kpp : 0);
    if (pi->CommitTotal > g_commit_peak) g_commit_peak = pi->CommitTotal;
    pi->CommitPeak = g_commit_peak;
    long long cached = meminfo_kb("Cached"), buffers = meminfo_kb("Buffers");
    pi->SystemCache = (SIZE_T)((cached > 0 ? cached : 0) + (buffers > 0 ? buffers : 0)) / kpp;
    long long sre = meminfo_kb("SReclaimable"), sun = meminfo_kb("SUnreclaim"), ks = meminfo_kb("KernelStack"),
              pt = meminfo_kb("PageTables");
    pi->KernelPaged = (SIZE_T)(sre > 0 ? sre : 0) / kpp;
    pi->KernelNonpaged = (SIZE_T)((sun > 0 ? sun : 0) + (ks > 0 ? ks : 0) + (pt > 0 ? pt : 0)) / kpp;
    pi->KernelTotal = pi->KernelPaged + pi->KernelNonpaged;
    pi->PageSize = (SIZE_T)pg;
    char buf[128];
    if (read_text_file("/proc/sys/fs/file-nr", buf, sizeof buf) > 0) pi->HandleCount = (DWORD)strtoul(buf, NULL, 10);
    pi->ProcessCount = count_processes();
    if (read_text_file("/proc/loadavg", buf, sizeof buf) > 0) {
        char *slash = strchr(buf, '/');
        if (slash) pi->ThreadCount = (DWORD)strtoul(slash + 1, NULL, 10);
    }
    return TRUE;
}

/* Linux decides residency itself; trimming always "succeeds". */
API BOOL EmptyWorkingSet(HANDLE h) { return handle_pid(h) >= 0; }

/* Working-set protection codes (MM_* values in the low 5 bits). */
static ULONG_PTR ws_protection(int prot, int priv_writable)
{
    int r = prot & PROT_READ, w = prot & PROT_WRITE, x = prot & PROT_EXEC;
    if (x && w) return priv_writable ? 6 : 7;    /* EXECUTE_READWRITE / EXECUTE_WRITECOPY */
    if (x) return r ? 3 : 2;                     /* EXECUTE_READ / EXECUTE */
    if (w) return priv_writable ? 4 : 5;         /* READWRITE / WRITECOPY */
    return r ? 1 : 0;                            /* READONLY */
}

/* QueryWorkingSet for this process: one entry per resident page, found
 * with mincore() over every mapping. Too small a buffer gives
 * ERROR_BAD_LENGTH with NumberOfEntries set to the count needed. */
API BOOL QueryWorkingSet(HANDLE h, void *pv, DWORD cb)
{
    int pid = handle_pid(h);
    if (pid < 0) return FALSE;
    if (pid != getpid()) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    if (!pv || cb < sizeof(ULONG_PTR)) { lsiw_set_last_error(ERROR_BAD_LENGTH); return FALSE; }
    ULONG_PTR *out = pv, max = (cb / sizeof(ULONG_PTR)) - 1, n = 0;
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) { lsiw_set_errno_error(errno); return FALSE; }
    long pg = sysconf(_SC_PAGESIZE);
    char line[4352];
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e;
        char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perm) < 3 || e <= s || s >= 0x800000000000ULL) continue;
        int prot = (perm[0] == 'r' ? PROT_READ : 0) | (perm[1] == 'w' ? PROT_WRITE : 0) | (perm[2] == 'x' ? PROT_EXEC : 0);
        size_t pages = (e - s) / (unsigned long)pg;
        unsigned char *vec = malloc(pages ? pages : 1);
        if (!vec) continue;
        if (!mincore((void *)s, e - s, vec))
            for (size_t i = 0; i < pages; i++) {
                if (!(vec[i] & 1)) continue;
                if (n < max) out[1 + n] = ((s + i * (unsigned long)pg) & ~0xFFFUL) |
                                          ws_protection(prot, perm[3] == 'p') | (perm[3] == 's' ? 0x100 : 0);
                n++;
            }
        free(vec);
    }
    fclose(f);
    out[0] = n;
    if (n > max) { lsiw_set_last_error(ERROR_BAD_LENGTH); return FALSE; }
    return TRUE;
}

typedef struct { void *VirtualAddress; ULONG_PTR VirtualAttributes; } PSAPI_WORKING_SET_EX_INFORMATION;

/* PSAPI_WORKING_SET_EX_BLOCK: Valid (bit 0), ShareCount (bits 1-3),
 * Win32Protection (bits 4-14, a PAGE_* value), Shared (bit 15). */
static DWORD page_protect_const(int prot)
{
    int w = prot & PROT_WRITE, x = prot & PROT_EXEC, r = prot & PROT_READ;
    if (x) return w ? 0x40 : r ? 0x20 : 0x10;
    if (w) return 0x04;
    return r ? 0x02 : 0x01;
}

API BOOL QueryWorkingSetEx(HANDLE h, void *pv, DWORD cb)
{
    int pid = handle_pid(h);
    if (pid < 0) return FALSE;
    if (pid != getpid()) { lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    if (!pv || cb < sizeof(PSAPI_WORKING_SET_EX_INFORMATION)) { lsiw_set_last_error(ERROR_BAD_LENGTH); return FALSE; }
    PSAPI_WORKING_SET_EX_INFORMATION *e = pv;
    size_t n = cb / sizeof *e;
    long pg = sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < n; i++) {
        uintptr_t a = (uintptr_t)e[i].VirtualAddress & ~(uintptr_t)(pg - 1);
        unsigned char v = 0;
        e[i].VirtualAttributes = 0;
        if (mincore((void *)a, (size_t)pg, &v) || !(v & 1)) continue;
        struct map_info mi;
        mi.file = 0;
        int prot = (!lsiw_query_maps(a, &mi) && mi.mapped) ? mi.prot : PROT_READ;
        e[i].VirtualAttributes = 1 | ((ULONG_PTR)1 << 1) | ((ULONG_PTR)page_protect_const(prot) << 4);
        if (mi.file) e[i].VirtualAttributes |= (ULONG_PTR)1 << 15;
    }
    return TRUE;
}

/* Working-set change tracking needs kernel support Windows provides and
 * Linux does not expose; both calls fail. */
API BOOL InitializeProcessForWsWatch(HANDLE h) { (void)h; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API BOOL GetWsChanges(HANDLE h, void *buf, DWORD cb) { (void)h; (void)buf; (void)cb; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }
API BOOL GetWsChangesEx(HANDLE h, void *buf, DWORD *cb) { (void)h; (void)buf; (void)cb; lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE; }

/* No Windows kernel drivers exist on Linux. */
API BOOL EnumDeviceDrivers(void **drivers, DWORD cb, DWORD *needed)
{
    (void)drivers; (void)cb;
    if (needed) *needed = 0;
    lsiw_set_last_error(ERROR_ACCESS_DENIED);
    return FALSE;
}

API DWORD GetDeviceDriverBaseNameA(void *base, char *buf, DWORD size) { (void)base; if (buf && size) buf[0] = 0; lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
API DWORD GetDeviceDriverBaseNameW(void *base, WCHAR *buf, DWORD size) { (void)base; if (buf && size) buf[0] = 0; lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
API DWORD GetDeviceDriverFileNameA(void *base, char *buf, DWORD size) { (void)base; if (buf && size) buf[0] = 0; lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
API DWORD GetDeviceDriverFileNameW(void *base, WCHAR *buf, DWORD size) { (void)base; if (buf && size) buf[0] = 0; lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }

/* No Windows page files: the enumeration succeeds without callbacks. */
API BOOL EnumPageFilesA(void *cb, void *ctx) { (void)cb; (void)ctx; return TRUE; }
API BOOL EnumPageFilesW(void *cb, void *ctx) { (void)cb; (void)ctx; return TRUE; }

/* psapi names kernel32 lacks: the A twin of GetModuleBaseName and
 * EnumProcessModulesEx (every module of an x64 process matches any filter). */
typedef DWORD (WINAPI *base_name_w_fn)(HANDLE, HMODULE, WCHAR *, DWORD);
typedef BOOL (WINAPI *enum_mods_fn)(HANDLE, HMODULE *, DWORD, DWORD *);

API DWORD GetModuleBaseNameA(HANDLE proc, HMODULE mod, char *buf, DWORD size)
{
    base_name_w_fn f = (base_name_w_fn)k32_api("K32GetModuleBaseNameW");
    WCHAR w[MAX_PATH * 4];
    if (!f || !buf || !size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD n = f(proc, mod, w, (DWORD)(sizeof w / sizeof w[0]));
    if (!n) return 0;
    w[n] = 0;
    char *u = lsiw_w2u(w);
    DWORD l = (DWORD)strlen(u);
    if (l >= size) l = size - 1;
    memcpy(buf, u, l);
    buf[l] = 0;
    free(u);
    return l;
}

API BOOL EnumProcessModulesEx(HANDLE proc, HMODULE *mods, DWORD cb, DWORD *needed, DWORD filter)
{
    if (filter > 3) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    enum_mods_fn f = (enum_mods_fn)k32_api("K32EnumProcessModules");
    if (!f) { lsiw_set_last_error(ERROR_INVALID_FUNCTION); return FALSE; }
    return f(proc, mods, cb, needed);
}

/* ================================================================== */
/* dbghelp / dbgcore                                                    */
/* ================================================================== */

/* Symbols come from export directories only (there is no PDB reader), so
 * every symbol is a public export: SYMFLAG_EXPORT, SymTagPublicSymbol. */
#define SYMOPT_UNDNAME        0x00000002
#define SYMOPT_DEFERRED_LOADS 0x00000004
#define SYMFLAG_EXPORT        0x00000200
#define SymTagPublicSymbol    10
#define UNDNAME_NAME_ONLY     0x1000
#define AddrModeFlat          3

static pthread_mutex_t g_sym_lock = PTHREAD_MUTEX_INITIALIZER;
static DWORD g_sym_options = SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS;
static char *g_sym_path;

typedef struct {
    ULONG SizeOfStruct, TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index, Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value, Address;
    ULONG Register, Scope, Tag, NameLen, MaxNameLen;
    char Name[1];
} SYMBOL_INFO;                                  /* SYMBOL_INFOW: same with WCHAR Name[1] */

typedef struct {
    DWORD SizeOfStruct;
    DWORD64 Address;
    DWORD Size, Flags, MaxNameLength;
    char Name[1];
} IMAGEHLP_SYMBOL64;

_Static_assert(offsetof(SYMBOL_INFO, Name) == 84, "SYMBOL_INFO.Name");
_Static_assert(offsetof(IMAGEHLP_SYMBOL64, Name) == 28, "IMAGEHLP_SYMBOL64.Name");

static void sym_set_path(const char *p)
{
    pthread_mutex_lock(&g_sym_lock);
    free(g_sym_path);
    g_sym_path = p ? strdup(p) : NULL;
    pthread_mutex_unlock(&g_sym_lock);
}

API BOOL SymInitialize(HANDLE proc, const char *path, BOOL invade) { (void)proc; (void)invade; sym_set_path(path); return TRUE; }
API BOOL SymInitializeW(HANDLE proc, const WCHAR *path, BOOL invade)
{
    char *u = lsiw_w2u(path);
    BOOL r = SymInitialize(proc, u, invade);
    free(u);
    return r;
}
API BOOL SymCleanup(HANDLE proc) { (void)proc; return TRUE; }
API DWORD SymSetOptions(DWORD o) { g_sym_options = o; return o; }
API DWORD SymGetOptions(void) { return g_sym_options; }
API BOOL SymSetSearchPath(HANDLE proc, const char *path) { (void)proc; sym_set_path(path); return TRUE; }
API BOOL SymSetSearchPathW(HANDLE proc, const WCHAR *path)
{
    char *u = lsiw_w2u(path);
    sym_set_path(u);
    free(u);
    (void)proc;
    return TRUE;
}

API BOOL SymGetSearchPath(HANDLE proc, char *buf, DWORD size)
{
    (void)proc;
    if (!buf || !size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_sym_lock);
    snprintf(buf, size, "%s", g_sym_path ? g_sym_path : ".");
    pthread_mutex_unlock(&g_sym_lock);
    return TRUE;
}

API BOOL SymGetSearchPathW(HANDLE proc, WCHAR *buf, DWORD size)
{
    (void)proc;
    if (!buf || !size) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_sym_lock);
    WCHAR *w = lsiw_u2w(g_sym_path ? g_sym_path : ".");
    pthread_mutex_unlock(&g_sym_lock);
    size_t l = lsiw_wcslen(w);
    if (l >= size) l = size - 1;
    memcpy(buf, w, l * sizeof(WCHAR));
    buf[l] = 0;
    free(w);
    return TRUE;
}

static const char *base_name_of(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '\\' || *s == '/') b = s + 1;
    return b;
}

/* "Loading" a module: its symbols are its exports, read on demand. The
 * result is the base: the one given, or that of the loaded module named. */
static DWORD64 sym_load(const char *image, const char *modname, DWORD64 base)
{
    if (base) return base;
    const char *n = image ? image : modname;
    struct lsiw_module *m = n ? lsiw_find_module(base_name_of(n)) : NULL;
    if (!m && n) {
        char *with = lsiw_strdup_printf("%s.dll", base_name_of(n));
        m = lsiw_find_module(with);
        free(with);
    }
    if (!m) { lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    return (DWORD64)(uintptr_t)m->base;
}

API DWORD64 SymLoadModuleEx(HANDLE proc, HANDLE file, const char *image, const char *mod, DWORD64 base,
                            DWORD size, void *data, DWORD flags)
{
    (void)proc; (void)file; (void)size; (void)data; (void)flags;
    return sym_load(image, mod, base);
}

API DWORD64 SymLoadModuleExW(HANDLE proc, HANDLE file, const WCHAR *image, const WCHAR *mod, DWORD64 base,
                             DWORD size, void *data, DWORD flags)
{
    (void)proc; (void)file; (void)size; (void)data; (void)flags;
    char *i = lsiw_w2u(image), *m = lsiw_w2u(mod);
    DWORD64 r = sym_load(i, m, base);
    free(i); free(m);
    return r;
}

API DWORD64 SymLoadModule64(HANDLE proc, HANDLE file, const char *image, const char *mod, DWORD64 base, DWORD size)
{
    (void)proc; (void)file; (void)size;
    return sym_load(image, mod, base);
}

API BOOL SymUnloadModule64(HANDLE proc, DWORD64 base) { (void)proc; (void)base; return TRUE; }

API DWORD64 SymGetModuleBase64(HANDLE proc, DWORD64 addr)
{
    (void)proc;
    struct lsiw_module *m = lsiw_module_from_address((const void *)(uintptr_t)addr);
    return m ? (DWORD64)(uintptr_t)m->base : 0;
}

typedef RUNTIME_FUNCTION *(WINAPI *rtl_lookup_fn)(ULONG_PTR, ULONG_PTR *, void *);
typedef void *(WINAPI *rtl_unwind_fn)(DWORD, ULONG_PTR, ULONG_PTR, RUNTIME_FUNCTION *, CONTEXT *,
                                      void **, ULONG_PTR *, void *);

API void *SymFunctionTableAccess64(HANDLE proc, DWORD64 addr)
{
    (void)proc;
    rtl_lookup_fn f = (rtl_lookup_fn)k32_api("RtlLookupFunctionEntry");
    ULONG_PTR base = 0;
    return f ? f((ULONG_PTR)addr, &base, NULL) : NULL;
}

/* The export directory of a loaded image (mapped as an image). */
static const IMAGE_EXPORT_DIRECTORY *image_exports(const uint8_t *base, uint32_t *dir_rva, uint32_t *dir_size)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return NULL;
    const IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d->VirtualAddress || !d->Size) return NULL;
    *dir_rva = d->VirtualAddress;
    *dir_size = d->Size;
    return (const IMAGE_EXPORT_DIRECTORY *)(base + d->VirtualAddress);
}

struct sym_hit { const char *name; uint64_t addr, modbase, size; };

/* Nearest named export at or below `addr`. Forwarders (RVAs inside the
 * export directory) are not code and are skipped. */
static int sym_from_exports(struct lsiw_module *m, uint64_t addr, struct sym_hit *hit)
{
    uint32_t drva, dsize;
    const uint8_t *base = m->base;
    const IMAGE_EXPORT_DIRECTORY *ed = image_exports(base, &drva, &dsize);
    if (!ed) return 0;
    const uint32_t *funcs = (const uint32_t *)(base + ed->AddressOfFunctions);
    const uint32_t *names = (const uint32_t *)(base + ed->AddressOfNames);
    const uint16_t *ords = (const uint16_t *)(base + ed->AddressOfNameOrdinals);
    uint64_t rel = addr - (uint64_t)(uintptr_t)base, best = 0, next = (uint64_t)m->size;
    const char *best_name = NULL;
    for (uint32_t i = 0; i < ed->NumberOfNames; i++) {
        if (ords[i] >= ed->NumberOfFunctions) continue;
        uint32_t rva = funcs[ords[i]];
        if (!rva || (rva >= drva && rva < drva + dsize)) continue;
        if (rva <= rel && (!best_name || rva > best)) { best = rva; best_name = (const char *)(base + names[i]); }
    }
    if (!best_name) return 0;
    for (uint32_t i = 0; i < ed->NumberOfFunctions; i++)
        if (funcs[i] > best && funcs[i] < next && !(funcs[i] >= drva && funcs[i] < drva + dsize)) next = funcs[i];
    hit->name = best_name;
    hit->addr = (uint64_t)(uintptr_t)base + best;
    hit->modbase = (uint64_t)(uintptr_t)base;
    hit->size = next - best;
    return 1;
}

/* An LSIW builtin function (implemented in this binary) by exact address. */
static int sym_from_builtins(uint64_t addr, struct sym_hit *hit)
{
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next) {
        if (!m->is_builtin || !m->builtin) continue;
        for (int t = 0; t < LSIW_MAX_TABLES && m->builtin->tables[t]; t++)
            for (const struct lsiw_export *e = m->builtin->tables[t]; e->name; e++)
                if ((uint64_t)(uintptr_t)e->addr == addr) {
                    hit->name = e->name;
                    hit->addr = addr;
                    hit->modbase = (uint64_t)(uintptr_t)m->base;
                    hit->size = 0;
                    return 1;
                }
    }
    return 0;
}

static int sym_lookup(uint64_t addr, struct sym_hit *hit)
{
    struct lsiw_module *m = lsiw_module_from_address((const void *)(uintptr_t)addr);
    if (m && !m->is_builtin && sym_from_exports(m, addr, hit)) return 1;
    if (sym_from_builtins(addr, hit)) return 1;
    lsiw_set_last_error(m ? ERROR_INVALID_ADDRESS : ERROR_MOD_NOT_FOUND);
    return 0;
}

static void fill_symbol_info(SYMBOL_INFO *si, const struct sym_hit *h)
{
    si->TypeIndex = 0;
    si->Reserved[0] = si->Reserved[1] = 0;
    si->Index = 0;
    si->Size = (ULONG)h->size;
    si->ModBase = h->modbase;
    si->Flags = SYMFLAG_EXPORT;
    si->Value = h->addr;
    si->Address = h->addr;
    si->Register = 0;
    si->Scope = 0;
    si->Tag = SymTagPublicSymbol;
    si->NameLen = (ULONG)strlen(h->name);
}

static void copy_name_a(char *dst, ULONG max, const char *name)
{
    if (!max) return;
    size_t l = strlen(name);
    if (l >= max) l = max - 1;
    memcpy(dst, name, l);
    dst[l] = 0;
}

static void copy_name_w(WCHAR *dst, ULONG max, const char *name)
{
    if (!max) return;
    WCHAR *w = lsiw_u2w(name);
    size_t l = lsiw_wcslen(w);
    if (l >= max) l = max - 1;
    memcpy(dst, w, l * sizeof(WCHAR));
    dst[l] = 0;
    free(w);
}

API BOOL SymFromAddr(HANDLE proc, DWORD64 addr, DWORD64 *disp, SYMBOL_INFO *si)
{
    (void)proc;
    struct sym_hit h;
    if (!si) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!sym_lookup(addr, &h)) return FALSE;
    fill_symbol_info(si, &h);
    copy_name_a(si->Name, si->MaxNameLen, h.name);
    if (disp) *disp = addr - h.addr;
    return TRUE;
}

API BOOL SymFromAddrW(HANDLE proc, DWORD64 addr, DWORD64 *disp, SYMBOL_INFO *si)
{
    (void)proc;
    struct sym_hit h;
    if (!si) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!sym_lookup(addr, &h)) return FALSE;
    fill_symbol_info(si, &h);
    copy_name_w((WCHAR *)(void *)si->Name, si->MaxNameLen, h.name);
    if (disp) *disp = addr - h.addr;
    return TRUE;
}

API BOOL SymGetSymFromAddr64(HANDLE proc, DWORD64 addr, DWORD64 *disp, IMAGEHLP_SYMBOL64 *sym)
{
    (void)proc;
    struct sym_hit h;
    if (!sym) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!sym_lookup(addr, &h)) return FALSE;
    sym->Address = h.addr;
    sym->Size = (DWORD)h.size;
    sym->Flags = SYMFLAG_EXPORT;
    copy_name_a(sym->Name, sym->MaxNameLength, h.name);
    if (disp) *disp = addr - h.addr;
    return TRUE;
}

/* No line-number information without PDBs. */
API BOOL SymGetLineFromAddr64(HANDLE proc, DWORD64 addr, DWORD *disp, void *line)
{
    (void)proc; (void)addr; (void)line;
    if (disp) *disp = 0;
    lsiw_set_last_error(ERROR_INVALID_ADDRESS);
    return FALSE;
}

API BOOL SymGetLineFromAddrW64(HANDLE proc, DWORD64 addr, DWORD *disp, void *line)
{
    return SymGetLineFromAddr64(proc, addr, disp, line);
}

/* Case-insensitive wildcard match ('*' and '?'), as SymEnumSymbols masks. */
static int wild_match(const char *pat, const char *s)
{
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (;; s++) { if (wild_match(pat + 1, s)) return 1; if (!*s) return 0; }
    }
    if (!*s) return 0;
    if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return 0;
    return wild_match(pat + 1, s + 1);
}

typedef BOOL (WINAPI *sym_enum_cb)(SYMBOL_INFO *, ULONG, void *);

static BOOL sym_enum(DWORD64 base, const char *mask, sym_enum_cb cb, void *ctx, int wide)
{
    if (!cb) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    const char *modpat = NULL, *sympat = mask && *mask ? mask : "*";
    char modbuf[260];
    const char *bang = mask ? strchr(mask, '!') : NULL;
    if (bang) {
        snprintf(modbuf, sizeof modbuf, "%.*s", (int)(bang - mask), mask);
        modpat = modbuf;
        sympat = bang[1] ? bang + 1 : "*";
    }
    union { SYMBOL_INFO si; uint8_t raw[sizeof(SYMBOL_INFO) + 2048 * sizeof(WCHAR)]; } u;
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next) {
        if (m->is_builtin) continue;
        if (base && (uintptr_t)m->base != (uintptr_t)base) continue;
        if (!base && modpat) {
            char stem[260];
            snprintf(stem, sizeof stem, "%s", m->name);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = 0;
            if (!wild_match(modpat, stem) && !wild_match(modpat, m->name)) continue;
        }
        uint32_t drva, dsize;
        const IMAGE_EXPORT_DIRECTORY *ed = image_exports(m->base, &drva, &dsize);
        if (!ed) continue;
        const uint32_t *funcs = (const uint32_t *)(m->base + ed->AddressOfFunctions);
        const uint32_t *names = (const uint32_t *)(m->base + ed->AddressOfNames);
        const uint16_t *ords = (const uint16_t *)(m->base + ed->AddressOfNameOrdinals);
        for (uint32_t i = 0; i < ed->NumberOfNames; i++) {
            if (ords[i] >= ed->NumberOfFunctions) continue;
            uint32_t rva = funcs[ords[i]];
            if (!rva || (rva >= drva && rva < drva + dsize)) continue;
            const char *name = (const char *)(m->base + names[i]);
            if (!wild_match(sympat, name)) continue;
            struct sym_hit h = { name, (uint64_t)(uintptr_t)m->base + rva, (uint64_t)(uintptr_t)m->base, 0 };
            memset(&u.si, 0, sizeof u.si);
            u.si.SizeOfStruct = sizeof(SYMBOL_INFO);
            u.si.MaxNameLen = 2048;
            fill_symbol_info(&u.si, &h);
            if (wide) copy_name_w((WCHAR *)(void *)u.si.Name, 2048, name);
            else copy_name_a(u.si.Name, 2048, name);
            if (!cb(&u.si, u.si.Size, ctx)) return TRUE;
        }
    }
    return TRUE;
}

API BOOL SymEnumSymbols(HANDLE proc, ULONG64 base, const char *mask, sym_enum_cb cb, void *ctx)
{
    (void)proc;
    return sym_enum(base, mask, cb, ctx, 0);
}

API BOOL SymEnumSymbolsW(HANDLE proc, ULONG64 base, const WCHAR *mask, sym_enum_cb cb, void *ctx)
{
    (void)proc;
    char *m = lsiw_w2u(mask);
    BOOL r = sym_enum(base, m, cb, ctx, 1);
    free(m);
    return r;
}

/* No symbol-loading events are ever generated, so the callback is unused. */
API BOOL SymRegisterCallback64(HANDLE proc, void *cb, ULONG64 ctx) { (void)proc; (void)cb; (void)ctx; return TRUE; }
API BOOL SymRegisterCallbackW64(HANDLE proc, void *cb, ULONG64 ctx) { (void)proc; (void)cb; (void)ctx; return TRUE; }

/* ---- StackWalk64 (AMD64) ------------------------------------------ */

typedef struct { DWORD64 Offset; WORD Segment; int Mode; } ADDRESS64;

typedef struct {
    ADDRESS64 AddrPC, AddrReturn, AddrFrame, AddrStack, AddrBStore;
    void *FuncTableEntry;
    DWORD64 Params[4];
    BOOL Far, Virtual;
    DWORD64 Reserved[3];
    uint8_t KdHelp[112];                        /* KDHELP64 */
} STACKFRAME64;

typedef struct {
    STACKFRAME64 f;
    DWORD StackFrameSize, InlineFrameContext;   /* STACKFRAME_EX tail */
} STACKFRAME_EX;

_Static_assert(sizeof(STACKFRAME64) == 264, "STACKFRAME64 size");
_Static_assert(offsetof(STACKFRAME64, Params) == 88, "STACKFRAME64.Params");

typedef BOOL (WINAPI *read_mem_fn)(HANDLE, DWORD64, void *, DWORD, DWORD *);

static int walk_read(HANDLE proc, read_mem_fn rd, uint64_t addr, void *out, DWORD n)
{
    if (rd) { DWORD got = 0; return rd(proc, addr, out, n, &got) && got == n; }
    return safe_read(out, (const void *)(uintptr_t)addr, n);
}

/* One frame up: RtlVirtualUnwind where unwind data exists, else a leaf
 * function (return address on top of the stack). */
static int walk_unwind(HANDLE proc, read_mem_fn rd, CONTEXT *c)
{
    rtl_lookup_fn lookup = (rtl_lookup_fn)k32_api("RtlLookupFunctionEntry");
    rtl_unwind_fn unwind = (rtl_unwind_fn)k32_api("RtlVirtualUnwind");
    ULONG_PTR base = 0, est = 0;
    void *hd = NULL;
    uint64_t old_rsp = c->Rsp;
    RUNTIME_FUNCTION *fn = lookup ? lookup((ULONG_PTR)c->Rip, &base, NULL) : NULL;
    if (fn && unwind) {   /* only this process is walked */
        unwind(0 /* UNW_FLAG_NHANDLER */, base, (ULONG_PTR)c->Rip, fn, c, &hd, &est, NULL);
    } else {
        uint64_t ret;
        if (!walk_read(proc, rd, c->Rsp, &ret, 8)) return 0;
        c->Rip = ret;
        c->Rsp += 8;
    }
    return c->Rsp > old_rsp;            /* the stack only grows toward callers */
}

/* The caller fills the CONTEXT (e.g. RtlCaptureContext); the first call
 * reports that frame, each later call unwinds one frame. FALSE when the
 * walk reaches RIP == 0 or cannot make progress. Unwinding through LSIW's
 * own (host) frames is not possible; such walks end there. */
static BOOL stack_walk(DWORD machine, HANDLE proc, STACKFRAME64 *sf, void *ctxp, read_mem_fn rd)
{
    if (machine != IMAGE_FILE_MACHINE_AMD64 || !sf || !ctxp) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    CONTEXT *c = ctxp;
    if (sf->Reserved[2]) {
        if (!walk_unwind(proc, rd, c)) return FALSE;
    }
    if (!c->Rip) return FALSE;
    sf->Reserved[2]++;
    sf->AddrPC.Offset = c->Rip;
    sf->AddrStack.Offset = c->Rsp;
    sf->AddrFrame.Offset = c->Rbp;
    sf->AddrPC.Mode = sf->AddrStack.Mode = sf->AddrFrame.Mode = sf->AddrReturn.Mode = AddrModeFlat;
    sf->AddrPC.Segment = sf->AddrStack.Segment = sf->AddrFrame.Segment = sf->AddrReturn.Segment = 0;
    sf->FuncTableEntry = SymFunctionTableAccess64(proc, c->Rip);
    CONTEXT peek;
    memcpy(&peek, c, sizeof peek);
    sf->AddrReturn.Offset = walk_unwind(proc, rd, &peek) ? peek.Rip : 0;
    for (int i = 0; i < 4; i++)
        if (!walk_read(proc, rd, peek.Rsp + 8 * (uint64_t)i, &sf->Params[i], 8)) sf->Params[i] = 0;
    sf->Far = FALSE;
    sf->Virtual = TRUE;
    return TRUE;
}

API BOOL StackWalk64(DWORD machine, HANDLE proc, HANDLE thread, STACKFRAME64 *sf, void *ctx,
                     read_mem_fn rd, void *fta, void *gmb, void *translate)
{
    (void)thread; (void)fta; (void)gmb; (void)translate;
    return stack_walk(machine, proc, sf, ctx, rd);
}

API BOOL StackWalkEx(DWORD machine, HANDLE proc, HANDLE thread, STACKFRAME_EX *sf, void *ctx,
                     read_mem_fn rd, void *fta, void *gmb, void *translate, DWORD flags)
{
    (void)thread; (void)fta; (void)gmb; (void)translate; (void)flags;
    if (!sf) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    BOOL r = stack_walk(machine, proc, &sf->f, ctx, rd);
    if (r) { sf->StackFrameSize = sizeof *sf; sf->InlineFrameContext = 0; }
    return r;
}

/* ---- UnDecorateSymbolName ----------------------------------------- */

/* With UNDNAME_NAME_ONLY, simple MSVC names "?name@Scope1@Scope2@@..." give
 * "Scope2::Scope1::name". Anything else (full signatures, operators,
 * templates) is not demangled and is returned as is. */
static char *undecorate(const char *in, DWORD flags)
{
    if (in[0] != '?' || !(flags & UNDNAME_NAME_ONLY) || in[1] == '?' || in[1] == '$') return strdup(in);
    const char *end = strstr(in, "@@");
    if (!end) return strdup(in);
    char *parts[32];
    int n = 0;
    for (const char *p = in + 1; p < end && n < 32; ) {
        const char *at = memchr(p, '@', (size_t)(end - p));
        if (!at) at = end;
        if (at == p || *p == '?' || isdigit((unsigned char)*p)) { for (int i = 0; i < n; i++) free(parts[i]); return strdup(in); }
        parts[n++] = strndup(p, (size_t)(at - p));
        p = at + 1;
    }
    size_t len = 1;
    for (int i = 0; i < n; i++) len += strlen(parts[i]) + 2;
    char *out = malloc(len);
    out[0] = 0;
    for (int i = n - 1; i >= 0; i--) {
        strcat(out, parts[i]);
        if (i) strcat(out, "::");
        free(parts[i]);
    }
    return out;
}

API DWORD UnDecorateSymbolName(const char *name, char *out, DWORD max, DWORD flags)
{
    if (!name || !out || !max) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    char *u = undecorate(name, flags);
    size_t l = strlen(u);
    if (l >= max) l = max - 1;
    memcpy(out, u, l);
    out[l] = 0;
    free(u);
    return (DWORD)l;
}

API DWORD UnDecorateSymbolNameW(const WCHAR *name, WCHAR *out, DWORD max, DWORD flags)
{
    if (!name || !out || !max) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    char *n = lsiw_w2u(name), *u = undecorate(n, flags);
    WCHAR *w = lsiw_u2w(u);
    size_t l = lsiw_wcslen(w);
    if (l >= max) l = max - 1;
    memcpy(out, w, l * sizeof(WCHAR));
    out[l] = 0;
    free(n); free(u); free(w);
    return (DWORD)l;
}

/* Minidumps are not written; failure is reported as an HRESULT in the last
 * error, as MiniDumpWriteDump does. */
API BOOL MiniDumpWriteDump(HANDLE proc, DWORD pid, HANDLE file, int type, void *exc, void *user, void *cb)
{
    (void)proc; (void)pid; (void)file; (void)type; (void)exc; (void)user; (void)cb;
    lsiw_set_last_error(0x80070000u | ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* ---- image helpers ------------------------------------------------- */

API void *ImageNtHeader(void *base)
{
    const uint8_t *b = base;
    if (!b || rd16(b) != IMAGE_DOS_SIGNATURE) return NULL;
    int32_t lfanew = (int32_t)rd32(b + 0x3c);
    if (lfanew <= 0 || lfanew > 0x10000000 || rd32(b + lfanew) != IMAGE_NT_SIGNATURE) return NULL;
    return (void *)(b + lfanew);
}

/* Directory `entry` of an image that is either mapped as an image (RVA =
 * offset) or a raw file image (RVA translated through the sections, whose
 * header is returned in *section). PE32 and PE32+ headers are handled. */
API void *ImageDirectoryEntryToDataEx(void *base, BOOLEAN mapped, USHORT entry, ULONG *size, IMAGE_SECTION_HEADER **section)
{
    if (section) *section = NULL;
    if (size) *size = 0;
    uint8_t *nt = ImageNtHeader(base);
    if (!nt) return NULL;
    uint8_t *opt = nt + 24;
    unsigned optsize = rd16(nt + 20), nsec = rd16(nt + 6);
    unsigned nrva_off, dd_off;
    switch (rd16(opt)) {
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC: nrva_off = 108; dd_off = 112; break;
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC: nrva_off = 92;  dd_off = 96;  break;
    default: return NULL;
    }
    if (entry >= rd32(opt + nrva_off) || dd_off + 8u * entry + 8 > optsize) return NULL;
    uint32_t rva = rd32(opt + dd_off + 8u * entry), len = rd32(opt + dd_off + 8u * entry + 4);
    if (!rva) return NULL;
    if (size) *size = len;
    if (mapped) return (uint8_t *)base + rva;
    IMAGE_SECTION_HEADER *s = (IMAGE_SECTION_HEADER *)(opt + optsize);
    for (unsigned i = 0; i < nsec; i++) {
        uint32_t span = s[i].VirtualSize > s[i].SizeOfRawData ? s[i].VirtualSize : s[i].SizeOfRawData;
        if (rva >= s[i].VirtualAddress && rva < s[i].VirtualAddress + span) {
            if (section) *section = &s[i];
            return (uint8_t *)base + s[i].PointerToRawData + (rva - s[i].VirtualAddress);
        }
    }
    return NULL;
}

API void *ImageDirectoryEntryToData(void *base, BOOLEAN mapped, USHORT entry, ULONG *size)
{
    return ImageDirectoryEntryToDataEx(base, mapped, entry, size, NULL);
}

typedef BOOL (WINAPI *enum_mod_cb_a)(const char *, DWORD64, ULONG, void *);
typedef BOOL (WINAPI *enum_mod_cb_w)(const WCHAR *, DWORD64, ULONG, void *);

/* Every module LSIW has loaded, builtin DLLs included, with full paths. */
API BOOL EnumerateLoadedModules64(HANDLE proc, enum_mod_cb_a cb, void *ctx)
{
    if (!cb || handle_pid(proc) != getpid()) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next)
        if (!cb(m->win_path ? m->win_path : m->name, (DWORD64)(uintptr_t)m->base, (ULONG)m->size, ctx)) break;
    return TRUE;
}

API BOOL EnumerateLoadedModulesW64(HANDLE proc, enum_mod_cb_w cb, void *ctx)
{
    if (!cb || handle_pid(proc) != getpid()) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (struct lsiw_module *m = lsiw_modules; m; m = m->next) {
        WCHAR *w = lsiw_u2w(m->win_path ? m->win_path : m->name);
        BOOL go = cb(w, (DWORD64)(uintptr_t)m->base, (ULONG)m->size, ctx);
        free(w);
        if (!go) break;
    }
    return TRUE;
}

/* Creates every directory of a path up to its last backslash. */
API BOOL MakeSureDirectoryPathExists(const char *path)
{
    if (!path) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    const char *last = strrchr(path, '\\'), *slash = strrchr(path, '/');
    if (slash > last) last = slash;
    if (!last) return TRUE;
    char *dir = strndup(path, (size_t)(last - path));
    if (strlen(dir) == 2 && dir[1] == ':') { free(dir); return TRUE; }
    char *lp = lsiw_path_a(dir);
    free(dir);
    int r = lp ? lsiw_mkdirs(lp, 0777) : -1;
    if (r) lsiw_set_errno_error(errno ? errno : ENOENT);
    free(lp);
    return r == 0;
}

/* Depth-first search for a file name (case-insensitive) under root. */
static int tree_search(const char *dir, const char *name, char *found, size_t cap, int depth)
{
    if (depth > 32) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int ok = 0;
    while (!ok && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *p = lsiw_strdup_printf("%s/%s", dir, e->d_name);
        struct stat st;
        if (!lstat(p, &st)) {
            if (S_ISREG(st.st_mode) && !strcasecmp(e->d_name, name)) { snprintf(found, cap, "%s", p); ok = 1; }
            else if (S_ISDIR(st.st_mode)) ok = tree_search(p, name, found, cap, depth + 1);
        }
        free(p);
    }
    closedir(d);
    return ok;
}

API BOOL SearchTreeForFile(const char *root, const char *input, char *output)
{
    if (!root || !input || !output) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char *lr = lsiw_path_a(root);
    char found[4096];
    int ok = lr && tree_search(lr, base_name_of(input), found, sizeof found, 0);
    free(lr);
    if (!ok) { lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    char *w = lsiw_linux_to_win(found);
    const char *r = w ? w : found;
    size_t l = strlen(r);
    if (l >= MAX_PATH) l = MAX_PATH - 1;      /* the output buffer is MAX_PATH */
    memcpy(output, r, l);
    output[l] = 0;
    free(w);
    return TRUE;
}

/* ================================================================== */
/* powrprof                                                             */
/* ================================================================== */

#define STATUS_SUCCESS          ((NTSTATUS)0)
#define STATUS_NOT_IMPLEMENTED  ((NTSTATUS)0xC0000002)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)

enum { SystemPowerCapabilities = 4, SystemBatteryState = 5, ProcessorInformation = 11 };

typedef struct { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; } PROCESSOR_POWER_INFORMATION;

typedef struct {
    BOOLEAN AcOnLine, BatteryPresent, Charging, Discharging, Spare1[3];
    BYTE Tag;
    ULONG MaxCapacity, RemainingCapacity;
    LONG Rate;
    ULONG EstimatedTime, DefaultAlert1, DefaultAlert2;
} SYSTEM_BATTERY_STATE;

typedef struct { DWORD Granularity, Capacity; } BATTERY_REPORTING_SCALE;

typedef struct {
    BOOLEAN PowerButtonPresent, SleepButtonPresent, LidPresent, SystemS1, SystemS2, SystemS3, SystemS4, SystemS5;
    BOOLEAN HiberFilePresent, FullWake, VideoDimPresent, ApmPresent, UpsPresent, ThermalControl, ProcessorThrottle;
    BYTE ProcessorMinThrottle, ProcessorMaxThrottle;
    BOOLEAN FastSystemS4, Hiberboot, WakeAlarmPresent, AoAc, DiskSpinDown;
    BYTE HiberFileType;
    BOOLEAN AoAcConnectivitySupported;
    BYTE spare3[6];
    BOOLEAN SystemBatteriesPresent, BatteriesAreShortTerm;
    BATTERY_REPORTING_SCALE BatteryScale[3];
    int AcOnLineWake, SoftLidWake, RtcWake, MinDeviceWakeState, DefaultLowLatencyWake;
} SYSTEM_POWER_CAPABILITIES;

_Static_assert(sizeof(SYSTEM_BATTERY_STATE) == 32, "SYSTEM_BATTERY_STATE size");
_Static_assert(sizeof(SYSTEM_POWER_CAPABILITIES) == 76, "SYSTEM_POWER_CAPABILITIES size");

static long cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

/* MHz of CPU `i` from cpufreq (kHz files), falling back to /proc/cpuinfo. */
static ULONG cpufreq_mhz(long i, const char *file)
{
    char p[128];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%ld/cpufreq/%s", i, file);
    long long khz = read_ll_file(p, -1);
    return khz > 0 ? (ULONG)(khz / 1000) : 0;
}

static ULONG cpuinfo_mhz(long index)
{
    FILE *f = fopen("/proc/cpuinfo", "re");
    if (!f) return 0;
    char line[256];
    long n = -1;
    ULONG mhz = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "processor", 9)) n++;
        else if (n == index && !strncmp(line, "cpu MHz", 7)) {
            char *c = strchr(line, ':');
            if (c) mhz = (ULONG)strtod(c + 1, NULL);
            break;
        }
    }
    fclose(f);
    return mhz;
}

static void battery_state(SYSTEM_BATTERY_STATE *b)
{
    memset(b, 0, sizeof *b);
    b->EstimatedTime = 0xFFFFFFFFu;                    /* unknown */
    DIR *d = opendir("/sys/class/power_supply");
    long long full = 0, now = 0, rate = 0;
    int mains_seen = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[512], t[64];
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
            if (read_text_file(p, t, sizeof t) <= 0) continue;
            if (!strcmp(t, "Mains")) {
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
                mains_seen = 1;
                if (read_ll_file(p, 0) > 0) b->AcOnLine = TRUE;
            } else if (!strcmp(t, "Battery")) {
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/scope", e->d_name);
                if (read_text_file(p, t, sizeof t) > 0 && !strcmp(t, "Device")) continue;  /* mouse, pad... */
                b->BatteryPresent = TRUE;
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
                if (read_text_file(p, t, sizeof t) > 0) {
                    if (!strcmp(t, "Charging")) b->Charging = TRUE;
                    else if (!strcmp(t, "Discharging")) b->Discharging = TRUE;
                }
                /* energy_* in uWh and power_now in uW; charge_* in uAh needs voltage. */
                long long ef, en, pw, volt;
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/energy_full", e->d_name);
                ef = read_ll_file(p, -1);
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/energy_now", e->d_name);
                en = read_ll_file(p, -1);
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/power_now", e->d_name);
                pw = read_ll_file(p, -1);
                snprintf(p, sizeof p, "/sys/class/power_supply/%s/voltage_now", e->d_name);
                volt = read_ll_file(p, -1);
                if (ef < 0 && volt > 0) {
                    snprintf(p, sizeof p, "/sys/class/power_supply/%s/charge_full", e->d_name);
                    long long cf = read_ll_file(p, -1);
                    snprintf(p, sizeof p, "/sys/class/power_supply/%s/charge_now", e->d_name);
                    long long cn = read_ll_file(p, -1);
                    snprintf(p, sizeof p, "/sys/class/power_supply/%s/current_now", e->d_name);
                    long long cur = read_ll_file(p, -1);
                    if (cf > 0) ef = cf * volt / 1000000;
                    if (cn >= 0) en = cn * volt / 1000000;
                    if (cur >= 0 && pw < 0) pw = cur * volt / 1000000;
                }
                if (ef > 0) full += ef / 1000;
                if (en >= 0) now += en / 1000;
                if (pw > 0) rate += pw / 1000;
            }
        }
        closedir(d);
    }
    if (!mains_seen && !b->BatteryPresent) b->AcOnLine = TRUE;    /* desktop machine */
    b->MaxCapacity = (ULONG)full;
    b->RemainingCapacity = (ULONG)now;
    if (b->Discharging) {
        b->Rate = -(LONG)rate;
        if (rate > 0) b->EstimatedTime = (ULONG)(now * 3600 / rate);
    } else {
        b->Rate = (LONG)rate;
    }
    b->DefaultAlert1 = (ULONG)(full / 10);    /* low: 10% */
    b->DefaultAlert2 = (ULONG)(full * 3 / 100);/* critical: 3% */
}

static void power_capabilities(SYSTEM_POWER_CAPABILITIES *c)
{
    memset(c, 0, sizeof *c);
    char st[256] = "";
    read_text_file("/sys/power/state", st, sizeof st);
    c->PowerButtonPresent = TRUE;
    c->LidPresent = !access("/proc/acpi/button/lid", F_OK);
    c->SystemS3 = strstr(st, "mem") != NULL;
    c->SystemS4 = strstr(st, "disk") != NULL;
    c->SystemS5 = TRUE;
    c->HiberFilePresent = c->SystemS4;
    c->ProcessorThrottle = !access("/sys/devices/system/cpu/cpu0/cpufreq", F_OK);
    c->ProcessorMinThrottle = c->ProcessorThrottle ? 0 : 100;
    c->ProcessorMaxThrottle = 100;
    c->DiskSpinDown = TRUE;
    SYSTEM_BATTERY_STATE b;
    battery_state(&b);
    c->SystemBatteriesPresent = b.BatteryPresent;
}

API NTSTATUS CallNtPowerInformation(int level, void *in, ULONG inlen, void *out, ULONG outlen)
{
    (void)in; (void)inlen;
    switch (level) {
    case ProcessorInformation: {
        long n = cpu_count();
        if (!out || outlen < n * sizeof(PROCESSOR_POWER_INFORMATION)) return STATUS_BUFFER_TOO_SMALL;
        PROCESSOR_POWER_INFORMATION *p = out;
        for (long i = 0; i < n; i++) {
            p[i].Number = (ULONG)i;
            p[i].MaxMhz = cpufreq_mhz(i, "cpuinfo_max_freq");
            p[i].CurrentMhz = cpufreq_mhz(i, "scaling_cur_freq");
            p[i].MhzLimit = cpufreq_mhz(i, "scaling_max_freq");
            if (!p[i].CurrentMhz) p[i].CurrentMhz = cpuinfo_mhz(i);
            if (!p[i].MaxMhz) p[i].MaxMhz = p[i].CurrentMhz;
            if (!p[i].MhzLimit) p[i].MhzLimit = p[i].MaxMhz;
            p[i].MaxIdleState = p[i].CurrentIdleState = 0;
        }
        return STATUS_SUCCESS;
    }
    case SystemBatteryState:
        if (!out || outlen < sizeof(SYSTEM_BATTERY_STATE)) return STATUS_BUFFER_TOO_SMALL;
        battery_state(out);
        return STATUS_SUCCESS;
    case SystemPowerCapabilities:
        if (!out || outlen < sizeof(SYSTEM_POWER_CAPABILITIES)) return STATUS_BUFFER_TOO_SMALL;
        power_capabilities(out);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_IMPLEMENTED;
}

API BOOLEAN GetPwrCapabilities(SYSTEM_POWER_CAPABILITIES *c)
{
    if (!c) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    power_capabilities(c);
    return TRUE;
}

API BOOLEAN IsPwrSuspendAllowed(void)   { SYSTEM_POWER_CAPABILITIES c; power_capabilities(&c); return c.SystemS3; }
API BOOLEAN IsPwrHibernateAllowed(void) { SYSTEM_POWER_CAPABILITIES c; power_capabilities(&c); return c.SystemS4; }
API BOOLEAN IsPwrShutdownAllowed(void)  { return TRUE; }

typedef struct { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID;

/* The Balanced scheme, the only one there is. The GUID is LocalAlloc'd. */
API DWORD PowerGetActiveScheme(HANDLE root, GUID **scheme)
{
    static const GUID balanced = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
    (void)root;
    if (!scheme) return ERROR_INVALID_PARAMETER;
    *scheme = lsiw_heap_alloc(sizeof(GUID), 0);
    if (!*scheme) return ERROR_NOT_ENOUGH_MEMORY;
    **scheme = balanced;
    return ERROR_SUCCESS;
}

/* No power-setting store exists: reads find nothing, writes are refused. */
API DWORD PowerReadACValueIndex(HANDLE r, const GUID *s, const GUID *g, const GUID *k, DWORD *v)
{ (void)r; (void)s; (void)g; (void)k; (void)v; return ERROR_FILE_NOT_FOUND; }
API DWORD PowerReadDCValueIndex(HANDLE r, const GUID *s, const GUID *g, const GUID *k, DWORD *v)
{ (void)r; (void)s; (void)g; (void)k; (void)v; return ERROR_FILE_NOT_FOUND; }
API DWORD PowerReadACValue(HANDLE r, const GUID *s, const GUID *g, const GUID *k, DWORD *t, BYTE *b, DWORD *n)
{ (void)r; (void)s; (void)g; (void)k; (void)t; (void)b; (void)n; return ERROR_FILE_NOT_FOUND; }
API DWORD PowerReadDCValue(HANDLE r, const GUID *s, const GUID *g, const GUID *k, DWORD *t, BYTE *b, DWORD *n)
{ (void)r; (void)s; (void)g; (void)k; (void)t; (void)b; (void)n; return ERROR_FILE_NOT_FOUND; }
API DWORD PowerWriteACValueIndex(HANDLE r, const GUID *s, const GUID *g, const GUID *k, DWORD v)
{ (void)r; (void)s; (void)g; (void)k; (void)v; return ERROR_ACCESS_DENIED; }
API DWORD PowerWriteDCValueIndex(HANDLE r, const GUID *s, const GUID *g, const GUID *k, DWORD v)
{ (void)r; (void)s; (void)g; (void)k; (void)v; return ERROR_ACCESS_DENIED; }
API DWORD PowerSetActiveScheme(HANDLE r, const GUID *s) { (void)r; (void)s; return ERROR_ACCESS_DENIED; }

/* Registrations succeed with a handle, but no notification is delivered:
 * Linux suspend/resume and power-setting changes are not relayed. */
static int g_power_notify_token;

API DWORD PowerRegisterSuspendResumeNotification(DWORD flags, HANDLE recipient, HANDLE *reg)
{
    (void)flags;
    if (!recipient || !reg) return ERROR_INVALID_PARAMETER;
    *reg = &g_power_notify_token;
    return ERROR_SUCCESS;
}

API DWORD PowerUnregisterSuspendResumeNotification(HANDLE reg) { return reg ? ERROR_SUCCESS : ERROR_INVALID_HANDLE; }

API DWORD PowerSettingRegisterNotification(const GUID *setting, DWORD flags, HANDLE recipient, HANDLE *reg)
{
    (void)flags;
    if (!setting || !recipient || !reg) return ERROR_INVALID_PARAMETER;
    *reg = &g_power_notify_token;
    return ERROR_SUCCESS;
}

API DWORD PowerSettingUnregisterNotification(HANDLE reg) { return reg ? ERROR_SUCCESS : ERROR_INVALID_HANDLE; }

/* ================================================================== */
/* netapi32                                                             */
/* ================================================================== */

#define NERR_Success        0
#define NERR_GroupNotFound  2220
#define NERR_UserNotFound   2221
#define NERR_DCNotFound     2453
#define PLATFORM_ID_NT      500

API DWORD NetApiBufferAllocate(DWORD cb, void **buf)
{
    if (!buf) return ERROR_INVALID_PARAMETER;
    *buf = lsiw_heap_alloc(cb ? cb : 1, 0);
    return *buf ? NERR_Success : ERROR_NOT_ENOUGH_MEMORY;
}

API DWORD NetApiBufferFree(void *buf)
{
    if (buf) lsiw_heap_free(buf);
    return NERR_Success;
}

API DWORD NetApiBufferReallocate(void *old, DWORD cb, void **buf)
{
    if (!buf) return ERROR_INVALID_PARAMETER;
    void *p = lsiw_heap_realloc(old, cb ? cb : 1);
    if (!p) return ERROR_NOT_ENOUGH_MEMORY;
    *buf = p;
    return NERR_Success;
}

API DWORD NetApiBufferSize(void *buf, DWORD *cb)
{
    if (!buf || !cb) return ERROR_INVALID_PARAMETER;
    *cb = (DWORD)lsiw_heap_size(buf);
    return NERR_Success;
}

/* NULL, "", "NAME" or "\\NAME" naming this machine. */
static int is_local_server(const WCHAR *server)
{
    if (!server || !*server) return 1;
    char *s = lsiw_w2u(server), *p = s;
    while (*p == '\\') p++;
    int r = !strcasecmp(p, computer_name()) || !strcasecmp(p, "localhost") || !strcmp(p, ".");
    free(s);
    return r;
}

/* One NetApiBufferFree-able block: a fixed structure followed by the
 * strings its pointers refer to. */
struct netbuf { uint8_t *base; size_t used, cap; };

static int nb_init(struct netbuf *b, size_t fixed, size_t strings)
{
    b->cap = fixed + strings;
    b->used = fixed;
    b->base = lsiw_heap_alloc(b->cap, 1);
    return b->base != NULL;
}

static WCHAR *nb_wstr(struct netbuf *b, const char *s)
{
    int n = lsiw_utf8_to_utf16(s, -1, NULL, 0);
    size_t bytes = (size_t)n * sizeof(WCHAR);
    if (b->used + bytes > b->cap) return NULL;
    WCHAR *w = (WCHAR *)(b->base + b->used);
    lsiw_utf8_to_utf16(s, -1, w, n);
    b->used += (bytes + 7) & ~(size_t)7;
    return w;
}

static size_t wbytes(const char *s) { return (((size_t)lsiw_utf8_to_utf16(s, -1, NULL, 0) * 2) + 7) & ~(size_t)7; }

typedef struct { DWORD platform_id; WCHAR *computername, *langroup; DWORD ver_major, ver_minor; WCHAR *lanroot; DWORD logged_on_users; } WKSTA_INFO_102;

API DWORD NetGetJoinInformation(const WCHAR *server, WCHAR **name, int *status)
{
    if (!is_local_server(server)) return ERROR_BAD_NETPATH;
    if (!name || !status) return ERROR_INVALID_PARAMETER;
    *name = heap_wstr("WORKGROUP");
    if (!*name) return ERROR_NOT_ENOUGH_MEMORY;
    *status = 2;                                  /* NetSetupWorkgroupName */
    return NERR_Success;
}

API DWORD NetWkstaGetInfo(const WCHAR *server, DWORD level, BYTE **out)
{
    if (!is_local_server(server)) return ERROR_BAD_NETPATH;
    if (!out) return ERROR_INVALID_PARAMETER;
    size_t fixed;
    switch (level) {
    case 100: fixed = 32; break;
    case 101: fixed = 40; break;
    case 102: fixed = sizeof(WKSTA_INFO_102); break;
    default: return ERROR_INVALID_LEVEL;
    }
    const char *lanroot = var_or("WINDIR", "C:\\windows");
    struct netbuf b;
    if (!nb_init(&b, fixed, wbytes(computer_name()) + wbytes("WORKGROUP") + wbytes(lanroot)))
        return ERROR_NOT_ENOUGH_MEMORY;
    WKSTA_INFO_102 *w = (WKSTA_INFO_102 *)b.base;   /* 100 and 101 are prefixes of 102 */
    w->platform_id = PLATFORM_ID_NT;
    w->computername = nb_wstr(&b, computer_name());
    w->langroup = nb_wstr(&b, "WORKGROUP");
    w->ver_major = 10;
    w->ver_minor = 0;
    if (level >= 101) w->lanroot = nb_wstr(&b, lanroot);
    if (level == 102) w->logged_on_users = 1;
    *out = b.base;
    return NERR_Success;
}

typedef struct { DWORD platform_id; WCHAR *name; DWORD version_major, version_minor, type; WCHAR *comment; } SERVER_INFO_101;

API DWORD NetServerGetInfo(const WCHAR *server, DWORD level, BYTE **out)
{
    if (!is_local_server(server)) return ERROR_BAD_NETPATH;
    if (!out) return ERROR_INVALID_PARAMETER;
    if (level != 100 && level != 101) return ERROR_INVALID_LEVEL;
    struct netbuf b;
    if (!nb_init(&b, sizeof(SERVER_INFO_101), wbytes(computer_name()) + wbytes(""))) return ERROR_NOT_ENOUGH_MEMORY;
    SERVER_INFO_101 *s = (SERVER_INFO_101 *)b.base;
    s->platform_id = PLATFORM_ID_NT;
    s->name = nb_wstr(&b, computer_name());
    if (level == 101) {
        s->version_major = 10;
        s->version_minor = 0;
        s->type = 0x00000001 | 0x00000002 | 0x00001000;   /* WORKSTATION | SERVER | NT */
        s->comment = nb_wstr(&b, "");
    }
    *out = b.base;
    return NERR_Success;
}

/* The user's SID (the USERSID variable) as a binary SID. */
static size_t parse_sid(const char *s, uint8_t *out, size_t cap)
{
    if (strncasecmp(s, "S-1-", 4)) return 0;
    char *end;
    uint64_t auth = strtoull(s + 4, &end, 10);
    uint32_t sub[15];
    int n = 0;
    while (*end == '-' && n < 15) sub[n++] = (uint32_t)strtoul(end + 1, &end, 10);
    size_t len = 8 + 4 * (size_t)n;
    if (*end || len > cap) return 0;
    out[0] = 1;
    out[1] = (uint8_t)n;
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(auth >> (8 * (5 - i)));
    memcpy(out + 8, sub, 4 * (size_t)n);
    return len;
}

typedef struct { WCHAR *name, *password; DWORD password_age, priv; WCHAR *home_dir, *comment; DWORD flags; WCHAR *script_path; } USER_INFO_1;
typedef struct { WCHAR *name, *comment, *usr_comment, *full_name; } USER_INFO_10;
typedef struct { WCHAR *name, *full_name, *comment; DWORD flags, user_id; } USER_INFO_20;
typedef struct { WCHAR *name, *full_name, *comment; DWORD flags; void *user_sid; } USER_INFO_23;

#define UF_SCRIPT              0x0001
#define UF_NORMAL_ACCOUNT      0x0200
#define UF_DONT_EXPIRE_PASSWD  0x10000

/* USER_INFO_<level> for the current user, in one heap block. */
static DWORD build_user_info(DWORD level, BYTE **out)
{
    char *full = display_name();
    const char *name = user_name();
    uint8_t sid[68];
    size_t sidlen = parse_sid(var_or("USERSID", "S-1-5-21-0-0-0-1000"), sid, sizeof sid);
    if (!sidlen) sidlen = parse_sid("S-1-5-21-0-0-0-1000", sid, sizeof sid);
    size_t fixed;
    switch (level) {
    case 0: fixed = sizeof(WCHAR *); break;
    case 1: fixed = sizeof(USER_INFO_1); break;
    case 10: fixed = sizeof(USER_INFO_10); break;
    case 20: fixed = sizeof(USER_INFO_20); break;
    case 23: fixed = sizeof(USER_INFO_23); break;
    default: free(full); return ERROR_INVALID_LEVEL;
    }
    struct netbuf b;
    if (!nb_init(&b, fixed, wbytes(name) + wbytes(full) + 3 * wbytes("") + sidlen + 8)) { free(full); return ERROR_NOT_ENOUGH_MEMORY; }
    DWORD flags = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_DONT_EXPIRE_PASSWD;
    switch (level) {
    case 0: *(WCHAR **)b.base = nb_wstr(&b, name); break;
    case 1: {
        USER_INFO_1 *u = (USER_INFO_1 *)b.base;
        u->name = nb_wstr(&b, name);
        u->priv = getuid() == 0 ? 2 : 1;                  /* USER_PRIV_ADMIN / USER_PRIV_USER */
        u->home_dir = nb_wstr(&b, "");
        u->comment = nb_wstr(&b, "");
        u->flags = flags;
        u->script_path = nb_wstr(&b, "");
        break;
    }
    case 10: {
        USER_INFO_10 *u = (USER_INFO_10 *)b.base;
        u->name = nb_wstr(&b, name);
        u->comment = u->usr_comment = nb_wstr(&b, "");
        u->full_name = nb_wstr(&b, full);
        break;
    }
    case 20: {
        USER_INFO_20 *u = (USER_INFO_20 *)b.base;
        u->name = nb_wstr(&b, name);
        u->full_name = nb_wstr(&b, full);
        u->comment = nb_wstr(&b, "");
        u->flags = flags;
        u->user_id = sidlen >= 12 ? rd32(sid + sidlen - 4) : 1000;   /* the RID */
        break;
    }
    case 23: {
        USER_INFO_23 *u = (USER_INFO_23 *)b.base;
        u->name = nb_wstr(&b, name);
        u->full_name = nb_wstr(&b, full);
        u->comment = nb_wstr(&b, "");
        u->flags = flags;
        u->user_sid = b.base + b.used;
        memcpy(b.base + b.used, sid, sidlen);
        b.used += sidlen;
        break;
    }
    }
    free(full);
    *out = b.base;
    return NERR_Success;
}

API DWORD NetUserGetInfo(const WCHAR *server, const WCHAR *user, DWORD level, BYTE **out)
{
    if (!is_local_server(server)) return ERROR_BAD_NETPATH;
    if (!user || !out) return ERROR_INVALID_PARAMETER;
    char *u = lsiw_w2u(user);
    int me = !strcasecmp(u, user_name());
    free(u);
    if (!me) return NERR_UserNotFound;
    return build_user_info(level, out);
}

API DWORD NetUserEnum(const WCHAR *server, DWORD level, DWORD filter, BYTE **out, DWORD maxlen,
                      DWORD *read, DWORD *total, DWORD *resume)
{
    (void)maxlen;
    if (!is_local_server(server)) return ERROR_BAD_NETPATH;
    if (!out || !read || !total) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    *read = 0;
    *total = 1;
    if ((filter && !(filter & 0x0002)) || (resume && *resume)) { *total = (filter && !(filter & 2)) ? 0 : 1; return NERR_Success; }
    DWORD r = build_user_info(level, out);
    if (r) return r;
    *read = 1;
    if (resume) *resume = 0;
    return NERR_Success;
}

/* Local group membership is not modelled. */
API DWORD NetLocalGroupGetMembers(const WCHAR *server, const WCHAR *group, DWORD level, BYTE **out,
                                  DWORD maxlen, DWORD *read, DWORD *total, ULONG_PTR *resume)
{
    (void)server; (void)group; (void)level; (void)maxlen; (void)resume;
    if (out) *out = NULL;
    if (read) *read = 0;
    if (total) *total = 0;
    return ERROR_ACCESS_DENIED;
}

/* Nothing is shared over SMB. */
API DWORD NetShareEnum(const WCHAR *server, DWORD level, BYTE **out, DWORD maxlen, DWORD *read, DWORD *total, DWORD *resume)
{
    (void)level; (void)maxlen; (void)resume;
    if (!is_local_server(server)) return ERROR_BAD_NETPATH;
    if (out) *out = NULL;
    if (read) *read = 0;
    if (total) *total = 0;
    return NERR_Success;
}

typedef struct { WCHAR *username, *logon_domain, *oth_domains, *logon_server; } WKSTA_USER_INFO_1;

API DWORD NetWkstaUserGetInfo(WCHAR *reserved, DWORD level, BYTE **out)
{
    if (reserved || !out) return ERROR_INVALID_PARAMETER;
    struct netbuf b;
    size_t strs = wbytes(user_name()) + 2 * wbytes(computer_name()) + wbytes("");
    switch (level) {
    case 0:
        if (!nb_init(&b, sizeof(WCHAR *), strs)) return ERROR_NOT_ENOUGH_MEMORY;
        *(WCHAR **)b.base = nb_wstr(&b, user_name());
        break;
    case 1: {
        if (!nb_init(&b, sizeof(WKSTA_USER_INFO_1), strs)) return ERROR_NOT_ENOUGH_MEMORY;
        WKSTA_USER_INFO_1 *w = (WKSTA_USER_INFO_1 *)b.base;
        w->username = nb_wstr(&b, user_name());
        w->logon_domain = nb_wstr(&b, computer_name());
        w->oth_domains = nb_wstr(&b, "");
        w->logon_server = nb_wstr(&b, computer_name());
        break;
    }
    case 1101:
        if (!nb_init(&b, sizeof(WCHAR *), strs)) return ERROR_NOT_ENOUGH_MEMORY;
        *(WCHAR **)b.base = nb_wstr(&b, "");
        break;
    default:
        return ERROR_INVALID_LEVEL;
    }
    *out = b.base;
    return NERR_Success;
}

/* A workgroup machine has no domain controller. */
API DWORD DsGetDcNameW(const WCHAR *computer, const WCHAR *domain, const GUID *guid, const WCHAR *site, ULONG flags, void **info)
{
    (void)computer; (void)domain; (void)guid; (void)site; (void)flags;
    if (info) *info = NULL;
    return ERROR_NO_SUCH_DOMAIN;
}

API DWORD DsGetDcNameA(const char *computer, const char *domain, const GUID *guid, const char *site, ULONG flags, void **info)
{
    (void)computer; (void)domain; (void)guid; (void)site; (void)flags;
    if (info) *info = NULL;
    return ERROR_NO_SUCH_DOMAIN;
}

API DWORD NetGetAnyDCName(const WCHAR *server, const WCHAR *domain, BYTE **out) { (void)server; (void)domain; if (out) *out = NULL; return NERR_DCNotFound; }
API DWORD NetGetDCName(const WCHAR *server, const WCHAR *domain, BYTE **out)    { (void)server; (void)domain; if (out) *out = NULL; return NERR_DCNotFound; }

/* ================================================================== */
/* mpr                                                                  */
/* ================================================================== */

/* There are no network drives or redirected resources. Each WNet call
 * returns its error and also stores it as the last error. */
static DWORD wnet_err(DWORD e) { lsiw_set_last_error(e); return e; }

API DWORD WNetGetConnectionA(const char *local, char *remote, DWORD *len) { (void)local; (void)remote; (void)len; return wnet_err(ERROR_NOT_CONNECTED); }
API DWORD WNetGetConnectionW(const WCHAR *local, WCHAR *remote, DWORD *len) { (void)local; (void)remote; (void)len; return wnet_err(ERROR_NOT_CONNECTED); }
API DWORD WNetGetUniversalNameA(const char *local, DWORD level, void *buf, DWORD *size) { (void)local; (void)level; (void)buf; (void)size; return wnet_err(ERROR_NOT_CONNECTED); }
API DWORD WNetGetUniversalNameW(const WCHAR *local, DWORD level, void *buf, DWORD *size) { (void)local; (void)level; (void)buf; (void)size; return wnet_err(ERROR_NOT_CONNECTED); }
API DWORD WNetAddConnection2A(void *res, const char *pw, const char *user, DWORD flags) { (void)res; (void)pw; (void)user; (void)flags; return wnet_err(ERROR_NO_NETWORK); }
API DWORD WNetAddConnection2W(void *res, const WCHAR *pw, const WCHAR *user, DWORD flags) { (void)res; (void)pw; (void)user; (void)flags; return wnet_err(ERROR_NO_NETWORK); }
API DWORD WNetAddConnection3A(HANDLE hwnd, void *res, const char *pw, const char *user, DWORD flags) { (void)hwnd; (void)res; (void)pw; (void)user; (void)flags; return wnet_err(ERROR_NO_NETWORK); }
API DWORD WNetAddConnection3W(HANDLE hwnd, void *res, const WCHAR *pw, const WCHAR *user, DWORD flags) { (void)hwnd; (void)res; (void)pw; (void)user; (void)flags; return wnet_err(ERROR_NO_NETWORK); }
API DWORD WNetCancelConnection2A(const char *name, DWORD flags, BOOL force) { (void)name; (void)flags; (void)force; return wnet_err(ERROR_NO_NETWORK); }
API DWORD WNetCancelConnection2W(const WCHAR *name, DWORD flags, BOOL force) { (void)name; (void)flags; (void)force; return wnet_err(ERROR_NO_NETWORK); }
API DWORD WNetOpenEnumA(DWORD scope, DWORD type, DWORD usage, void *res, HANDLE *h) { (void)scope; (void)type; (void)usage; (void)res; if (h) *h = NULL; return wnet_err(ERROR_NO_NETWORK); }
API DWORD WNetOpenEnumW(DWORD scope, DWORD type, DWORD usage, void *res, HANDLE *h) { (void)scope; (void)type; (void)usage; (void)res; if (h) *h = NULL; return wnet_err(ERROR_NO_NETWORK); }
/* No enumeration handle can exist. */
API DWORD WNetEnumResourceA(HANDLE h, DWORD *count, void *buf, DWORD *size) { (void)h; (void)count; (void)buf; (void)size; return wnet_err(ERROR_INVALID_HANDLE); }
API DWORD WNetEnumResourceW(HANDLE h, DWORD *count, void *buf, DWORD *size) { (void)h; (void)count; (void)buf; (void)size; return wnet_err(ERROR_INVALID_HANDLE); }
API DWORD WNetCloseEnum(HANDLE h) { (void)h; return wnet_err(ERROR_INVALID_HANDLE); }

/* The user name (the local resource name is irrelevant: no connections). */
API DWORD WNetGetUserA(const char *name, char *user, DWORD *len)
{
    (void)name;
    if (!len) return wnet_err(ERROR_INVALID_PARAMETER);
    DWORD need = (DWORD)strlen(user_name()) + 1;
    if (!user || *len < need) { *len = need; return wnet_err(ERROR_MORE_DATA); }
    memcpy(user, user_name(), need);
    return ERROR_SUCCESS;
}

API DWORD WNetGetUserW(const WCHAR *name, WCHAR *user, DWORD *len)
{
    (void)name;
    if (!len) return wnet_err(ERROR_INVALID_PARAMETER);
    DWORD need = (DWORD)lsiw_utf8_to_utf16(user_name(), -1, NULL, 0);
    if (!user || *len < need) { *len = need; return wnet_err(ERROR_MORE_DATA); }
    lsiw_utf8_to_utf16(user_name(), -1, user, (int)need);
    return ERROR_SUCCESS;
}

/* No provider ever reports an extended error. */
API DWORD WNetGetLastErrorA(DWORD *err, char *buf, DWORD bufsize, char *name, DWORD namesize)
{
    if (err) *err = 0;
    if (buf && bufsize) buf[0] = 0;
    if (name && namesize) name[0] = 0;
    return ERROR_SUCCESS;
}

API DWORD WNetGetLastErrorW(DWORD *err, WCHAR *buf, DWORD bufsize, WCHAR *name, DWORD namesize)
{
    if (err) *err = 0;
    if (buf && bufsize) buf[0] = 0;
    if (name && namesize) name[0] = 0;
    return ERROR_SUCCESS;
}

/* ================================================================== */
/* wtsapi32                                                             */
/* ================================================================== */

/* One interactive session, id 1, connected at the console. */
#define LSIW_SESSION_ID      1
#define WTS_CURRENT_SESSION  0xFFFFFFFFu
#define WTSActive            0

enum {
    WTSInitialProgram, WTSApplicationName, WTSWorkingDirectory, WTSOEMId, WTSSessionId, WTSUserName,
    WTSWinStationName, WTSDomainName, WTSConnectState, WTSClientBuildNumber, WTSClientName,
    WTSClientDirectory, WTSClientProductId, WTSClientHardwareId, WTSClientAddress, WTSClientDisplay,
    WTSClientProtocolType, WTSIsRemoteSession = 29
};

API DWORD WTSGetActiveConsoleSessionId(void) { return LSIW_SESSION_ID; }

static BOOL wts_query(DWORD session, int cls, void **buf, DWORD *bytes, int wide)
{
    if (!buf || !bytes) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *buf = NULL;
    *bytes = 0;
    if (session != WTS_CURRENT_SESSION && session != LSIW_SESSION_ID) { lsiw_set_last_error(7022 /* ERROR_CTX_WINSTATION_NOT_FOUND */); return FALSE; }
    const char *s = NULL;
    uint8_t bin[24];
    size_t binlen = 0;
    memset(bin, 0, sizeof bin);
    switch (cls) {
    case WTSInitialProgram: case WTSApplicationName: case WTSWorkingDirectory: case WTSOEMId:
    case WTSClientName: case WTSClientDirectory:
        s = ""; break;
    case WTSUserName: s = user_name(); break;
    case WTSWinStationName: s = "Console"; break;
    case WTSDomainName: s = computer_name(); break;
    case WTSSessionId: { DWORD v = LSIW_SESSION_ID; memcpy(bin, &v, 4); binlen = 4; break; }
    case WTSConnectState: { int v = WTSActive; memcpy(bin, &v, 4); binlen = 4; break; }
    case WTSClientBuildNumber: case WTSClientHardwareId: binlen = 4; break;
    case WTSClientProductId: case WTSClientProtocolType: binlen = 2; break;   /* 0 = console */
    case WTSClientAddress: binlen = 24; break;                                /* AF_UNSPEC */
    case WTSClientDisplay: binlen = 12; break;
    case WTSIsRemoteSession: binlen = 1; break;                                /* FALSE */
    default: lsiw_set_last_error(ERROR_NOT_SUPPORTED); return FALSE;
    }
    if (s) {
        *buf = wide ? (void *)heap_wstr(s) : (void *)heap_str(s);
        if (!*buf) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        *bytes = wide ? (DWORD)(lsiw_wcslen(*buf) + 1) * 2 : (DWORD)strlen(*buf) + 1;
    } else {
        *buf = lsiw_heap_alloc(binlen, 0);
        if (!*buf) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        memcpy(*buf, bin, binlen);
        *bytes = (DWORD)binlen;
    }
    return TRUE;
}

API BOOL WTSQuerySessionInformationW(HANDLE server, DWORD session, int cls, WCHAR **buf, DWORD *bytes)
{
    (void)server;
    return wts_query(session, cls, (void **)buf, bytes, 1);
}

API BOOL WTSQuerySessionInformationA(HANDLE server, DWORD session, int cls, char **buf, DWORD *bytes)
{
    (void)server;
    return wts_query(session, cls, (void **)buf, bytes, 0);
}

API void WTSFreeMemory(void *p) { if (p) lsiw_heap_free(p); }
API BOOL WTSFreeMemoryExW(int cls, void *p, ULONG n) { (void)cls; (void)n; WTSFreeMemory(p); return TRUE; }
API BOOL WTSFreeMemoryExA(int cls, void *p, ULONG n) { (void)cls; (void)n; WTSFreeMemory(p); return TRUE; }

typedef struct { DWORD SessionId; void *pWinStationName; int State; } WTS_SESSION_INFO;

static BOOL wts_enum(DWORD version, WTS_SESSION_INFO **info, DWORD *count, int wide)
{
    if (version != 1 || !info || !count) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    size_t fixed = sizeof(WTS_SESSION_INFO);
    WTS_SESSION_INFO *s = lsiw_heap_alloc(fixed + 32, 1);
    if (!s) { lsiw_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    s->SessionId = LSIW_SESSION_ID;
    s->State = WTSActive;
    s->pWinStationName = (uint8_t *)s + fixed;
    if (wide) lsiw_utf8_to_utf16("Console", -1, s->pWinStationName, 16);
    else memcpy(s->pWinStationName, "Console", 8);
    *info = s;
    *count = 1;
    return TRUE;
}

API BOOL WTSEnumerateSessionsW(HANDLE server, DWORD reserved, DWORD version, WTS_SESSION_INFO **info, DWORD *count)
{
    (void)server;
    if (reserved) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return wts_enum(version, info, count, 1);
}

API BOOL WTSEnumerateSessionsA(HANDLE server, DWORD reserved, DWORD version, WTS_SESSION_INFO **info, DWORD *count)
{
    (void)server;
    if (reserved) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return wts_enum(version, info, count, 0);
}

/* Session changes (lock, logoff, remote connect) never occur here, so
 * registering for them succeeds and nothing is ever sent. */
API BOOL WTSRegisterSessionNotification(HANDLE hwnd, DWORD flags) { (void)hwnd; (void)flags; return TRUE; }
API BOOL WTSRegisterSessionNotificationEx(HANDLE server, HANDLE hwnd, DWORD flags) { (void)server; (void)hwnd; (void)flags; return TRUE; }
API BOOL WTSUnRegisterSessionNotification(HANDLE hwnd) { (void)hwnd; return TRUE; }
API BOOL WTSUnRegisterSessionNotificationEx(HANDLE server, HANDLE hwnd) { (void)server; (void)hwnd; return TRUE; }

/* Only services running as LocalSystem may call this. */
API BOOL WTSQueryUserToken(ULONG session, HANDLE *tok)
{
    (void)session;
    if (tok) *tok = NULL;
    lsiw_set_last_error(ERROR_PRIVILEGE_NOT_HELD);
    return FALSE;
}

API void WTSCloseServer(HANDLE server) { (void)server; }

/* ================================================================== */
/* dwmapi                                                               */
/* ================================================================== */

/* Composition is always on (as on Windows 8 and later). Per-window DWM
 * attributes are not available for any window. */
API HRESULT DwmIsCompositionEnabled(BOOL *enabled)
{
    if (!enabled) return E_INVALIDARG;
    *enabled = TRUE;
    return S_OK;
}

API HRESULT DwmEnableComposition(UINT action) { (void)action; return S_OK; }   /* a no-op since Windows 8 */
API HRESULT DwmGetWindowAttribute(HANDLE hwnd, DWORD attr, void *pv, DWORD cb) { (void)hwnd; (void)attr; (void)pv; (void)cb; return E_HANDLE; }
API HRESULT DwmSetWindowAttribute(HANDLE hwnd, DWORD attr, const void *pv, DWORD cb) { (void)hwnd; (void)attr; (void)pv; (void)cb; return E_HANDLE; }
API HRESULT DwmFlush(void) { return S_OK; }

API HRESULT DwmGetColorizationColor(DWORD *color, BOOL *opaque)
{
    if (!color || !opaque) return E_INVALIDARG;
    *color = 0xC40078D7u;          /* ARGB: the default Windows blue */
    *opaque = FALSE;
    return S_OK;
}

/* ================================================================== */
/* shcore                                                               */
/* ================================================================== */

/* Every monitor is 96 DPI (100%). The awareness can be set once. */
static int g_dpi_awareness = -1;

API HRESULT SetProcessDpiAwareness(int value)
{
    if (value < 0 || value > 2) return E_INVALIDARG;
    if (!__sync_bool_compare_and_swap(&g_dpi_awareness, -1, value)) return E_ACCESSDENIED;
    return S_OK;
}

API HRESULT GetProcessDpiAwareness(HANDLE proc, int *value)
{
    (void)proc;
    if (!value) return E_INVALIDARG;
    *value = g_dpi_awareness >= 0 ? g_dpi_awareness : 1;   /* PROCESS_SYSTEM_DPI_AWARE */
    return S_OK;
}

API HRESULT GetDpiForMonitor(HANDLE monitor, int type, UINT *x, UINT *y)
{
    (void)monitor;
    if (type < 0 || type > 2 || !x || !y) return E_INVALIDARG;
    *x = *y = 96;
    return S_OK;
}

API HRESULT GetScaleFactorForMonitor(HANDLE monitor, int *scale)
{
    (void)monitor;
    if (!scale) return E_INVALIDARG;
    *scale = 100;                  /* SCALE_100_PERCENT */
    return S_OK;
}

API int GetScaleFactorForDevice(int device) { (void)device; return 100; }

/* ================================================================== */
/* dnsapi                                                               */
/* ================================================================== */

#define DNS_TYPE_A                      1
#define DNS_TYPE_AAAA                   28
#define DNS_ERROR_RCODE_SERVER_FAILURE  9002
#define DNS_ERROR_RCODE_NAME_ERROR      9003
#define DNS_ERROR_RCODE_NOT_IMPLEMENTED 9004
#define DNS_INFO_NO_RECORDS             9501

typedef struct DNS_RECORD {
    struct DNS_RECORD *pNext;
    void *pName;
    WORD wType, wDataLength;
    DWORD Flags;                   /* Section:2 Delete:1 CharSet:2 Unused:3 Reserved:24 */
    DWORD dwTtl, dwReserved;
    union { DWORD A; BYTE AAAA[16]; } Data;
} DNS_RECORD;

_Static_assert(offsetof(DNS_RECORD, Data) == 32, "DNS_RECORD.Data");

API void DnsRecordListFree(DNS_RECORD *list, int type)
{
    (void)type;
    while (list) {
        DNS_RECORD *n = list->pNext;
        if (list->pName) lsiw_heap_free(list->pName);
        lsiw_heap_free(list);
        list = n;
    }
}

/* DnsFreeFlat (0) frees one block; DnsFreeRecordList (1) a record list. */
API void DnsFree(void *p, int type)
{
    if (!p) return;
    if (type == 1) DnsRecordListFree(p, type);
    else lsiw_heap_free(p);
}

/* A and AAAA records through getaddrinfo (hosts file, then DNS, as
 * DNS_QUERY_STANDARD does). The system resolver does not expose TTLs, so
 * dwTtl is 0. Other record types cannot be answered this way.
 * charset: 1 Unicode, 2 UTF-8, 3 ANSI (names are stored that way). */
static LONG dns_query(const char *name, WORD type, DNS_RECORD **out, int charset)
{
    if (!name || !out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    if (type != DNS_TYPE_A && type != DNS_TYPE_AAAA) return DNS_ERROR_RCODE_NOT_IMPLEMENTED;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = type == DNS_TYPE_A ? AF_INET : AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(name, NULL, &hints, &res);
    switch (rc) {
    case 0: break;
    case EAI_NONAME: return DNS_ERROR_RCODE_NAME_ERROR;
#ifdef EAI_NODATA
    case EAI_NODATA:
#endif
#ifdef EAI_ADDRFAMILY
    case EAI_ADDRFAMILY:
#endif
        return DNS_INFO_NO_RECORDS;
    case EAI_AGAIN: return ERROR_TIMEOUT;
    case EAI_MEMORY: return ERROR_NOT_ENOUGH_MEMORY;
    default: return DNS_ERROR_RCODE_SERVER_FAILURE;
    }
    DNS_RECORD *head = NULL, **tail = &head;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family != hints.ai_family) continue;
        const void *addr = type == DNS_TYPE_A ? (const void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr
                                              : (const void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
        size_t alen = type == DNS_TYPE_A ? 4 : 16;
        int dup = 0;
        for (DNS_RECORD *r = head; r; r = r->pNext) if (!memcmp(&r->Data, addr, alen)) dup = 1;
        if (dup) continue;
        DNS_RECORD *r = lsiw_heap_alloc(sizeof *r, 1);
        if (!r) break;
        r->pName = charset == 1 ? (void *)heap_wstr(name) : (void *)heap_str(name);
        r->wType = type;
        r->wDataLength = (WORD)alen;
        r->Flags = 1u | ((DWORD)charset << 3);      /* DnsSectionAnswer */
        memcpy(&r->Data, addr, alen);
        *tail = r;
        tail = &r->pNext;
    }
    freeaddrinfo(res);
    if (!head) return DNS_INFO_NO_RECORDS;
    *out = head;
    return ERROR_SUCCESS;
}

API LONG DnsQuery_W(const WCHAR *name, WORD type, DWORD options, void *extra, DNS_RECORD **out, void *reserved)
{
    (void)options; (void)extra; (void)reserved;
    char *u = lsiw_w2u(name);
    LONG r = dns_query(u, type, out, 1);
    free(u);
    return r;
}

API LONG DnsQuery_UTF8(const char *name, WORD type, DWORD options, void *extra, DNS_RECORD **out, void *reserved)
{
    (void)options; (void)extra; (void)reserved;
    return dns_query(name, type, out, 2);
}

API LONG DnsQuery_A(const char *name, WORD type, DWORD options, void *extra, DNS_RECORD **out, void *reserved)
{
    (void)options; (void)extra; (void)reserved;
    return dns_query(name, type, out, 3);
}

/* Resolver caching is the host's business (systemd-resolved, nscd...). */
API BOOL DnsFlushResolverCache(void) { return TRUE; }

/* ================================================================== */
/* Export table                                                         */
/* ================================================================== */

const struct lsiw_export lsiw_misc_exports[] = {
    /* winmm */
    E(timeGetTime), E(timeGetSystemTime), E(timeBeginPeriod), E(timeEndPeriod), E(timeGetDevCaps),
    E(timeSetEvent), E(timeKillEvent), E(PlaySoundA), E(PlaySoundW), EX("PlaySound", PlaySoundA),
    E(sndPlaySoundA), E(sndPlaySoundW),
    E(waveOutGetNumDevs), E(waveInGetNumDevs), E(midiOutGetNumDevs), E(midiInGetNumDevs), E(mixerGetNumDevs),
    E(auxGetNumDevs), E(joyGetNumDevs), E(waveOutOpen), E(waveInOpen), E(waveOutGetDevCapsA), E(waveOutGetDevCapsW),
    E(waveInGetDevCapsA), E(waveInGetDevCapsW), E(midiOutOpen), E(mixerOpen),
    E(mciSendStringA), E(mciSendStringW), E(mciSendCommandA), E(mciSendCommandW),
    E(mciGetErrorStringA), E(mciGetErrorStringW), E(joyGetPosEx), E(joyGetPos), E(joyGetDevCapsA), E(joyGetDevCapsW),
    /* version */
    E(GetFileVersionInfoSizeA), E(GetFileVersionInfoSizeW), E(GetFileVersionInfoSizeExA), E(GetFileVersionInfoSizeExW),
    E(GetFileVersionInfoA), E(GetFileVersionInfoW), E(GetFileVersionInfoExA), E(GetFileVersionInfoExW),
    E(VerQueryValueA), E(VerQueryValueW), E(VerLanguageNameA), E(VerLanguageNameW),
    E(VerFindFileA), E(VerFindFileW), E(VerInstallFileA), E(VerInstallFileW),
    /* userenv */
    E(GetUserProfileDirectoryA), E(GetUserProfileDirectoryW), E(GetProfilesDirectoryA), E(GetProfilesDirectoryW),
    E(GetAllUsersProfileDirectoryA), E(GetAllUsersProfileDirectoryW), E(GetDefaultUserProfileDirectoryA),
    E(GetDefaultUserProfileDirectoryW), E(GetProfileType), E(CreateEnvironmentBlock), E(DestroyEnvironmentBlock),
    E(ExpandEnvironmentStringsForUserA), E(ExpandEnvironmentStringsForUserW), E(LoadUserProfileA), E(LoadUserProfileW),
    E(UnloadUserProfile), E(GetAppContainerFolderPath), E(CreateProfile),
    /* secur32 / sspicli */
    E(GetUserNameExA), E(GetUserNameExW), E(GetComputerObjectNameA), E(GetComputerObjectNameW),
    E(AcquireCredentialsHandleA), E(AcquireCredentialsHandleW),
    EX("InitializeSecurityContextA", sspi_invalid_handle), EX("InitializeSecurityContextW", sspi_invalid_handle),
    EX("AcceptSecurityContext", sspi_invalid_handle), EX("QueryContextAttributesA", sspi_invalid_handle),
    EX("QueryContextAttributesW", sspi_invalid_handle), EX("DeleteSecurityContext", sspi_invalid_handle),
    EX("FreeCredentialsHandle", sspi_invalid_handle), EX("QueryCredentialsAttributesA", sspi_invalid_handle),
    EX("QueryCredentialsAttributesW", sspi_invalid_handle), EX("CompleteAuthToken", sspi_invalid_handle),
    EX("ApplyControlToken", sspi_invalid_handle), EX("ImpersonateSecurityContext", sspi_invalid_handle),
    EX("RevertSecurityContext", sspi_invalid_handle), EX("MakeSignature", sspi_invalid_handle),
    EX("VerifySignature", sspi_invalid_handle), EX("EncryptMessage", sspi_invalid_handle),
    EX("DecryptMessage", sspi_invalid_handle), EX("QuerySecurityContextToken", sspi_invalid_handle),
    E(FreeContextBuffer), E(EnumerateSecurityPackagesA), E(EnumerateSecurityPackagesW),
    E(QuerySecurityPackageInfoA), E(QuerySecurityPackageInfoW), E(InitSecurityInterfaceA), E(InitSecurityInterfaceW),
    /* psapi (module functions and GetProcessMemoryInfo come from the kernel32 tables) */
    E(EnumProcesses), E(GetProcessImageFileNameA), E(GetProcessImageFileNameW),
    E(GetMappedFileNameA), E(GetMappedFileNameW), E(GetPerformanceInfo),
    E(EmptyWorkingSet), E(QueryWorkingSet), E(QueryWorkingSetEx), E(InitializeProcessForWsWatch),
    E(GetWsChanges), E(GetWsChangesEx), E(EnumDeviceDrivers), E(GetDeviceDriverBaseNameA), E(GetDeviceDriverBaseNameW),
    E(GetDeviceDriverFileNameA), E(GetDeviceDriverFileNameW), E(EnumPageFilesA), E(EnumPageFilesW),
    E(GetModuleBaseNameA), E(EnumProcessModulesEx),
    EX("K32EnumProcesses", EnumProcesses), EX("K32GetProcessImageFileNameA", GetProcessImageFileNameA),
    EX("K32GetProcessImageFileNameW", GetProcessImageFileNameW), EX("K32GetMappedFileNameA", GetMappedFileNameA),
    EX("K32GetMappedFileNameW", GetMappedFileNameW), EX("K32GetPerformanceInfo", GetPerformanceInfo),
    EX("K32EmptyWorkingSet", EmptyWorkingSet),
    EX("K32QueryWorkingSet", QueryWorkingSet), EX("K32QueryWorkingSetEx", QueryWorkingSetEx),
    EX("K32GetModuleBaseNameA", GetModuleBaseNameA), EX("K32EnumProcessModulesEx", EnumProcessModulesEx),
    EX("K32EnumDeviceDrivers", EnumDeviceDrivers), EX("K32EnumPageFilesA", EnumPageFilesA),
    EX("K32EnumPageFilesW", EnumPageFilesW),
    /* dbghelp / dbgcore */
    E(SymInitialize), E(SymInitializeW), E(SymCleanup), E(SymSetOptions), E(SymGetOptions),
    E(SymSetSearchPath), E(SymSetSearchPathW), E(SymGetSearchPath), E(SymGetSearchPathW),
    E(SymLoadModuleEx), E(SymLoadModuleExW), E(SymLoadModule64), E(SymUnloadModule64),
    EX("SymUnloadModule", SymUnloadModule64), E(SymGetModuleBase64), E(SymFunctionTableAccess64),
    E(SymFromAddr), E(SymFromAddrW), E(SymGetSymFromAddr64), E(SymGetLineFromAddr64), E(SymGetLineFromAddrW64),
    E(SymEnumSymbols), E(SymEnumSymbolsW), E(SymRegisterCallback64), E(SymRegisterCallbackW64),
    E(StackWalk64), E(StackWalkEx), E(UnDecorateSymbolName), E(UnDecorateSymbolNameW), E(MiniDumpWriteDump),
    E(ImageNtHeader), E(ImageDirectoryEntryToData), E(ImageDirectoryEntryToDataEx),
    E(EnumerateLoadedModules64), E(EnumerateLoadedModulesW64),
    EX("EnumerateLoadedModules", EnumerateLoadedModules64), EX("EnumerateLoadedModulesW", EnumerateLoadedModulesW64),
    EX("EnumerateLoadedModulesEx", EnumerateLoadedModules64), EX("EnumerateLoadedModulesExW", EnumerateLoadedModulesW64),
    E(MakeSureDirectoryPathExists), E(SearchTreeForFile),
    /* powrprof */
    E(CallNtPowerInformation), E(GetPwrCapabilities), E(IsPwrSuspendAllowed), E(IsPwrHibernateAllowed),
    E(IsPwrShutdownAllowed), E(PowerGetActiveScheme), E(PowerReadACValueIndex), E(PowerReadDCValueIndex),
    E(PowerReadACValue), E(PowerReadDCValue), E(PowerWriteACValueIndex), E(PowerWriteDCValueIndex),
    E(PowerSetActiveScheme), E(PowerRegisterSuspendResumeNotification), E(PowerUnregisterSuspendResumeNotification),
    E(PowerSettingRegisterNotification), E(PowerSettingUnregisterNotification),
    /* netapi32 */
    E(NetApiBufferAllocate), E(NetApiBufferFree), E(NetApiBufferReallocate), E(NetApiBufferSize),
    E(NetGetJoinInformation), E(NetWkstaGetInfo), E(NetServerGetInfo), E(NetUserGetInfo), E(NetUserEnum),
    E(NetLocalGroupGetMembers), E(NetShareEnum), E(NetWkstaUserGetInfo), E(DsGetDcNameA), E(DsGetDcNameW),
    E(NetGetAnyDCName), E(NetGetDCName),
    /* mpr */
    E(WNetGetConnectionA), E(WNetGetConnectionW), E(WNetGetUniversalNameA), E(WNetGetUniversalNameW),
    E(WNetAddConnection2A), E(WNetAddConnection2W), E(WNetAddConnection3A), E(WNetAddConnection3W),
    E(WNetCancelConnection2A), E(WNetCancelConnection2W), E(WNetOpenEnumA), E(WNetOpenEnumW),
    E(WNetEnumResourceA), E(WNetEnumResourceW), E(WNetCloseEnum), E(WNetGetUserA), E(WNetGetUserW),
    E(WNetGetLastErrorA), E(WNetGetLastErrorW),
    /* wtsapi32 */
    E(WTSQuerySessionInformationA), E(WTSQuerySessionInformationW), E(WTSFreeMemory), E(WTSFreeMemoryExA),
    E(WTSFreeMemoryExW), E(WTSEnumerateSessionsA), E(WTSEnumerateSessionsW), E(WTSGetActiveConsoleSessionId),
    E(WTSRegisterSessionNotification), E(WTSRegisterSessionNotificationEx), E(WTSUnRegisterSessionNotification),
    E(WTSUnRegisterSessionNotificationEx), E(WTSQueryUserToken), E(WTSCloseServer),
    /* dwmapi */
    E(DwmIsCompositionEnabled), E(DwmEnableComposition), E(DwmGetWindowAttribute), E(DwmSetWindowAttribute),
    E(DwmFlush), E(DwmGetColorizationColor),
    /* shcore */
    E(SetProcessDpiAwareness), E(GetProcessDpiAwareness), E(GetDpiForMonitor), E(GetScaleFactorForMonitor),
    E(GetScaleFactorForDevice),
    /* dnsapi */
    E(DnsQuery_A), E(DnsQuery_W), E(DnsQuery_UTF8), E(DnsRecordListFree), E(DnsFree),
    E(DnsFlushResolverCache),
    { NULL, NULL }
};
