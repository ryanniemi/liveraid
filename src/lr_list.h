#ifndef LR_LIST_H
#define LR_LIST_H

/*
 * lr_list — minimal intrusive doubly-linked list.
 *
 * Usage pattern (same as tommyds tommy_list):
 *   - Embed an lr_list_node inside the value struct.
 *   - Call lr_list_insert_tail(l, &obj->node, obj).
 *   - Iterate: for (lr_list_node *n = lr_list_head(l); n; n = n->next)
 *                  obj = (MyType *)n->data;
 */

#include <stdint.h>
#include <stddef.h>

typedef struct lr_list_node {
    struct lr_list_node *prev;
    struct lr_list_node *next;
    void                *data;
} lr_list_node;

typedef struct {
    lr_list_node *head;
    lr_list_node *tail;
    uint32_t      count;
} lr_list;

static inline void lr_list_init(lr_list *l)
{
    l->head = l->tail = NULL;
    l->count = 0;
}

static inline lr_list_node *lr_list_head(const lr_list *l)
{
    return l->head;
}

void lr_list_insert_tail(lr_list *l, lr_list_node *node, void *data);
void lr_list_remove(lr_list *l, lr_list_node *node);
void lr_list_foreach(lr_list *l, void (*fn)(void *data));

#endif /* LR_LIST_H */
