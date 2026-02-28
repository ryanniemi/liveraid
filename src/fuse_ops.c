#define FUSE_USE_VERSION 35
#include <fuse.h>

/* Sentinel value for fi->fh when the file's drive is unavailable but
 * parity recovery is possible.  (uint64_t)-1 = UINT64_MAX; when cast
 * to int it is -1, so close(-1) is never accidentally called. */
#define LR_DEAD_DRIVE_FH  ((uint64_t)-1)

#include "fuse_ops.h"
#include "state.h"
#include "metadata.h"
#include "journal.h"
#include "parity.h"
#include "ctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

/*--------------------------------------------------------------------
 * Helper: build the real path for vpath on a given drive.
 * Works for both files and directories; vpath may be "/".
 * Caller must hold state_lock (at minimum rdlock) before calling.
 *------------------------------------------------------------------*/
static void real_path_on_drive(lr_state *s, unsigned drive_idx,
                                const char *vpath,
                                char *real, size_t real_sz)
{
    const char *rel = vpath[0] == '/' ? vpath + 1 : vpath;
    if (rel[0] == '\0')
        snprintf(real, real_sz, "%s", s->drives[drive_idx].dir);
    else
        snprintf(real, real_sz, "%s%s", s->drives[drive_idx].dir, rel);
}

/*--------------------------------------------------------------------
 * Helper: is vpath a directory prefix for any known file?
 * Caller must hold state_lock.
 *------------------------------------------------------------------*/
static int is_virtual_dir(lr_state *s, const char *vpath)
{
    if (strcmp(vpath, "/") == 0)
        return 1;

    size_t vlen = strlen(vpath);
    lr_list_node *node = lr_list_head(&s->file_list);
    while (node) {
        lr_file *f = (lr_file *)node->data;
        if (strncmp(f->vpath, vpath, vlen) == 0) {
            char c = f->vpath[vlen];
            if (c == '/' || c == '\0')
                return 1;
        }
        node = node->next;
    }
    return 0;
}

/*--------------------------------------------------------------------
 * Helper: is vpath a directory — either a virtual prefix or an actual
 * directory present on at least one data drive (covers empty dirs).
 * Caller must hold state_lock.
 *------------------------------------------------------------------*/
static int is_any_dir(lr_state *s, const char *vpath)
{
    if (is_virtual_dir(s, vpath))
        return 1;
    for (unsigned i = 0; i < s->drive_count; i++) {
        char real[PATH_MAX];
        real_path_on_drive(s, i, vpath, real, sizeof(real));
        struct stat st;
        if (lstat(real, &st) == 0 && S_ISDIR(st.st_mode))
            return 1;
    }
    return 0;
}

/*--------------------------------------------------------------------
 * Helper: find or create a dir_table entry for path, seeding metadata
 * from the first real backing directory when creating a new entry.
 * Caller must hold state_lock (wrlock).
 *------------------------------------------------------------------*/
static lr_dir *dir_get_or_create(lr_state *s, const char *path)
{
    lr_dir *d = state_find_dir(s, path);
    if (d)
        return d;

    d = calloc(1, sizeof(lr_dir));
    if (!d)
        return NULL;

    snprintf(d->vpath, PATH_MAX, "%s", path);

    for (unsigned i = 0; i < s->drive_count; i++) {
        char real[PATH_MAX];
        real_path_on_drive(s, i, path, real, sizeof(real));
        struct stat st;
        if (lstat(real, &st) == 0 && S_ISDIR(st.st_mode)) {
            d->mode       = st.st_mode;
            d->uid        = st.st_uid;
            d->gid        = st.st_gid;
            d->mtime_sec  = st.st_mtim.tv_sec;
            d->mtime_nsec = st.st_mtim.tv_nsec;
            break;
        }
    }
    if (!d->mode)
        d->mode = S_IFDIR | 0755;

    state_insert_dir(s, d);
    return d;
}

/*--------------------------------------------------------------------
 * Helper: create parent directories for real_file_path on drive_idx,
 * inheriting modes from the corresponding directory on another drive
 * when available, falling back to 0755.
 *
 * drive_dir_len is strlen(s->drives[drive_idx].dir) — used to strip
 * the drive prefix and recover the virtual path component.
 *
 * Caller must hold state_lock (at least rdlock).
 *------------------------------------------------------------------*/
static void mkdirs_p(lr_state *s, unsigned drive_idx,
                     const char *real_file_path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", real_file_path);

    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = '\0'; /* tmp is now the parent directory real path */

    size_t drive_dir_len = strlen(s->drives[drive_idx].dir);

    /* Walk from left, creating each missing component */
    for (char *p = tmp + drive_dir_len; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';

        struct stat st;
        if (lstat(tmp, &st) != 0) {
            /* Component missing — find its mode from another drive */
            mode_t mode = 0755;
            /* Virtual path = '/' + (tmp + drive_dir_len) */
            char vpath[PATH_MAX];
            snprintf(vpath, sizeof(vpath), "/%s", tmp + drive_dir_len);
            for (unsigned i = 0; i < s->drive_count; i++) {
                if (i == drive_idx) continue;
                char other[PATH_MAX];
                real_path_on_drive(s, i, vpath, other, sizeof(other));
                struct stat ost;
                if (lstat(other, &ost) == 0 && S_ISDIR(ost.st_mode)) {
                    mode = ost.st_mode & 07777;
                    break;
                }
            }
            mkdir(tmp, mode);
        }
        *p = '/';
    }

    /* Final (leaf) directory */
    struct stat st;
    if (lstat(tmp, &st) != 0) {
        mode_t mode = 0755;
        char vpath[PATH_MAX];
        snprintf(vpath, sizeof(vpath), "/%s", tmp + drive_dir_len);
        for (unsigned i = 0; i < s->drive_count; i++) {
            if (i == drive_idx) continue;
            char other[PATH_MAX];
            real_path_on_drive(s, i, vpath, other, sizeof(other));
            struct stat ost;
            if (lstat(other, &ost) == 0 && S_ISDIR(ost.st_mode)) {
                mode = ost.st_mode & 07777;
                break;
            }
        }
        mkdir(tmp, mode);
    }
}

/*--------------------------------------------------------------------
 * getattr
 *------------------------------------------------------------------*/
static int lr_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi)
{
    (void)fi;
    lr_state *s = g_state;

    memset(st, 0, sizeof(*st));

    pthread_rwlock_rdlock(&s->state_lock);

    /* Root directory */
    if (strcmp(path, "/") == 0) {
        /* Use the first drive's root stat if available */
        for (unsigned i = 0; i < s->drive_count; i++) {
            char real[PATH_MAX];
            real_path_on_drive(s, i, "/", real, sizeof(real));
            struct stat real_st;
            if (lstat(real, &real_st) == 0 && S_ISDIR(real_st.st_mode)) {
                pthread_rwlock_unlock(&s->state_lock);
                *st = real_st;
                st->st_nlink = 2;
                return 0;
            }
        }
        pthread_rwlock_unlock(&s->state_lock);
        st->st_mode  = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }

    /* Real file? */
    lr_file *f = state_find_file(s, path);
    if (f) {
        struct stat real_st;
        if (lstat(f->real_path, &real_st) == 0) {
            *st = real_st;
        } else {
            /* File in table but not on disk: use stored metadata */
            st->st_mode          = f->mode ? f->mode : (S_IFREG | 0644);
            st->st_nlink         = 1;
            st->st_size          = f->size;
            st->st_uid           = f->uid;
            st->st_gid           = f->gid;
            st->st_mtim.tv_sec   = f->mtime_sec;
            st->st_mtim.tv_nsec  = f->mtime_nsec;
        }
        pthread_rwlock_unlock(&s->state_lock);
        return 0;
    }

    /* Directory? Check dir_table first (authoritative), then real dirs. */
    if (is_any_dir(s, path)) {
        lr_dir *d = state_find_dir(s, path);
        if (d) {
            st->st_mode        = S_IFDIR | (d->mode & 07777);
            st->st_nlink       = 2;
            st->st_uid         = d->uid;
            st->st_gid         = d->gid;
            st->st_mtim.tv_sec  = d->mtime_sec;
            st->st_mtim.tv_nsec = d->mtime_nsec;
            pthread_rwlock_unlock(&s->state_lock);
            return 0;
        }
        for (unsigned i = 0; i < s->drive_count; i++) {
            char real[PATH_MAX];
            real_path_on_drive(s, i, path, real, sizeof(real));
            struct stat real_st;
            if (lstat(real, &real_st) == 0 && S_ISDIR(real_st.st_mode)) {
                pthread_rwlock_unlock(&s->state_lock);
                *st = real_st;
                st->st_nlink = 2;
                return 0;
            }
        }
        /* Virtual dir with no backing real directory */
        pthread_rwlock_unlock(&s->state_lock);
        st->st_mode  = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }

    pthread_rwlock_unlock(&s->state_lock);
    return -ENOENT;
}

/*--------------------------------------------------------------------
 * readdir
 *------------------------------------------------------------------*/
static int lr_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi;
    lr_state *s = g_state;
    int use_plus = (flags & FUSE_READDIR_PLUS) != 0;

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    size_t path_len = strlen(path);

    /* Track emitted names to avoid duplicates */
    char  **seen     = NULL;
    size_t  seen_cap = 0;
    size_t  seen_cnt = 0;

    /* Returns 1 if name was already seen, 0 otherwise (and adds it) */
#define SEEN_ADD(nm) ({                                         \
    int _found = 0;                                             \
    for (size_t _i = 0; _i < seen_cnt; _i++) {                 \
        if (strcmp(seen[_i], (nm)) == 0) { _found = 1; break; } \
    }                                                           \
    if (!_found) {                                              \
        if (seen_cnt == seen_cap) {                             \
            seen_cap = seen_cap ? seen_cap * 2 : 32;           \
            seen = realloc(seen, seen_cap * sizeof(char *));    \
        }                                                       \
        seen[seen_cnt++] = strdup(nm);                         \
    }                                                           \
    _found;                                                     \
})

    pthread_rwlock_rdlock(&s->state_lock);
    lr_list_node *node = lr_list_head(&s->file_list);

    while (node) {
        lr_file *f = (lr_file *)node->data;
        const char *fp = f->vpath;

        if (strncmp(fp, path, path_len) != 0) {
            node = node->next;
            continue;
        }

        const char *rest = fp + path_len;
        if (path_len > 1 && rest[0] != '/') {
            node = node->next;
            continue;
        }
        if (path_len == 1 && rest[0] == '/')
            rest++;
        else if (path_len > 1 && rest[0] == '/')
            rest++;

        if (rest[0] == '\0') {
            node = node->next;
            continue;
        }

        char name[NAME_MAX + 1];
        const char *slash = strchr(rest, '/');
        if (slash) {
            size_t n = (size_t)(slash - rest);
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            strncpy(name, rest, n);
            name[n] = '\0';
        } else {
            strncpy(name, rest, sizeof(name) - 1);
            name[sizeof(name)-1] = '\0';
        }

        if (!SEEN_ADD(name)) {
            struct stat st;
            struct stat *stp = NULL;
            enum fuse_fill_dir_flags fill_flags = 0;
            if (use_plus && !slash) {
                /* Direct file — fill stat from real path or stored metadata */
                memset(&st, 0, sizeof(st));
                if (lstat(f->real_path, &st) != 0) {
                    st.st_mode         = f->mode ? f->mode : (S_IFREG | 0644);
                    st.st_nlink        = 1;
                    st.st_size         = f->size;
                    st.st_uid          = f->uid;
                    st.st_gid          = f->gid;
                    st.st_mtim.tv_sec  = f->mtime_sec;
                    st.st_mtim.tv_nsec = f->mtime_nsec;
                }
                stp        = &st;
                fill_flags = FUSE_FILL_DIR_PLUS;
            }
            filler(buf, name, stp, 0, fill_flags);
        }
        node = node->next;
    }

    /* Also scan real drive directories for subdirs not in file_table
     * (e.g. empty directories created via mkdir). */
    unsigned drive_count = s->drive_count;
    pthread_rwlock_unlock(&s->state_lock);

    for (unsigned i = 0; i < drive_count; i++) {
        char real[PATH_MAX];
        pthread_rwlock_rdlock(&s->state_lock);
        real_path_on_drive(s, i, path, real, sizeof(real));
        pthread_rwlock_unlock(&s->state_lock);

        DIR *dp = opendir(real);
        if (!dp)
            continue;
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            /* Only emit directories from this pass; files are owned by file_table */
            if (de->d_type == DT_DIR || de->d_type == DT_UNKNOWN) {
                char sub[PATH_MAX];
                snprintf(sub, sizeof(sub), "%s/%s", real, de->d_name);
                struct stat st;
                int have_stat = 0;
                if (de->d_type == DT_UNKNOWN || use_plus) {
                    have_stat = (lstat(sub, &st) == 0);
                    if (de->d_type == DT_UNKNOWN &&
                        (!have_stat || !S_ISDIR(st.st_mode)))
                        continue;
                }
                if (!SEEN_ADD(de->d_name)) {
                    /* For PLUS, prefer dir_table metadata if available */
                    if (use_plus && have_stat) {
                        char subvpath[PATH_MAX];
                        snprintf(subvpath, sizeof(subvpath), "%s/%s",
                                 strcmp(path, "/") == 0 ? "" : path,
                                 de->d_name);
                        pthread_rwlock_rdlock(&s->state_lock);
                        lr_dir *dd = state_find_dir(s, subvpath);
                        if (dd) {
                            st.st_mode        = S_IFDIR | (dd->mode & 07777);
                            st.st_uid         = dd->uid;
                            st.st_gid         = dd->gid;
                            st.st_mtim.tv_sec  = dd->mtime_sec;
                            st.st_mtim.tv_nsec = dd->mtime_nsec;
                            st.st_nlink       = 2;
                        }
                        pthread_rwlock_unlock(&s->state_lock);
                    }
                    filler(buf, de->d_name,
                           (use_plus && have_stat) ? &st : NULL, 0,
                           (use_plus && have_stat) ? FUSE_FILL_DIR_PLUS : 0);
                }
            }
        }
        closedir(dp);
    }

    for (size_t i = 0; i < seen_cnt; i++)
        free(seen[i]);
    free(seen);
#undef SEEN_ADD

    return 0;
}

/*--------------------------------------------------------------------
 * open / release
 *------------------------------------------------------------------*/
static int lr_open(const char *path, struct fuse_file_info *fi)
{
    lr_state *s = g_state;

    /* Increment open_count before releasing the lock so the live-rebuild
     * thread never sees open_count == 0 while we are mid-open. */
    pthread_rwlock_rdlock(&s->state_lock);
    lr_file *f = state_find_file(s, path);
    if (!f) {
        pthread_rwlock_unlock(&s->state_lock);
        return -ENOENT;
    }
    char real[PATH_MAX];
    snprintf(real, sizeof(real), "%s", f->real_path);
    int has_parity = (s->parity != NULL && s->parity->levels > 0);
    f->open_count++;
    pthread_rwlock_unlock(&s->state_lock);

    int fd = open(real, fi->flags & ~O_CREAT);
    if (fd >= 0) {
        fi->fh = (uint64_t)fd;
        return 0;
    }

    /* Open failed.  For read-only opens, allow recovery via parity. */
    int saved = errno;
    if ((fi->flags & O_ACCMODE) == O_RDONLY &&
        (saved == ENOENT || saved == EIO || saved == ENXIO) &&
        has_parity) {
        fi->fh = LR_DEAD_DRIVE_FH;
        return 0;
    }

    /* Open failed with no recovery path — undo the open_count increment. */
    pthread_rwlock_rdlock(&s->state_lock);
    lr_file *f2 = state_find_file(s, path);
    if (f2 && f2->open_count > 0)
        f2->open_count--;
    pthread_rwlock_unlock(&s->state_lock);
    return -saved;
}

static int lr_release(const char *path, struct fuse_file_info *fi)
{
    lr_state *s = g_state;
    pthread_rwlock_rdlock(&s->state_lock);
    lr_file *f = state_find_file(s, path);
    if (f && f->open_count > 0)
        f->open_count--;
    pthread_rwlock_unlock(&s->state_lock);

    if (fi->fh != 0 && fi->fh != LR_DEAD_DRIVE_FH)
        close((int)fi->fh);
    return 0;
}

/*--------------------------------------------------------------------
 * read / write
 *------------------------------------------------------------------*/
static int lr_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    if (fi->fh != LR_DEAD_DRIVE_FH) {
        ssize_t n = pread((int)fi->fh, buf, size, offset);
        if (n >= 0)
            return (int)n;
        if (errno != EIO)
            return -errno;
    }

    /* EIO or dead-drive sentinel: attempt transparent recovery from parity */
    lr_state *s = g_state;
    pthread_rwlock_rdlock(&s->state_lock);

    lr_file *f = state_find_file(s, path);
    if (!f || !s->parity || s->parity->levels == 0) {
        pthread_rwlock_unlock(&s->state_lock);
        return -EIO;
    }

    uint32_t bs          = s->cfg.block_size;
    unsigned drive_idx   = f->drive_idx;
    uint32_t pos_start   = f->parity_pos_start;
    uint32_t block_count = f->block_count;
    int64_t  file_size   = f->size;

    if ((int64_t)offset >= file_size) {
        pthread_rwlock_unlock(&s->state_lock);
        return 0;
    }
    if ((int64_t)(offset + (off_t)size) > file_size)
        size = (size_t)(file_size - offset);

    uint32_t first_blk = (uint32_t)(offset / bs);
    uint32_t last_blk  = (uint32_t)((offset + (off_t)size - 1) / bs);

    void *tmp = malloc(bs);
    if (!tmp) {
        pthread_rwlock_unlock(&s->state_lock);
        return -ENOMEM;
    }

    ssize_t total = 0;
    for (uint32_t blk = first_blk; blk <= last_blk && blk < block_count; blk++) {
        uint32_t pos = pos_start + blk;
        if (parity_recover_block(s, drive_idx, pos, tmp) != 0) {
            free(tmp);
            pthread_rwlock_unlock(&s->state_lock);
            return total > 0 ? (int)total : -EIO;
        }
        off_t    blk_base   = (off_t)blk * bs;
        off_t    copy_start = (offset > blk_base) ? (offset - blk_base) : 0;
        size_t   copy_len   = bs - (size_t)copy_start;
        if (copy_len > size - (size_t)total)
            copy_len = size - (size_t)total;
        memcpy(buf + total, (char *)tmp + copy_start, copy_len);
        total += (ssize_t)copy_len;
    }

    free(tmp);
    pthread_rwlock_unlock(&s->state_lock);
    return (int)total;
}

/*--------------------------------------------------------------------
 * create
 *------------------------------------------------------------------*/
static int lr_create(const char *path, mode_t mode,
                     struct fuse_file_info *fi)
{
    lr_state *s = g_state;

    pthread_rwlock_wrlock(&s->state_lock);

    if (state_find_file(s, path)) {
        lr_file *f = state_find_file(s, path);
        int fd = open(f->real_path, fi->flags, mode);
        if (fd < 0) {
            pthread_rwlock_unlock(&s->state_lock);
            return -errno;
        }
        f->open_count++;
        fi->fh = (uint64_t)fd;
        pthread_rwlock_unlock(&s->state_lock);
        return 0;
    }

    unsigned drive_idx = state_pick_drive(s);
    if (drive_idx == UINT32_MAX) {
        pthread_rwlock_unlock(&s->state_lock);
        return -ENOSPC;
    }
    lr_drive *drive    = &s->drives[drive_idx];

    char real[PATH_MAX];
    const char *rel = path[0] == '/' ? path + 1 : path;
    snprintf(real, sizeof(real), "%s%s", drive->dir, rel);

    /* Create parent directories, inheriting modes from other drives */
    mkdirs_p(s, drive_idx, real);

    int fd = open(real, fi->flags | O_CREAT, mode);
    if (fd < 0) {
        int saved = errno;
        pthread_rwlock_unlock(&s->state_lock);
        return -saved;
    }

    uint32_t pos_start = alloc_positions(&s->drives[drive_idx].pos_alloc, 0);

    lr_file *f = calloc(1, sizeof(lr_file));
    if (!f) {
        close(fd);
        pthread_rwlock_unlock(&s->state_lock);
        return -ENOMEM;
    }

    snprintf(f->vpath,     sizeof(f->vpath),    "%s", path);
    snprintf(f->real_path, sizeof(f->real_path), "%s", real);
    f->drive_idx        = drive_idx;
    f->size             = 0;
    f->block_count      = 0;
    f->parity_pos_start = pos_start;
    f->mtime_sec        = time(NULL);

    /* Capture actual mode/uid/gid assigned by the kernel after creation */
    struct stat created_st;
    if (fstat(fd, &created_st) == 0) {
        f->mode = created_st.st_mode;
        f->uid  = created_st.st_uid;
        f->gid  = created_st.st_gid;
    } else {
        f->mode = S_IFREG | (mode & 0777);
        f->uid  = (uid_t)getuid();
        f->gid  = (gid_t)getgid();
        fprintf(stderr, "lr_create: fstat FAILED errno=%d fallback mode=%o uid=%u gid=%u\n",
                errno, (unsigned)f->mode, (unsigned)f->uid, (unsigned)f->gid);
    }

    state_insert_file(s, f);
    f->open_count = 1;
    state_rebuild_pos_index(s, drive_idx);

    pthread_rwlock_unlock(&s->state_lock);

    fi->fh = (uint64_t)fd;
    return 0;
}

/*--------------------------------------------------------------------
 * unlink
 *------------------------------------------------------------------*/
static int lr_unlink(const char *path)
{
    lr_state *s = g_state;

    pthread_rwlock_wrlock(&s->state_lock);

    lr_file *f = state_remove_file(s, path);
    if (!f) {
        pthread_rwlock_unlock(&s->state_lock);
        return -ENOENT;
    }

    char real[PATH_MAX];
    snprintf(real, sizeof(real), "%s", f->real_path);
    unsigned drive_idx   = f->drive_idx;
    uint32_t pos_start   = f->parity_pos_start;
    uint32_t block_count = f->block_count;

    if (block_count > 0 && s->journal)
        journal_mark_dirty_range(s->journal, pos_start, block_count);

    free(f);

    unlink(real);
    free_positions(&s->drives[drive_idx].pos_alloc, pos_start, block_count);
    state_rebuild_pos_index(s, drive_idx);

    pthread_rwlock_unlock(&s->state_lock);
    return 0;
}

/*--------------------------------------------------------------------
 * rename
 *------------------------------------------------------------------*/
static int lr_rename(const char *from, const char *to, unsigned int flags)
{
    (void)flags;
    lr_state *s = g_state;

    pthread_rwlock_wrlock(&s->state_lock);

    lr_file *f = state_find_file(s, from);
    if (!f) {
        pthread_rwlock_unlock(&s->state_lock);
        return -ENOENT;
    }

    char old_real[PATH_MAX];
    snprintf(old_real, sizeof(old_real), "%s", f->real_path);

    char new_real[PATH_MAX];
    const char *rel = to[0] == '/' ? to + 1 : to;
    snprintf(new_real, sizeof(new_real), "%s%s",
             s->drives[f->drive_idx].dir, rel);

    lr_hash_remove(&s->file_table, &f->vpath_node);
    lr_list_remove(&s->file_list, &f->list_node);

    snprintf(f->vpath,     sizeof(f->vpath),    "%s", to);
    snprintf(f->real_path, sizeof(f->real_path), "%s", new_real);

    /* Create parent directories for new path, inheriting modes */
    mkdirs_p(s, f->drive_idx, new_real);

    if (rename(old_real, new_real) != 0) {
        snprintf(f->vpath,     sizeof(f->vpath),    "%s", from);
        snprintf(f->real_path, sizeof(f->real_path), "%s", old_real);
        state_insert_file(s, f);
        pthread_rwlock_unlock(&s->state_lock);
        return -errno;
    }

    state_insert_file(s, f);

    pthread_rwlock_unlock(&s->state_lock);
    return 0;
}

/*--------------------------------------------------------------------
 * mkdir / rmdir
 *------------------------------------------------------------------*/
static int lr_mkdir(const char *path, mode_t mode)
{
    lr_state *s = g_state;

    pthread_rwlock_wrlock(&s->state_lock);
    unsigned drive_idx = state_pick_drive(s);
    char real[PATH_MAX];
    real_path_on_drive(s, drive_idx, path, real, sizeof(real));
    pthread_rwlock_unlock(&s->state_lock);

    if (mkdir(real, mode) != 0)
        return -errno;

    /* Record directory metadata in dir_table */
    lr_dir *d = calloc(1, sizeof(lr_dir));
    if (d) {
        snprintf(d->vpath, PATH_MAX, "%s", path);
        struct stat st;
        if (lstat(real, &st) == 0) {
            d->mode       = st.st_mode;
            d->uid        = st.st_uid;
            d->gid        = st.st_gid;
            d->mtime_sec  = st.st_mtim.tv_sec;
            d->mtime_nsec = st.st_mtim.tv_nsec;
        } else {
            d->mode = S_IFDIR | (mode & 07777);
        }
        pthread_rwlock_wrlock(&s->state_lock);
        state_insert_dir(s, d);
        pthread_rwlock_unlock(&s->state_lock);
    }

    return 0;
}

static int lr_rmdir(const char *path)
{
    lr_state *s = g_state;

    pthread_rwlock_wrlock(&s->state_lock);
    lr_dir *d = state_remove_dir(s, path);
    unsigned count = s->drive_count;
    pthread_rwlock_unlock(&s->state_lock);

    free(d);

    for (unsigned i = 0; i < count; i++) {
        pthread_rwlock_rdlock(&s->state_lock);
        char real[PATH_MAX];
        real_path_on_drive(s, i, path, real, sizeof(real));
        pthread_rwlock_unlock(&s->state_lock);
        rmdir(real);
    }
    return 0;
}

/*--------------------------------------------------------------------
 * truncate
 *------------------------------------------------------------------*/
static int lr_truncate(const char *path, off_t size,
                       struct fuse_file_info *fi)
{
    (void)fi;
    lr_state *s = g_state;

    pthread_rwlock_wrlock(&s->state_lock);

    lr_file *f = state_find_file(s, path);
    if (!f) {
        pthread_rwlock_unlock(&s->state_lock);
        return -ENOENT;
    }

    int rc = truncate(f->real_path, size);
    if (rc != 0) {
        int saved = errno;
        pthread_rwlock_unlock(&s->state_lock);
        return -saved;
    }

    uint32_t old_blocks = f->block_count;
    uint32_t new_blocks = blocks_for_size((uint64_t)size, s->cfg.block_size);

    f->size        = (int64_t)size;
    f->block_count = new_blocks;

    lr_pos_allocator *pa = &s->drives[f->drive_idx].pos_alloc;
    if (new_blocks > old_blocks) {
        uint32_t dirty_start, dirty_count;
        if (old_blocks == 0) {
            f->parity_pos_start = alloc_positions(pa, new_blocks);
            dirty_start = f->parity_pos_start;
            dirty_count = new_blocks;
        } else if (f->parity_pos_start + old_blocks == pa->next_free) {
            dirty_start = f->parity_pos_start + old_blocks;
            dirty_count = new_blocks - old_blocks;
            pa->next_free += dirty_count;
        } else {
            free_positions(pa, f->parity_pos_start, old_blocks);
            f->parity_pos_start = alloc_positions(pa, new_blocks);
            dirty_start = f->parity_pos_start;
            dirty_count = new_blocks;
        }
        if (s->journal)
            journal_mark_dirty_range(s->journal, dirty_start, dirty_count);
    } else if (new_blocks < old_blocks) {
        if (s->journal)
            journal_mark_dirty_range(s->journal,
                                     f->parity_pos_start + new_blocks,
                                     old_blocks - new_blocks);
        free_positions(pa,
                       f->parity_pos_start + new_blocks,
                       old_blocks - new_blocks);
    }

    state_rebuild_pos_index(s, f->drive_idx);

    pthread_rwlock_unlock(&s->state_lock);
    return 0;
}

/*--------------------------------------------------------------------
 * statfs
 *------------------------------------------------------------------*/
static int lr_statfs(const char *path, struct statvfs *sv)
{
    (void)path;
    lr_state *s = g_state;

    memset(sv, 0, sizeof(*sv));

    pthread_rwlock_rdlock(&s->state_lock);
    unsigned count = s->drive_count;
    pthread_rwlock_unlock(&s->state_lock);

    uint64_t total = 0, free_b = 0, avail = 0;
    unsigned long bsize = 4096;

    for (unsigned i = 0; i < count; i++) {
        pthread_rwlock_rdlock(&s->state_lock);
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", s->drives[i].dir);
        pthread_rwlock_unlock(&s->state_lock);

        struct statvfs dsv;
        if (statvfs(dir, &dsv) == 0) {
            bsize  = dsv.f_frsize;
            total += dsv.f_blocks;
            free_b += dsv.f_bfree;
            avail  += dsv.f_bavail;
        }
    }

    sv->f_bsize   = bsize;
    sv->f_frsize  = bsize;
    sv->f_blocks  = total;
    sv->f_bfree   = free_b;
    sv->f_bavail  = avail;
    sv->f_namemax = 255;
    return 0;
}

/*--------------------------------------------------------------------
 * utimens — files and directories
 *------------------------------------------------------------------*/
static int lr_utimens(const char *path, const struct timespec ts[2],
                      struct fuse_file_info *fi)
{
    (void)fi;
    lr_state *s = g_state;

    pthread_rwlock_wrlock(&s->state_lock);

    lr_file *f = state_find_file(s, path);
    if (f) {
        char real[PATH_MAX];
        snprintf(real, sizeof(real), "%s", f->real_path);
        int rc = utimensat(AT_FDCWD, real, ts, 0);
        if (rc == 0) {
            struct stat st;
            if (lstat(real, &st) == 0) {
                f->mtime_sec  = st.st_mtim.tv_sec;
                f->mtime_nsec = st.st_mtim.tv_nsec;
            }
        }
        pthread_rwlock_unlock(&s->state_lock);
        return rc ? -errno : 0;
    }

    /* Directory: apply to every drive that has it, update dir_table */
    if (is_any_dir(s, path)) {
        lr_dir *d = dir_get_or_create(s, path);
        unsigned count = s->drive_count;
        int ret = -ENOENT;
        for (unsigned i = 0; i < count; i++) {
            char real[PATH_MAX];
            real_path_on_drive(s, i, path, real, sizeof(real));
            struct stat st;
            if (lstat(real, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (utimensat(AT_FDCWD, real, ts, 0) == 0) {
                    ret = 0;
                    if (d && lstat(real, &st) == 0) {
                        d->mtime_sec  = st.st_mtim.tv_sec;
                        d->mtime_nsec = st.st_mtim.tv_nsec;
                    }
                } else if (ret == -ENOENT) {
                    ret = -errno;
                }
            }
        }
        /* Virtual-only dir: store the times even with no real backing */
        if (ret == -ENOENT && is_virtual_dir(s, path)) {
            if (d) {
                d->mtime_sec  = ts[1].tv_sec;
                d->mtime_nsec = ts[1].tv_nsec;
            }
            ret = 0;
        }
        pthread_rwlock_unlock(&s->state_lock);
        return ret;
    }

    pthread_rwlock_unlock(&s->state_lock);
    return -ENOENT;
}

/*--------------------------------------------------------------------
 * chmod — files and directories
 *------------------------------------------------------------------*/
static int lr_chmod(const char *path, mode_t mode,
                    struct fuse_file_info *fi)
{
    (void)fi;
    lr_state *s = g_state;

    pthread_rwlock_wrlock(&s->state_lock);

    lr_file *f = state_find_file(s, path);
    if (f) {
        char real[PATH_MAX];
        snprintf(real, sizeof(real), "%s", f->real_path);
        int rc = chmod(real, mode);
        if (rc == 0)
            f->mode = (f->mode & ~(mode_t)07777) | (mode & 07777);
        pthread_rwlock_unlock(&s->state_lock);
        return rc ? -errno : 0;
    }

    /* Directory: apply to every drive that has it, update dir_table */
    if (is_any_dir(s, path)) {
        lr_dir *d = dir_get_or_create(s, path);
        unsigned count = s->drive_count;
        int ret = -ENOENT;
        for (unsigned i = 0; i < count; i++) {
            char real[PATH_MAX];
            real_path_on_drive(s, i, path, real, sizeof(real));
            struct stat st;
            if (lstat(real, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (chmod(real, mode) == 0)
                    ret = 0;
                else if (ret == -ENOENT)
                    ret = -errno;
            }
        }
        if (d)
            d->mode = (d->mode & ~(mode_t)07777) | (mode & 07777);
        if (ret == -ENOENT && is_virtual_dir(s, path))
            ret = 0;
        pthread_rwlock_unlock(&s->state_lock);
        return ret;
    }

    pthread_rwlock_unlock(&s->state_lock);
    return -ENOENT;
}

/*--------------------------------------------------------------------
 * chown — files and directories
 *------------------------------------------------------------------*/
static int lr_chown(const char *path, uid_t uid, gid_t gid,
                    struct fuse_file_info *fi)
{
    (void)fi;
    lr_state *s = g_state;

    pthread_rwlock_wrlock(&s->state_lock);

    lr_file *f = state_find_file(s, path);
    if (f) {
        char real[PATH_MAX];
        snprintf(real, sizeof(real), "%s", f->real_path);
        int rc = lchown(real, uid, gid);
        if (rc == 0) {
            if (uid != (uid_t)-1) f->uid = uid;
            if (gid != (gid_t)-1) f->gid = gid;
        }
        pthread_rwlock_unlock(&s->state_lock);
        return rc ? -errno : 0;
    }

    /* Directory: apply to every drive that has it, update dir_table */
    if (is_any_dir(s, path)) {
        lr_dir *d = dir_get_or_create(s, path);
        unsigned count = s->drive_count;
        int ret = -ENOENT;
        for (unsigned i = 0; i < count; i++) {
            char real[PATH_MAX];
            real_path_on_drive(s, i, path, real, sizeof(real));
            struct stat st;
            if (lstat(real, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (lchown(real, uid, gid) == 0)
                    ret = 0;
                else if (ret == -ENOENT)
                    ret = -errno;
            }
        }
        if (d) {
            if (uid != (uid_t)-1) d->uid = uid;
            if (gid != (gid_t)-1) d->gid = gid;
        }
        if (ret == -ENOENT && is_virtual_dir(s, path))
            ret = 0;
        pthread_rwlock_unlock(&s->state_lock);
        return ret;
    }

    pthread_rwlock_unlock(&s->state_lock);
    return -ENOENT;
}

/*--------------------------------------------------------------------
 * flush / fsync
 *------------------------------------------------------------------*/
static int lr_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path; (void)fi;
    return 0;
}

static int lr_fsync(const char *path, int datasync,
                    struct fuse_file_info *fi)
{
    lr_state *s = g_state;
    (void)datasync;

    if (fi->fh == LR_DEAD_DRIVE_FH)
        return -EIO;

    /* Sync the real file data first */
    if (fi->fh != 0) {
        if (fdatasync((int)fi->fh) != 0)
            return -errno;
    }

    /* Also flush any dirty parity positions for this file so the caller's
     * durability guarantee extends to parity as well. */
    if (s->journal) {
        pthread_rwlock_rdlock(&s->state_lock);
        lr_file *f = state_find_file(s, path);
        uint32_t pos_start  = f ? f->parity_pos_start : 0;
        uint32_t block_count = f ? f->block_count      : 0;
        pthread_rwlock_unlock(&s->state_lock);

        if (block_count > 0)
            journal_mark_dirty_range(s->journal, pos_start, block_count);
        journal_flush(s->journal);
    }

    return 0;
}

/*--------------------------------------------------------------------
 * write
 *------------------------------------------------------------------*/
static int lr_write2(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    if (fi->fh == LR_DEAD_DRIVE_FH)
        return -EIO;

    lr_state *s = g_state;

    ssize_t n = pwrite((int)fi->fh, buf, size, offset);
    if (n < 0)
        return -errno;

    int64_t new_end = (int64_t)(offset + n);

    pthread_rwlock_wrlock(&s->state_lock);
    lr_file *f = state_find_file(s, path);
    if (f) {
        uint32_t bs         = s->cfg.block_size;
        uint32_t old_blocks = f->block_count;
        uint32_t new_blocks = blocks_for_size(
                (uint64_t)(new_end > f->size ? new_end : f->size), bs);
        uint32_t dirty_start = 0, dirty_count = 0;

        lr_pos_allocator *pa = &s->drives[f->drive_idx].pos_alloc;
        if (new_blocks > old_blocks) {
            if (old_blocks == 0) {
                f->parity_pos_start = alloc_positions(pa, new_blocks);
                dirty_start = f->parity_pos_start;
                dirty_count = new_blocks;
            } else if (f->parity_pos_start + old_blocks == pa->next_free) {
                dirty_start = f->parity_pos_start + old_blocks;
                dirty_count = new_blocks - old_blocks;
                pa->next_free += dirty_count;
            } else {
                free_positions(pa, f->parity_pos_start, old_blocks);
                f->parity_pos_start = alloc_positions(pa, new_blocks);
                dirty_start = f->parity_pos_start;
                dirty_count = new_blocks;
            }
            f->block_count = new_blocks;
            state_rebuild_pos_index(s, f->drive_idx);
        }

        if (new_end > f->size)
            f->size = new_end;

        if (f->block_count > 0 && s->journal) {
            if (dirty_count > 0)
                journal_mark_dirty_range(s->journal, dirty_start, dirty_count);

            uint32_t first_blk = (uint32_t)(offset / bs);
            uint32_t last_blk  = (uint32_t)((offset + n - 1) / bs);
            if (last_blk < f->block_count)
                journal_mark_dirty_range(s->journal,
                                         f->parity_pos_start + first_blk,
                                         last_blk - first_blk + 1);
        }
    }
    pthread_rwlock_unlock(&s->state_lock);

    return (int)n;
}

/*--------------------------------------------------------------------
 * destroy
 *------------------------------------------------------------------*/
static void lr_destroy(void *private_data)
{
    (void)private_data;
    lr_state *s = g_state;
    if (!s)
        return;

    /* Stop control server before tearing down state */
    if (s->ctrl) {
        ctrl_stop(s->ctrl);
        free(s->ctrl);
        s->ctrl = NULL;
    }

    if (s->journal) {
        journal_flush(s->journal);
        journal_done(s->journal);
        free(s->journal);
        s->journal = NULL;
    }

    metadata_save(s);

    if (s->parity) {
        parity_close(s->parity);
        free(s->parity);
        s->parity = NULL;
    }
}

/*--------------------------------------------------------------------
 * ops table
 *------------------------------------------------------------------*/
const struct fuse_operations lr_fuse_ops = {
    .getattr  = lr_getattr,
    .readdir  = lr_readdir,
    .open     = lr_open,
    .release  = lr_release,
    .read     = lr_read,
    .write    = lr_write2,
    .create   = lr_create,
    .unlink   = lr_unlink,
    .rename   = lr_rename,
    .mkdir    = lr_mkdir,
    .rmdir    = lr_rmdir,
    .truncate = lr_truncate,
    .statfs   = lr_statfs,
    .utimens  = lr_utimens,
    .chmod    = lr_chmod,
    .chown    = lr_chown,
    .flush    = lr_flush,
    .fsync    = lr_fsync,
    .destroy  = lr_destroy,
};
