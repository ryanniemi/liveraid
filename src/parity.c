#include "parity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Vector allocator                                                     */
/* ------------------------------------------------------------------ */

/*
 * Single allocation layout:
 *   [ void*[n] pointer array | <pad to 64-byte boundary> | block 0 | block 1 | … ]
 *
 * All block pointers are 64-byte aligned, satisfying ISA-L AVX2 requirements.
 * Free *freeptr (the raw malloc result) to release everything.
 */
void **lr_alloc_vector(int n, uint32_t block_size, void **freeptr)
{
    size_t ptrs_bytes  = (size_t)n * sizeof(void *);
    size_t ptrs_padded = (ptrs_bytes + 63) & ~(size_t)63;
    size_t total       = ptrs_padded + (size_t)n * block_size + 63;

    void *raw = malloc(total);
    if (!raw) {
        *freeptr = NULL;
        return NULL;
    }
    *freeptr = raw;

    /* Align pointer array base to 64 bytes */
    uint8_t *base  = (uint8_t *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
    void   **v     = (void **)base;
    uint8_t *data  = base + ptrs_padded;

    for (int i = 0; i < n; i++)
        v[i] = data + (size_t)i * block_size;

    return v;
}

/* ------------------------------------------------------------------ */
/* Parity file open / close                                            */
/* ------------------------------------------------------------------ */

int parity_open(lr_parity_handle *ph, const lr_config *cfg)
{
    unsigned nd = cfg->drive_count;
    unsigned np = cfg->parity_levels;
    unsigned i;

    memset(ph, 0, sizeof(*ph));
    ph->block_size = cfg->block_size;
    ph->levels     = np;
    ph->nd         = nd;

    for (i = 0; i < LR_LEV_MAX; i++)
        ph->fds[i] = -1;

    for (i = 0; i < np; i++) {
        ph->fds[i] = open(cfg->parity_path[i], O_RDWR | O_CREAT, 0644);
        if (ph->fds[i] < 0) {
            fprintf(stderr, "parity_open: cannot open '%s': %s\n",
                    cfg->parity_path[i], strerror(errno));
            parity_close(ph);
            return -1;
        }
    }

    if (nd == 0 || np == 0)
        return 0; /* no RAID math needed */

    /* Build (nd+np) x nd Cauchy encoding matrix */
    ph->enc_matrix = malloc((nd + np) * nd);
    if (!ph->enc_matrix) {
        parity_close(ph);
        return -1;
    }
    gf_gen_cauchy1_matrix(ph->enc_matrix, (int)(nd + np), (int)nd);

    /* Precompute GF encode tables for the np parity rows */
    ph->gftbls = malloc(32 * nd * np);
    if (!ph->gftbls) {
        parity_close(ph);
        return -1;
    }
    /* Parity rows start at row nd of enc_matrix */
    ec_init_tables((int)nd, (int)np,
                   ph->enc_matrix + nd * nd,
                   ph->gftbls);

    return 0;
}

void parity_close(lr_parity_handle *ph)
{
    unsigned i;
    for (i = 0; i < LR_LEV_MAX; i++) {
        if (ph->fds[i] >= 0) {
            close(ph->fds[i]);
            ph->fds[i] = -1;
        }
    }
    free(ph->enc_matrix);
    free(ph->gftbls);
    ph->enc_matrix = NULL;
    ph->gftbls     = NULL;
}

/* ------------------------------------------------------------------ */
/* Block I/O                                                            */
/* ------------------------------------------------------------------ */

int parity_read_block(lr_parity_handle *ph, unsigned lev, uint32_t pos,
                      void *buf)
{
    if (lev >= ph->levels || ph->fds[lev] < 0)
        return -1;

    off_t   offset = (off_t)pos * ph->block_size;
    ssize_t n      = pread(ph->fds[lev], buf, ph->block_size, offset);
    if (n != (ssize_t)ph->block_size) {
        if (n >= 0)
            memset((char *)buf + n, 0, ph->block_size - n); /* sparse read */
        else
            return -1;
    }
    return 0;
}

int parity_write_block(lr_parity_handle *ph, unsigned lev, uint32_t pos,
                       const void *buf)
{
    if (lev >= ph->levels || ph->fds[lev] < 0)
        return -1;

    off_t   offset = (off_t)pos * ph->block_size;
    ssize_t n      = pwrite(ph->fds[lev], buf, ph->block_size, offset);
    if (n != (ssize_t)ph->block_size)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parity update                                                        */
/* ------------------------------------------------------------------ */

int parity_update_position(lr_state *s, uint32_t pos, void **scratch_v)
{
    unsigned d;
    unsigned nd         = s->drive_count;
    unsigned np         = s->parity ? s->parity->levels : 0;
    uint32_t block_size = s->cfg.block_size;

    if (np == 0 || !s->parity)
        return 0;

    /* Fill data slots scratch_v[0..nd-1] */
    for (d = 0; d < nd; d++) {
        lr_file *f = state_find_file_at_pos(s, d, pos);
        if (!f) {
            memset(scratch_v[d], 0, block_size);
        } else {
            uint32_t blk_off = pos - f->parity_pos_start;
            off_t    offset  = (off_t)blk_off * block_size;
            int      fd      = open(f->real_path, O_RDONLY);
            if (fd < 0) {
                memset(scratch_v[d], 0, block_size);
            } else {
                ssize_t n = pread(fd, scratch_v[d], block_size, offset);
                close(fd);
                if (n < (ssize_t)block_size) {
                    if (n > 0)
                        memset((char *)scratch_v[d] + n, 0, block_size - n);
                    else
                        memset(scratch_v[d], 0, block_size);
                }
            }
        }
    }

    /* Compute parity into scratch_v[nd..nd+np-1] */
    ec_encode_data((int)block_size, (int)nd, (int)np,
                   s->parity->gftbls,
                   (uint8_t **)scratch_v,
                   (uint8_t **)scratch_v + nd);

    /* Write parity blocks */
    for (unsigned p = 0; p < np; p++)
        parity_write_block(s->parity, p, pos, scratch_v[nd + p]);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Block recovery                                                       */
/* ------------------------------------------------------------------ */

int parity_recover_block(lr_state *s, unsigned drive_idx, uint32_t pos,
                         void *out_buf)
{
    if (!s->parity || s->parity->levels == 0 || drive_idx >= s->drive_count)
        return -1;

    unsigned nd         = s->drive_count;
    unsigned np         = s->parity->levels;
    uint32_t block_size = s->cfg.block_size;

    void *freeptr = NULL;
    void **v = lr_alloc_vector((int)(nd + np), block_size, &freeptr);
    if (!v)
        return -1;

    /* failed[] is maintained in sorted order (required by ISA-L decode) */
    int failed[LR_LEV_MAX];
    int nfailed = 1;
    failed[0] = (int)drive_idx;
    memset(v[drive_idx], 0, block_size);

    /* Read all surviving data drives */
    for (unsigned d = 0; d < nd; d++) {
        if (d == drive_idx)
            continue;

        lr_file *f = state_find_file_at_pos(s, d, pos);
        if (!f) {
            memset(v[d], 0, block_size);
            continue;
        }

        uint32_t blk_off = pos - f->parity_pos_start;
        int fd = open(f->real_path, O_RDONLY);
        int ok = 0;
        if (fd >= 0) {
            ssize_t n = pread(fd, v[d], block_size,
                              (off_t)blk_off * block_size);
            close(fd);
            if (n >= 0) {
                if (n < (ssize_t)block_size)
                    memset((char *)v[d] + n, 0, block_size - n);
                ok = 1;
            }
        }

        if (!ok) {
            if (nfailed >= (int)np) {
                free(freeptr);
                return -1;
            }
            /* Insert d into failed[] in sorted order */
            int i = nfailed - 1;
            while (i >= 0 && failed[i] > (int)d) {
                failed[i + 1] = failed[i];
                i--;
            }
            failed[i + 1] = (int)d;
            nfailed++;
            memset(v[d], 0, block_size);
        }
    }

    /* Read the nfailed lowest parity levels */
    for (int p = 0; p < nfailed; p++) {
        if (parity_read_block(s->parity, (unsigned)p, pos, v[nd + p]) != 0)
            memset(v[nd + p], 0, block_size);
    }

    /*
     * Build the nd×nd decode submatrix from:
     *   - surviving data drives (identity rows of enc_matrix)
     *   - nfailed parity rows
     * Then invert to get decode coefficients.
     */
    uint8_t *enc_matrix = s->parity->enc_matrix;

    uint8_t *dec_matrix = malloc(nd * nd);
    uint8_t *inv_matrix = malloc(nd * nd);
    uint8_t *dec_tbls   = malloc(32 * nd * (unsigned)nfailed);
    if (!dec_matrix || !inv_matrix || !dec_tbls) {
        free(dec_matrix); free(inv_matrix); free(dec_tbls);
        free(freeptr);
        return -1;
    }

    /* Surviving rows: non-failed data drives (in order) + nfailed parity rows */
    int si = 0, fi = 0;
    int surv_rows[LR_DRIVE_MAX];
    for (int d = 0; d < (int)nd; d++) {
        if (fi < nfailed && d == failed[fi]) { fi++; continue; }
        surv_rows[si++] = d;
    }
    for (int p = 0; p < nfailed; p++)
        surv_rows[si++] = (int)nd + p;

    for (int i = 0; i < (int)nd; i++)
        memcpy(dec_matrix + i * nd, enc_matrix + surv_rows[i] * nd, nd);

    if (gf_invert_matrix(dec_matrix, inv_matrix, (int)nd) != 0) {
        free(dec_matrix); free(inv_matrix); free(dec_tbls);
        free(freeptr);
        return -1;
    }

    /* Extract decode rows for each failed drive */
    uint8_t *decode_rows = malloc((unsigned)nfailed * nd);
    if (!decode_rows) {
        free(dec_matrix); free(inv_matrix); free(dec_tbls);
        free(freeptr);
        return -1;
    }
    for (int i = 0; i < nfailed; i++)
        memcpy(decode_rows + i * nd, inv_matrix + failed[i] * nd, nd);

    ec_init_tables((int)nd, nfailed, decode_rows, dec_tbls);
    free(decode_rows);

    /* Collect surviving input pointers (same order as surv_rows) */
    uint8_t *src[LR_DRIVE_MAX];
    si = 0; fi = 0;
    for (int d = 0; d < (int)nd; d++) {
        if (fi < nfailed && d == failed[fi]) { fi++; continue; }
        src[si++] = (uint8_t *)v[d];
    }
    for (int p = 0; p < nfailed; p++)
        src[si++] = (uint8_t *)v[nd + p];

    /* Output: one pointer per failed drive */
    uint8_t *dst[LR_LEV_MAX];
    for (int i = 0; i < nfailed; i++)
        dst[i] = (uint8_t *)v[failed[i]];

    ec_encode_data((int)block_size, (int)nd, nfailed, dec_tbls, src, dst);

    memcpy(out_buf, v[drive_idx], block_size);

    free(dec_matrix);
    free(inv_matrix);
    free(dec_tbls);
    free(freeptr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scrub                                                                */
/* ------------------------------------------------------------------ */

int parity_scrub(lr_state *s, lr_scrub_result *result, int repair)
{
    memset(result, 0, sizeof(*result));

    if (!s->parity || s->parity->levels == 0)
        return 0;

    unsigned nd         = s->drive_count;
    unsigned np         = s->parity->levels;
    uint32_t block_size = s->cfg.block_size;

    /*
     * Allocate: nd data + np computed-parity + np stored-parity slots.
     * ec_encode_data writes into v[nd..nd+np-1]; stored parity goes into
     * v[nd+np..nd+2*np-1] for byte-for-byte comparison.
     */
    void *freeptr = NULL;
    void **v = lr_alloc_vector((int)(nd + 2 * np), block_size, &freeptr);
    if (!v)
        return -1;

    pthread_rwlock_rdlock(&s->state_lock);
    uint32_t max_pos = 0;
    for (unsigned d = 0; d < s->drive_count; d++) {
        if (s->drives[d].pos_alloc.next_free > max_pos)
            max_pos = s->drives[d].pos_alloc.next_free;
    }
    pthread_rwlock_unlock(&s->state_lock);

    for (uint32_t pos = 0; pos < max_pos; pos++) {
        pthread_rwlock_rdlock(&s->state_lock);

        int read_err = 0;
        for (unsigned d = 0; d < nd; d++) {
            lr_file *f = state_find_file_at_pos(s, d, pos);
            if (!f) {
                memset(v[d], 0, block_size);
                continue;
            }
            uint32_t blk_off = pos - f->parity_pos_start;
            int fd = open(f->real_path, O_RDONLY);
            if (fd < 0) {
                memset(v[d], 0, block_size);
                read_err = 1;
                continue;
            }
            ssize_t n = pread(fd, v[d], block_size,
                              (off_t)blk_off * block_size);
            close(fd);
            if (n < (ssize_t)block_size) {
                if (n > 0)
                    memset((char *)v[d] + n, 0, block_size - n);
                else {
                    memset(v[d], 0, block_size);
                    read_err = 1;
                }
            }
        }

        pthread_rwlock_unlock(&s->state_lock);

        result->positions_checked++;

        if (read_err) {
            result->read_errors++;
            continue;
        }

        /* Compute expected parity into v[nd..nd+np-1] */
        ec_encode_data((int)block_size, (int)nd, (int)np,
                       s->parity->gftbls,
                       (uint8_t **)v,
                       (uint8_t **)v + nd);

        /* Read stored parity into v[nd+np..nd+2*np-1] and compare */
        int mismatch        = 0;
        int parity_read_err = 0;
        for (unsigned p = 0; p < np; p++) {
            if (parity_read_block(s->parity, p, pos, v[nd + np + p]) != 0) {
                parity_read_err = 1;
                break;
            }
            if (memcmp(v[nd + p], v[nd + np + p], block_size) != 0)
                mismatch = 1;
        }

        if (parity_read_err) {
            result->read_errors++;
        } else if (mismatch) {
            result->parity_mismatches++;
            if (repair) {
                int write_err = 0;
                for (unsigned p = 0; p < np; p++) {
                    if (parity_write_block(s->parity, p, pos, v[nd + p]) != 0)
                        write_err = 1;
                }
                if (!write_err)
                    result->parity_fixed++;
            }
        }
    }

    free(freeptr);
    return 0;
}
