/* The Windows handle table.
 *
 * HANDLE values are small multiples of 4, as on Windows. Each one refers to
 * a kernel-object stand-in: a file descriptor, a directory search, a thread,
 * a child process, or a synchronisation object. All waits go through one
 * process-wide mutex and condition variable, which keeps
 * WaitForMultipleObjects simple and correct.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _GNU_SOURCE
#include "native.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_HANDLES 65536

static struct lsiw_handle_obj *g_table[MAX_HANDLES];
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wait_cond;
static HANDLE g_std[3];

void lsiw_wait_lock(void)   { pthread_mutex_lock(&g_wait_lock); }
void lsiw_wait_unlock(void) { pthread_mutex_unlock(&g_wait_lock); }
void lsiw_wait_broadcast(void) { pthread_cond_broadcast(&g_wait_cond); }

int lsiw_wait_cond(const struct timespec *deadline)
{
    if (!deadline) return pthread_cond_wait(&g_wait_cond, &g_wait_lock);
    return pthread_cond_timedwait(&g_wait_cond, &g_wait_lock, deadline);
}

static inline uintptr_t idx_to_h(int i) { return (uintptr_t)(i + 1) * 4; }
static inline int h_to_idx(HANDLE h)
{
    uintptr_t v = (uintptr_t)h;
    if (!v || (v & 3) || v / 4 > MAX_HANDLES) return -1;
    return (int)(v / 4) - 1;
}

HANDLE lsiw_handle_new(int type, struct lsiw_handle_obj **out)
{
    struct lsiw_handle_obj *o = calloc(1, sizeof *o);
    o->type = type;
    o->refs = 1;
    o->fd = -1;
    pthread_mutex_lock(&g_table_lock);
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!g_table[i]) {
            g_table[i] = o;
            pthread_mutex_unlock(&g_table_lock);
            if (out) *out = o;
            return (HANDLE)idx_to_h(i);
        }
    }
    pthread_mutex_unlock(&g_table_lock);
    free(o);
    lsiw_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
    return NULL;
}

struct lsiw_handle_obj *lsiw_handle_get(HANDLE h, int type)
{
    int i = h_to_idx(h);
    if (i < 0) return NULL;
    struct lsiw_handle_obj *o = g_table[i];
    if (!o || (type >= 0 && o->type != type)) return NULL;
    return o;
}

static void destroy(struct lsiw_handle_obj *o)
{
    if (o->dir) closedir(o->dir);     /* H_FIND, or a folder enumerated by handle */
    if ((o->type == H_FILE || o->type == H_MAPPING || o->type == H_PROCESS) && o->fd > 2) close(o->fd);
    if (o->free_data) o->free_data(o->data);
    free(o->path);
    free(o->pattern);
    free(o->dir_path);
    /* Thread objects are freed by the thread itself once it has exited. */
    if (o->type == H_THREAD && !o->signaled) return;
    free(o->pthread);
    free(o);
}

int lsiw_handle_close(HANDLE h)
{
    int i = h_to_idx(h);
    if (i < 0) return 0;
    pthread_mutex_lock(&g_table_lock);
    struct lsiw_handle_obj *o = g_table[i];
    if (!o) { pthread_mutex_unlock(&g_table_lock); return 0; }
    g_table[i] = NULL;
    for (int k = 0; k < 3; k++) if (g_std[k] == h) g_std[k] = NULL;
    int last = --o->refs <= 0;
    pthread_mutex_unlock(&g_table_lock);
    if (last) destroy(o);
    return 1;
}

HANDLE lsiw_handle_dup(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, -1);
    if (!o) return NULL;
    if (o->type == H_FILE) {
        struct lsiw_handle_obj *n;
        HANDLE nh = lsiw_handle_new(H_FILE, &n);
        if (!nh) return NULL;
        n->fd = fcntl(o->fd, F_DUPFD_CLOEXEC, 0);
        n->path = o->path ? strdup(o->path) : NULL;
        n->is_console = o->is_console;
        n->append = o->append;
        return nh;
    }
    pthread_mutex_lock(&g_table_lock);
    o->refs++;
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!g_table[i]) {
            g_table[i] = o;
            pthread_mutex_unlock(&g_table_lock);
            return (HANDLE)idx_to_h(i);
        }
    }
    o->refs--;
    pthread_mutex_unlock(&g_table_lock);
    return NULL;
}

HANDLE lsiw_handle_from_fd(int fd, int console)
{
    struct lsiw_handle_obj *o;
    HANDLE h = lsiw_handle_new(H_FILE, &o);
    if (h) { o->fd = fd; o->is_console = console; }
    return h;
}

int lsiw_fd_from_handle(HANDLE h)
{
    struct lsiw_handle_obj *o = lsiw_handle_get(h, H_FILE);
    return o ? o->fd : -1;
}

HANDLE lsiw_std_handle(int which) { return (which >= 0 && which < 3) ? g_std[which] : NULL; }
void lsiw_std_handle_set(int which, HANDLE h) { if (which >= 0 && which < 3) g_std[which] = h; }

void lsiw_handles_init(void)
{
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_REALTIME);
    pthread_cond_init(&g_wait_cond, &ca);
    pthread_condattr_destroy(&ca);
    for (int i = 0; i < 3; i++) g_std[i] = lsiw_handle_from_fd(i, isatty(i));
}
