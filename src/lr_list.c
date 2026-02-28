#include "lr_list.h"

void lr_list_insert_tail(lr_list *l, lr_list_node *node, void *data)
{
    node->data = data;
    node->next = NULL;
    node->prev = l->tail;
    if (l->tail)
        l->tail->next = node;
    else
        l->head = node;
    l->tail = node;
    l->count++;
}

void lr_list_remove(lr_list *l, lr_list_node *node)
{
    if (node->prev) node->prev->next = node->next;
    else            l->head = node->next;

    if (node->next) node->next->prev = node->prev;
    else            l->tail = node->prev;

    node->prev = node->next = NULL;
    l->count--;
}

void lr_list_foreach(lr_list *l, void (*fn)(void *data))
{
    lr_list_node *n = l->head;
    while (n) {
        lr_list_node *next = n->next;
        fn(n->data);
        n = next;
    }
}
