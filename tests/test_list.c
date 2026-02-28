#include "test_harness.h"
#include "lr_list.h"

#include <stdint.h>

typedef struct { int val; lr_list_node node; } lobj;
#define LOBJ(v) ((lobj){ .val = (v) })

static void test_init(void)
{
    lr_list l;
    lr_list_init(&l);
    ASSERT(l.head  == NULL);
    ASSERT(l.tail  == NULL);
    ASSERT_INT_EQ(l.count, 0);
}

static void test_insert_tail_and_iterate(void)
{
    lr_list l;
    lr_list_init(&l);
    lobj a = LOBJ(1), b = LOBJ(2), c = LOBJ(3);
    lr_list_insert_tail(&l, &a.node, &a);
    lr_list_insert_tail(&l, &b.node, &b);
    lr_list_insert_tail(&l, &c.node, &c);
    ASSERT_INT_EQ(l.count, 3);

    /* Verify insertion order: head → a, b, c */
    lr_list_node *n = lr_list_head(&l);
    ASSERT_INT_EQ(((lobj *)n->data)->val, 1);  n = n->next;
    ASSERT_INT_EQ(((lobj *)n->data)->val, 2);  n = n->next;
    ASSERT_INT_EQ(((lobj *)n->data)->val, 3);
    ASSERT(n->next == NULL);
    ASSERT(l.tail == &c.node);
}

static void test_remove_head(void)
{
    lr_list l;
    lr_list_init(&l);
    lobj a = LOBJ(1), b = LOBJ(2);
    lr_list_insert_tail(&l, &a.node, &a);
    lr_list_insert_tail(&l, &b.node, &b);
    lr_list_remove(&l, &a.node);
    ASSERT_INT_EQ(l.count, 1);
    ASSERT(l.head == &b.node);
    ASSERT(l.tail == &b.node);
    ASSERT_INT_EQ(((lobj *)lr_list_head(&l)->data)->val, 2);
}

static void test_remove_tail(void)
{
    lr_list l;
    lr_list_init(&l);
    lobj a = LOBJ(1), b = LOBJ(2);
    lr_list_insert_tail(&l, &a.node, &a);
    lr_list_insert_tail(&l, &b.node, &b);
    lr_list_remove(&l, &b.node);
    ASSERT_INT_EQ(l.count, 1);
    ASSERT(l.tail == &a.node);
    ASSERT_INT_EQ(((lobj *)l.tail->data)->val, 1);
}

static void test_remove_middle(void)
{
    lr_list l;
    lr_list_init(&l);
    lobj a = LOBJ(1), b = LOBJ(2), c = LOBJ(3);
    lr_list_insert_tail(&l, &a.node, &a);
    lr_list_insert_tail(&l, &b.node, &b);
    lr_list_insert_tail(&l, &c.node, &c);
    lr_list_remove(&l, &b.node);
    ASSERT_INT_EQ(l.count, 2);
    ASSERT_INT_EQ(((lobj *)l.head->data)->val, 1);
    ASSERT_INT_EQ(((lobj *)l.tail->data)->val, 3);
    ASSERT(l.head->next == &c.node);
    ASSERT(l.tail->prev == &a.node);
}

static void test_remove_only_element(void)
{
    lr_list l;
    lr_list_init(&l);
    lobj a = LOBJ(42);
    lr_list_insert_tail(&l, &a.node, &a);
    lr_list_remove(&l, &a.node);
    ASSERT_INT_EQ(l.count, 0);
    ASSERT(l.head == NULL);
    ASSERT(l.tail == NULL);
}

int main(void)
{
    printf("test_list\n");
    RUN(test_init);
    RUN(test_insert_tail_and_iterate);
    RUN(test_remove_head);
    RUN(test_remove_tail);
    RUN(test_remove_middle);
    RUN(test_remove_only_element);
    REPORT();
}
