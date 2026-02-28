#ifndef LR_HASH_H
#define LR_HASH_H

/*
 * lr_hash — minimal intrusive separate-chaining hash map.
 *
 * Usage pattern (same as tommyds tommy_hashdyn):
 *   - Embed an lr_hash_node inside the value struct.
 *   - Call lr_hash_insert(h, &obj->node, obj, hash).
 *   - lr_hash_search returns the obj pointer (or NULL).
 *   - lr_hash_remove takes the embedded node pointer directly.
 */

#include <stdint.h>
#include <stdlib.h>

typedef struct lr_hash_node {
    struct lr_hash_node *next;
    uint32_t             hash;
    void                *data;
} lr_hash_node;

typedef struct {
    lr_hash_node **buckets;
    uint32_t       nbuckets; /* always a power of 2 */
    uint32_t       count;
} lr_hash;

void  lr_hash_init(lr_hash *h);
void  lr_hash_done(lr_hash *h);

void  lr_hash_insert(lr_hash *h, lr_hash_node *node, void *data, uint32_t hash);

/* Returns data pointer if found, NULL otherwise.
 * cmp(arg, obj) must return 0 on match. */
void *lr_hash_search(const lr_hash *h, uint32_t hash,
                     int (*cmp)(const void *arg, const void *obj),
                     const void *arg);

/* Remove by embedded node pointer (O(bucket-chain) in worst case). */
void  lr_hash_remove(lr_hash *h, lr_hash_node *node);

/* FNV-1a string hash — fast, good distribution for path strings. */
static inline uint32_t lr_hash_string(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

#endif /* LR_HASH_H */
