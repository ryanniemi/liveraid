#define _GNU_SOURCE   /* for open_memstream */
#include "metadata.h"
#include "state.h"
#include "alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>

#define META_VERSION 1

/* ------------------------------------------------------------------ */
/* CRC32 (IEEE 802.3 / zlib polynomial)                                */
/* ------------------------------------------------------------------ */

static uint32_t crc32_table[256];
static int      crc32_initialized;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_initialized = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    if (!crc32_initialized)
        crc32_init();
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    return crc;
}

/* ------------------------------------------------------------------ */
/* Load                                                                */
/* ------------------------------------------------------------------ */

int metadata_load(lr_state *s)
{
    FILE *f = NULL;
    char  line[PATH_MAX + 256];
    int   lineno = 0;
    unsigned i;

    /* Try each content path in order; load the first one found */
    for (i = 0; i < s->cfg.content_count; i++) {
        f = fopen(s->cfg.content_paths[i], "r");
        if (f)
            break;
    }

    if (!f) {
        /* No content file yet — fresh start */
        return 0;
    }

    uint32_t file_block_size = s->cfg.block_size;
    uint32_t running_crc = 0xFFFFFFFFU;
    unsigned content_idx = i; /* save index of loaded content path for CRC message */

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        size_t raw_len = strlen(line);  /* includes '\n' if present */
        char *p = line;

        /* CRC footer check: done before stripping newline */
        if (strncmp(p, "# crc32:", 8) == 0) {
            uint32_t stored   = (uint32_t)strtoul(p + 9, NULL, 16);
            uint32_t computed = running_crc ^ 0xFFFFFFFFU;
            if (stored != computed)
                fprintf(stderr,
                        "metadata: CRC mismatch in '%s' "
                        "(stored %08X, computed %08X) — file may be corrupt\n",
                        s->cfg.content_paths[content_idx], stored, computed);
            break; /* no more records after the CRC line */
        }

        /* Accumulate CRC over the raw line bytes (including '\n') */
        running_crc = crc32_update(running_crc, (const uint8_t *)p, raw_len);

        /* Strip newline */
        size_t len = raw_len;
        if (len > 0 && p[len-1] == '\n') p[len-1] = '\0';
        if (len > 1 && p[len-2] == '\r') p[len-2] = '\0';

        /* Parse known header directives before skipping all '#' lines */
        if (strncmp(p, "# drive_next_free:", 18) == 0) {
            char dname[64]; uint32_t nfp;
            if (sscanf(p + 18, "%63s %u", dname, &nfp) == 2) {
                for (unsigned di = 0; di < s->drive_count; di++) {
                    if (strcmp(s->drives[di].name, dname) == 0) {
                        if (nfp > s->drives[di].pos_alloc.next_free)
                            s->drives[di].pos_alloc.next_free = nfp;
                        break;
                    }
                }
            }
            continue;
        }
        if (strncmp(p, "# drive_free_extent:", 20) == 0) {
            char dname[64]; uint32_t start, cnt;
            if (sscanf(p + 20, "%63s %u %u", dname, &start, &cnt) == 3) {
                for (unsigned di = 0; di < s->drive_count; di++) {
                    if (strcmp(s->drives[di].name, dname) == 0) {
                        free_positions(&s->drives[di].pos_alloc, start, cnt);
                        break;
                    }
                }
            }
            continue;
        }
        /* old global format: next_free_pos / free_extent — ignored on upgrade;
         * per-drive next_free is derived from file records below instead */
        if (strncmp(p, "# next_free_pos:", 16) == 0)
            continue;
        if (strncmp(p, "# free_extent:", 14) == 0)
            continue;

        /* Skip remaining comments/empty lines */
        if (p[0] == '#' || p[0] == '\0')
            continue;

        /* Directory records: dir|VPATH|MODE|UID|GID|MTIME_SEC|MTIME_NSEC */
        if (strncmp(p, "dir|", 4) == 0) {
            char buf[PATH_MAX + 256];
            strncpy(buf, p + 4, sizeof(buf) - 1);
            buf[sizeof(buf)-1] = '\0';

            char *tok;
            char *vpath    = buf;
            tok = strchr(vpath, '|'); if (!tok) continue; *tok++ = '\0';
            char *mode_s   = tok;
            tok = strchr(mode_s, '|'); if (!tok) continue; *tok++ = '\0';
            char *uid_s    = tok;
            tok = strchr(uid_s, '|'); if (!tok) continue; *tok++ = '\0';
            char *gid_s    = tok;
            tok = strchr(gid_s, '|'); if (!tok) continue; *tok++ = '\0';
            char *mtime_s  = tok;
            tok = strchr(mtime_s, '|'); if (!tok) continue; *tok++ = '\0';
            char *mtime_ns_s = tok;

            if (strlen(vpath) >= PATH_MAX) {
                fprintf(stderr, "metadata: dir vpath too long at line %d, skipping\n",
                        lineno);
                continue;
            }

            lr_dir *dir = calloc(1, sizeof(lr_dir));
            if (!dir) { fclose(f); return -1; }

            snprintf(dir->vpath, PATH_MAX, "%s", vpath);
            dir->mode       = (mode_t)strtoul(mode_s,    NULL, 8);
            dir->uid        = (uid_t) strtoul(uid_s,     NULL, 10);
            dir->gid        = (gid_t) strtoul(gid_s,     NULL, 10);
            dir->mtime_sec  = (time_t)strtoll(mtime_s,   NULL, 10);
            dir->mtime_nsec = strtol(mtime_ns_s,         NULL, 10);
            if (!dir->mode)
                dir->mode = S_IFDIR | 0755;

            state_insert_dir(s, dir);
            continue;
        }

        /* Symlink records: symlink|VPATH|TARGET|MTIME_SEC|MTIME_NSEC|UID|GID */
        if (strncmp(p, "symlink|", 8) == 0) {
            char buf[PATH_MAX * 2 + 256];
            strncpy(buf, p + 8, sizeof(buf) - 1);
            buf[sizeof(buf)-1] = '\0';

            char *tok;
            char *vpath      = buf;
            tok = strchr(vpath, '|'); if (!tok) continue; *tok++ = '\0';
            char *target     = tok;
            tok = strchr(target, '|'); if (!tok) continue; *tok++ = '\0';
            char *mtime_s    = tok;
            tok = strchr(mtime_s, '|'); if (!tok) continue; *tok++ = '\0';
            char *mtime_ns_s = tok;
            tok = strchr(mtime_ns_s, '|'); if (!tok) continue; *tok++ = '\0';
            char *uid_s      = tok;
            tok = strchr(uid_s, '|'); if (!tok) continue; *tok++ = '\0';
            char *gid_s      = tok;

            if (strlen(vpath) >= PATH_MAX || strlen(target) >= PATH_MAX) {
                fprintf(stderr,
                        "metadata: symlink vpath/target too long at line %d, skipping\n",
                        lineno);
                continue;
            }

            lr_symlink *sl = calloc(1, sizeof(lr_symlink));
            if (!sl) { fclose(f); return -1; }
            snprintf(sl->vpath,  PATH_MAX, "%s", vpath);
            snprintf(sl->target, PATH_MAX, "%s", target);
            sl->mtime_sec  = (time_t)strtoll(mtime_s,    NULL, 10);
            sl->mtime_nsec = strtol(mtime_ns_s,           NULL, 10);
            sl->uid        = (uid_t)strtoul(uid_s,        NULL, 10);
            sl->gid        = (gid_t)strtoul(gid_s,        NULL, 10);
            state_insert_symlink(s, sl);
            continue;
        }

        /* File records: file|DRIVE|VPATH|SIZE|POS_START|BLOCKS|MTIME_SEC|MTIME_NSEC */
        if (strncmp(p, "file|", 5) != 0)
            continue;

        char *tok;
        char  buf[PATH_MAX + 256];
        strncpy(buf, p + 5, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';

        char *drive_name = buf;
        tok = strchr(drive_name, '|'); if (!tok) continue;
        *tok++ = '\0';
        char *vpath = tok;
        tok = strchr(vpath, '|'); if (!tok) continue;
        *tok++ = '\0';
        char *size_s = tok;
        tok = strchr(size_s, '|'); if (!tok) continue;
        *tok++ = '\0';
        char *pos_s = tok;
        tok = strchr(pos_s, '|'); if (!tok) continue;
        *tok++ = '\0';
        char *cnt_s = tok;
        tok = strchr(cnt_s, '|'); if (!tok) continue;
        *tok++ = '\0';
        char *mtime_s = tok;
        tok = strchr(mtime_s, '|'); if (!tok) continue;
        *tok++ = '\0';
        char *mtime_ns_s = tok;

        /* Find drive index */
        unsigned drive_idx = UINT32_MAX;
        for (i = 0; i < s->drive_count; i++) {
            if (strcmp(s->drives[i].name, drive_name) == 0) {
                drive_idx = i;
                break;
            }
        }
        if (drive_idx == UINT32_MAX) {
            fprintf(stderr, "metadata: unknown drive '%s' at line %d, skipping\n",
                    drive_name, lineno);
            continue;
        }
        if (strlen(vpath) >= PATH_MAX) {
            fprintf(stderr, "metadata: file vpath too long at line %d, skipping\n",
                    lineno);
            continue;
        }

        lr_file *file = calloc(1, sizeof(lr_file));
        if (!file) {
            fclose(f);
            return -1;
        }

        snprintf(file->vpath, PATH_MAX, "%s", vpath);
        snprintf(file->real_path, PATH_MAX, "%s%s",
                 s->drives[drive_idx].dir,
                 vpath[0] == '/' ? vpath + 1 : vpath);
        file->drive_idx        = drive_idx;
        file->size             = (int64_t)strtoll(size_s, NULL, 10);
        file->parity_pos_start = (uint32_t)strtoul(pos_s, NULL, 10);
        file->block_count      = (uint32_t)strtoul(cnt_s, NULL, 10);
        file->mtime_sec        = (time_t)strtoll(mtime_s, NULL, 10);
        /* mtime_ns_s may have trailing fields: |MODE|UID|GID (v2 format) */
        tok = strchr(mtime_ns_s, '|');
        if (tok) {
            *tok++ = '\0';
            char *mode_s = tok;
            tok = strchr(mode_s, '|');
            if (tok) {
                *tok++ = '\0';
                char *uid_s = tok;
                tok = strchr(uid_s, '|');
                if (tok) {
                    *tok++ = '\0';
                    char *gid_s = tok;
                    file->mode = (mode_t)strtoul(mode_s, NULL, 8); /* octal */
                    file->uid  = (uid_t) strtoul(uid_s,  NULL, 10);
                    file->gid  = (gid_t) strtoul(gid_s,  NULL, 10);
                }
            }
        }
        file->mtime_nsec = strtol(mtime_ns_s, NULL, 10);
        if (!file->mode)
            file->mode = S_IFREG | 0644; /* default for old-format files */

        /* Validate block_count against size */
        uint32_t expected = blocks_for_size((uint64_t)file->size, file_block_size);
        if (file->block_count != expected) {
            fprintf(stderr, "metadata: block_count mismatch for %s: stored %u, computed %u\n",
                    vpath, file->block_count, expected);
            file->block_count = expected;
        }

        /* Ensure this drive's allocator covers this file's position range */
        uint32_t end = file->parity_pos_start + file->block_count;
        if (end > s->drives[drive_idx].pos_alloc.next_free)
            s->drives[drive_idx].pos_alloc.next_free = end;

        state_insert_file(s, file);
    }

    fclose(f);

    /* Rebuild position indexes */
    for (i = 0; i < s->drive_count; i++)
        state_rebuild_pos_index(s, i);

    /* Integrity check: warn if any two files on the same drive have
     * overlapping parity position ranges (indicates a corrupt content file). */
    for (i = 0; i < s->drive_count; i++) {
        lr_pos_entry *idx = s->pos_index[i];
        uint32_t     cnt  = s->pos_index_count[i];
        for (uint32_t k = 1; k < cnt; k++) {
            uint32_t prev_end = idx[k-1].pos_start + idx[k-1].block_count;
            if (idx[k].pos_start < prev_end) {
                fprintf(stderr,
                        "metadata: WARNING: overlapping parity positions on "
                        "drive '%s': [%u,%u) and [%u,%u) — content file may "
                        "be corrupt\n",
                        s->drives[i].name,
                        idx[k-1].pos_start, prev_end,
                        idx[k].pos_start,
                        idx[k].pos_start + idx[k].block_count);
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Save                                                                */
/* ------------------------------------------------------------------ */

static int write_to_path(lr_state *s, const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    /* Step 1: build content in a memory buffer so we can CRC it */
    char  *mbuf  = NULL;
    size_t msize = 0;
    FILE  *mf    = open_memstream(&mbuf, &msize);
    if (!mf) {
        fprintf(stderr, "metadata_save: open_memstream failed: %s\n",
                strerror(errno));
        return -1;
    }

    fprintf(mf, "# liveraid content\n");
    fprintf(mf, "# version: %d\n", META_VERSION);
    fprintf(mf, "# blocksize: %u\n", s->cfg.block_size);
    for (unsigned d = 0; d < s->drive_count; d++) {
        lr_pos_allocator *pa = &s->drives[d].pos_alloc;
        fprintf(mf, "# drive_next_free: %s %u\n", s->drives[d].name, pa->next_free);
        for (uint32_t ei = 0; ei < pa->ext_count; ei++)
            fprintf(mf, "# drive_free_extent: %s %u %u\n",
                    s->drives[d].name,
                    pa->extents[ei].start,
                    pa->extents[ei].count);
    }

    lr_list_node *node = lr_list_head(&s->file_list);
    while (node) {
        lr_file *file = (lr_file *)node->data;
        fprintf(mf, "file|%s|%s|%lld|%u|%u|%lld|%ld|%o|%u|%u\n",
                s->drives[file->drive_idx].name,
                file->vpath,
                (long long)file->size,
                file->parity_pos_start,
                file->block_count,
                (long long)file->mtime_sec,
                file->mtime_nsec,
                (unsigned)file->mode,
                (unsigned)file->uid,
                (unsigned)file->gid);
        node = node->next;
    }

    node = lr_list_head(&s->dir_list);
    while (node) {
        lr_dir *dir = (lr_dir *)node->data;
        fprintf(mf, "dir|%s|%o|%u|%u|%lld|%ld\n",
                dir->vpath,
                (unsigned)dir->mode,
                (unsigned)dir->uid,
                (unsigned)dir->gid,
                (long long)dir->mtime_sec,
                dir->mtime_nsec);
        node = node->next;
    }

    node = lr_list_head(&s->symlink_list);
    while (node) {
        lr_symlink *sl = (lr_symlink *)node->data;
        fprintf(mf, "symlink|%s|%s|%lld|%ld|%u|%u\n",
                sl->vpath, sl->target,
                (long long)sl->mtime_sec, sl->mtime_nsec,
                (unsigned)sl->uid, (unsigned)sl->gid);
        node = node->next;
    }

    /* Step 2: flush to get msize, compute CRC of the body, append footer */
    fflush(mf);
    uint32_t crc = crc32_update(0xFFFFFFFFU, (const uint8_t *)mbuf, msize)
                   ^ 0xFFFFFFFFU;
    fprintf(mf, "# crc32: %08X\n", crc);
    fclose(mf);  /* updates mbuf/msize to include the CRC footer */

    /* Step 3: write mbuf atomically to the target path */
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "metadata_save: cannot open '%s': %s\n",
                tmp, strerror(errno));
        free(mbuf);
        return -1;
    }
    size_t written = fwrite(mbuf, 1, msize, f);
    free(mbuf);

    if (written != msize) {
        fprintf(stderr, "metadata_save: short write to '%s'\n", tmp);
        fclose(f);
        unlink(tmp);
        return -1;
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(tmp, path) != 0) {
        fprintf(stderr, "metadata_save: rename '%s' -> '%s': %s\n",
                tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }

    return 0;
}

int metadata_save(lr_state *s)
{
    int rc = 0;
    unsigned i;
    for (i = 0; i < s->cfg.content_count; i++) {
        if (write_to_path(s, s->cfg.content_paths[i]) != 0)
            rc = -1;
    }
    return rc;
}
