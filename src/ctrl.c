#include "ctrl.h"
#include "state.h"
#include "parity.h"
#include "journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

/*--------------------------------------------------------------------
 * Send formatted string to connection fd.
 * Returns -1 if the write fails (client disconnected).
 *------------------------------------------------------------------*/
static int ctrl_send(int conn, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return 0;
    if (n >= (int)sizeof(buf))
        n = (int)sizeof(buf) - 1;
    return (write(conn, buf, (size_t)n) < 0) ? -1 : 0;
}

/*--------------------------------------------------------------------
 * mkdir -p for the parent directory of real_file_path.
 *------------------------------------------------------------------*/
static void ctrl_mkdirs(const char *real_file_path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", real_file_path);

    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

/*--------------------------------------------------------------------
 * Rebuild one file from parity while the filesystem is live.
 *
 * Looks up the file under rdlock, checks that it is not currently
 * open (open_count == 0), snapshots its metadata, then recovers
 * block-by-block while holding rdlock only around the parity call.
 *
 * Returns:
 *   0   — file rebuilt successfully
 *   1   — file skipped (busy or gone)
 *  -1   — rebuild failed (error sent to conn)
 *------------------------------------------------------------------*/
static int live_rebuild_one_file(lr_ctrl *c, int conn,
                                  unsigned drive_idx, const char *vpath)
{
    lr_state *s = c->state;
    uint32_t bs = s->cfg.block_size;

    /* --- Snapshot metadata under rdlock; check open_count --- */
    pthread_rwlock_rdlock(&s->state_lock);
    lr_file *f = state_find_file(s, vpath);
    if (!f || f->drive_idx != drive_idx) {
        pthread_rwlock_unlock(&s->state_lock);
        return 1;   /* gone or moved to another drive — skip silently */
    }
    if (f->open_count > 0) {
        pthread_rwlock_unlock(&s->state_lock);
        ctrl_send(conn, "skip %s busy\n", vpath);
        return 1;
    }

    char     real_path[PATH_MAX];
    uint32_t pos_start   = f->parity_pos_start;
    uint32_t block_count = f->block_count;
    int64_t  file_size   = f->size;
    mode_t   mode        = f->mode;
    uid_t    uid         = f->uid;
    gid_t    gid         = f->gid;
    time_t   mtime_sec   = f->mtime_sec;
    long     mtime_nsec  = f->mtime_nsec;
    snprintf(real_path, sizeof(real_path), "%s", f->real_path);
    pthread_rwlock_unlock(&s->state_lock);

    /* --- Create parent dirs --- */
    ctrl_mkdirs(real_path);

    /* --- Open/truncate output file --- */
    mode_t create_mode = (mode & 07777) ? (mode & 07777) : 0644;
    int fd = open(real_path, O_WRONLY | O_CREAT | O_TRUNC, create_mode);
    if (fd < 0) {
        ctrl_send(conn, "fail %s cannot create: %s\n", vpath, strerror(errno));
        return -1;
    }

    void *block_buf = malloc(bs);
    if (!block_buf) {
        close(fd);
        unlink(real_path);
        ctrl_send(conn, "fail %s out of memory\n", vpath);
        return -1;
    }

    /* --- Recover blocks --- */
    int ok = 1;
    for (uint32_t blk = 0; blk < block_count; blk++) {
        uint32_t pos = pos_start + blk;

        pthread_rwlock_rdlock(&s->state_lock);
        int rc = parity_recover_block(s, drive_idx, pos, block_buf);
        pthread_rwlock_unlock(&s->state_lock);

        if (rc != 0) {
            ctrl_send(conn, "fail %s parity error at block %u\n", vpath, blk);
            ok = 0;
            break;
        }

        size_t write_len = bs;
        if (blk == block_count - 1 && file_size > 0) {
            size_t tail = (size_t)(file_size % bs);
            if (tail != 0)
                write_len = tail;
        }

        ssize_t n = pwrite(fd, block_buf, write_len, (off_t)blk * bs);
        if (n != (ssize_t)write_len) {
            ctrl_send(conn, "fail %s write error at block %u: %s\n",
                      vpath, blk, strerror(errno));
            ok = 0;
            break;
        }
    }

    free(block_buf);
    close(fd);

    if (!ok) {
        unlink(real_path);
        return -1;
    }

    /* --- Restore metadata --- */
    if (mode & 07777)
        chmod(real_path, mode & 07777);
    if (uid != 0 || gid != 0)
        if (lchown(real_path, uid, gid) != 0) {}
    if (mtime_sec != 0) {
        struct timespec ts[2];
        ts[0].tv_sec  = mtime_sec;  ts[0].tv_nsec = mtime_nsec;
        ts[1].tv_sec  = mtime_sec;  ts[1].tv_nsec = mtime_nsec;
        utimensat(AT_FDCWD, real_path, ts, 0);
    }

    ctrl_send(conn, "ok %s\n", vpath);
    return 0;
}

/*--------------------------------------------------------------------
 * Rebuild all files on drive_name; stream progress to conn.
 *------------------------------------------------------------------*/
static void live_do_rebuild(lr_ctrl *c, int conn, const char *drive_name)
{
    lr_state *s = c->state;

    /* --- Find drive index under rdlock --- */
    pthread_rwlock_rdlock(&s->state_lock);
    unsigned drive_idx = (unsigned)-1;
    for (unsigned i = 0; i < s->drive_count; i++) {
        if (strcmp(s->drives[i].name, drive_name) == 0) {
            drive_idx = i;
            break;
        }
    }
    if (drive_idx == (unsigned)-1) {
        pthread_rwlock_unlock(&s->state_lock);
        ctrl_send(conn, "error drive '%s' not found\n", drive_name);
        return;
    }

    /* Count files on this drive and snapshot their vpaths */
    unsigned total = 0;
    lr_list_node *node = lr_list_head(&s->file_list);
    while (node) {
        lr_file *f = (lr_file *)node->data;
        if (f->drive_idx == drive_idx)
            total++;
        node = node->next;
    }

    char (*vpaths)[PATH_MAX] = NULL;
    if (total > 0) {
        vpaths = malloc((size_t)total * PATH_MAX);
        if (!vpaths) {
            pthread_rwlock_unlock(&s->state_lock);
            ctrl_send(conn, "error out of memory\n");
            return;
        }
        unsigned idx = 0;
        node = lr_list_head(&s->file_list);
        while (node && idx < total) {
            lr_file *f = (lr_file *)node->data;
            if (f->drive_idx == drive_idx)
                snprintf(vpaths[idx++], PATH_MAX, "%s", f->vpath);
            node = node->next;
        }
        total = idx;
    }
    pthread_rwlock_unlock(&s->state_lock);

    ctrl_send(conn, "progress 0 %u (starting)\n", total);

    unsigned rebuilt = 0, failed = 0, skipped = 0;
    for (unsigned i = 0; i < total; i++) {
        ctrl_send(conn, "progress %u %u %s\n", i + 1, total, vpaths[i]);
        int rc = live_rebuild_one_file(c, conn, drive_idx, vpaths[i]);
        if (rc == 0)
            rebuilt++;
        else if (rc > 0)
            skipped++;
        else
            failed++;
    }

    free(vpaths);

    ctrl_send(conn, "done %u %u skipped=%u\n", rebuilt, failed, skipped);
}

/*--------------------------------------------------------------------
 * Run a scrub or repair pass and stream the result to conn.
 * repair=0: verify only; repair=1: verify and fix mismatched parity.
 *------------------------------------------------------------------*/
static void live_do_scrub(lr_ctrl *c, int conn, int repair)
{
    lr_state *s = c->state;

    if (!s->parity || s->parity->levels == 0) {
        ctrl_send(conn, "error no parity configured\n");
        return;
    }

    lr_scrub_result result;
    parity_scrub(s, &result, repair);

    if (repair)
        ctrl_send(conn, "done %u %u fixed=%u errors=%u\n",
                  result.positions_checked,
                  result.parity_mismatches,
                  result.parity_fixed,
                  result.read_errors);
    else
        ctrl_send(conn, "done %u %u errors=%u\n",
                  result.positions_checked,
                  result.parity_mismatches,
                  result.read_errors);
}

/*--------------------------------------------------------------------
 * Handle one connection: read command line, dispatch.
 *------------------------------------------------------------------*/
static void handle_connection(lr_ctrl *c, int conn)
{
    char line[512];
    int  len = 0;

    /* Read one line (up to newline or buffer full) */
    while (len < (int)sizeof(line) - 1) {
        ssize_t n = read(conn, line + len, 1);
        if (n <= 0)
            break;
        if (line[len] == '\n') {
            line[len] = '\0';
            break;
        }
        len++;
    }
    line[len] = '\0';

    /* Strip trailing CR */
    while (len > 0 && line[len - 1] == '\r')
        line[--len] = '\0';

    if (strncmp(line, "rebuild ", 8) == 0)
        live_do_rebuild(c, conn, line + 8);
    else if (strcmp(line, "scrub repair") == 0)
        live_do_scrub(c, conn, 1);
    else if (strcmp(line, "scrub") == 0)
        live_do_scrub(c, conn, 0);
    else
        ctrl_send(conn, "error unknown command\n");
}

/*--------------------------------------------------------------------
 * Accept thread
 *------------------------------------------------------------------*/
static void *ctrl_thread(void *arg)
{
    lr_ctrl *c = (lr_ctrl *)arg;

    while (c->running) {
        int conn = accept(c->sock_fd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINVAL || errno == EBADF || errno == ENOTSOCK)
                break;  /* socket was closed by ctrl_stop */
            if (errno == EINTR)
                continue;
            break;
        }
        handle_connection(c, conn);
        close(conn);
    }
    return NULL;
}

/*--------------------------------------------------------------------
 * Public API
 *------------------------------------------------------------------*/

int ctrl_start(lr_ctrl *c, struct lr_state *s)
{
    c->state   = s;
    c->running = 1;
    c->sock_fd = -1;

    if (s->cfg.content_count == 0)
        return -1;

    snprintf(c->sock_path, sizeof(c->sock_path), "%s.ctrl",
             s->cfg.content_paths[0]);

    /* Verify socket path fits in sockaddr_un.sun_path */
    struct sockaddr_un sa;
    if (strlen(c->sock_path) >= sizeof(sa.sun_path))
        return -1;

    c->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->sock_fd < 0)
        return -1;

    unlink(c->sock_path);   /* remove stale socket if present */

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, c->sock_path, strlen(c->sock_path) + 1);

    if (bind(c->sock_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(c->sock_fd, 4) != 0) {
        close(c->sock_fd);
        c->sock_fd = -1;
        return -1;
    }

    if (pthread_create(&c->thread, NULL, ctrl_thread, c) != 0) {
        close(c->sock_fd);
        c->sock_fd = -1;
        unlink(c->sock_path);
        return -1;
    }

    return 0;
}

void ctrl_stop(lr_ctrl *c)
{
    if (c->sock_fd < 0)
        return;

    c->running = 0;
    shutdown(c->sock_fd, SHUT_RDWR);
    close(c->sock_fd);
    c->sock_fd = -1;

    pthread_join(c->thread, NULL);
    unlink(c->sock_path);
}
