#ifndef LR_ALLOC_H
#define LR_ALLOC_H

#include <stdint.h>

/*
 * Global parity-position allocator.
 *
 * All data drives share one global position namespace. Position K means
 * block K on each drive (zero-block if no file occupies that position on
 * that drive).
 *
 * Free positions are tracked as a sorted array of extents (start, count).
 * Allocation uses first-fit search; adjacent extents are merged on free.
 * next_free is the bump high-water mark, used when no suitable extent exists.
 *
 * The extent list is in-memory only; next_free is the only value persisted
 * in the content file. Freed positions from previous sessions are not
 * reclaimed on remount.
 */

typedef struct {
    uint32_t start;
    uint32_t count;
} lr_extent;

typedef struct {
    uint32_t   next_free;   /* Bump high-water mark */
    lr_extent *extents;     /* Sorted free extents (by start position) */
    uint32_t   ext_count;
    uint32_t   ext_cap;
} lr_pos_allocator;

void     alloc_init(lr_pos_allocator *a);
void     alloc_done(lr_pos_allocator *a);

/*
 * Allocate `count` contiguous positions. Returns the start position.
 * Uses first-fit search through free extents, falling back to bump allocation.
 * Passing count == 0 returns next_free without side effects (used by lr_create
 * to probe the current high-water mark before the first write).
 */
uint32_t alloc_positions(lr_pos_allocator *a, uint32_t count);

/*
 * Return `count` contiguous positions starting at `start` to the free pool.
 * The range is inserted into the sorted extent list and merged with any
 * adjacent free extents. If the freed range abuts next_free, the high-water
 * mark is reclaimed.
 */
void     free_positions(lr_pos_allocator *a, uint32_t start, uint32_t count);

#endif /* LR_ALLOC_H */
