#include "test_harness.h"
#include "metadata.h"
#include "state.h"
#include "config.h"
#include "alloc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#define CONTENT_PATH "/tmp/lr_test_meta.content"

static void make_config(lr_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->block_size       = 65536;
    cfg->placement_policy = LR_PLACE_ROUNDROBIN;
    cfg->parity_threads   = 1;
    snprintf(cfg->drives[0].name, 64,       "d0");
    snprintf(cfg->drives[0].dir,  PATH_MAX, "/tmp");
    cfg->drive_count = 1;
    snprintf(cfg->content_paths[0], PATH_MAX, CONTENT_PATH);
    cfg->content_count = 1;
    snprintf(cfg->mountpoint, PATH_MAX, "/tmp/lr_test_mount");
}

static void cleanup(void) { unlink(CONTENT_PATH); }

/* ------------------------------------------------------------------ */

/* No content file present → fresh start, empty state. */
static void test_fresh_start_no_file(void)
{
    unlink(CONTENT_PATH);
    lr_config cfg; make_config(&cfg);
    lr_state  s;   state_init(&s, &cfg);
    ASSERT_INT_EQ(metadata_load(&s), 0);
    ASSERT_INT_EQ(s.file_list.count, 0);
    ASSERT_INT_EQ(s.drives[0].pos_alloc.next_free, 0);
    state_done(&s);
}

/* Save a file and reload it; verify all 11 record fields survive. */
static void test_roundtrip_file(void)
{
    unlink(CONTENT_PATH);
    lr_config cfg; make_config(&cfg);

    /* --- save --- */
    {
        lr_state s; state_init(&s, &cfg);
        lr_file *f = calloc(1, sizeof(lr_file));
        snprintf(f->vpath,     PATH_MAX, "/foo.mkv");
        snprintf(f->real_path, PATH_MAX, "/tmp/foo.mkv");
        f->drive_idx        = 0;
        f->size             = 65536;
        f->parity_pos_start = 0;
        f->block_count      = 1;
        f->mtime_sec        = 1234567890;
        f->mtime_nsec       = 123456789;
        f->mode             = S_IFREG | 0644;
        f->uid              = 1001;
        f->gid              = 1002;
        state_insert_file(&s, f);
        ASSERT_INT_EQ(metadata_save(&s), 0);
        state_done(&s);
    }

    /* --- load --- */
    {
        lr_state s; state_init(&s, &cfg);
        ASSERT_INT_EQ(metadata_load(&s), 0);
        ASSERT_INT_EQ(s.file_list.count, 1);
        lr_file *f = state_find_file(&s, "/foo.mkv");
        ASSERT(f != NULL);
        ASSERT_INT_EQ(f->drive_idx,        0);
        ASSERT_INT_EQ(f->size,             65536);
        ASSERT_INT_EQ(f->parity_pos_start, 0);
        ASSERT_INT_EQ(f->block_count,      1);
        ASSERT_INT_EQ(f->mtime_sec,        1234567890);
        ASSERT_INT_EQ(f->mtime_nsec,       123456789);
        ASSERT_INT_EQ(f->mode,             (int)(S_IFREG | 0644));
        ASSERT_INT_EQ(f->uid,              1001);
        ASSERT_INT_EQ(f->gid,              1002);
        state_done(&s);
    }
    cleanup();
}

/* Save a dir and reload it; verify all dir fields survive. */
static void test_roundtrip_dir(void)
{
    unlink(CONTENT_PATH);
    lr_config cfg; make_config(&cfg);

    /* --- save --- */
    {
        lr_state s; state_init(&s, &cfg);
        lr_dir *d = calloc(1, sizeof(lr_dir));
        snprintf(d->vpath, PATH_MAX, "/movies");
        d->mode       = S_IFDIR | 0755;
        d->uid        = 500;
        d->gid        = 501;
        d->mtime_sec  = 9999999;
        d->mtime_nsec = 42;
        state_insert_dir(&s, d);
        ASSERT_INT_EQ(metadata_save(&s), 0);
        state_done(&s);
    }

    /* --- load --- */
    {
        lr_state s; state_init(&s, &cfg);
        ASSERT_INT_EQ(metadata_load(&s), 0);
        lr_dir *d = state_find_dir(&s, "/movies");
        ASSERT(d != NULL);
        ASSERT_INT_EQ(d->mode,       (int)(S_IFDIR | 0755));
        ASSERT_INT_EQ(d->uid,        500);
        ASSERT_INT_EQ(d->gid,        501);
        ASSERT_INT_EQ(d->mtime_sec,  9999999);
        ASSERT_INT_EQ(d->mtime_nsec, 42);
        state_done(&s);
    }
    cleanup();
}

/* Old 8-field file record (no |MODE|UID|GID) → defaults applied. */
static void test_old_format_compat(void)
{
    FILE *raw = fopen(CONTENT_PATH, "w");
    ASSERT(raw != NULL);
    /* format: file|DRIVE|VPATH|SIZE|POS_START|BLOCKS|MTIME_SEC|MTIME_NSEC */
    fprintf(raw, "file|d0|/old.txt|4096|0|1|1000000|500000000\n");
    fclose(raw);

    lr_config cfg; make_config(&cfg);
    lr_state  s;   state_init(&s, &cfg);
    ASSERT_INT_EQ(metadata_load(&s), 0);

    lr_file *f = state_find_file(&s, "/old.txt");
    ASSERT(f != NULL);
    ASSERT_INT_EQ(f->size,       4096);
    ASSERT_INT_EQ(f->mode,       (int)(S_IFREG | 0644));
    ASSERT_INT_EQ(f->uid,        0);
    ASSERT_INT_EQ(f->gid,        0);
    ASSERT_INT_EQ(f->mtime_sec,  1000000);
    ASSERT_INT_EQ(f->mtime_nsec, 500000000);

    state_done(&s);
    cleanup();
}

/* Allocator state (next_free + free extents) survives a save/load round-trip. */
static void test_allocator_persisted(void)
{
    unlink(CONTENT_PATH);
    lr_config cfg; make_config(&cfg);

    /* --- save --- */
    {
        lr_state s; state_init(&s, &cfg);
        alloc_positions(&s.drives[0].pos_alloc, 10); /* next_free = 10 */
        free_positions(&s.drives[0].pos_alloc, 2, 2); /* extent [2,4) */
        ASSERT_INT_EQ(metadata_save(&s), 0);
        state_done(&s);
    }

    /* --- load --- */
    {
        lr_state s; state_init(&s, &cfg);
        ASSERT_INT_EQ(metadata_load(&s), 0);
        lr_pos_allocator *pa = &s.drives[0].pos_alloc;
        ASSERT_INT_EQ(pa->next_free,       10);
        ASSERT_INT_EQ(pa->ext_count,        1);
        ASSERT_INT_EQ(pa->extents[0].start, 2);
        ASSERT_INT_EQ(pa->extents[0].count, 2);
        state_done(&s);
    }
    cleanup();
}

int main(void)
{
    printf("test_metadata\n");
    RUN(test_fresh_start_no_file);
    RUN(test_roundtrip_file);
    RUN(test_roundtrip_dir);
    RUN(test_old_format_compat);
    RUN(test_allocator_persisted);
    REPORT();
}
