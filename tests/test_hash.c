#include "test_harness.h"
#include "lr_hash.h"

#include <stdint.h>
#include <string.h>

/* Simple test container with an embedded hash node. */
typedef struct {
    int          value;
    lr_hash_node node;
} hobj;

static int cmp_int(const void *arg, const void *obj)
{
    int key            = *(const int *)arg;
    const hobj *o = (const hobj *)obj;
    return key != o->value;     /* 0 = match */
}

static void test_init_done(void)
{
    lr_hash h;
    lr_hash_init(&h);
    ASSERT(h.buckets  != NULL);
    ASSERT(h.nbuckets >  0);
    ASSERT_INT_EQ(h.count, 0);
    lr_hash_done(&h);
    ASSERT(h.buckets == NULL);
    ASSERT_INT_EQ(h.count, 0);
}

static void test_insert_and_find(void)
{
    lr_hash h;
    lr_hash_init(&h);
    hobj o = { .value = 42 };
    uint32_t hash = 42u;
    lr_hash_insert(&h, &o.node, &o, hash);
    ASSERT_INT_EQ(h.count, 1);
    int key = 42;
    hobj *found = (hobj *)lr_hash_search(&h, hash, cmp_int, &key);
    ASSERT(found != NULL);
    ASSERT_INT_EQ(found->value, 42);
    lr_hash_done(&h);
}

static void test_not_found_empty(void)
{
    lr_hash h;
    lr_hash_init(&h);
    int key = 99;
    ASSERT(lr_hash_search(&h, 99u, cmp_int, &key) == NULL);
    lr_hash_done(&h);
}

static void test_not_found_wrong_hash(void)
{
    lr_hash h;
    lr_hash_init(&h);
    hobj o = { .value = 1 };
    lr_hash_insert(&h, &o.node, &o, 1u);
    int key = 2;
    /* Hash 2 goes to a different bucket — not found. */
    ASSERT(lr_hash_search(&h, 2u, cmp_int, &key) == NULL);
    lr_hash_done(&h);
}

static void test_remove(void)
{
    lr_hash h;
    lr_hash_init(&h);
    hobj o = { .value = 7 };
    lr_hash_insert(&h, &o.node, &o, 7u);
    ASSERT_INT_EQ(h.count, 1);
    lr_hash_remove(&h, &o.node);
    ASSERT_INT_EQ(h.count, 0);
    int key = 7;
    ASSERT(lr_hash_search(&h, 7u, cmp_int, &key) == NULL);
    lr_hash_done(&h);
}

static void test_many_entries_and_growth(void)
{
    /* Insert enough entries to trigger bucket growth, then verify all are findable. */
    lr_hash h;
    lr_hash_init(&h);
#define N 48
    hobj objs[N];
    for (int i = 0; i < N; i++) {
        objs[i].value = i * 100;
        lr_hash_insert(&h, &objs[i].node, &objs[i], (uint32_t)(i * 100));
    }
    ASSERT_INT_EQ(h.count, N);
    for (int i = 0; i < N; i++) {
        int key = i * 100;
        hobj *f = (hobj *)lr_hash_search(&h, (uint32_t)key, cmp_int, &key);
        ASSERT(f != NULL);
        ASSERT_INT_EQ(f->value, key);
    }
#undef N
    lr_hash_done(&h);
}

/* Force a hash chain by using hashes that land in the same bucket
 * (initial nbuckets=16; hashes that are multiples of 16 all map to bucket 0). */
static void test_chain_remove_middle(void)
{
    lr_hash h;
    lr_hash_init(&h);
    hobj a = { .value = 1 };   /* hash  0 → bucket 0 */
    hobj b = { .value = 2 };   /* hash 16 → bucket 0 */
    hobj c = { .value = 3 };   /* hash 32 → bucket 0 */
    lr_hash_insert(&h, &a.node, &a, 0u);
    lr_hash_insert(&h, &b.node, &b, 16u);
    lr_hash_insert(&h, &c.node, &c, 32u);
    ASSERT_INT_EQ(h.count, 3);
    lr_hash_remove(&h, &b.node);
    ASSERT_INT_EQ(h.count, 2);
    int k1 = 1, k2 = 2, k3 = 3;
    ASSERT(lr_hash_search(&h,  0u, cmp_int, &k1) != NULL);
    ASSERT(lr_hash_search(&h, 16u, cmp_int, &k2) == NULL);
    ASSERT(lr_hash_search(&h, 32u, cmp_int, &k3) != NULL);
    lr_hash_done(&h);
}

static void test_string_hash_stable(void)
{
    uint32_t h1 = lr_hash_string("/movies/foo.mkv");
    uint32_t h2 = lr_hash_string("/movies/foo.mkv");
    ASSERT_INT_EQ(h1, h2);
    uint32_t h3 = lr_hash_string("/movies/bar.mkv");
    ASSERT(h1 != h3);
    /* Empty string has a defined value (FNV offset basis). */
    uint32_t h4 = lr_hash_string("");
    uint32_t h5 = lr_hash_string("");
    ASSERT_INT_EQ(h4, h5);
}

int main(void)
{
    printf("test_hash\n");
    RUN(test_init_done);
    RUN(test_insert_and_find);
    RUN(test_not_found_empty);
    RUN(test_not_found_wrong_hash);
    RUN(test_remove);
    RUN(test_many_entries_and_growth);
    RUN(test_chain_remove_middle);
    RUN(test_string_hash_stable);
    REPORT();
}
