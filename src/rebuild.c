#include "rebuild.h"
#include "config.h"
#include "state.h"
#include "metadata.h"
#include "parity.h"
#include "lr_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <getopt.h>
#include <limits.h>

/*--------------------------------------------------------------------
 * Create parent directories for real_file_path (simple mkdir -p).
 *------------------------------------------------------------------*/
static void mkdirs_for_rebuild(const char *real_file_path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", real_file_path);

    /* Find parent */
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = '\0';

    /* Walk components left to right, creating each missing one */
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
 * Reconstruct one file from parity onto its drive path.
 * Returns 0 on success, -1 on failure.
 *------------------------------------------------------------------*/
static int rebuild_one_file(lr_state *s, unsigned drive_idx, lr_file *f,
                             void *block_buf)
{
    uint32_t bs = s->cfg.block_size;

    /* Ensure parent directories exist */
    mkdirs_for_rebuild(f->real_path);

    mode_t create_mode = (f->mode & 07777) ? (f->mode & 07777) : 0644;
    int fd = open(f->real_path, O_WRONLY | O_CREAT | O_TRUNC, create_mode);
    if (fd < 0) {
        fprintf(stderr, "rebuild:   cannot create '%s': %s\n",
                f->real_path, strerror(errno));
        return -1;
    }

    int ok = 1;
    for (uint32_t blk = 0; blk < f->block_count; blk++) {
        uint32_t pos = f->parity_pos_start + blk;

        pthread_rwlock_rdlock(&s->state_lock);
        int rc = parity_recover_block(s, drive_idx, pos, block_buf);
        pthread_rwlock_unlock(&s->state_lock);

        if (rc != 0) {
            fprintf(stderr, "rebuild:   parity_recover_block failed at pos %u\n", pos);
            ok = 0;
            break;
        }

        /* Last block: only write bytes within the actual file size */
        size_t write_len = bs;
        if (blk == f->block_count - 1 && f->size > 0) {
            size_t tail = (size_t)(f->size % bs);
            if (tail != 0)
                write_len = tail;
        }

        ssize_t n = pwrite(fd, block_buf, write_len, (off_t)blk * bs);
        if (n != (ssize_t)write_len) {
            fprintf(stderr, "rebuild:   pwrite failed at block %u: %s\n",
                    blk, strerror(errno));
            ok = 0;
            break;
        }
    }

    close(fd);

    if (!ok) {
        unlink(f->real_path); /* remove partial file */
        return -1;
    }

    /* Restore file metadata */
    if (f->mode & 07777)
        chmod(f->real_path, f->mode & 07777);

    if (f->uid != 0 || f->gid != 0) {
        /* Best-effort: may fail if not running as root */
        if (lchown(f->real_path, f->uid, f->gid) != 0) {}
    }

    if (f->mtime_sec != 0) {
        struct timespec ts[2];
        ts[0].tv_sec  = f->mtime_sec;
        ts[0].tv_nsec = f->mtime_nsec;
        ts[1].tv_sec  = f->mtime_sec;
        ts[1].tv_nsec = f->mtime_nsec;
        utimensat(AT_FDCWD, f->real_path, ts, 0);
    }

    return 0;
}

/*--------------------------------------------------------------------
 * Iterate all files on drive_idx and reconstruct each from parity.
 *------------------------------------------------------------------*/
static int do_rebuild(lr_state *s, unsigned drive_idx)
{
    uint32_t bs = s->cfg.block_size;

    /* Count files on this drive */
    unsigned total = 0;
    lr_list_node *node = lr_list_head(&s->file_list);
    while (node) {
        lr_file *f = (lr_file *)node->data;
        if (f->drive_idx == drive_idx)
            total++;
        node = node->next;
    }

    fprintf(stderr, "rebuild: drive '%s' (%s) — %u file(s) to reconstruct\n",
            s->drives[drive_idx].name, s->drives[drive_idx].dir, total);

    if (total == 0) {
        fprintf(stderr, "rebuild: nothing to do\n");
        return 0;
    }

    void *block_buf = malloc(bs);
    if (!block_buf) {
        fprintf(stderr, "rebuild: out of memory\n");
        return 1;
    }

    unsigned rebuilt = 0, failed = 0;
    node = lr_list_head(&s->file_list);
    while (node) {
        lr_file *f = (lr_file *)node->data;
        node = node->next;

        if (f->drive_idx != drive_idx)
            continue;

        if (rebuild_one_file(s, drive_idx, f, block_buf) == 0) {
            rebuilt++;
            fprintf(stderr, "rebuild: [%u/%u] OK   %s\n",
                    rebuilt + failed, total, f->vpath);
        } else {
            failed++;
            fprintf(stderr, "rebuild: [%u/%u] FAIL %s\n",
                    rebuilt + failed, total, f->vpath);
        }
    }

    free(block_buf);

    fprintf(stderr, "rebuild: complete — %u rebuilt, %u failed\n",
            rebuilt, failed);
    return (failed > 0) ? 1 : 0;
}

/*--------------------------------------------------------------------
 * Attempt rebuild via the running process's control socket.
 *
 * Returns:
 *   0  — all files rebuilt successfully
 *   1  — some files failed
 *  -2  — no live process listening (fall through to offline rebuild)
 *------------------------------------------------------------------*/
static int try_live_rebuild(const char *sock_path, const char *drive_name)
{
    struct sockaddr_un sa;
    if (strlen(sock_path) >= sizeof(sa.sun_path))
        return -2;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -2;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, sock_path, strlen(sock_path) + 1);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -2;
    }

    /* Send command */
    char cmd[256];
    int  n = snprintf(cmd, sizeof(cmd), "rebuild %s\n", drive_name);
    if (write(fd, cmd, (size_t)n) != n) {
        close(fd);
        return -2;
    }

    /* Read and print streaming response */
    int had_failures = 0;
    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return -2;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
        fflush(stdout);
        if (strncmp(line, "done ", 5) == 0) {
            unsigned r = 0, f = 0;
            sscanf(line + 5, "%u %u", &r, &f);
            if (f > 0)
                had_failures = 1;
        } else if (strncmp(line, "error ", 6) == 0) {
            had_failures = 1;
        }
    }
    fclose(fp);  /* also closes fd */

    return had_failures ? 1 : 0;
}

/*--------------------------------------------------------------------
 * Entry point: parse -c CONFIG -d DRIVE_NAME and run rebuild.
 *------------------------------------------------------------------*/
int cmd_rebuild(int argc, char *argv[])
{
    char *config_path = NULL;
    char *drive_name  = NULL;
    int   opt;

    optind = 1; /* reset getopt state */
    while ((opt = getopt(argc, argv, "c:d:")) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'd': drive_name  = optarg; break;
        default:
            fprintf(stderr,
                    "Usage: liveraid rebuild -c CONFIG -d DRIVE_NAME\n");
            return 1;
        }
    }

    if (!config_path || !drive_name) {
        fprintf(stderr,
                "rebuild: -c CONFIG and -d DRIVE_NAME are required\n"
                "Usage: liveraid rebuild -c CONFIG -d DRIVE_NAME\n");
        return 1;
    }

    /* Load config */
    lr_config *cfg = calloc(1, sizeof(lr_config));
    if (!cfg) {
        fprintf(stderr, "rebuild: out of memory\n");
        return 1;
    }
    if (config_load(config_path, cfg) != 0) {
        fprintf(stderr, "rebuild: cannot load config '%s'\n", config_path);
        free(cfg);
        return 1;
    }

    /* Try live rebuild if a running liveraid process is listening */
    if (cfg->content_count > 0) {
        char sock_path[PATH_MAX + 8];
        snprintf(sock_path, sizeof(sock_path), "%s.ctrl",
                 cfg->content_paths[0]);
        int lr = try_live_rebuild(sock_path, drive_name);
        if (lr >= 0) {
            free(cfg);
            return lr;
        }
        /* lr == -2: no live process, fall through to offline rebuild */
    }

    /* Initialise state */
    lr_state *s = calloc(1, sizeof(lr_state));
    if (!s) {
        fprintf(stderr, "rebuild: out of memory\n");
        free(cfg);
        return 1;
    }
    if (state_init(s, cfg) != 0) {
        fprintf(stderr, "rebuild: state_init failed\n");
        free(cfg);
        free(s);
        return 1;
    }
    free(cfg);

    /* Load metadata (file table) */
    if (metadata_load(s) != 0) {
        fprintf(stderr, "rebuild: metadata_load failed\n");
        state_done(s);
        free(s);
        return 1;
    }

    /* Find drive index */
    unsigned drive_idx = (unsigned)-1;
    for (unsigned i = 0; i < s->drive_count; i++) {
        if (strcmp(s->drives[i].name, drive_name) == 0) {
            drive_idx = i;
            break;
        }
    }
    if (drive_idx == (unsigned)-1) {
        fprintf(stderr, "rebuild: drive '%s' not found in config\n",
                drive_name);
        state_done(s);
        free(s);
        return 1;
    }

    /* Open parity */
    lr_parity_handle *ph = calloc(1, sizeof(lr_parity_handle));
    if (!ph) {
        fprintf(stderr, "rebuild: out of memory\n");
        state_done(s);
        free(s);
        return 1;
    }
    if (parity_open(ph, &s->cfg) != 0) {
        fprintf(stderr, "rebuild: cannot open parity files\n");
        free(ph);
        state_done(s);
        free(s);
        return 1;
    }
    s->parity = ph;

    int rc = do_rebuild(s, drive_idx);

    parity_close(ph);
    free(ph);
    s->parity = NULL;
    state_done(s);
    free(s);
    return rc;
}
