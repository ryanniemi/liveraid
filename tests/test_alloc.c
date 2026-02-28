#include "test_harness.h"
#include "alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_init_done(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    ASSERT_INT_EQ(a.next_free,  0);
    ASSERT(a.extents == NULL);
    ASSERT_INT_EQ(a.ext_count,  0);
    ASSERT_INT_EQ(a.ext_cap,    0);
    alloc_done(&a);
    ASSERT_INT_EQ(a.next_free,  0);
    ASSERT(a.extents == NULL);
}

/* alloc(0) probes the high-water mark without advancing it. */
static void test_alloc_zero_count(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    ASSERT_INT_EQ(alloc_positions(&a, 0), 0);
    ASSERT_INT_EQ(a.next_free, 0);
    alloc_positions(&a, 5);
    ASSERT_INT_EQ(alloc_positions(&a, 0), 5);
    ASSERT_INT_EQ(a.next_free, 5);
    alloc_done(&a);
}

/* Sequential allocations bump next_free without gaps. */
static void test_alloc_sequential(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    ASSERT_INT_EQ(alloc_positions(&a, 3),  0);
    ASSERT_INT_EQ(alloc_positions(&a, 2),  3);
    ASSERT_INT_EQ(alloc_positions(&a, 5),  5);
    ASSERT_INT_EQ(a.next_free, 10);
    ASSERT_INT_EQ(a.ext_count,  0);
    alloc_done(&a);
}

/* Freeing a range that abuts next_free lowers the high-water mark. */
static void test_free_reclaims_high_water(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 8);
    free_positions(&a, 5, 3);           /* [5,8) == next_free → reclaim */
    ASSERT_INT_EQ(a.next_free, 5);
    ASSERT_INT_EQ(a.ext_count, 0);
    alloc_done(&a);
}

/* Freeing a non-adjacent interior range creates a free extent. */
static void test_free_creates_extent(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 8);
    free_positions(&a, 2, 3);
    ASSERT_INT_EQ(a.ext_count,            1);
    ASSERT_INT_EQ(a.extents[0].start,     2);
    ASSERT_INT_EQ(a.extents[0].count,     3);
    ASSERT_INT_EQ(a.next_free,            8);   /* high-water unchanged */
    alloc_done(&a);
}

/* Freeing a block whose right edge touches an existing extent merges them. */
static void test_free_merge_left_neighbor(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 10);
    free_positions(&a, 4, 4);           /* extent [4,8) */
    free_positions(&a, 2, 2);           /* [2,4) abuts [4,8) on right → merge */
    ASSERT_INT_EQ(a.ext_count,            1);
    ASSERT_INT_EQ(a.extents[0].start,     2);
    ASSERT_INT_EQ(a.extents[0].count,     6);
    alloc_done(&a);
}

/* Freeing a block whose left edge touches an existing extent merges them. */
static void test_free_merge_right_neighbor(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 10);
    free_positions(&a, 2, 2);           /* extent [2,4) */
    free_positions(&a, 4, 2);           /* [4,6) abuts [2,4) on left → merge */
    ASSERT_INT_EQ(a.ext_count,            1);
    ASSERT_INT_EQ(a.extents[0].start,     2);
    ASSERT_INT_EQ(a.extents[0].count,     4);
    alloc_done(&a);
}

/* Freeing a block that bridges two extents merges all three, then reclaims
 * if the combined extent touches next_free. */
static void test_free_merge_both_neighbors(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 9);             /* next_free = 9 */
    free_positions(&a, 0, 3);           /* extent [0,3) */
    free_positions(&a, 6, 3);           /* [6,9) abuts next_free=9 → reclaim,
                                         * next_free=6; ext_count still 1 */
    ASSERT_INT_EQ(a.ext_count,  1);
    ASSERT_INT_EQ(a.next_free,  6);
    free_positions(&a, 3, 3);           /* bridges [0,3) and next_free=6
                                         * → [0,6) then reclaim → empty */
    ASSERT_INT_EQ(a.ext_count,  0);
    ASSERT_INT_EQ(a.next_free,  0);
    alloc_done(&a);
}

/* Allocation should come from the free extent rather than bumping next_free. */
static void test_alloc_reuses_free_extent(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 8);
    free_positions(&a, 2, 4);           /* free [2,6) */
    ASSERT_INT_EQ(alloc_positions(&a, 2),  2);   /* first-fit from [2,6) */
    ASSERT_INT_EQ(a.ext_count,             1);
    ASSERT_INT_EQ(a.extents[0].start,      4);   /* remainder [4,6) */
    ASSERT_INT_EQ(a.extents[0].count,      2);
    ASSERT_INT_EQ(a.next_free,             8);   /* bump not touched */
    alloc_done(&a);
}

/* An allocation that exactly fits an extent removes that extent entirely. */
static void test_alloc_exact_fit_removes_extent(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 8);
    free_positions(&a, 2, 3);
    ASSERT_INT_EQ(a.ext_count, 1);
    ASSERT_INT_EQ(alloc_positions(&a, 3), 2);
    ASSERT_INT_EQ(a.ext_count, 0);
    alloc_done(&a);
}

/* alloc skips extents that are too small (first-fit). */
static void test_alloc_first_fit_skips_small(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 10);
    free_positions(&a, 1, 1);           /* tiny [1,2) */
    free_positions(&a, 5, 3);           /* [5,8) */
    ASSERT_INT_EQ(a.ext_count, 2);
    ASSERT_INT_EQ(alloc_positions(&a, 2), 5);   /* [1,2) too small; use [5,8) */
    ASSERT_INT_EQ(a.extents[0].start,  1);      /* tiny extent still present */
    ASSERT_INT_EQ(a.extents[1].start,  7);      /* remainder [7,8) */
    alloc_done(&a);
}

/* When no free extent fits, allocation falls back to bumping next_free. */
static void test_alloc_falls_back_to_bump(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 5);
    free_positions(&a, 1, 1);           /* extent [1,2) — too small for 3 */
    ASSERT_INT_EQ(alloc_positions(&a, 3), 5);   /* bump from next_free */
    ASSERT_INT_EQ(a.next_free, 8);
    ASSERT_INT_EQ(a.ext_count, 1);     /* tiny extent still there */
    alloc_done(&a);
}

/* Multiple frees maintain sorted extent order. */
static void test_free_multiple_extents_sorted(void)
{
    lr_pos_allocator a;
    alloc_init(&a);
    alloc_positions(&a, 10);
    free_positions(&a, 7, 1);
    free_positions(&a, 3, 1);
    free_positions(&a, 1, 1);
    ASSERT_INT_EQ(a.ext_count,         3);
    ASSERT_INT_EQ(a.extents[0].start,  1);
    ASSERT_INT_EQ(a.extents[1].start,  3);
    ASSERT_INT_EQ(a.extents[2].start,  7);
    alloc_done(&a);
}

int main(void)
{
    printf("test_alloc\n");
    RUN(test_init_done);
    RUN(test_alloc_zero_count);
    RUN(test_alloc_sequential);
    RUN(test_free_reclaims_high_water);
    RUN(test_free_creates_extent);
    RUN(test_free_merge_left_neighbor);
    RUN(test_free_merge_right_neighbor);
    RUN(test_free_merge_both_neighbors);
    RUN(test_alloc_reuses_free_extent);
    RUN(test_alloc_exact_fit_removes_extent);
    RUN(test_alloc_first_fit_skips_small);
    RUN(test_alloc_falls_back_to_bump);
    RUN(test_free_multiple_extents_sorted);
    REPORT();
}
