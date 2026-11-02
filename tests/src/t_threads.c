/* t_threads: threads and synchronisation objects.
 *
 * Thread creation and exit codes, interlocked operations, critical
 * sections, slim reader/writer locks with condition variables, events,
 * semaphores, mutexes (recursion, ownership, abandonment), multiple-object
 * waits, TLS (TlsAlloc, GCC's __thread, the PE's static .tls section and its
 * TLS callbacks), one-time initialisation, the thread pool, fibers and APCs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define __USE_MINGW_ANSI_STDIO 0
#include "lsiw_test.h"
#include <process.h>

/* GetCurrentFiber() reads gs:[0x20] through an intrinsic GCC misjudges. */
#pragma GCC diagnostic ignored "-Warray-bounds"

#define NTHREADS 8
#define NITER 50000

/* ---- creation, exit codes ---- */

static HANDLE g_go;

static DWORD WINAPI simple_thread(LPVOID arg)
{
    WaitForSingleObject(g_go, INFINITE);
    return (DWORD)(uintptr_t)arg + 1000;
}

static unsigned __stdcall crt_thread(void *arg)
{
    *(volatile int *)arg = 77;
    _endthreadex(55);
    return 0;
}

static DWORD WINAPI exit_thread(LPVOID arg)
{
    (void)arg;
    ExitThread(4321);
}

static volatile LONG g_suspended_ran;
static DWORD WINAPI suspended_thread(LPVOID arg)
{
    (void)arg;
    InterlockedExchange(&g_suspended_ran, 1);
    return 0;
}

static void test_create(void)
{
    g_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD tid = 0;
    HANDLE h = CreateThread(NULL, 0, simple_thread, (LPVOID)(uintptr_t)234, 0, &tid);
    CHECK(h != NULL && tid != 0, "CreateThread");
    DWORD code = 0;
    CHECK(GetExitCodeThread(h, &code) && code == STILL_ACTIVE, "GetExitCodeThread while running is STILL_ACTIVE");
    CHECK(WaitForSingleObject(h, 0) == WAIT_TIMEOUT, "wait on a running thread times out");
    CHECK(GetThreadId(h) == tid, "GetThreadId");
    SetEvent(g_go);
    CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0, "WaitForSingleObject on the thread");
    CHECKF(GetExitCodeThread(h, &code) && code == 1234, "GetExitCodeThread", "code=%lu", (unsigned long)code);
    CloseHandle(h);
    CloseHandle(g_go);

    volatile int flag = 0;
    uintptr_t th = _beginthreadex(NULL, 0, crt_thread, (void *)&flag, 0, NULL);
    CHECK(th != 0, "_beginthreadex");
    CHECK(WaitForSingleObject((HANDLE)th, 5000) == WAIT_OBJECT_0 && flag == 77, "_beginthreadex thread ran");
    CHECK(GetExitCodeThread((HANDLE)th, &code) && code == 55, "_endthreadex exit code");
    CloseHandle((HANDLE)th);

    h = CreateThread(NULL, 0, exit_thread, NULL, 0, NULL);
    WaitForSingleObject(h, 5000);
    CHECK(GetExitCodeThread(h, &code) && code == 4321, "ExitThread exit code");
    CloseHandle(h);

    h = CreateThread(NULL, 0, suspended_thread, NULL, CREATE_SUSPENDED, NULL);
    Sleep(50);
    CHECK(h && g_suspended_ran == 0, "CREATE_SUSPENDED thread does not run");
    CHECK(ResumeThread(h) == 1, "ResumeThread returns the previous suspend count");
    CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && g_suspended_ran == 1, "resumed thread runs");
    CloseHandle(h);
}

/* ---- counters ---- */

static volatile LONG g_atomic;
static LONG g_plain;
static CRITICAL_SECTION g_cs;
static DWORD g_tids[NTHREADS];

static DWORD WINAPI count_thread(LPVOID arg)
{
    g_tids[(uintptr_t)arg] = GetCurrentThreadId();
    for (int i = 0; i < NITER; i++) {
        InterlockedIncrement(&g_atomic);
        EnterCriticalSection(&g_cs);
        g_plain++;
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

static void test_counters(void)
{
    HANDLE h[NTHREADS];
    InitializeCriticalSection(&g_cs);
    for (int i = 0; i < NTHREADS; i++) h[i] = CreateThread(NULL, 0, count_thread, (LPVOID)(uintptr_t)i, 0, NULL);
    DWORD w = WaitForMultipleObjects(NTHREADS, h, TRUE, 30000);
    CHECKF(w < WAIT_OBJECT_0 + NTHREADS, "WaitForMultipleObjects(all) on 8 threads", "w=%lx", (unsigned long)w);
    CHECKF(g_atomic == NTHREADS * NITER, "InterlockedIncrement from 8 threads", "got %ld", (long)g_atomic);
    CHECKF(g_plain == NTHREADS * NITER, "CRITICAL_SECTION from 8 threads", "got %ld", (long)g_plain);
    int distinct = 1;
    for (int i = 0; i < NTHREADS; i++) {
        if (g_tids[i] == 0 || g_tids[i] == GetCurrentThreadId()) distinct = 0;
        for (int j = 0; j < i; j++) if (g_tids[i] == g_tids[j]) distinct = 0;
    }
    CHECK(distinct, "GetCurrentThreadId distinct per thread");
    for (int i = 0; i < NTHREADS; i++) CloseHandle(h[i]);

    EnterCriticalSection(&g_cs);
    CHECK(TryEnterCriticalSection(&g_cs), "CRITICAL_SECTION is recursive");
    LeaveCriticalSection(&g_cs);
    LeaveCriticalSection(&g_cs);
    DeleteCriticalSection(&g_cs);

    LONG v = 5;
    CHECK(InterlockedCompareExchange(&v, 9, 5) == 5 && v == 9, "InterlockedCompareExchange");
    CHECK(InterlockedExchangeAdd(&v, 3) == 9 && v == 12, "InterlockedExchangeAdd");
}

/* ---- SRW lock + condition variable: bounded queue ---- */

#define QSIZE 4
#define NITEMS 1000
static SRWLOCK g_srw = SRWLOCK_INIT;
static CONDITION_VARIABLE g_notempty = CONDITION_VARIABLE_INIT, g_notfull = CONDITION_VARIABLE_INIT;
static int g_queue[QSIZE], g_qhead, g_qcount;

static DWORD WINAPI producer(LPVOID arg)
{
    (void)arg;
    for (int i = 1; i <= NITEMS; i++) {
        AcquireSRWLockExclusive(&g_srw);
        while (g_qcount == QSIZE) SleepConditionVariableSRW(&g_notfull, &g_srw, INFINITE, 0);
        g_queue[(g_qhead + g_qcount++) % QSIZE] = i;
        WakeConditionVariable(&g_notempty);
        ReleaseSRWLockExclusive(&g_srw);
    }
    return 0;
}

static DWORD WINAPI consumer(LPVOID arg)
{
    long long *sum = arg;
    for (int i = 1; i <= NITEMS; i++) {
        AcquireSRWLockExclusive(&g_srw);
        while (g_qcount == 0) SleepConditionVariableSRW(&g_notempty, &g_srw, INFINITE, 0);
        int v = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % QSIZE;
        g_qcount--;
        WakeConditionVariable(&g_notfull);
        ReleaseSRWLockExclusive(&g_srw);
        *sum += v;
    }
    return 0;
}

static CRITICAL_SECTION g_cvcs;
static CONDITION_VARIABLE g_cv2 = CONDITION_VARIABLE_INIT;
static int g_ready;

static DWORD WINAPI cv_cs_waiter(LPVOID arg)
{
    (void)arg;
    EnterCriticalSection(&g_cvcs);
    while (!g_ready) SleepConditionVariableCS(&g_cv2, &g_cvcs, INFINITE);
    g_ready++;
    LeaveCriticalSection(&g_cvcs);
    return 0;
}

static void test_srw_cv(void)
{
    long long sum = 0;
    HANDLE h[2];
    h[0] = CreateThread(NULL, 0, consumer, &sum, 0, NULL);
    h[1] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    CHECK(WaitForMultipleObjects(2, h, TRUE, 30000) == WAIT_OBJECT_0, "producer/consumer finish");
    CHECKF(sum == (long long)NITEMS * (NITEMS + 1) / 2, "SRWLOCK + CONDITION_VARIABLE queue", "sum=%lld", sum);
    CloseHandle(h[0]);
    CloseHandle(h[1]);

    AcquireSRWLockShared(&g_srw);
    CHECK(TryAcquireSRWLockShared(&g_srw), "SRW shared lock taken twice");
    CHECK(!TryAcquireSRWLockExclusive(&g_srw), "SRW exclusive fails while shared");
    ReleaseSRWLockShared(&g_srw);
    ReleaseSRWLockShared(&g_srw);
    CHECK(TryAcquireSRWLockExclusive(&g_srw), "SRW exclusive after release");
    ReleaseSRWLockExclusive(&g_srw);

    AcquireSRWLockExclusive(&g_srw);
    DWORD t0 = GetTickCount();
    SetLastError(0);
    BOOL ok = SleepConditionVariableSRW(&g_notempty, &g_srw, 50, 0);
    CHECKF(!ok && GetLastError() == ERROR_TIMEOUT && GetTickCount() - t0 >= 40,
           "SleepConditionVariableSRW timeout -> ERROR_TIMEOUT", "ok=%d err=%lu", ok, GetLastError());
    ReleaseSRWLockExclusive(&g_srw);

    InitializeCriticalSection(&g_cvcs);
    HANDLE w = CreateThread(NULL, 0, cv_cs_waiter, NULL, 0, NULL);
    Sleep(20);
    EnterCriticalSection(&g_cvcs);
    g_ready = 1;
    WakeAllConditionVariable(&g_cv2);
    LeaveCriticalSection(&g_cvcs);
    CHECK(WaitForSingleObject(w, 5000) == WAIT_OBJECT_0 && g_ready == 2, "SleepConditionVariableCS woken");
    CloseHandle(w);
    DeleteCriticalSection(&g_cvcs);
}

/* ---- events, semaphores, mutexes ---- */

static HANDLE g_mutex;
static DWORD WINAPI mutex_try(LPVOID arg)
{
    (void)arg;
    return WaitForSingleObject(g_mutex, 0);
}

static DWORD WINAPI mutex_abandon(LPVOID arg)
{
    (void)arg;
    return WaitForSingleObject(g_mutex, 0);   /* exits while owning it */
}

static DWORD WINAPI event_setter(LPVOID arg)
{
    Sleep(30);
    SetEvent((HANDLE)arg);
    return 0;
}

static void test_objects(void)
{
    HANDLE m = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(WaitForSingleObject(m, 0) == WAIT_TIMEOUT, "manual event starts unsignalled");
    SetEvent(m);
    CHECK(WaitForSingleObject(m, 0) == WAIT_OBJECT_0 && WaitForSingleObject(m, 0) == WAIT_OBJECT_0,
          "manual-reset event stays signalled");
    ResetEvent(m);
    CHECK(WaitForSingleObject(m, 0) == WAIT_TIMEOUT, "ResetEvent");
    HANDLE a = CreateEventA(NULL, FALSE, TRUE, NULL);
    CHECK(WaitForSingleObject(a, 0) == WAIT_OBJECT_0 && WaitForSingleObject(a, 0) == WAIT_TIMEOUT,
          "auto-reset event releases one wait");
    HANDLE t = CreateThread(NULL, 0, event_setter, a, 0, NULL);
    CHECK(WaitForSingleObject(a, 5000) == WAIT_OBJECT_0, "event set by another thread wakes the waiter");
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);

    /* Multiple objects: all / any / timeout. */
    HANDLE two[2] = { m, a };
    SetEvent(m);
    CHECK(WaitForMultipleObjects(2, two, FALSE, 0) == WAIT_OBJECT_0, "WaitForMultipleObjects any: first");
    ResetEvent(m);
    SetEvent(a);
    CHECK(WaitForMultipleObjects(2, two, FALSE, 0) == WAIT_OBJECT_0 + 1, "WaitForMultipleObjects any: second");
    CHECK(WaitForMultipleObjects(2, two, FALSE, 20) == WAIT_TIMEOUT, "WaitForMultipleObjects any: timeout");
    SetEvent(m);
    CHECK(WaitForMultipleObjects(2, two, TRUE, 20) == WAIT_TIMEOUT, "WaitForMultipleObjects all: one missing");
    CHECK(WaitForSingleObject(m, 0) == WAIT_OBJECT_0, "failed wait-all consumes nothing");
    SetEvent(a);
    CHECK(WaitForMultipleObjects(2, two, TRUE, 0) == WAIT_OBJECT_0, "WaitForMultipleObjects all: both");
    CHECK(WaitForSingleObject(a, 0) == WAIT_TIMEOUT, "wait-all consumed the auto-reset event");
    CloseHandle(a);

    /* Named objects. */
    /* A stale ERROR_ALREADY_EXISTS must be cleared: single-instance checks
     * test GetLastError() right after creating a named object. */
    SetLastError(ERROR_ALREADY_EXISTS);
    HANDLE n1 = CreateEventA(NULL, TRUE, FALSE, "LSIW_T_THREADS_EVENT");
    CHECKF(n1 && GetLastError() == 0, "named CreateEventA (new) sets last error 0", "err=%lu", GetLastError());
    HANDLE n2 = CreateEventA(NULL, TRUE, FALSE, "LSIW_T_THREADS_EVENT");
    CHECK(n2 && GetLastError() == ERROR_ALREADY_EXISTS, "named CreateEventA again -> ERROR_ALREADY_EXISTS");
    HANDLE n3 = OpenEventA(EVENT_ALL_ACCESS, FALSE, "LSIW_T_THREADS_EVENT");
    SetEvent(n1);
    CHECK(n3 && WaitForSingleObject(n2, 0) == WAIT_OBJECT_0 && WaitForSingleObject(n3, 0) == WAIT_OBJECT_0,
          "named handles share one event");
    CloseHandle(n1); CloseHandle(n2); CloseHandle(n3);
    SetLastError(ERROR_ALREADY_EXISTS);
    HANDLE nm1 = CreateMutexA(NULL, FALSE, "LSIW_T_THREADS_MUTEX");
    CHECK(nm1 && GetLastError() == 0, "named CreateMutexA (new)");
    HANDLE nm2 = CreateMutexA(NULL, FALSE, "LSIW_T_THREADS_MUTEX");
    CHECK(nm2 && GetLastError() == ERROR_ALREADY_EXISTS, "named CreateMutexA again -> ERROR_ALREADY_EXISTS");
    CloseHandle(nm1); CloseHandle(nm2);
    SetLastError(0);
    CHECKF(OpenEventA(EVENT_ALL_ACCESS, FALSE, "LSIW_T_NO_SUCH_EVENT") == NULL && GetLastError() == ERROR_FILE_NOT_FOUND,
           "OpenEventA missing -> ERROR_FILE_NOT_FOUND", "err=%lu", GetLastError());
    CloseHandle(m);

    HANDLE s = CreateSemaphoreA(NULL, 2, 3, NULL);
    CHECK(WaitForSingleObject(s, 0) == WAIT_OBJECT_0 && WaitForSingleObject(s, 0) == WAIT_OBJECT_0 &&
          WaitForSingleObject(s, 0) == WAIT_TIMEOUT, "semaphore count 2");
    LONG prev = -1;
    CHECK(ReleaseSemaphore(s, 3, &prev) && prev == 0, "ReleaseSemaphore to max");
    SetLastError(0);
    CHECKF(!ReleaseSemaphore(s, 1, &prev) && GetLastError() == ERROR_TOO_MANY_POSTS,
           "ReleaseSemaphore beyond max -> ERROR_TOO_MANY_POSTS", "err=%lu", GetLastError());
    CloseHandle(s);

    g_mutex = CreateMutexA(NULL, FALSE, NULL);
    CHECK(WaitForSingleObject(g_mutex, 0) == WAIT_OBJECT_0 && WaitForSingleObject(g_mutex, 0) == WAIT_OBJECT_0,
          "mutex is recursive for its owner");
    DWORD code = 0;
    t = CreateThread(NULL, 0, mutex_try, NULL, 0, NULL);
    WaitForSingleObject(t, 5000);
    GetExitCodeThread(t, &code);
    CHECKF(code == WAIT_TIMEOUT, "owned mutex blocks other threads", "code=%lx", (unsigned long)code);
    CloseHandle(t);
    CHECK(ReleaseMutex(g_mutex) && ReleaseMutex(g_mutex), "ReleaseMutex twice");
    SetLastError(0);
    CHECKF(!ReleaseMutex(g_mutex) && GetLastError() == ERROR_NOT_OWNER,
           "ReleaseMutex when not owner -> ERROR_NOT_OWNER", "err=%lu", GetLastError());
    t = CreateThread(NULL, 0, mutex_abandon, NULL, 0, NULL);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    DWORD w = WaitForSingleObject(g_mutex, 1000);
    CHECKF(w == WAIT_ABANDONED, "mutex of an exited owner -> WAIT_ABANDONED", "w=%lx", (unsigned long)w);
    if (w == WAIT_ABANDONED || w == WAIT_OBJECT_0) ReleaseMutex(g_mutex);
    CloseHandle(g_mutex);
}

/* ---- TLS ---- */

static DWORD g_slot;
static __thread int g_thread_var = 11;

/* Static TLS through the PE's .tls section, accessed the way MSVC code
 * does: gs:[0x58] -> ThreadLocalStoragePointer[_tls_index] + offset. The
 * MinGW runtime's tlssup.o defines _tls_index, _tls_start and the TLS
 * directory; GCC itself uses emulated TLS for __thread. */
extern ULONG _tls_index;
extern char _tls_start;
__attribute__((section(".tls$LSIW"))) int g_static_tls = 42;

static int *static_tls_ptr(void)
{
    char **tls_array;
    __asm__ volatile ("movq %%gs:0x58, %0" : "=r"(tls_array));
    return (int *)(tls_array[_tls_index] + ((char *)&g_static_tls - &_tls_start));
}

/* TLS callbacks: the loader calls them for process and thread attach. */
static volatile LONG g_cb_process_attach, g_cb_thread_attach, g_cb_thread_detach;
static void NTAPI tls_callback(PVOID h, DWORD reason, PVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) InterlockedIncrement(&g_cb_process_attach);
    if (reason == DLL_THREAD_ATTACH) InterlockedIncrement(&g_cb_thread_attach);
    if (reason == DLL_THREAD_DETACH) InterlockedIncrement(&g_cb_thread_detach);
}
__attribute__((section(".CRT$XLL"), used)) const PIMAGE_TLS_CALLBACK g_tls_cb_ptr = tls_callback;

static DWORD WINAPI tls_thread(LPVOID arg)
{
    uintptr_t id = (uintptr_t)arg;
    int ok = 1;
    if (TlsGetValue(g_slot) != NULL) ok = 0;
    TlsSetValue(g_slot, (LPVOID)(id * 100));
    if (g_thread_var != 11) ok = 0;
    g_thread_var = (int)id;
    int *st = static_tls_ptr();
    if (*st != 42) ok = 0;
    *st = (int)id + 500;
    Sleep(30);
    if (TlsGetValue(g_slot) != (LPVOID)(id * 100)) ok = 0;
    if (g_thread_var != (int)id) ok = 0;
    if (*static_tls_ptr() != (int)id + 500) ok = 0;
    return ok;
}

static void test_tls(void)
{
    g_slot = TlsAlloc();
    CHECK(g_slot != TLS_OUT_OF_INDEXES, "TlsAlloc");
    SetLastError(1234);
    CHECK(TlsGetValue(g_slot) == NULL && GetLastError() == ERROR_SUCCESS, "TlsGetValue new slot is NULL, error 0");
    TlsSetValue(g_slot, (LPVOID)7);
    g_thread_var = 99;
    CHECK(*static_tls_ptr() == 42, "static .tls initial value in the main thread");
    CHECK(static_tls_ptr() != &g_static_tls, "static .tls block is a per-thread copy of the template");
    *static_tls_ptr() = 99;

    LONG attach_before = g_cb_thread_attach;
    HANDLE h[4];
    for (int i = 0; i < 4; i++) h[i] = CreateThread(NULL, 0, tls_thread, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
    WaitForMultipleObjects(4, h, TRUE, 10000);
    int all = 1;
    for (int i = 0; i < 4; i++) {
        DWORD c = 0;
        GetExitCodeThread(h[i], &c);
        if (c != 1) all = 0;
        CloseHandle(h[i]);
    }
    CHECK(all, "TlsSetValue, __thread and static .tls are per thread");
    CHECK(TlsGetValue(g_slot) == (LPVOID)7 && g_thread_var == 99, "main thread values unchanged");
    CHECK(*static_tls_ptr() == 99, "main thread static .tls unchanged");
    CHECK(TlsFree(g_slot), "TlsFree");

    CHECK(g_cb_process_attach == 1, "TLS callback got DLL_PROCESS_ATTACH before main");
    CHECKF(g_cb_thread_attach - attach_before >= 4, "TLS callback got DLL_THREAD_ATTACH for new threads",
           "attach=%ld", (long)(g_cb_thread_attach - attach_before));
    CHECK(g_cb_thread_detach >= 4, "TLS callback got DLL_THREAD_DETACH");

    DWORD fls = FlsAlloc(NULL);
    CHECK(fls != FLS_OUT_OF_INDEXES && FlsSetValue(fls, (PVOID)5) && FlsGetValue(fls) == (PVOID)5, "FlsAlloc/FlsSetValue");
    FlsFree(fls);
}

/* ---- once, timing, thread pool, fibers, APCs ---- */

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_once_calls;
static BOOL CALLBACK once_fn(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param;
    InterlockedIncrement(&g_once_calls);
    Sleep(20);
    *ctx = (PVOID)0x1230;
    return TRUE;
}

static DWORD WINAPI once_thread(LPVOID arg)
{
    (void)arg;
    PVOID ctx = NULL;
    return InitOnceExecuteOnce(&g_once, once_fn, NULL, &ctx) && ctx == (PVOID)0x1230;
}

static volatile LONG g_work_count;
static void CALLBACK work_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK work)
{
    (void)inst; (void)work;
    Sleep(5);
    InterlockedAdd(&g_work_count, (LONG)(uintptr_t)ctx);
}

static LPVOID g_main_fiber, g_fiber;
static int g_fiber_steps;
static void WINAPI fiber_fn(LPVOID arg)
{
    g_fiber_steps += (int)(uintptr_t)arg;
    SwitchToFiber(g_main_fiber);
    g_fiber_steps += 10;
    SwitchToFiber(g_main_fiber);
}

static int g_apc;
static void CALLBACK apc_fn(ULONG_PTR arg) { g_apc += (int)arg; }

static void test_misc(void)
{
    HANDLE h[4];
    for (int i = 0; i < 4; i++) h[i] = CreateThread(NULL, 0, once_thread, NULL, 0, NULL);
    WaitForMultipleObjects(4, h, TRUE, 10000);
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        DWORD c = 0;
        GetExitCodeThread(h[i], &c);
        ok &= c == 1;
        CloseHandle(h[i]);
    }
    CHECKF(ok && g_once_calls == 1, "InitOnceExecuteOnce runs once for 4 threads", "calls=%ld", (long)g_once_calls);

    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    Sleep(100);
    QueryPerformanceCounter(&b);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    CHECKF(ms >= 95.0 && ms < 3000.0, "Sleep(100) sleeps at least ~100 ms", "%.1f ms", ms);
    Sleep(0);
    SwitchToThread();

    g_main_fiber = ConvertThreadToFiber(NULL);
    CHECK(g_main_fiber != NULL && GetCurrentFiber() == g_main_fiber, "ConvertThreadToFiber");
    g_fiber = CreateFiber(0, fiber_fn, (LPVOID)1);
    CHECK(g_fiber != NULL, "CreateFiber");
    SwitchToFiber(g_fiber);
    CHECK(g_fiber_steps == 1, "SwitchToFiber runs the fiber");
    SwitchToFiber(g_fiber);
    CHECK(g_fiber_steps == 11, "fiber resumes where it switched away");
    DeleteFiber(g_fiber);
    CHECK(ConvertFiberToThread(), "ConvertFiberToThread");

    CHECK(QueueUserAPC(apc_fn, GetCurrentThread(), 5), "QueueUserAPC");
    CHECK(g_apc == 0, "APC waits for an alertable wait");
    DWORD r = SleepEx(1000, TRUE);
    CHECKF(r == WAIT_IO_COMPLETION && g_apc == 5, "SleepEx alertable runs the APC -> WAIT_IO_COMPLETION",
           "r=%lx apc=%d", (unsigned long)r, g_apc);

    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) &&
          GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_ABOVE_NORMAL, "thread priority round trip");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    /* Last: the thread pool lives in a part of kernel32 an engine may lack. */
    PTP_WORK work = CreateThreadpoolWork(work_cb, (PVOID)3, NULL);
    CHECK(work != NULL, "CreateThreadpoolWork");
    if (work) {
        for (int i = 0; i < 10; i++) SubmitThreadpoolWork(work);
        WaitForThreadpoolWorkCallbacks(work, FALSE);
        CHECKF(g_work_count == 30, "thread pool ran 10 work items", "count=%ld", (long)g_work_count);
        CloseThreadpoolWork(work);
    }
}

int main(void)
{
    t_init("t_threads");
    test_create();
    test_counters();
    test_srw_cv();
    test_objects();
    test_tls();
    test_misc();
    return t_done();
}
