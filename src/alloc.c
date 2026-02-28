#include "alloc.h"

#include <stdlib.h>
#include <string.h>

void alloc_init(lr_pos_allocator *a)
{
    memset(a, 0, sizeof(*a));
}

void alloc_done(lr_pos_allocator *a)
{
    free(a->free_list);
    memset(a, 0, sizeof(*a));
}

uint32_t alloc_positions(lr_pos_allocator *a, uint32_t count)
{
    /* Try to reuse a single free position if count == 1 */
    if (count == 1 && a->free_count > 0) {
        return a->free_list[--a->free_count];
    }

    /* Bump allocate */
    uint32_t start = a->next_free;
    a->next_free += count;
    return start;
}

void free_positions(lr_pos_allocator *a, uint32_t start, uint32_t count)
{
    uint32_t i;
    /* Return each position to the free list */
    for (i = 0; i < count; i++) {
        if (a->free_count >= a->free_cap) {
            uint32_t new_cap = a->free_cap ? a->free_cap * 2 : 64;
            uint32_t *p = realloc(a->free_list, new_cap * sizeof(uint32_t));
            if (!p)
                return; /* leak on OOM */
            a->free_list = p;
            a->free_cap  = new_cap;
        }
        a->free_list[a->free_count++] = start + i;
    }
}
