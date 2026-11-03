/* Play time of the programs of the library.
 *
 * Every start of a library program (`lsiw programs run ID`, an LSIW
 * shortcut (.lwlnk), LSIW Control's Launch button, an applications-menu
 * shortcut) is a session. The `lsiw` process that starts the program
 * stays behind as a small watcher (src/cli/library.c) and records here:
 *
 *   <data>/playtime/ID          total, launches, first and last played...
 *   <data>/playtime/ID.run.PID  one per running session (pid, boot, start)
 *
 * Time is credited while the program runs, every LSIW_PLAYTIME_TICK
 * seconds and when it ends, so a crash of the program, of the watcher or
 * of the whole computer loses at most one tick. Elapsed time is measured
 * on the monotonic clock, which stops while the computer sleeps, and a
 * watermark (`credited`, a wall-clock time) makes two sessions of the
 * same program at once count once. A .run file whose watcher is gone
 * (killed, power loss, another boot) is stale: it is removed when the
 * records are read, and the result of that session is "unknown".
 *
 * All writes happen under an flock(2) of <data>/playtime/.lock, through a
 * temporary file and rename(2).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsiw.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct lsiw_session {
    char *id;
    char *run_file;
    long long start;         /* wall clock, seconds  */
    long long last_wall;     /* wall clock of the last credit */
    double last_mono;        /* monotonic clock of the last credit */
};

int lsiw_playtime_enabled(void)
{
    const char *e = getenv("LSIW_NO_PLAYTIME");
    if (e && *e && strcmp(e, "0")) return 0;
    const char *v = lsiw_setting_get("programs", "track_playtime", "true");
    return !(v && (!strcasecmp(v, "false") || !strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcmp(v, "0")));
}

char *lsiw_playtime_dir(void)
{
    return lsiw_strdup_printf("%s/playtime", lsiw_data_dir());
}

/* Ids are made of [a-z0-9-] by lsiw_program_new, but a .lwlnk can carry
 * anything: never let one name a file outside the folder. */
static int safe_id(const char *id)
{
    if (!id || !*id || *id == '.' || strlen(id) > 200) return 0;
    for (const char *c = id; *c; c++)
        if (*c == '/' || (unsigned char)*c < 32) return 0;
    return 1;
}

static char *record_path(const char *id)
{
    char *dir = lsiw_playtime_dir();
    char *p = lsiw_strdup_printf("%s/%s", dir, id);
    free(dir);
    return p;
}

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static char *boot_id(void)
{
    char *t = lsiw_read_text("/proc/sys/kernel/random/boot_id");
    if (!t) return strdup("");
    t[strcspn(t, "\r\n")] = 0;
    return t;
}

/* Field 22 of /proc/PID/stat: when the process started, in clock ticks
 * since boot. Tells a live watcher from a new process that got its pid. */
static unsigned long long proc_start(long pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    char *t = lsiw_read_text(path);
    if (!t) return 0;
    unsigned long long v = 0;
    char *p = strrchr(t, ')');
    if (p) {
        int field = 2;
        for (p++; *p && field < 22; p++)
            if (*p == ' ') field++;
        if (field == 22) v = strtoull(p, NULL, 10);
    }
    free(t);
    return v;
}

static int lock_dir(void)
{
    char *dir = lsiw_playtime_dir();
    lsiw_mkdirs(dir, 0755);
    char *lp = lsiw_strdup_printf("%s/.lock", dir);
    int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    free(lp);
    free(dir);
    if (fd >= 0) while (flock(fd, LOCK_EX) < 0 && errno == EINTR) ;
    return fd;
}

static void unlock_dir(int fd)
{
    if (fd >= 0) close(fd);
}

static const char *kv(const char *text, const char *key, char *buf, size_t size)
{
    size_t kl = strlen(key);
    for (const char *l = text; l && *l; ) {
        const char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l) : strlen(l);
        if (len > kl && !strncmp(l, key, kl) && l[kl] == '=') {
            size_t vl = len - kl - 1;
            if (vl >= size) vl = size - 1;
            memcpy(buf, l + kl + 1, vl);
            buf[vl] = 0;
            return buf;
        }
        l = e ? e + 1 : NULL;
    }
    return NULL;
}

static long long kv_ll(const char *text, const char *key)
{
    char b[64];
    return kv(text, key, b, sizeof b) ? strtoll(b, NULL, 10) : 0;
}

static void read_record(const char *id, struct lsiw_playtime *pt, long long *credited)
{
    memset(pt, 0, sizeof *pt);
    if (credited) *credited = 0;
    char *path = record_path(id);
    char *t = lsiw_read_text(path);
    free(path);
    if (!t) return;
    pt->total = kv_ll(t, "total");
    pt->launches = kv_ll(t, "launches");
    pt->first = kv_ll(t, "first");
    pt->last_start = kv_ll(t, "last_start");
    pt->last_end = kv_ll(t, "last_end");
    pt->last_session = kv_ll(t, "last_session");
    pt->unclean = kv_ll(t, "unclean");
    if (!kv(t, "last_result", pt->last_result, sizeof pt->last_result)) pt->last_result[0] = 0;
    if (credited) *credited = kv_ll(t, "credited");
    free(t);
    if (pt->total < 0) pt->total = 0;
}

static int write_record(const char *id, const struct lsiw_playtime *pt, long long credited)
{
    char *path = record_path(id);
    char *tmp = lsiw_strdup_printf("%s.%ld.tmp", path, (long)getpid());
    FILE *f = fopen(tmp, "w");
    int rc = -1;
    if (f) {
        fprintf(f, "# LSIW play time of the program '%s' (seconds; times since 1970)\n", id);
        fprintf(f, "total=%lld\nlaunches=%lld\nfirst=%lld\nlast_start=%lld\nlast_end=%lld\n"
                   "last_session=%lld\nlast_result=%s\nunclean=%lld\ncredited=%lld\n",
                pt->total, pt->launches, pt->first, pt->last_start, pt->last_end,
                pt->last_session, pt->last_result, pt->unclean, credited);
        if (fclose(f) == 0 && rename(tmp, path) == 0) rc = 0;
    }
    if (rc < 0) unlink(tmp);
    free(tmp);
    free(path);
    return rc;
}

/* A .run file of a watcher that is gone. Must hold the lock. */
static int run_file_stale(const char *path, const char *boot)
{
    char *t = lsiw_read_text(path);
    if (!t) return 0;
    char b[64];
    long pid = (long)kv_ll(t, "pid");
    unsigned long long st = (unsigned long long)kv_ll(t, "pstart");
    int stale = 0;
    if (!kv(t, "boot", b, sizeof b) || strcmp(b, boot)) stale = 1;
    else if (pid <= 0 || (kill((pid_t)pid, 0) < 0 && errno == ESRCH)) stale = 1;
    else if (st && proc_start(pid) != st) stale = 1;
    free(t);
    return stale;
}

/* Count the live sessions of `id` (all ids when NULL: returns 0) and
 * remove stale ones, marking their record. Must hold the lock. */
static int sweep(const char *only)
{
    char *dir = lsiw_playtime_dir();
    DIR *d = opendir(dir);
    if (!d) { free(dir); return 0; }
    char *boot = boot_id();
    int live = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char *dot = strstr(e->d_name, ".run.");
        if (!dot || e->d_name[0] == '.') continue;
        size_t il = (size_t)(dot - e->d_name);
        if (only && (strlen(only) != il || strncmp(only, e->d_name, il))) continue;
        char *path = lsiw_strdup_printf("%s/%s", dir, e->d_name);
        if (run_file_stale(path, boot)) {
            char *id = strndup(e->d_name, il);
            struct lsiw_playtime pt;
            long long credited;
            read_record(id, &pt, &credited);
            pt.unclean++;
            snprintf(pt.last_result, sizeof pt.last_result, "unknown");
            write_record(id, &pt, credited);
            free(id);
            unlink(path);
        } else live++;
        free(path);
    }
    closedir(d);
    free(boot);
    free(dir);
    return live;
}

int lsiw_playtime_get(const char *id, struct lsiw_playtime *out)
{
    memset(out, 0, sizeof *out);
    if (!safe_id(id)) return -1;
    char *dir = lsiw_playtime_dir();
    struct stat st;
    int have = stat(dir, &st) == 0;
    free(dir);
    if (!have) return 0;
    int fd = lock_dir();
    int live = sweep(id);
    read_record(id, out, NULL);
    unlock_dir(fd);
    out->running = live;
    return 0;
}

void lsiw_playtime_reset(const char *id)
{
    if (!safe_id(id)) return;
    char *dir = lsiw_playtime_dir();
    struct stat st;
    int have = stat(dir, &st) == 0;
    free(dir);
    if (!have) return;
    int fd = lock_dir();
    char *path = record_path(id);
    unlink(path);
    free(path);
    unlock_dir(fd);
}

long long lsiw_playtime_stamp(void)
{
    char *dir = lsiw_playtime_dir();
    struct stat st;
    long long v = 0;
    if (stat(dir, &st) == 0) v = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    free(dir);
    return v;
}

struct lsiw_session *lsiw_playtime_begin(const char *id)
{
    if (!safe_id(id) || !lsiw_playtime_enabled()) return NULL;
    int fd = lock_dir();
    if (fd < 0) return NULL;
    sweep(id);
    struct lsiw_session *s = calloc(1, sizeof *s);
    s->id = strdup(id);
    s->start = s->last_wall = (long long)time(NULL);
    s->last_mono = mono_now();
    char *dir = lsiw_playtime_dir();
    s->run_file = lsiw_strdup_printf("%s/%s.run.%ld", dir, id, (long)getpid());
    free(dir);
    char *boot = boot_id();
    FILE *f = fopen(s->run_file, "w");
    if (f) {
        fprintf(f, "pid=%ld\npstart=%llu\nboot=%s\nstart=%lld\n", (long)getpid(),
                proc_start((long)getpid()), boot, s->start);
        fclose(f);
    }
    free(boot);
    struct lsiw_playtime pt;
    long long credited;
    read_record(id, &pt, &credited);
    pt.launches++;
    if (!pt.first) pt.first = s->start;
    pt.last_start = s->start;
    pt.last_session = 0;
    snprintf(pt.last_result, sizeof pt.last_result, "running");
    write_record(id, &pt, credited);
    unlock_dir(fd);
    return s;
}

/* Credit the time since the last credit, minus what another session of
 * the same program already credited. Must hold the lock. */
static void credit(struct lsiw_session *s, const char *result)
{
    long long now = (long long)time(NULL);
    double mono = mono_now();
    long long elapsed = (long long)(mono - s->last_mono + 0.5);
    struct lsiw_playtime pt;
    long long credited;
    read_record(s->id, &pt, &credited);
    long long from = s->last_wall > credited ? s->last_wall : credited;
    long long wall = now - from;
    long long add = elapsed < wall ? elapsed : wall;
    if (add < 0) add = 0;
    pt.total += add;
    if (now > credited) credited = now;
    if (now >= s->start) pt.last_end = now;
    pt.last_session = now - s->start;
    if (pt.last_session < 0) pt.last_session = 0;
    if (result) snprintf(pt.last_result, sizeof pt.last_result, "%s", result);
    write_record(s->id, &pt, credited);
    s->last_wall = now;
    s->last_mono = mono;
}

void lsiw_playtime_tick(struct lsiw_session *s)
{
    if (!s) return;
    int fd = lock_dir();
    credit(s, NULL);
    unlock_dir(fd);
}

void lsiw_playtime_end(struct lsiw_session *s, const char *result)
{
    if (!s) return;
    int fd = lock_dir();
    unlink(s->run_file);
    /* Another session of the same program still running keeps the
     * "running" result. */
    credit(s, sweep(s->id) ? NULL : result);
    unlock_dir(fd);
    free(s->run_file);
    free(s->id);
    free(s);
}

char *lsiw_playtime_format(long long seconds)
{
    if (seconds < 0) seconds = 0;
    if (seconds < 60) return lsiw_strdup_printf("%lld s", seconds);
    long long m = seconds / 60;
    if (m < 60) return lsiw_strdup_printf("%lld min", m);
    long long h = m / 60;
    if (h < 100) return lsiw_strdup_printf("%lld h %02lld min", h, m % 60);
    return lsiw_strdup_printf("%lld h", h);
}

char *lsiw_playtime_when(long long t)
{
    if (t <= 0) return strdup("never");
    time_t now = time(NULL), tt = (time_t)t;
    struct tm a, b;
    localtime_r(&now, &a);
    localtime_r(&tt, &b);
    char buf[64];
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) strftime(buf, sizeof buf, "today %H:%M", &b);
    else if ((a.tm_year == b.tm_year && a.tm_yday == b.tm_yday + 1) ||
             (a.tm_year == b.tm_year + 1 && a.tm_yday == 0 && b.tm_mon == 11 && b.tm_mday == 31))
        strftime(buf, sizeof buf, "yesterday %H:%M", &b);
    else strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &b);
    return strdup(buf);
}
