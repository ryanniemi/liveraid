#include "state.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/statvfs.h>

lr_state *g_state = NULL;

int state_init(lr_state *s, const lr_config *cfg)
{
    unsigned i;

    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;

    for (i = 0; i < cfg->drive_count; i++) {
        lr_drive *d = &s->drives[i];
        snprintf(d->name, sizeof(d->name), "%s", cfg->drives[i].name);
        snprintf(d->dir,  sizeof(d->dir),  "%s", cfg->drives[i].dir);
        size_t len = strlen(d->dir);
        if (len > 0 && d->dir[len-1] != '/') {
            if (len + 1 < PATH_MAX) {
                d->dir[len]   = '/';
                d->dir[len+1] = '\0';
            }
        }
        d->idx = i;
    }
    s->drive_count = cfg->drive_count;

    lr_hash_init(&s->file_table);
    lr_list_init(&s->file_list);

    lr_hash_init(&s->dir_table);
    lr_list_init(&s->dir_list);

    alloc_init(&s->pos_alloc);

    if (pthread_rwlock_init(&s->state_lock, NULL) != 0) {
        fprintf(stderr, "state_init: rwlock_init failed\n");
        return -1;
    }

    return 0;
}

void state_done(lr_state *s)
{
    unsigned i;

    /* Free all file records */
    lr_list_node *node = lr_list_head(&s->file_list);
    while (node) {
        lr_list_node *next = node->next;
        free(node->data);
        node = next;
    }
    lr_hash_done(&s->file_table);

    /* Free all dir records */
    node = lr_list_head(&s->dir_list);
    while (node) {
        lr_list_node *next = node->next;
        free(node->data);
        node = next;
    }
    lr_hash_done(&s->dir_table);

    alloc_done(&s->pos_alloc);

    for (i = 0; i < LR_DRIVE_MAX; i++) {
        free(s->pos_index[i]);
        s->pos_index[i] = NULL;
    }

    pthread_rwlock_destroy(&s->state_lock);
    memset(s, 0, sizeof(*s));
}

void state_insert_file(lr_state *s, lr_file *f)
{
    uint32_t h = lr_hash_string(f->vpath);
    lr_hash_insert(&s->file_table, &f->vpath_node, f, h);
    lr_list_insert_tail(&s->file_list, &f->list_node, f);
}

static int find_compare(const void *arg, const void *obj)
{
    const char    *vpath = (const char *)arg;
    const lr_file *f     = (const lr_file *)obj;
    return strcmp(vpath, f->vpath) != 0; /* 0 = match */
}

lr_file *state_find_file(lr_state *s, const char *vpath)
{
    uint32_t h = lr_hash_string(vpath);
    return (lr_file *)lr_hash_search(&s->file_table, h, find_compare, vpath);
}

lr_file *state_remove_file(lr_state *s, const char *vpath)
{
    lr_file *f = state_find_file(s, vpath);
    if (!f)
        return NULL;
    lr_hash_remove(&s->file_table, &f->vpath_node);
    lr_list_remove(&s->file_list, &f->list_node);
    return f;
}

/* ------------------------------------------------------------------ */
/* Directory table                                                      */
/* ------------------------------------------------------------------ */

static int dir_find_compare(const void *arg, const void *obj)
{
    const char   *vpath = (const char *)arg;
    const lr_dir *d     = (const lr_dir *)obj;
    return strcmp(vpath, d->vpath) != 0; /* 0 = match */
}

void state_insert_dir(lr_state *s, lr_dir *d)
{
    uint32_t h = lr_hash_string(d->vpath);
    lr_hash_insert(&s->dir_table, &d->vpath_node, d, h);
    lr_list_insert_tail(&s->dir_list, &d->list_node, d);
}

lr_dir *state_find_dir(lr_state *s, const char *vpath)
{
    uint32_t h = lr_hash_string(vpath);
    return (lr_dir *)lr_hash_search(&s->dir_table, h, dir_find_compare, vpath);
}

lr_dir *state_remove_dir(lr_state *s, const char *vpath)
{
    lr_dir *d = state_find_dir(s, vpath);
    if (!d)
        return NULL;
    lr_hash_remove(&s->dir_table, &d->vpath_node);
    lr_list_remove(&s->dir_list, &d->list_node);
    return d;
}

unsigned state_pick_drive(lr_state *s)
{
    unsigned i, best = 0;

    if (s->drive_count == 0)
        return UINT32_MAX; /* caller must check */

    if (s->cfg.placement_policy == LR_PLACE_ROUNDROBIN) {
        unsigned idx = s->rr_next % s->drive_count;
        s->rr_next++;
        return idx;
    }

    /* MOSTFREE: pick drive with most free space */
    uint64_t best_free = 0;
    for (i = 0; i < s->drive_count; i++) {
        struct statvfs sv;
        if (statvfs(s->drives[i].dir, &sv) == 0) {
            uint64_t free_bytes = (uint64_t)sv.f_bavail * sv.f_frsize;
            if (free_bytes > best_free) {
                best_free = free_bytes;
                best = i;
            }
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Position index                                                       */
/* ------------------------------------------------------------------ */

static int pos_entry_cmp(const void *a, const void *b)
{
    const lr_pos_entry *pa = (const lr_pos_entry *)a;
    const lr_pos_entry *pb = (const lr_pos_entry *)b;
    if (pa->pos_start < pb->pos_start) return -1;
    if (pa->pos_start > pb->pos_start) return  1;
    return 0;
}

void state_rebuild_pos_index(lr_state *s, unsigned drive_idx)
{
    uint32_t count = 0;
    lr_list_node *node = lr_list_head(&s->file_list);
    while (node) {
        lr_file *f = (lr_file *)node->data;
        if (f->drive_idx == drive_idx)
            count++;
        node = node->next;
    }

    free(s->pos_index[drive_idx]);
    s->pos_index[drive_idx]       = NULL;
    s->pos_index_count[drive_idx] = 0;

    if (count == 0)
        return;

    lr_pos_entry *arr = malloc(count * sizeof(lr_pos_entry));
    if (!arr)
        return;

    uint32_t i = 0;
    node = lr_list_head(&s->file_list);
    while (node) {
        lr_file *f = (lr_file *)node->data;
        if (f->drive_idx == drive_idx) {
            arr[i].pos_start   = f->parity_pos_start;
            arr[i].block_count = f->block_count;
            arr[i].file        = f;
            i++;
        }
        node = node->next;
    }

    qsort(arr, count, sizeof(lr_pos_entry), pos_entry_cmp);

    s->pos_index[drive_idx]       = arr;
    s->pos_index_count[drive_idx] = count;
}

lr_file *state_find_file_at_pos(lr_state *s, unsigned drive_idx, uint32_t pos)
{
    lr_pos_entry *arr   = s->pos_index[drive_idx];
    uint32_t      count = s->pos_index_count[drive_idx];
    int lo = 0, hi = (int)count - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        lr_pos_entry *e = &arr[mid];
        if (pos >= e->pos_start && pos < e->pos_start + e->block_count)
            return e->file;
        if (pos < e->pos_start)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}
