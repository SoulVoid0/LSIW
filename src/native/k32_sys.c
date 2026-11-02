/*
 * LSIW - kernel32: process and thread snapshots (Toolhelp32), job objects,
 * process/thread attribute lists, locale enumeration and time zones.
 *
 * Copyright (c) 2026 The LSIW authors. AGPL-3.0-or-later.
 */
#define _GNU_SOURCE
#include "native.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ERROR_BAD_LENGTH_         24
#define ERROR_NO_MORE_FILES_      18
#define ERROR_INSUFFICIENT_BUFFER_ 122
#define ERROR_GEN_FAILURE_        31
#define ERROR_PRIVILEGE_NOT_HELD_ 1314

#define CURRENT_PROCESS ((HANDLE)(intptr_t)-1)

typedef struct { WORD year, month, dow, day, hour, minute, second, ms; } SYSTEMTIME;
typedef int64_t LPARAM;
typedef int64_t LONG_PTR;
typedef uint16_t USHORT;

/* ================================================================== */
/* Toolhelp32 snapshots                                                 */
/* ================================================================== */

#define TH32CS_SNAPHEAPLIST 0x1
#define TH32CS_SNAPPROCESS  0x2
#define TH32CS_SNAPTHREAD   0x4
#define TH32CS_SNAPMODULE   0x8
#define TH32CS_SNAPMODULE32 0x10

struct snap_proc { DWORD pid, ppid, threads; char exe[260]; };
struct snap_thread { DWORD tid, pid; };
struct snap_mod { void *base; DWORD size; char name[260]; char path[520]; };

struct snapshot {
    DWORD owner;                       /* process the module list belongs to */
    struct snap_proc *procs; int nprocs, iproc;
    struct snap_thread *threads; int nthreads, ithread;
    struct snap_mod *mods; int nmods, imod;
};

static void snapshot_free(void *p)
{
    struct snapshot *s = p;
    free(s->procs); free(s->threads); free(s->mods); free(s);
}

static void copy_str(char *dst, size_t n, const char *src)
{
    size_t len = strnlen(src, n - 1);
    memcpy(dst, src, len);
    dst[len] = 0;
}

static char *read_small(const char *path, size_t *len)
{
    FILE *f = fopen(path, "re");
    if (!f) return NULL;
    char *buf = malloc(4097);
    size_t n = fread(buf, 1, 4096, f);
    fclose(f);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

/* /proc/<pid>/stat: parent pid and thread count (fields 4 and 20). */
static int proc_stat(int pid, DWORD *ppid, DWORD *threads, char comm[32])
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    char *s = read_small(path, NULL);
    if (!s) return 0;
    char *l = strchr(s, '('), *r = strrchr(s, ')');
    if (!l || !r) { free(s); return 0; }
    size_t n = (size_t)(r - l - 1);
    if (n > 31) n = 31;
    memcpy(comm, l + 1, n);
    comm[n] = 0;
    /* After ") ": state ppid pgrp session tty tpgid flags minflt cminflt
     * majflt cmajflt utime stime cutime cstime priority nice num_threads */
    long v[18] = { 0 };
    char state;
    sscanf(r + 2, "%c %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld",
           &state, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
           &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16]);
    free(s);
    if (state == 'Z' || state == 'X') return 0;     /* exited */
    *ppid = (DWORD)v[0];
    *threads = (DWORD)v[16];
    return 1;
}

static const char *base_name(const char *p)
{
    const char *b = p;
    for (const char *c = p; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

static int has_win_ext(const char *s)
{
    size_t n = strlen(s);
    return n > 4 && (!strcasecmp(s + n - 4, ".exe") || !strcasecmp(s + n - 4, ".com") ||
                     !strcasecmp(s + n - 4, ".bat") || !strcasecmp(s + n - 4, ".cmd"));
}

/* The executable name Windows programs expect for a process: for processes
 * running LSIW itself, the Windows program it runs (found in its command
 * line); for other Linux processes, the name of their binary. */
static void proc_exe_name(int pid, const char *comm, const char *self_exe, char out[260])
{
    char path[64], link[4096];
    snprintf(path, sizeof path, "/proc/%d/exe", pid);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n > 0) {
        link[n] = 0;
        char *del = strstr(link, " (deleted)");
        if (del) *del = 0;
        if (self_exe[0] && !strcmp(link, self_exe)) {
            snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
            size_t len;
            char *cmd = read_small(path, &len);
            if (cmd) {
                for (size_t i = 0; i < len; i += strlen(cmd + i) + 1)
                    if (has_win_ext(cmd + i)) { copy_str(out, 260, base_name(cmd + i)); free(cmd); return; }
                free(cmd);
            }
        }
        copy_str(out, 260, base_name(link));
        return;
    }
    copy_str(out, 260, comm);
}

static void snap_processes(struct snapshot *s, int want_threads)
{
    char self_exe[4096] = "";
    ssize_t n = readlink("/proc/self/exe", self_exe, sizeof self_exe - 1);
    self_exe[n > 0 ? n : 0] = 0;
    DIR *d = opendir("/proc");
    if (!d) return;
    int cap = 0, tcap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        DWORD ppid, threads;
        char comm[32];
        if (!proc_stat(pid, &ppid, &threads, comm)) continue;
        if (s->nprocs == cap) { cap = cap ? cap * 2 : 256; s->procs = realloc(s->procs, (size_t)cap * sizeof *s->procs); }
        struct snap_proc *p = &s->procs[s->nprocs++];
        p->pid = (DWORD)pid; p->ppid = ppid; p->threads = threads;
        proc_exe_name(pid, comm, self_exe, p->exe);
        if (!want_threads) continue;
        char tpath[64];
        snprintf(tpath, sizeof tpath, "/proc/%d/task", pid);
        DIR *td = opendir(tpath);
        if (!td) continue;
        struct dirent *te;
        while ((te = readdir(td))) {
            if (!isdigit((unsigned char)te->d_name[0])) continue;
            if (s->nthreads == tcap) { tcap = tcap ? tcap * 2 : 1024; s->threads = realloc(s->threads, (size_t)tcap * sizeof *s->threads); }
            s->threads[s->nthreads].tid = (DWORD)atoi(te->d_name);
            s->threads[s->nthreads].pid = (DWORD)pid;
            s->nthreads++;
        }
        closedir(td);
    }
    closedir(d);
}

static void snap_modules(struct snapshot *s, DWORD pid)
{
    if (pid == (DWORD)getpid()) {
        int cap = 0;
        for (int pass = 0; pass < 2; pass++)          /* the executable comes first, as on Windows */
            for (struct lsiw_module *m = lsiw_modules; m; m = m->next) {
                if ((pass == 0) != (m->is_exe != 0)) continue;
                if (s->nmods == cap) { cap = cap ? cap * 2 : 32; s->mods = realloc(s->mods, (size_t)cap * sizeof *s->mods); }
                struct snap_mod *x = &s->mods[s->nmods++];
                x->base = m->base;
                x->size = (DWORD)m->size;
                copy_str(x->name, sizeof x->name, m->name);
                if (m->win_path) copy_str(x->path, sizeof x->path, m->win_path);
                else snprintf(x->path, sizeof x->path, "C:\\windows\\system32\\%s", m->name);
            }
        return;
    }
    /* Another process: only its executable is known. */
    char comm[32] = "", self_exe[4096] = "";
    DWORD ppid, threads;
    if (!proc_stat((int)pid, &ppid, &threads, comm)) return;
    ssize_t n = readlink("/proc/self/exe", self_exe, sizeof self_exe - 1);
    self_exe[n > 0 ? n : 0] = 0;
    s->mods = calloc(1, sizeof *s->mods);
    s->nmods = 1;
    proc_exe_name((int)pid, comm, self_exe, s->mods[0].name);
    copy_str(s->mods[0].path, sizeof s->mods[0].name, s->mods[0].name);
}

API HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid)
{
    struct snapshot *s = calloc(1, sizeof *s);
    if (!pid) pid = (DWORD)getpid();
    s->owner = pid;
    if (flags & (TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD)) snap_processes(s, (flags & TH32CS_SNAPTHREAD) != 0);
    if (flags & (TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32)) {
        if (pid != (DWORD)getpid() && kill((pid_t)pid, 0) < 0 && errno == ESRCH) {
            snapshot_free(s);
            lsiw_set_last_error(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }
        snap_modules(s, pid);
    }
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_SNAPSHOT, &o);
    if (!h) { snapshot_free(s); return INVALID_HANDLE_VALUE; }
    o->data = s;
    o->free_data = snapshot_free;
    return h;
}

static struct snapshot *snap_of(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_SNAPSHOT);
    if (!o) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    return o->data;
}

/* PROCESSENTRY32(W): dwSize, cntUsage, th32ProcessID, th32DefaultHeapID,
 * th32ModuleID, cntThreads, th32ParentProcessID, pcPriClassBase, dwFlags,
 * szExeFile at offset 44. */
static BOOL process_entry(HANDLE h, void *pe, int first, int wide)
{
    struct snapshot *s = snap_of(h);
    if (!s) return FALSE;
    uint8_t *e = pe;
    if (*(DWORD *)e < 44 + (wide ? 2 : 1)) { lsiw_set_last_error(ERROR_BAD_LENGTH_); return FALSE; }
    if (first) s->iproc = 0;
    if (s->iproc >= s->nprocs) { lsiw_set_last_error(ERROR_NO_MORE_FILES_); return FALSE; }
    struct snap_proc *p = &s->procs[s->iproc++];
    DWORD size = *(DWORD *)e;
    memset(e + 4, 0, size - 4);
    *(DWORD *)(e + 8) = p->pid;
    *(DWORD *)(e + 28) = p->threads;
    *(DWORD *)(e + 32) = p->ppid;
    *(LONG *)(e + 36) = 8;                               /* NORMAL_PRIORITY_CLASS base */
    size_t room = (size - 44) / (wide ? 2 : 1);
    if (room > 260) room = 260;
    if (wide) lsiw_utf8_to_utf16(p->exe, -1, (WCHAR *)(e + 44), (int)room);
    else copy_str((char *)e + 44, room, p->exe);
    return TRUE;
}
API BOOL Process32FirstW(HANDLE h, void *pe) { return process_entry(h, pe, 1, 1); }
API BOOL Process32NextW(HANDLE h, void *pe)  { return process_entry(h, pe, 0, 1); }
API BOOL Process32First(HANDLE h, void *pe)  { return process_entry(h, pe, 1, 0); }
API BOOL Process32Next(HANDLE h, void *pe)   { return process_entry(h, pe, 0, 0); }

/* THREADENTRY32: dwSize, cntUsage, th32ThreadID, th32OwnerProcessID,
 * tpBasePri, tpDeltaPri, dwFlags (28 bytes). */
static BOOL thread_entry(HANDLE h, void *te, int first)
{
    struct snapshot *s = snap_of(h);
    if (!s) return FALSE;
    DWORD *e = te;
    if (e[0] < 16) { lsiw_set_last_error(ERROR_BAD_LENGTH_); return FALSE; }
    if (first) s->ithread = 0;
    if (s->ithread >= s->nthreads) { lsiw_set_last_error(ERROR_NO_MORE_FILES_); return FALSE; }
    struct snap_thread *t = &s->threads[s->ithread++];
    DWORD size = e[0];
    memset(e + 1, 0, size - 4);
    e[2] = t->tid;
    e[3] = t->pid;
    if (size >= 20) e[4] = 8;                            /* THREAD_PRIORITY_NORMAL base */
    return TRUE;
}
API BOOL Thread32First(HANDLE h, void *te) { return thread_entry(h, te, 1); }
API BOOL Thread32Next(HANDLE h, void *te)  { return thread_entry(h, te, 0); }

/* MODULEENTRY32(W): dwSize, th32ModuleID, th32ProcessID, GlblcntUsage,
 * ProccntUsage, modBaseAddr@24, modBaseSize@32, hModule@40, szModule@48
 * (256 chars), szExePath (260 chars). */
static BOOL module_entry(HANDLE h, void *me, int first, int wide)
{
    struct snapshot *s = snap_of(h);
    if (!s) return FALSE;
    uint8_t *e = me;
    DWORD need = wide ? 1080 : 568;
    if (*(DWORD *)e < need) { lsiw_set_last_error(ERROR_BAD_LENGTH_); return FALSE; }
    if (first) s->imod = 0;
    if (s->imod >= s->nmods) { lsiw_set_last_error(ERROR_NO_MORE_FILES_); return FALSE; }
    struct snap_mod *m = &s->mods[s->imod++];
    memset(e + 4, 0, need - 4);
    *(DWORD *)(e + 4) = 1;
    *(DWORD *)(e + 8) = s->owner;
    *(DWORD *)(e + 12) = 0xFFFF;
    *(DWORD *)(e + 16) = 0xFFFF;
    *(void **)(e + 24) = m->base;
    *(DWORD *)(e + 32) = m->size;
    *(void **)(e + 40) = m->base;
    if (wide) {
        lsiw_utf8_to_utf16(m->name, -1, (WCHAR *)(e + 48), 256);
        lsiw_utf8_to_utf16(m->path, -1, (WCHAR *)(e + 48 + 512), 260);
    } else {
        copy_str((char *)e + 48, 256, m->name);
        copy_str((char *)e + 48 + 256, 260, m->path);
    }
    return TRUE;
}
API BOOL Module32FirstW(HANDLE h, void *me) { return module_entry(h, me, 1, 1); }
API BOOL Module32NextW(HANDLE h, void *me)  { return module_entry(h, me, 0, 1); }
API BOOL Module32First(HANDLE h, void *me)  { return module_entry(h, me, 1, 0); }
API BOOL Module32Next(HANDLE h, void *me)   { return module_entry(h, me, 0, 0); }

/* Heaps are not exposed as separate lists. */
API BOOL Heap32ListFirst(HANDLE h, void *hl) { (void)h; (void)hl; lsiw_set_last_error(ERROR_NO_MORE_FILES_); return FALSE; }
API BOOL Heap32ListNext(HANDLE h, void *hl)  { (void)h; (void)hl; lsiw_set_last_error(ERROR_NO_MORE_FILES_); return FALSE; }
API BOOL Heap32First(void *he, DWORD pid, ULONG_PTR id) { (void)he; (void)pid; (void)id; lsiw_set_last_error(ERROR_NO_MORE_FILES_); return FALSE; }
API BOOL Heap32Next(void *he) { (void)he; lsiw_set_last_error(ERROR_NO_MORE_FILES_); return FALSE; }
API BOOL Toolhelp32ReadProcessMemory(DWORD pid, const void *base, void *buf, SIZE_T n, SIZE_T *done)
{
    if (pid && pid != (DWORD)getpid()) { if (done) *done = 0; lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
    memcpy(buf, base, n);
    if (done) *done = n;
    return TRUE;
}

/* ================================================================== */
/* Job objects                                                          */
/* ================================================================== */

#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x2000

struct job {
    pthread_mutex_t lock;
    int *pids; int npids, cap;
    DWORD total;                  /* processes ever assigned */
    uint8_t limit[64];            /* JOBOBJECT_BASIC_LIMIT_INFORMATION */
    uint8_t ext[48 + 32];         /* IO_COUNTERS + memory limits (extended info tail) */
    char *name;
    int refs;
};

static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct job *g_named_jobs[64];
static struct job *g_self_job;    /* the job this process was assigned to */

static DWORD job_flags(struct job *j) { return *(DWORD *)(j->limit + 16); }

static void job_release(void *p)
{
    struct job *j = p;
    pthread_mutex_lock(&g_jobs_lock);
    int last = --j->refs <= 0;
    if (last) {
        for (int i = 0; i < 64; i++) if (g_named_jobs[i] == j) g_named_jobs[i] = NULL;
        if (g_self_job == j) g_self_job = NULL;
    }
    pthread_mutex_unlock(&g_jobs_lock);
    if (!last) return;
    /* Closing the last handle of a kill-on-close job ends its processes. */
    if (job_flags(j) & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
        for (int i = 0; i < j->npids; i++)
            if (j->pids[i] != getpid()) kill(j->pids[i], SIGKILL);
    free(j->pids);
    free(j->name);
    pthread_mutex_destroy(&j->lock);
    free(j);
}

static HANDLE job_handle(struct job *j)
{
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_JOB, &o);
    if (!h) return NULL;
    o->data = j;
    o->free_data = job_release;
    return h;
}

static HANDLE create_job(const char *name, int open_only)
{
    pthread_mutex_lock(&g_jobs_lock);
    if (name && *name)
        for (int i = 0; i < 64; i++)
            if (g_named_jobs[i] && !strcmp(g_named_jobs[i]->name, name)) {
                struct job *j = g_named_jobs[i];
                j->refs++;
                pthread_mutex_unlock(&g_jobs_lock);
                HANDLE h = job_handle(j);
                if (!h) { job_release(j); return NULL; }
                lsiw_set_last_error(open_only ? 0 : ERROR_ALREADY_EXISTS);
                return h;
            }
    if (open_only) { pthread_mutex_unlock(&g_jobs_lock); lsiw_set_last_error(ERROR_FILE_NOT_FOUND); return NULL; }
    struct job *j = calloc(1, sizeof *j);
    pthread_mutex_init(&j->lock, NULL);
    j->refs = 1;
    if (name && *name) {
        j->name = strdup(name);
        for (int i = 0; i < 64; i++) if (!g_named_jobs[i]) { g_named_jobs[i] = j; break; }
    }
    pthread_mutex_unlock(&g_jobs_lock);
    HANDLE h = job_handle(j);
    if (!h) { job_release(j); return NULL; }
    lsiw_set_last_error(0);
    return h;
}

API HANDLE CreateJobObjectA(void *sa, const char *name) { (void)sa; return create_job(name, 0); }
API HANDLE CreateJobObjectW(void *sa, const WCHAR *name)
{ (void)sa; char *n = name ? lsiw_w2u(name) : NULL; HANDLE h = create_job(n, 0); free(n); return h; }
API HANDLE OpenJobObjectA(DWORD access, BOOL inherit, const char *name) { (void)access; (void)inherit; return create_job(name, 1); }
API HANDLE OpenJobObjectW(DWORD access, BOOL inherit, const WCHAR *name)
{ (void)access; (void)inherit; char *n = name ? lsiw_w2u(name) : NULL; HANDLE h = create_job(n, 1); free(n); return h; }

static struct job *job_of(HANDLE h)
{
    if (!h) return g_self_job;
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_JOB);
    return o ? o->data : NULL;
}

static int pid_of_process(HANDLE p)
{
    if (p == CURRENT_PROCESS) return getpid();
    struct lsiw_handle_obj *o = lsiw_handle_get(p, H_PROCESS);
    return o ? o->pid : -1;
}

API BOOL AssignProcessToJobObject(HANDLE job, HANDLE process)
{
    struct job *j = job ? job_of(job) : NULL;
    int pid = pid_of_process(process);
    if (!j || pid <= 0) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&j->lock);
    if (j->npids == j->cap) { j->cap = j->cap ? j->cap * 2 : 8; j->pids = realloc(j->pids, (size_t)j->cap * sizeof(int)); }
    j->pids[j->npids++] = pid;
    j->total++;
    pthread_mutex_unlock(&j->lock);
    if (pid == getpid()) {
        pthread_mutex_lock(&g_jobs_lock);
        if (!g_self_job) { g_self_job = j; j->refs++; }
        pthread_mutex_unlock(&g_jobs_lock);
    }
    return TRUE;
}

API BOOL IsProcessInJob(HANDLE process, HANDLE job, BOOL *result)
{
    int pid = pid_of_process(process);
    if (pid <= 0 || !result) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    *result = FALSE;
    if (!job) { *result = pid == getpid() ? g_self_job != NULL : FALSE; return TRUE; }
    struct job *j = job_of(job);
    if (!j) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&j->lock);
    for (int i = 0; i < j->npids; i++) if (j->pids[i] == pid) *result = TRUE;
    pthread_mutex_unlock(&j->lock);
    return TRUE;
}

static int job_active(struct job *j)
{
    int n = 0;
    for (int i = 0; i < j->npids; i++) {
        DWORD ppid, threads; char comm[32];
        if (j->pids[i] == getpid() || proc_stat(j->pids[i], &ppid, &threads, comm)) n++;
    }
    return n;
}

API BOOL QueryInformationJobObject(HANDLE job, int cls, void *buf, DWORD len, DWORD *ret)
{
    struct job *j = job_of(job);
    if (job && !j) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    if (!j && cls != 15) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    DWORD need;
    switch (cls) {
    case 1:  need = 48; break;    /* JobObjectBasicAccountingInformation */
    case 2:  need = 64; break;    /* JobObjectBasicLimitInformation */
    case 3:  need = 8; break;     /* JobObjectBasicProcessIdList (header) */
    case 8:  need = 96; break;    /* JobObjectBasicAndIoAccountingInformation */
    case 9:  need = 144; break;   /* JobObjectExtendedLimitInformation */
    case 15: need = 8; break;     /* JobObjectCpuRateControlInformation */
    default: lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE;
    }
    if (len < need) { lsiw_set_last_error(ERROR_BAD_LENGTH_); return FALSE; }
    memset(buf, 0, need);
    if (!j) { if (ret) *ret = need; return TRUE; }   /* no job: no CPU rate limit */
    pthread_mutex_lock(&j->lock);
    uint8_t *b = buf;
    switch (cls) {
    case 1: case 8:
        *(DWORD *)(b + 36) = j->total;
        *(DWORD *)(b + 40) = (DWORD)job_active(j);
        *(DWORD *)(b + 44) = j->total - *(DWORD *)(b + 40);
        break;
    case 2:
        memcpy(b, j->limit, 64);
        break;
    case 3: {
        DWORD fit = (len - 8) / sizeof(ULONG_PTR), k = 0;
        *(DWORD *)b = (DWORD)j->npids;
        for (int i = 0; i < j->npids && k < fit; i++) ((ULONG_PTR *)(b + 8))[k++] = (ULONG_PTR)j->pids[i];
        *(DWORD *)(b + 4) = k;
        need = 8 + k * (DWORD)sizeof(ULONG_PTR);
        if (k < (DWORD)j->npids) { pthread_mutex_unlock(&j->lock); if (ret) *ret = need; lsiw_set_last_error(ERROR_MORE_DATA); return FALSE; }
        break; }
    case 9:
        memcpy(b, j->limit, 64);
        memcpy(b + 64, j->ext, sizeof j->ext);
        break;
    }
    pthread_mutex_unlock(&j->lock);
    if (ret) *ret = need;
    return TRUE;
}

API BOOL SetInformationJobObject(HANDLE job, int cls, void *buf, DWORD len)
{
    struct job *j = job ? job_of(job) : NULL;
    if (!j) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&j->lock);
    BOOL ok = TRUE;
    switch (cls) {
    case 2: if (len < 64) ok = FALSE; else memcpy(j->limit, buf, 64); break;
    case 9: if (len < 144) ok = FALSE; else { memcpy(j->limit, buf, 64); memcpy(j->ext, (uint8_t *)buf + 64, sizeof j->ext); } break;
    case 4:  /* JobObjectBasicUIRestrictions */
    case 7:  /* JobObjectAssociateCompletionPortInformation */
    case 15: /* JobObjectCpuRateControlInformation */
    case 18: /* JobObjectGroupInformation */
        break;   /* accepted; not enforced */
    default: lsiw_set_last_error(ERROR_INVALID_PARAMETER); ok = FALSE; pthread_mutex_unlock(&j->lock); return FALSE;
    }
    pthread_mutex_unlock(&j->lock);
    if (!ok) lsiw_set_last_error(ERROR_BAD_LENGTH_);
    return ok;
}

API BOOL TerminateJobObject(HANDLE job, UINT code)
{
    struct job *j = job ? job_of(job) : NULL;
    if (!j) { lsiw_set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&j->lock);
    int self = 0;
    for (int i = 0; i < j->npids; i++) {
        if (j->pids[i] == getpid()) self = 1;
        else kill(j->pids[i], SIGKILL);
    }
    pthread_mutex_unlock(&j->lock);
    if (self) lsiw_exit_process(code);
    return TRUE;
}

/* ================================================================== */
/* Process/thread attribute lists                                       */
/* ================================================================== */

struct attr_entry { ULONG_PTR attr; SIZE_T size; void *value; };
struct attr_list { DWORD max, count; ULONG_PTR reserved[2]; struct attr_entry e[]; };

API BOOL InitializeProcThreadAttributeList(struct attr_list *l, DWORD count, DWORD flags, SIZE_T *size)
{
    if (!size || flags) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    SIZE_T need = sizeof(struct attr_list) + (SIZE_T)count * sizeof(struct attr_entry);
    if (!l || *size < need) { *size = need; lsiw_set_last_error(ERROR_INSUFFICIENT_BUFFER_); return FALSE; }
    memset(l, 0, need);
    l->max = count;
    *size = need;
    return TRUE;
}

API BOOL UpdateProcThreadAttribute(struct attr_list *l, DWORD flags, ULONG_PTR attr, void *value, SIZE_T size,
                                   void *prev, SIZE_T *ret)
{
    (void)prev; (void)ret;
    if (!l || flags) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (DWORD i = 0; i < l->count; i++)
        if (l->e[i].attr == attr) { l->e[i].value = value; l->e[i].size = size; return TRUE; }
    if (l->count >= l->max) { lsiw_set_last_error(ERROR_GEN_FAILURE_); return FALSE; }
    l->e[l->count++] = (struct attr_entry){ attr, size, value };
    return TRUE;
}

API void DeleteProcThreadAttributeList(struct attr_list *l) { (void)l; }

/* ================================================================== */
/* Locales, priority boost, computer name                               */
/* ================================================================== */

typedef BOOL (WINAPI *locale_proc_w)(WCHAR *);
typedef BOOL (WINAPI *locale_proc_a)(char *);
typedef BOOL (WINAPI *locale_proc_ex)(WCHAR *, DWORD, LPARAM);
typedef BOOL (WINAPI *uilang_proc_w)(WCHAR *, LONG_PTR);
typedef BOOL (WINAPI *uilang_proc_a)(char *, LONG_PTR);

/* LSIW presents a single locale, en-US (LCID 0x0409), everywhere. */
API BOOL EnumSystemLocalesW(locale_proc_w proc, DWORD flags)
{ (void)flags; if (!proc) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; } WCHAR s[] = { '0','0','0','0','0','4','0','9',0 }; proc(s); return TRUE; }
API BOOL EnumSystemLocalesA(locale_proc_a proc, DWORD flags)
{ (void)flags; if (!proc) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; } char s[] = "00000409"; proc(s); return TRUE; }
API BOOL EnumSystemLocalesEx(locale_proc_ex proc, DWORD flags, LPARAM lp, void *reserved)
{
    (void)reserved;
    if (!proc) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    WCHAR spec[] = { 'e','n','-','U','S',0 }, neutral[] = { 'e','n',0 };
    if (!flags || (flags & 0x21)) { if (!proc(spec, 0x21 /* LOCALE_WINDOWS|LOCALE_SPECIFICDATA */, lp)) return TRUE; }
    if (flags & 0x10) proc(neutral, 0x11 /* LOCALE_WINDOWS|LOCALE_NEUTRALDATA */, lp);
    return TRUE;
}
API BOOL EnumUILanguagesW(uilang_proc_w proc, DWORD flags, LONG_PTR lp)
{
    if (!proc) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    WCHAR name[] = { 'e','n','-','U','S',0 }, id[] = { '0','4','0','9',0 };
    proc((flags & 0x8 /* MUI_LANGUAGE_NAME */) ? name : id, lp);
    return TRUE;
}
API BOOL EnumUILanguagesA(uilang_proc_a proc, DWORD flags, LONG_PTR lp)
{
    if (!proc) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    char name[] = "en-US", id[] = "0409";
    proc((flags & 0x8) ? name : id, lp);
    return TRUE;
}

/* Linux has no priority boost; report it as enabled and accept changes. */
API BOOL SetProcessPriorityBoost(HANDLE p, BOOL disable) { (void)p; (void)disable; return TRUE; }
API BOOL GetProcessPriorityBoost(HANDLE p, BOOL *disabled) { (void)p; if (disabled) *disabled = FALSE; return TRUE; }
API BOOL GetThreadPriorityBoost(HANDLE t, BOOL *disabled) { (void)t; if (disabled) *disabled = FALSE; return TRUE; }

/* Renaming the machine needs administrator rights on Windows too. */
API BOOL SetComputerNameA(const char *n) { (void)n; lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
API BOOL SetComputerNameW(const WCHAR *n) { (void)n; lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
API BOOL SetComputerNameExA(int t, const char *n) { (void)t; (void)n; lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }
API BOOL SetComputerNameExW(int t, const WCHAR *n) { (void)t; (void)n; lsiw_set_last_error(ERROR_ACCESS_DENIED); return FALSE; }

/* ================================================================== */
/* Time zones                                                           */
/* ================================================================== */

#define EPOCH_DIFF_ 116444736000000000ULL

/* TIME_ZONE_INFORMATION (172 bytes) and DYNAMIC_TIME_ZONE_INFORMATION (432). */
struct tzinfo { LONG bias; WCHAR std_name[32]; SYSTEMTIME std_date; LONG std_bias; WCHAR dl_name[32]; SYSTEMTIME dl_date; LONG dl_bias; };
struct dyn_tzinfo { struct tzinfo tz; WCHAR key[128]; BYTE dyn_disabled; BYTE pad[3]; };

/* IANA zone -> Windows time zone key (CLDR windowsZones, main mappings). */
static const struct { const char *iana, *win; } g_zones[] = {
    { "Etc/GMT+12", "Dateline Standard Time" }, { "Etc/GMT+11", "UTC-11" }, { "Pacific/Pago_Pago", "UTC-11" },
    { "Pacific/Honolulu", "Hawaiian Standard Time" }, { "America/Anchorage", "Alaskan Standard Time" },
    { "America/Los_Angeles", "Pacific Standard Time" }, { "America/Vancouver", "Pacific Standard Time" },
    { "America/Tijuana", "Pacific Standard Time (Mexico)" }, { "America/Phoenix", "US Mountain Standard Time" },
    { "America/Denver", "Mountain Standard Time" }, { "America/Edmonton", "Mountain Standard Time" },
    { "America/Boise", "Mountain Standard Time" }, { "America/Chihuahua", "Mountain Standard Time (Mexico)" },
    { "America/Mazatlan", "Mountain Standard Time (Mexico)" }, { "America/Guatemala", "Central America Standard Time" },
    { "America/Chicago", "Central Standard Time" }, { "America/Winnipeg", "Central Standard Time" },
    { "America/Mexico_City", "Central Standard Time (Mexico)" }, { "America/Regina", "Canada Central Standard Time" },
    { "America/Bogota", "SA Pacific Standard Time" }, { "America/Lima", "SA Pacific Standard Time" },
    { "America/New_York", "Eastern Standard Time" }, { "America/Toronto", "Eastern Standard Time" },
    { "America/Detroit", "Eastern Standard Time" }, { "America/Indianapolis", "US Eastern Standard Time" },
    { "America/Indiana/Indianapolis", "US Eastern Standard Time" }, { "America/Havana", "Cuba Standard Time" },
    { "America/Caracas", "Venezuela Standard Time" }, { "America/Halifax", "Atlantic Standard Time" },
    { "America/La_Paz", "SA Western Standard Time" }, { "America/Santiago", "Pacific SA Standard Time" },
    { "America/St_Johns", "Newfoundland Standard Time" }, { "America/Sao_Paulo", "E. South America Standard Time" },
    { "America/Buenos_Aires", "Argentina Standard Time" }, { "America/Argentina/Buenos_Aires", "Argentina Standard Time" },
    { "America/Montevideo", "Montevideo Standard Time" }, { "Etc/GMT+2", "UTC-02" }, { "Atlantic/Azores", "Azores Standard Time" },
    { "Atlantic/Cape_Verde", "Cape Verde Standard Time" }, { "Etc/UTC", "UTC" }, { "Etc/GMT", "UTC" }, { "UTC", "UTC" },
    { "GMT", "UTC" }, { "Etc/Universal", "UTC" }, { "Europe/London", "GMT Standard Time" }, { "Europe/Dublin", "GMT Standard Time" },
    { "Europe/Lisbon", "GMT Standard Time" }, { "Atlantic/Reykjavik", "Greenwich Standard Time" },
    { "Africa/Casablanca", "Morocco Standard Time" }, { "Europe/Berlin", "W. Europe Standard Time" },
    { "Europe/Amsterdam", "W. Europe Standard Time" }, { "Europe/Rome", "W. Europe Standard Time" },
    { "Europe/Vienna", "W. Europe Standard Time" }, { "Europe/Zurich", "W. Europe Standard Time" },
    { "Europe/Stockholm", "W. Europe Standard Time" }, { "Europe/Oslo", "W. Europe Standard Time" },
    { "Europe/Luxembourg", "W. Europe Standard Time" }, { "Europe/Budapest", "Central Europe Standard Time" },
    { "Europe/Prague", "Central Europe Standard Time" }, { "Europe/Belgrade", "Central Europe Standard Time" },
    { "Europe/Bratislava", "Central Europe Standard Time" }, { "Europe/Ljubljana", "Central Europe Standard Time" },
    { "Europe/Paris", "Romance Standard Time" }, { "Europe/Brussels", "Romance Standard Time" },
    { "Europe/Madrid", "Romance Standard Time" }, { "Europe/Copenhagen", "Romance Standard Time" },
    { "Europe/Warsaw", "Central European Standard Time" }, { "Europe/Zagreb", "Central European Standard Time" },
    { "Europe/Sarajevo", "Central European Standard Time" }, { "Africa/Lagos", "W. Central Africa Standard Time" },
    { "Europe/Bucharest", "GTB Standard Time" }, { "Europe/Athens", "GTB Standard Time" },
    { "Asia/Beirut", "Middle East Standard Time" }, { "Africa/Cairo", "Egypt Standard Time" },
    { "Europe/Helsinki", "FLE Standard Time" }, { "Europe/Kiev", "FLE Standard Time" }, { "Europe/Kyiv", "FLE Standard Time" },
    { "Europe/Riga", "FLE Standard Time" }, { "Europe/Sofia", "FLE Standard Time" }, { "Europe/Tallinn", "FLE Standard Time" },
    { "Europe/Vilnius", "FLE Standard Time" }, { "Asia/Jerusalem", "Israel Standard Time" },
    { "Africa/Johannesburg", "South Africa Standard Time" }, { "Europe/Kaliningrad", "Kaliningrad Standard Time" },
    { "Europe/Moscow", "Russian Standard Time" }, { "Europe/Minsk", "Belarus Standard Time" },
    { "Europe/Istanbul", "Turkey Standard Time" }, { "Asia/Riyadh", "Arab Standard Time" }, { "Asia/Baghdad", "Arabic Standard Time" },
    { "Africa/Nairobi", "E. Africa Standard Time" }, { "Asia/Tehran", "Iran Standard Time" }, { "Asia/Dubai", "Arabian Standard Time" },
    { "Asia/Baku", "Azerbaijan Standard Time" }, { "Asia/Tbilisi", "Georgian Standard Time" }, { "Asia/Yerevan", "Caucasus Standard Time" },
    { "Asia/Kabul", "Afghanistan Standard Time" }, { "Asia/Tashkent", "West Asia Standard Time" },
    { "Asia/Yekaterinburg", "Ekaterinburg Standard Time" }, { "Asia/Karachi", "Pakistan Standard Time" },
    { "Asia/Calcutta", "India Standard Time" }, { "Asia/Kolkata", "India Standard Time" },
    { "Asia/Colombo", "Sri Lanka Standard Time" }, { "Asia/Katmandu", "Nepal Standard Time" }, { "Asia/Kathmandu", "Nepal Standard Time" },
    { "Asia/Almaty", "Central Asia Standard Time" }, { "Asia/Dhaka", "Bangladesh Standard Time" },
    { "Asia/Omsk", "Omsk Standard Time" }, { "Asia/Rangoon", "Myanmar Standard Time" }, { "Asia/Yangon", "Myanmar Standard Time" },
    { "Asia/Bangkok", "SE Asia Standard Time" }, { "Asia/Jakarta", "SE Asia Standard Time" },
    { "Asia/Ho_Chi_Minh", "SE Asia Standard Time" }, { "Asia/Saigon", "SE Asia Standard Time" },
    { "Asia/Novosibirsk", "N. Central Asia Standard Time" }, { "Asia/Krasnoyarsk", "North Asia Standard Time" },
    { "Asia/Shanghai", "China Standard Time" }, { "Asia/Hong_Kong", "China Standard Time" }, { "Asia/Chongqing", "China Standard Time" },
    { "Asia/Irkutsk", "North Asia East Standard Time" }, { "Asia/Singapore", "Singapore Standard Time" },
    { "Asia/Kuala_Lumpur", "Singapore Standard Time" }, { "Asia/Manila", "Singapore Standard Time" },
    { "Australia/Perth", "W. Australia Standard Time" }, { "Asia/Taipei", "Taipei Standard Time" },
    { "Asia/Ulaanbaatar", "Ulaanbaatar Standard Time" }, { "Asia/Tokyo", "Tokyo Standard Time" },
    { "Asia/Seoul", "Korea Standard Time" }, { "Asia/Yakutsk", "Yakutsk Standard Time" },
    { "Australia/Adelaide", "Cen. Australia Standard Time" }, { "Australia/Darwin", "AUS Central Standard Time" },
    { "Australia/Brisbane", "E. Australia Standard Time" }, { "Australia/Sydney", "AUS Eastern Standard Time" },
    { "Australia/Melbourne", "AUS Eastern Standard Time" }, { "Australia/Canberra", "AUS Eastern Standard Time" },
    { "Pacific/Port_Moresby", "West Pacific Standard Time" }, { "Australia/Hobart", "Tasmania Standard Time" },
    { "Asia/Vladivostok", "Vladivostok Standard Time" }, { "Asia/Magadan", "Magadan Standard Time" },
    { "Pacific/Noumea", "Central Pacific Standard Time" }, { "Pacific/Auckland", "New Zealand Standard Time" },
    { "Etc/GMT-12", "UTC+12" }, { "Pacific/Fiji", "Fiji Standard Time" }, { "Pacific/Tongatapu", "Tonga Standard Time" },
    { "Pacific/Apia", "Samoa Standard Time" }, { "Pacific/Kiritimati", "Line Islands Standard Time" },
};

/* The IANA name of the local zone: $TZ, /etc/localtime, /etc/timezone. */
static void local_zone_name(char *out, size_t n)
{
    out[0] = 0;
    const char *tz = getenv("TZ");
    if (tz && *tz) {
        if (*tz == ':') tz++;
        const char *z = strstr(tz, "zoneinfo/");
        snprintf(out, n, "%s", z ? z + 9 : tz);
        return;
    }
    char link[512];
    ssize_t r = readlink("/etc/localtime", link, sizeof link - 1);
    if (r > 0) {
        link[r] = 0;
        const char *z = strstr(link, "zoneinfo/");
        if (z) { snprintf(out, n, "%s", z + 9); return; }
    }
    FILE *f = fopen("/etc/timezone", "re");
    if (f) {
        if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
        fclose(f);
    }
}

static const char *windows_zone_key(void)
{
    char iana[256];
    local_zone_name(iana, sizeof iana);
    for (size_t i = 0; i < sizeof g_zones / sizeof g_zones[0]; i++)
        if (!strcmp(g_zones[i].iana, iana)) return g_zones[i].win;
    return NULL;
}

static long gmtoff_at(time_t t, int *isdst)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (isdst) *isdst = tm.tm_isdst > 0;
    return tm.tm_gmtoff;
}

/* Find the UTC instants in [from, to) where the local UTC offset changes. */
static int find_transitions(time_t from, time_t to, time_t *out, int max)
{
    int n = 0;
    long prev = gmtoff_at(from, NULL);
    for (time_t a = from; a < to && n < max; a += 86400) {
        time_t b = a + 86400 < to ? a + 86400 : to;
        if (gmtoff_at(b, NULL) == prev) continue;
        time_t lo = a, hi = b;                        /* offset(lo) == prev != offset(hi) */
        while (hi - lo > 1) {
            time_t mid = lo + (hi - lo) / 2;
            if (gmtoff_at(mid, NULL) == prev) lo = mid; else hi = mid;
        }
        out[n++] = hi;
        prev = gmtoff_at(hi, NULL);
    }
    return n;
}

static int days_in_month(int y, int m)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : d[m - 1];
}

/* A transition as a Windows recurring rule: month, weekday, occurrence
 * (5 = last) and the local wall-clock time just before the change. */
static void rule_for(time_t t, long offset_before, SYSTEMTIME *r)
{
    time_t wall = t + offset_before;
    struct tm tm;
    gmtime_r(&wall, &tm);
    memset(r, 0, sizeof *r);
    r->month = (WORD)(tm.tm_mon + 1);
    r->dow = (WORD)tm.tm_wday;
    int week = (tm.tm_mday - 1) / 7 + 1;
    if (tm.tm_mday + 7 > days_in_month(tm.tm_year + 1900, tm.tm_mon + 1)) week = 5;
    r->day = (WORD)week;
    r->hour = (WORD)tm.tm_hour;
    r->minute = (WORD)tm.tm_min;
    r->second = (WORD)tm.tm_sec;
}

static void set_names(struct tzinfo *tz, const char *key, int has_dst)
{
    if (key) {
        lsiw_utf8_to_utf16(key, -1, tz->std_name, 32);
        char dl[64];
        const char *s = strstr(key, "Standard");
        if (s) snprintf(dl, sizeof dl, "%.*sDaylight%s", (int)(s - key), key, s + 8);
        else snprintf(dl, sizeof dl, "%s", key);
        lsiw_utf8_to_utf16(dl, -1, tz->dl_name, 32);
        return;
    }
    lsiw_utf8_to_utf16(tzname[0] && *tzname[0] ? tzname[0] : "UTC", -1, tz->std_name, 32);
    lsiw_utf8_to_utf16(has_dst && tzname[1] && *tzname[1] ? tzname[1] : (tzname[0] ? tzname[0] : "UTC"), -1, tz->dl_name, 32);
}

/* Fill TIME_ZONE_INFORMATION for `year` from the system time zone database.
 * Returns 1 if the zone observes daylight saving time that year. */
static int tz_for_year(int year, struct tzinfo *tz)
{
    tzset();
    memset(tz, 0, sizeof *tz);
    struct tm jan = { .tm_year = year - 1900, .tm_mon = 0, .tm_mday = 1 };
    time_t from = timegm(&jan) - 14 * 3600;
    struct tm next = { .tm_year = year + 1 - 1900, .tm_mon = 0, .tm_mday = 1 };
    time_t to = timegm(&next) + 14 * 3600;
    time_t tr[8];
    int n = find_transitions(from, to, tr, 8);
    long std_off = 0, dst_off = 0;
    int have_std = 0, have_dst = 0;
    time_t to_dst = 0, to_std = 0;
    for (int i = 0; i < n; i++) {
        int isdst;
        long after = gmtoff_at(tr[i], &isdst), before = gmtoff_at(tr[i] - 1, NULL);
        if (isdst && !to_dst) { to_dst = tr[i]; dst_off = after; std_off = before; have_dst = have_std = 1; }
        else if (!isdst && !to_std) { to_std = tr[i]; std_off = after; dst_off = before; have_std = have_dst = 1; }
    }
    if (!have_std) {
        /* No changes this year: the zone's offset is its standard offset. */
        struct tm mid = { .tm_year = year - 1900, .tm_mon = 6, .tm_mday = 1 };
        int isdst;
        std_off = gmtoff_at(timegm(&mid), &isdst);
        if (isdst) std_off = gmtoff_at(timegm(&jan), NULL);
    }
    const char *key = windows_zone_key();
    tz->bias = (LONG)(-std_off / 60);
    int dst = have_dst && to_dst && to_std && dst_off != std_off;
    if (dst) {
        tz->dl_bias = (LONG)(-(dst_off - std_off) / 60);
        rule_for(to_dst, std_off, &tz->dl_date);
        rule_for(to_std, dst_off, &tz->std_date);
    }
    set_names(tz, key, dst);
    return dst;
}

static int current_year(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_year + 1900;
}

API DWORD GetTimeZoneInformation(struct tzinfo *tz)
{
    if (!tz) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0xFFFFFFFFu; }
    int dst = tz_for_year(current_year(), tz);
    if (!dst) return 0;                                 /* TIME_ZONE_ID_UNKNOWN */
    int isdst;
    gmtoff_at(time(NULL), &isdst);
    return isdst ? 2 : 1;                               /* DAYLIGHT : STANDARD */
}

API DWORD GetDynamicTimeZoneInformation(struct dyn_tzinfo *d)
{
    if (!d) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return 0xFFFFFFFFu; }
    memset(d, 0, sizeof *d);
    DWORD r = GetTimeZoneInformation(&d->tz);
    const char *key = windows_zone_key();
    if (key) lsiw_utf8_to_utf16(key, -1, d->key, 128);
    else if (d->tz.bias == 0 && !d->tz.dl_bias) lsiw_utf8_to_utf16("UTC", -1, d->key, 128);
    return r;
}

API BOOL GetTimeZoneInformationForYear(USHORT year, struct dyn_tzinfo *dtzi, struct tzinfo *tz)
{
    (void)dtzi;   /* only the local zone's rules are available */
    if (!tz) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    tz_for_year(year ? year : current_year(), tz);
    return TRUE;
}

API DWORD GetDynamicTimeZoneInformationEffectiveYears(const struct dyn_tzinfo *d, DWORD *first, DWORD *last)
{
    (void)d;
    if (first) *first = 1970;
    if (last) *last = (DWORD)current_year() + 30;
    return 0;
}

API DWORD EnumDynamicTimeZoneInformation(DWORD index, struct dyn_tzinfo *d)
{
    if (!d) return ERROR_INVALID_PARAMETER;
    if (index) return ERROR_NO_MORE_ITEMS;
    GetDynamicTimeZoneInformation(d);
    return 0;
}

API BOOL SetTimeZoneInformation(const struct tzinfo *tz) { (void)tz; lsiw_set_last_error(ERROR_PRIVILEGE_NOT_HELD_); return FALSE; }
API BOOL SetDynamicTimeZoneInformation(const struct dyn_tzinfo *d) { (void)d; lsiw_set_last_error(ERROR_PRIVILEGE_NOT_HELD_); return FALSE; }

/* --- conversions with an explicit TIME_ZONE_INFORMATION --- */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t st_to_seconds(const SYSTEMTIME *st)
{ return days_from_civil(st->year, st->month, st->day) * 86400 + st->hour * 3600 + st->minute * 60 + st->second; }

static void seconds_to_st(int64_t s, WORD ms, SYSTEMTIME *st)
{
    int64_t days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    st->year = (WORD)(y + (m <= 2));
    st->month = (WORD)m;
    st->day = (WORD)d;
    st->dow = (WORD)((days - 719468 + 4) % 7 < 0 ? (days - 719468 + 4) % 7 + 7 : (days - 719468 + 4) % 7);
    st->hour = (WORD)(rem / 3600);
    st->minute = (WORD)(rem / 60 % 60);
    st->second = (WORD)(rem % 60);
    st->ms = ms;
}

/* Local wall-clock seconds (in the zone's standard time frame) at which a
 * recurring or absolute rule fires in `year`. */
static int64_t rule_time(const SYSTEMTIME *r, int year)
{
    SYSTEMTIME d = *r;
    if (!r->year) {
        int64_t first = days_from_civil(year, r->month, 1);
        int first_dow = (int)(((first + 4) % 7 + 7) % 7);
        int day = 1 + (r->dow - first_dow + 7) % 7 + (r->day - 1) * 7;
        while (day > days_in_month(year, r->month)) day -= 7;
        d.year = (WORD)year;
        d.day = (WORD)day;
    }
    return st_to_seconds(&d);
}

/* Is the UTC instant `utc` in daylight time under `tz`? */
static int in_dst(const struct tzinfo *tz, int64_t utc)
{
    if (!tz->dl_date.month || !tz->std_date.month) return 0;
    int64_t std_local = utc - (int64_t)tz->bias * 60;
    SYSTEMTIME y;
    seconds_to_st(std_local, 0, &y);
    int64_t start = rule_time(&tz->dl_date, y.year);                                /* in standard time */
    int64_t end = rule_time(&tz->std_date, y.year) + (int64_t)tz->dl_bias * 60;     /* daylight -> standard */
    if (start < end) return std_local >= start && std_local < end;                   /* northern hemisphere */
    return std_local >= start || std_local < end;                                    /* southern hemisphere */
}

static int64_t zone_bias_min(const struct tzinfo *tz, int64_t utc)
{ return tz->bias + (in_dst(tz, utc) ? tz->dl_bias : tz->std_bias); }

API BOOL SystemTimeToTzSpecificLocalTime(const struct tzinfo *tz, const SYSTEMTIME *u, SYSTEMTIME *l)
{
    if (!u || !l) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct tzinfo cur;
    if (!tz) { GetTimeZoneInformation(&cur); tz = &cur; }
    int64_t utc = st_to_seconds(u);
    seconds_to_st(utc - zone_bias_min(tz, utc) * 60, u->ms, l);
    return TRUE;
}
API BOOL SystemTimeToTzSpecificLocalTimeEx(const struct dyn_tzinfo *d, const SYSTEMTIME *u, SYSTEMTIME *l)
{ return SystemTimeToTzSpecificLocalTime(d ? &d->tz : NULL, u, l); }

API BOOL TzSpecificLocalTimeToSystemTime(const struct tzinfo *tz, const SYSTEMTIME *l, SYSTEMTIME *u)
{
    if (!u || !l) { lsiw_set_last_error(ERROR_INVALID_PARAMETER); return FALSE; }
    struct tzinfo cur;
    if (!tz) { GetTimeZoneInformation(&cur); tz = &cur; }
    int64_t local = st_to_seconds(l);
    /* Try the standard-time interpretation first, as Windows does. */
    int64_t utc = local + ((int64_t)tz->bias + tz->std_bias) * 60;
    if (in_dst(tz, utc)) utc = local + ((int64_t)tz->bias + tz->dl_bias) * 60;
    seconds_to_st(utc, l->ms, u);
    return TRUE;
}
API BOOL TzSpecificLocalTimeToSystemTimeEx(const struct dyn_tzinfo *d, const SYSTEMTIME *l, SYSTEMTIME *u)
{ return TzSpecificLocalTimeToSystemTime(d ? &d->tz : NULL, l, u); }

const struct lsiw_export lsiw_k32sys_exports[] = {
    E(CreateToolhelp32Snapshot), E(Process32FirstW), E(Process32NextW), E(Process32First), E(Process32Next),
    E(Thread32First), E(Thread32Next), E(Module32FirstW), E(Module32NextW), E(Module32First), E(Module32Next),
    E(Heap32ListFirst), E(Heap32ListNext), E(Heap32First), E(Heap32Next), E(Toolhelp32ReadProcessMemory),
    E(CreateJobObjectA), E(CreateJobObjectW), E(OpenJobObjectA), E(OpenJobObjectW), E(AssignProcessToJobObject),
    E(IsProcessInJob), E(QueryInformationJobObject), E(SetInformationJobObject), E(TerminateJobObject),
    E(InitializeProcThreadAttributeList), E(UpdateProcThreadAttribute), E(DeleteProcThreadAttributeList),
    E(EnumSystemLocalesW), E(EnumSystemLocalesA), E(EnumSystemLocalesEx), E(EnumUILanguagesW), E(EnumUILanguagesA),
    E(SetProcessPriorityBoost), E(GetProcessPriorityBoost), E(GetThreadPriorityBoost),
    E(SetComputerNameA), E(SetComputerNameW), E(SetComputerNameExA), E(SetComputerNameExW),
    E(GetTimeZoneInformation), E(GetDynamicTimeZoneInformation), E(GetTimeZoneInformationForYear),
    E(GetDynamicTimeZoneInformationEffectiveYears), E(EnumDynamicTimeZoneInformation),
    E(SetTimeZoneInformation), E(SetDynamicTimeZoneInformation),
    E(SystemTimeToTzSpecificLocalTime), E(SystemTimeToTzSpecificLocalTimeEx),
    E(TzSpecificLocalTimeToSystemTime), E(TzSpecificLocalTimeToSystemTimeEx),
    { NULL, NULL }
};
