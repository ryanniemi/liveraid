#include "lr_hash.h"
#include <string.h>

#define INIT_BUCKETS 16u

void lr_hash_init(lr_hash *h)
{
    h->buckets  = calloc(INIT_BUCKETS, sizeof(lr_hash_node *));
    h->nbuckets = h->buckets ? INIT_BUCKETS : 0;
    h->count    = 0;
}

void lr_hash_done(lr_hash *h)
{
    free(h->buckets);
    h->buckets  = NULL;
    h->nbuckets = 0;
    h->count    = 0;
}

static void grow(lr_hash *h)
{
    uint32_t new_n = h->nbuckets * 2;
    lr_hash_node **new_b = calloc(new_n, sizeof(lr_hash_node *));
    if (!new_b)
        return;

    for (uint32_t i = 0; i < h->nbuckets; i++) {
        lr_hash_node *node = h->buckets[i];
        while (node) {
            lr_hash_node *next = node->next;
            uint32_t bi   = node->hash & (new_n - 1);
            node->next    = new_b[bi];
            new_b[bi]     = node;
            node = next;
        }
    }
    free(h->buckets);
    h->buckets  = new_b;
    h->nbuckets = new_n;
}

void lr_hash_insert(lr_hash *h, lr_hash_node *node, void *data, uint32_t hash)
{
    if (h->count >= h->nbuckets * 3 / 4)
        grow(h);

    uint32_t bi    = hash & (h->nbuckets - 1);
    node->hash     = hash;
    node->data     = data;
    node->next     = h->buckets[bi];
    h->buckets[bi] = node;
    h->count++;
}

void *lr_hash_search(const lr_hash *h, uint32_t hash,
                     int (*cmp)(const void *arg, const void *obj),
                     const void *arg)
{
    if (!h->buckets)
        return NULL;
    uint32_t bi = hash & (h->nbuckets - 1);
    for (lr_hash_node *n = h->buckets[bi]; n; n = n->next) {
        if (n->hash == hash && cmp(arg, n->data) == 0)
            return n->data;
    }
    return NULL;
}

void lr_hash_remove(lr_hash *h, lr_hash_node *node)
{
    uint32_t bi = node->hash & (h->nbuckets - 1);
    lr_hash_node **pp = &h->buckets[bi];
    while (*pp) {
        if (*pp == node) {
            *pp        = node->next;
            node->next = NULL;
            h->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}
