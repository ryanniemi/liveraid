#include "test_harness.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CONF "/tmp/lr_test_config.conf"

static void write_conf(const char *body)
{
    FILE *f = fopen(CONF, "w");
    fputs(body, f);
    fclose(f);
}

/* ------------------------------------------------------------------ */

static void test_minimal_valid(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), 0);
    ASSERT_INT_EQ(cfg.drive_count, 1);
    ASSERT_STR_EQ(cfg.drives[0].name,    "d0");
    ASSERT_STR_EQ(cfg.drives[0].dir,     "/tmp/d0");
    ASSERT_INT_EQ(cfg.content_count, 1);
    ASSERT_STR_EQ(cfg.content_paths[0],  "/tmp/lr.content");
    ASSERT_STR_EQ(cfg.mountpoint,        "/tmp/lr_mount");
}

/* Default blocksize, placement, parity_threads when not specified. */
static void test_defaults(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), 0);
    ASSERT_INT_EQ(cfg.block_size,       256 * 1024);
    ASSERT_INT_EQ(cfg.placement_policy, LR_PLACE_MOSTFREE);
    ASSERT_INT_EQ(cfg.parity_threads,   1);
    ASSERT_INT_EQ(cfg.parity_levels,    0);
}

/* All four placement policy strings accepted. */
static void test_placement_policies(void)
{
    const char *names[]    = { "roundrobin", "lfs", "pfrd", "mostfree" };
    int         expected[] = { LR_PLACE_ROUNDROBIN, LR_PLACE_LFS,
                                LR_PLACE_PFRD, LR_PLACE_MOSTFREE };
    for (int i = 0; i < 4; i++) {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "data d0 /tmp/d0\ncontent /tmp/lr.content\n"
            "mountpoint /tmp/lr_mount\nplacement %s\n",
            names[i]);
        write_conf(buf);
        lr_config cfg;
        ASSERT_INT_EQ(config_load(CONF, &cfg), 0);
        ASSERT_INT_EQ(cfg.placement_policy, expected[i]);
    }
}

static void test_parity_single_level(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
        "parity 1 /tmp/parity1\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), 0);
    ASSERT_INT_EQ(cfg.parity_levels, 1);
    ASSERT_STR_EQ(cfg.parity_path[0], "/tmp/parity1");
}

static void test_parity_two_levels(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
        "parity 1 /tmp/parity1\n"
        "parity 2 /tmp/parity2\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), 0);
    ASSERT_INT_EQ(cfg.parity_levels, 2);
    ASSERT_STR_EQ(cfg.parity_path[0], "/tmp/parity1");
    ASSERT_STR_EQ(cfg.parity_path[1], "/tmp/parity2");
}

/* parity 1 and parity 3 with no parity 2 → gap error. */
static void test_parity_gap_error(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
        "parity 1 /tmp/parity1\n"
        "parity 3 /tmp/parity3\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), -1);
}

static void test_no_drives_error(void)
{
    write_conf(
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), -1);
}

static void test_no_content_error(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "mountpoint /tmp/lr_mount\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), -1);
}

static void test_no_mountpoint_error(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), -1);
}

static void test_bad_blocksize_zero(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
        "blocksize 0\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), -1);
}

/* UINT32_MAX/1024 = 4194303; one more is over the limit. */
static void test_bad_blocksize_too_large(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
        "blocksize 4194304\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), -1);
}

static void test_blocksize_valid(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
        "blocksize 512\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), 0);
    ASSERT_INT_EQ(cfg.block_size, 512 * 1024);
}

static void test_bad_parity_threads_zero(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
        "parity_threads 0\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), -1);
}

static void test_bad_parity_threads_too_large(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
        "parity_threads 65\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), -1);
}

/* Unknown directives print a warning but do not fail. */
static void test_unknown_directive_nonfatal(void)
{
    write_conf(
        "data d0 /tmp/d0\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
        "futurekeyword somevalue\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), 0);
    ASSERT_INT_EQ(cfg.drive_count, 1);
}

/* Comments and blank lines are stripped before parsing. */
static void test_comments_and_blank_lines(void)
{
    write_conf(
        "# this is a comment\n"
        "\n"
        "  data d0 /tmp/d0  # inline comment\n"
        "\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), 0);
    ASSERT_INT_EQ(cfg.drive_count, 1);
    ASSERT_STR_EQ(cfg.drives[0].name, "d0");
}

static void test_multiple_drives(void)
{
    write_conf(
        "data alpha /mnt/disk1\n"
        "data beta  /mnt/disk2\n"
        "data gamma /mnt/disk3\n"
        "content /tmp/lr.content\n"
        "mountpoint /tmp/lr_mount\n"
    );
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), 0);
    ASSERT_INT_EQ(cfg.drive_count, 3);
    ASSERT_STR_EQ(cfg.drives[0].name, "alpha");
    ASSERT_STR_EQ(cfg.drives[1].name, "beta");
    ASSERT_STR_EQ(cfg.drives[2].name, "gamma");
}

static void test_file_not_found(void)
{
    unlink(CONF);
    lr_config cfg;
    ASSERT_INT_EQ(config_load(CONF, &cfg), -1);
}

int main(void)
{
    printf("test_config\n");
    RUN(test_minimal_valid);
    RUN(test_defaults);
    RUN(test_placement_policies);
    RUN(test_parity_single_level);
    RUN(test_parity_two_levels);
    RUN(test_parity_gap_error);
    RUN(test_no_drives_error);
    RUN(test_no_content_error);
    RUN(test_no_mountpoint_error);
    RUN(test_bad_blocksize_zero);
    RUN(test_bad_blocksize_too_large);
    RUN(test_blocksize_valid);
    RUN(test_bad_parity_threads_zero);
    RUN(test_bad_parity_threads_too_large);
    RUN(test_unknown_directive_nonfatal);
    RUN(test_comments_and_blank_lines);
    RUN(test_multiple_drives);
    RUN(test_file_not_found);
    REPORT();
}
