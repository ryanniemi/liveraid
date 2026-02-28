#ifndef LR_ALLOC_H
#define LR_ALLOC_H

#include <stdint.h>

/*
 * Per-drive parity-position allocator.
 *
 * Each data drive has its own independent position namespace. Position K
 * on drive D means block K of that drive's files and the corresponding
 * block in each parity file. Drives with no file at position K contribute
 * a zero block; parity at that position covers only the drive(s) that
 * actually have data there.
 *
 * Free positions are tracked as a sorted array of extents (start, count).
 * Allocation uses first-fit search; adjacent extents are merged on free.
 * next_free is the bump high-water mark, used when no suitable extent exists.
 *
 * Both next_free and the extent list are persisted in the content file
 * (as drive_next_free / drive_free_extent header lines) and restored on load.
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
