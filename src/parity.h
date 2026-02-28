#ifndef LR_PARITY_H
#define LR_PARITY_H

#include "state.h"
#include <stdint.h>
#include <isa-l/erasure_code.h>

/*
 * Parity file handles and I/O (ISA-L backend).
 *
 * The Cauchy encoding matrix and precomputed GF tables are built once
 * at parity_open() time from the drive/parity counts in the config.
 */
typedef struct lr_parity_handle {
    int      fds[LR_LEV_MAX];   /* open file descriptors, -1 if unused */
    unsigned levels;             /* number of parity levels (np) */
    unsigned nd;                 /* number of data drives at open time */
    uint32_t block_size;

    /* ISA-L: (nd+np)*nd Cauchy encoding matrix and precomputed tables */
    uint8_t *enc_matrix;         /* (nd+np) * nd bytes */
    uint8_t *gftbls;             /* 32 * nd * np bytes */
} lr_parity_handle;

/* Open/create parity files.  Returns 0 on success. */
int  parity_open(lr_parity_handle *ph, const lr_config *cfg);
void parity_close(lr_parity_handle *ph);

/* Read one block from parity level `lev` at position `pos`. */
int  parity_read_block(lr_parity_handle *ph, unsigned lev, uint32_t pos,
                       void *buf);

/* Write one block to parity level `lev` at position `pos`. */
int  parity_write_block(lr_parity_handle *ph, unsigned lev, uint32_t pos,
                        const void *buf);

/*
 * Recompute parity for position `pos` using current data blocks.
 * `scratch_v` must have room for (nd + np) blocks of block_size bytes;
 * use lr_alloc_vector() to obtain it.
 */
int  parity_update_position(lr_state *s, uint32_t pos, void **scratch_v);

/*
 * Reconstruct one data block for drive `drive_idx` at parity position `pos`.
 * Additional drives returning EIO are auto-detected (up to np total failures).
 *
 * Caller must hold state_lock for reading.
 * Returns 0 on success, -1 if too many failures or parity unavailable.
 */
int  parity_recover_block(lr_state *s, unsigned drive_idx, uint32_t pos,
                          void *out_buf);

/* ------------------------------------------------------------------ */
/* Scrub                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t positions_checked;
    uint32_t parity_mismatches;
    uint32_t read_errors;
} lr_scrub_result;

int  parity_scrub(lr_state *s, lr_scrub_result *result);

/* ------------------------------------------------------------------ */
/* Vector allocator                                                     */
/* ------------------------------------------------------------------ */

/*
 * Allocate n blocks of block_size bytes each, 64-byte aligned (safe for
 * ISA-L AVX2 paths).  Returns a void*[n] pointer array; free *freeptr
 * (not the returned pointer) to release everything.
 *
 * Returns NULL on allocation failure.
 */
void **lr_alloc_vector(int n, uint32_t block_size, void **freeptr);

#endif /* LR_PARITY_H */
