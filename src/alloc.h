#ifndef LR_ALLOC_H
#define LR_ALLOC_H

#include <stdint.h>

/*
 * Global parity-position allocator.
 *
 * All data drives share one global position namespace. Position K means
 * block K on each drive (zero-block if no file occupies that position on
 * that drive).
 */
typedef struct {
    uint32_t  next_free;    /* Next unused position */
    uint32_t *free_list;    /* Positions returned by deleted files */
    uint32_t  free_count;
    uint32_t  free_cap;
} lr_pos_allocator;

void     alloc_init(lr_pos_allocator *a);
void     alloc_done(lr_pos_allocator *a);

/*
 * Allocate `count` contiguous positions.  Returns the start position.
 * This is a simple bump allocator; freed positions are kept in free_list
 * for single-block reuse only.
 */
uint32_t alloc_positions(lr_pos_allocator *a, uint32_t count);

/*
 * Return `count` contiguous positions starting at `start` to the free pool.
 * Only single-block ranges are currently recycled; multi-block ranges just
 * bump next_free on the fly, so we increment next_free to account for
 * any gap rather than fragmenting. For Phase-1 we do a simple approach.
 */
void     free_positions(lr_pos_allocator *a, uint32_t start, uint32_t count);

#endif /* LR_ALLOC_H */
