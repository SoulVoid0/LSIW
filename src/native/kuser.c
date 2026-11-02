/* KUSER_SHARED_DATA: the read-only page Windows maps at 0x7FFE0000 in every
 * process. Programs read the clocks there directly instead of calling the
 * system: the Go runtime's nanotime() and time.Now(), MSVC's GetTickCount
 * inlines and some anti-tamper code. The fields live at fixed offsets; a
 * background thread refreshes the clock fields every millisecond.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "native.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define KUSER_ADDR ((uintptr_t)0x7FFE0000)
#define KUSER_SIZE 0x1000

#define EPOCH_DIFF_100NS 116444736000000000ULL   /* 1601-01-01 to 1970-01-01 */

static volatile uint8_t *g_ku;

/* KSYSTEM_TIME { ULONG LowPart; LONG High1Time; LONG High2Time; }: written
 * High2, Low, High1 so a reader that sees High1 == High2 has a whole value. */
static void put_ksystem_time(size_t off, uint64_t v)
{
    volatile uint32_t *t = (volatile uint32_t *)(g_ku + off);
    t[2] = (uint32_t)(v >> 32);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t[0] = (uint32_t)v;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t[1] = (uint32_t)(v >> 32);
}

static uint64_t ts_100ns(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100;
}

static void refresh(void)
{
    uint64_t boot = ts_100ns(CLOCK_BOOTTIME);
    put_ksystem_time(0x008, boot);                                    /* InterruptTime */
    put_ksystem_time(0x014, ts_100ns(CLOCK_REALTIME) + EPOCH_DIFF_100NS);   /* SystemTime */
    /* TickCount counts 15.625 ms ticks; GetTickCount = ticks * mult >> 24. */
    uint64_t ticks = boot / 156250;
    put_ksystem_time(0x320, ticks);                                   /* TickCount */
    *(volatile uint32_t *)(g_ku + 0x000) = (uint32_t)ticks;           /* TickCountLowDeprecated */

    /* Time-zone bias (UTC - local, 100 ns units); changes at DST switches. */
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int64_t bias = -(int64_t)lt.tm_gmtoff * 10000000LL;
    put_ksystem_time(0x020, (uint64_t)bias);
    *(volatile uint32_t *)(g_ku + 0x240) = lt.tm_isdst > 0 ? 2 : 1;  /* TimeZoneId: daylight / standard */
}

static void *updater(void *arg)
{
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 1000000L;
        if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) ;
        refresh();
    }
    return NULL;
}

static void put_wstr(size_t off, const char *s, size_t max)
{
    volatile uint16_t *w = (volatile uint16_t *)(g_ku + off);
    size_t i = 0;
    for (; s[i] && i + 1 < max; i++) w[i] = (uint8_t)s[i];
    w[i] = 0;
}

static void set_features(void)
{
    volatile uint8_t *pf = g_ku + 0x274;              /* ProcessorFeatures[64] */
    __builtin_cpu_init();
    pf[1] = 0;                                        /* PF_FLOATING_POINT_EMULATED */
    pf[2] = 1;                                        /* PF_COMPARE_EXCHANGE_DOUBLE */
    pf[3] = 1;                                        /* PF_MMX_INSTRUCTIONS_AVAILABLE */
    pf[6] = 1;                                        /* PF_XMMI_INSTRUCTIONS_AVAILABLE */
    pf[8] = 1;                                        /* PF_RDTSC_INSTRUCTION_AVAILABLE */
    pf[9] = 1;                                        /* PF_PAE_ENABLED */
    pf[10] = 1;                                       /* PF_XMMI64_INSTRUCTIONS_AVAILABLE */
    pf[12] = 1;                                       /* PF_NX_ENABLED */
    pf[13] = __builtin_cpu_supports("sse3") ? 1 : 0;  /* PF_SSE3_INSTRUCTIONS_AVAILABLE */
    pf[14] = 1;                                       /* PF_COMPARE_EXCHANGE128 */
    pf[17] = 1;                                       /* PF_XSAVE_ENABLED */
    pf[20] = 1;                                       /* PF_SECOND_LEVEL_ADDRESS_TRANSLATION */
    pf[23] = 1;                                       /* PF_FASTFAIL_AVAILABLE */
    pf[36] = __builtin_cpu_supports("ssse3") ? 1 : 0;
    pf[37] = __builtin_cpu_supports("sse4.1") ? 1 : 0;
    pf[38] = __builtin_cpu_supports("sse4.2") ? 1 : 0;
    pf[39] = __builtin_cpu_supports("avx") ? 1 : 0;
    pf[40] = __builtin_cpu_supports("avx2") ? 1 : 0;
    pf[41] = __builtin_cpu_supports("avx512f") ? 1 : 0;
}

/* Map and fill the page. Failure (address taken, mmap_min_addr) is not fatal:
 * only programs that read the page directly are affected. */
int lsiw_kuser_init(void)
{
    if (g_ku) return 0;
    void *p = mmap((void *)KUSER_ADDR, KUSER_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || (uintptr_t)p != KUSER_ADDR) {
        if (p != MAP_FAILED) munmap(p, KUSER_SIZE);
        if (lsiw_verbose) lsiw_log("KUSER_SHARED_DATA not mapped at 0x7FFE0000: %s\n", strerror(errno));
        return -1;
    }
    g_ku = p;
    *(volatile uint32_t *)(g_ku + 0x004) = 0x0FA00000;        /* TickCountMultiplier */
    *(volatile uint16_t *)(g_ku + 0x02C) = 0x8664;            /* ImageNumberLow */
    *(volatile uint16_t *)(g_ku + 0x02E) = 0x8664;            /* ImageNumberHigh */
    put_wstr(0x030, "C:\\windows", 260);                      /* NtSystemRoot */
    *(volatile uint32_t *)(g_ku + 0x238) = 32;                /* MaxStackTraceDepth */
    *(volatile uint32_t *)(g_ku + 0x244) = 0x200000;          /* LargePageMinimum */
    *(volatile uint32_t *)(g_ku + 0x260) = 19045;             /* NtBuildNumber */
    *(volatile uint32_t *)(g_ku + 0x264) = 1;                 /* NtProductType: NtProductWinNt */
    *(volatile uint8_t  *)(g_ku + 0x268) = 1;                 /* ProductTypeIsValid */
    *(volatile uint16_t *)(g_ku + 0x26A) = 9;                 /* NativeProcessorArchitecture: AMD64 */
    *(volatile uint32_t *)(g_ku + 0x26C) = 10;                /* NtMajorVersion */
    *(volatile uint32_t *)(g_ku + 0x270) = 0;                 /* NtMinorVersion */
    set_features();
    *(volatile uint32_t *)(g_ku + 0x2D8) = 1;                 /* ActiveConsoleId */
    long pages = sysconf(_SC_PHYS_PAGES);
    *(volatile uint32_t *)(g_ku + 0x2E8) = pages > 0 ? (uint32_t)pages : 0;   /* NumberOfPhysicalPages */
    *(volatile uint64_t *)(g_ku + 0x2F8) = 0xC3;              /* TestRetInstruction: ret */
    *(volatile int64_t  *)(g_ku + 0x300) = 10000000;          /* QpcFrequency */
    uint32_t cookie;
    if (getentropy(&cookie, sizeof cookie) != 0) cookie = (uint32_t)getpid() * 2654435761u;
    *(volatile uint32_t *)(g_ku + 0x330) = cookie;            /* Cookie */
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    *(volatile uint32_t *)(g_ku + 0x3C0) = cpus > 0 ? (uint32_t)cpus : 1;   /* ActiveProcessorCount */
    *(volatile uint16_t *)(g_ku + 0x3C4) = 1;                 /* ActiveGroupCount */
    refresh();

    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 64 * 1024);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);                 /* keep signals for program threads */
    int r = pthread_create(&t, &a, updater, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&a);
    return r == 0 ? 0 : -1;
}
