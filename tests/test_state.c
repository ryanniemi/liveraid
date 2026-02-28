#include "test_harness.h"
#include "state.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Build a minimal config for state_init.  Uses roundrobin so drive-selection
 * tests never need real directories for statvfs. */
static void make_config(lr_config *cfg, unsigned drive_count)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->block_size       = 65536;
    cfg->placement_policy = LR_PLACE_ROUNDROBIN;
    cfg->parity_threads   = 1;
    for (unsigned i = 0; i < drive_count && i < LR_DRIVE_MAX; i++) {
        snprintf(cfg->drives[i].name, 64,       "d%u", i);
        snprintf(cfg->drives[i].dir,  PATH_MAX, "/tmp/lr_test_drive%u", i);
    }
    cfg->drive_count = drive_count;
    snprintf(cfg->content_paths[0], PATH_MAX, "/tmp/lr_test.content");
    cfg->content_count = 1;
    snprintf(cfg->mountpoint, PATH_MAX, "/tmp/lr_test_mount");
}

static lr_file *make_file(const char *vpath, unsigned drive_idx,
                           uint32_t pos_start, uint32_t block_count)
{
    lr_file *f = calloc(1, sizeof(lr_file));
    snprintf(f->vpath,     sizeof(f->vpath),    "%s", vpath);
    snprintf(f->real_path, sizeof(f->real_path), "/tmp/real%s", vpath);
    f->drive_idx        = drive_idx;
    f->parity_pos_start = pos_start;
    f->block_count      = block_count;
    f->mode             = S_IFREG | 0644;
    return f;
}

/* ------------------------------------------------------------------ */

static void test_init_done(void)
{
    lr_config cfg;
    make_config(&cfg, 2);
    lr_state s;
    ASSERT_INT_EQ(state_init(&s, &cfg), 0);
    ASSERT_INT_EQ(s.drive_count, 2);
    ASSERT_STR_EQ(s.drives[0].name, "d0");
    ASSERT_STR_EQ(s.drives[1].name, "d1");
    /* Drive dirs must end with '/'. */
    size_t len = strlen(s.drives[0].dir);
    ASSERT(len > 0 && s.drives[0].dir[len - 1] == '/');
    state_done(&s);
}

static void test_file_insert_find(void)
{
    lr_config cfg; make_config(&cfg, 1);
    lr_state s;    state_init(&s, &cfg);
    lr_file *f = make_file("/foo.txt", 0, 0, 2);
    state_insert_file(&s, f);
    ASSERT_INT_EQ(s.file_list.count, 1);
    lr_file *found = state_find_file(&s, "/foo.txt");
    ASSERT(found == f);
    state_done(&s);
}

static void test_file_not_found(void)
{
    lr_config cfg; make_config(&cfg, 1);
    lr_state s;    state_init(&s, &cfg);
    ASSERT(state_find_file(&s, "/no_such_file") == NULL);
    state_done(&s);
}

static void test_file_remove(void)
{
    lr_config cfg; make_config(&cfg, 1);
    lr_state s;    state_init(&s, &cfg);
    lr_file *f = make_file("/bar.txt", 0, 0, 1);
    state_insert_file(&s, f);
    lr_file *removed = state_remove_file(&s, "/bar.txt");
    ASSERT(removed == f);
    ASSERT_INT_EQ(s.file_list.count, 0);
    ASSERT(state_find_file(&s, "/bar.txt") == NULL);
    free(removed);
    state_done(&s);
}

static void test_multiple_files(void)
{
    lr_config cfg; make_config(&cfg, 1);
    lr_state s;    state_init(&s, &cfg);
    lr_file *f1 = make_file("/a.txt", 0, 0,  1);
    lr_file *f2 = make_file("/b.txt", 0, 1,  2);
    lr_file *f3 = make_file("/c.txt", 0, 3,  3);
    state_insert_file(&s, f1);
    state_insert_file(&s, f2);
    state_insert_file(&s, f3);
    ASSERT_INT_EQ(s.file_list.count, 3);
    ASSERT(state_find_file(&s, "/a.txt") == f1);
    ASSERT(state_find_file(&s, "/b.txt") == f2);
    ASSERT(state_find_file(&s, "/c.txt") == f3);
    state_done(&s);
}

static void test_dir_insert_find_remove(void)
{
    lr_config cfg; make_config(&cfg, 1);
    lr_state s;    state_init(&s, &cfg);
    lr_dir *d = calloc(1, sizeof(lr_dir));
    snprintf(d->vpath, sizeof(d->vpath), "/movies");
    d->mode = S_IFDIR | 0755;
    state_insert_dir(&s, d);
    lr_dir *found = state_find_dir(&s, "/movies");
    ASSERT(found == d);
    lr_dir *removed = state_remove_dir(&s, "/movies");
    ASSERT(removed == d);
    ASSERT(state_find_dir(&s, "/movies") == NULL);
    free(removed);
    state_done(&s);
}

/* blocks_for_size: ceiling(size/block_size), 0 for size=0. */
static void test_blocks_for_size(void)
{
    ASSERT_INT_EQ(blocks_for_size(      0, 65536), 0);
    ASSERT_INT_EQ(blocks_for_size(      1, 65536), 1);
    ASSERT_INT_EQ(blocks_for_size(  65536, 65536), 1);
    ASSERT_INT_EQ(blocks_for_size(  65537, 65536), 2);
    ASSERT_INT_EQ(blocks_for_size( 131072, 65536), 2);
    ASSERT_INT_EQ(blocks_for_size( 131073, 65536), 3);
}

static void test_pos_index_rebuild_and_search(void)
{
    lr_config cfg; make_config(&cfg, 1);
    lr_state s;    state_init(&s, &cfg);

    /* Three files on drive 0 with disjoint, out-of-order position ranges. */
    lr_file *f1 = make_file("/a", 0, 10, 3); /* [10,13) */
    lr_file *f2 = make_file("/b", 0,  0, 5); /* [0,5)   */
    lr_file *f3 = make_file("/c", 0, 20, 2); /* [20,22) */
    state_insert_file(&s, f1);
    state_insert_file(&s, f2);
    state_insert_file(&s, f3);
    state_rebuild_pos_index(&s, 0);
    ASSERT_INT_EQ(s.pos_index_count[0], 3);

    /* Hits inside each range. */
    ASSERT(state_find_file_at_pos(&s, 0,  0) == f2);
    ASSERT(state_find_file_at_pos(&s, 0,  4) == f2);
    ASSERT(state_find_file_at_pos(&s, 0, 10) == f1);
    ASSERT(state_find_file_at_pos(&s, 0, 12) == f1);
    ASSERT(state_find_file_at_pos(&s, 0, 20) == f3);
    ASSERT(state_find_file_at_pos(&s, 0, 21) == f3);
    /* Gaps between ranges return NULL. */
    ASSERT(state_find_file_at_pos(&s, 0,  5) == NULL);
    ASSERT(state_find_file_at_pos(&s, 0, 13) == NULL);
    ASSERT(state_find_file_at_pos(&s, 0, 99) == NULL);

    state_done(&s);
}

/* Round-robin policy cycles through drives deterministically. */
static void test_pick_drive_roundrobin(void)
{
    lr_config cfg;
    make_config(&cfg, 3);
    cfg.placement_policy = LR_PLACE_ROUNDROBIN;
    lr_state s; state_init(&s, &cfg);
    ASSERT_INT_EQ(state_pick_drive(&s), 0);
    ASSERT_INT_EQ(state_pick_drive(&s), 1);
    ASSERT_INT_EQ(state_pick_drive(&s), 2);
    ASSERT_INT_EQ(state_pick_drive(&s), 0);
    state_done(&s);
}

int main(void)
{
    printf("test_state\n");
    RUN(test_init_done);
    RUN(test_file_insert_find);
    RUN(test_file_not_found);
    RUN(test_file_remove);
    RUN(test_multiple_files);
    RUN(test_dir_insert_find_remove);
    RUN(test_blocks_for_size);
    RUN(test_pos_index_rebuild_and_search);
    RUN(test_pick_drive_roundrobin);
    REPORT();
}
