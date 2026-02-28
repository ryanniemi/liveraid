#include "alloc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

void alloc_init(lr_pos_allocator *a)
{
    memset(a, 0, sizeof(*a));
}

void alloc_done(lr_pos_allocator *a)
{
    free(a->extents);
    memset(a, 0, sizeof(*a));
}

uint32_t alloc_positions(lr_pos_allocator *a, uint32_t count)
{
    uint32_t i;

    if (count == 0)
        return a->next_free;

    /* First-fit search through sorted free extents */
    for (i = 0; i < a->ext_count; i++) {
        if (a->extents[i].count >= count) {
            uint32_t start = a->extents[i].start;
            a->extents[i].start += count;
            a->extents[i].count -= count;
            if (a->extents[i].count == 0) {
                memmove(&a->extents[i], &a->extents[i + 1],
                        (a->ext_count - i - 1) * sizeof(lr_extent));
                a->ext_count--;
            }
            return start;
        }
    }

    /* No suitable extent found — bump allocate */
    if (count > UINT32_MAX - a->next_free) {
        fprintf(stderr, "liveraid: fatal: parity position namespace exhausted\n");
        return UINT32_MAX;
    }
    uint32_t start = a->next_free;
    a->next_free += count;
    return start;
}

void free_positions(lr_pos_allocator *a, uint32_t start, uint32_t count)
{
    uint32_t i;
    int merge_prev, merge_next;

    if (count == 0)
        return;

    /* Find sorted insertion point */
    for (i = 0; i < a->ext_count; i++) {
        if (a->extents[i].start > start)
            break;
    }

    merge_prev = (i > 0 &&
                  a->extents[i - 1].start + a->extents[i - 1].count == start);
    merge_next = (i < a->ext_count &&
                  start + count == a->extents[i].start);

    if (merge_prev && merge_next) {
        /* Bridge the gap: absorb right extent into left */
        a->extents[i - 1].count += count + a->extents[i].count;
        memmove(&a->extents[i], &a->extents[i + 1],
                (a->ext_count - i - 1) * sizeof(lr_extent));
        a->ext_count--;
    } else if (merge_prev) {
        a->extents[i - 1].count += count;
    } else if (merge_next) {
        a->extents[i].start  = start;
        a->extents[i].count += count;
    } else {
        /* Insert new extent at position i */
        if (a->ext_count >= a->ext_cap) {
            uint32_t new_cap = a->ext_cap ? a->ext_cap * 2 : 16;
            lr_extent *p = realloc(a->extents, new_cap * sizeof(lr_extent));
            if (!p)
                return; /* leak on OOM */
            a->extents  = p;
            a->ext_cap  = new_cap;
        }
        memmove(&a->extents[i + 1], &a->extents[i],
                (a->ext_count - i) * sizeof(lr_extent));
        a->extents[i].start = start;
        a->extents[i].count = count;
        a->ext_count++;
    }

    /* If the last extent now abuts the bump high-water mark, reclaim it */
    if (a->ext_count > 0) {
        uint32_t last = a->ext_count - 1;
        if (a->extents[last].start + a->extents[last].count == a->next_free) {
            a->next_free = a->extents[last].start;
            a->ext_count--;
        }
    }
}
