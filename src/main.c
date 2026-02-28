#define FUSE_USE_VERSION 35
#include <fuse.h>

#include "config.h"
#include "state.h"
#include "metadata.h"
#include "fuse_ops.h"
#include "parity.h"
#include "journal.h"
#include "rebuild.h"
#include "ctrl.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>

/* SIGUSR1 handler: request a scrub pass (verify parity, report mismatches) */
static void sigusr1_handler(int sig)
{
    (void)sig;
    if (g_state && g_state->journal)
        g_state->journal->scrub_pending = 1;
}

/* SIGUSR2 handler: request a repair pass (verify and fix mismatched parity) */
static void sigusr2_handler(int sig)
{
    (void)sig;
    if (g_state && g_state->journal)
        g_state->journal->repair_pending = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "liveraid %s\n"
        "\n"
        "Usage: %s -c CONFIG [FUSE_OPTIONS] MOUNTPOINT\n"
        "       %s rebuild -c CONFIG -d DRIVE_NAME\n"
        "\n"
        "Options:\n"
        "  -c CONFIG    Path to liveraid.conf\n"
        "  -d           Enable FUSE debug output\n"
        "  -f           Run in foreground\n"
        "  -V           Print version and exit\n"
        "\n"
        "Signals (send to mounted process):\n"
        "  SIGUSR1      Verify parity — report mismatches, do not fix\n"
        "  SIGUSR2      Repair parity — rewrite any mismatched parity blocks\n"
        "               (also use after adding a new parity level)\n"
        "\n"
        "Example:\n"
        "  %s -c /etc/liveraid.conf /mnt/array\n",
        lr_version, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    /* Dispatch subcommands before FUSE argument processing */
    if (argc >= 2 && strcmp(argv[1], "rebuild") == 0)
        return cmd_rebuild(argc - 1, argv + 1);

    char *config_path = NULL;
    int   opt;
    int   show_help = 0;

    /* Extract -c CONFIG before passing remaining args to fuse_main */
    /* We'll rebuild argv without -c CONFIG for fuse */
    char **fuse_argv = malloc((argc + 2) * sizeof(char *));
    if (!fuse_argv) {
        fprintf(stderr, "liveraid: out of memory\n");
        return 1;
    }
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strncmp(argv[i], "-c", 2) == 0 && strlen(argv[i]) > 2) {
            config_path = argv[i] + 2;
        } else if (strcmp(argv[i], "-V") == 0 ||
                   strcmp(argv[i], "--version") == 0) {
            printf("liveraid %s\n", lr_version);
            free(fuse_argv);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            show_help = 1;
            fuse_argv[fuse_argc++] = argv[i];
        } else {
            fuse_argv[fuse_argc++] = argv[i];
        }
    }
    fuse_argv[fuse_argc] = NULL;
    (void)opt; (void)show_help;

    if (!config_path) {
        usage(argv[0]);
        free(fuse_argv);
        return 1;
    }

    /* ---- Load config ---- */
    lr_config *cfg = calloc(1, sizeof(lr_config));
    if (!cfg) {
        fprintf(stderr, "liveraid: out of memory\n");
        free(fuse_argv);
        return 1;
    }

    if (config_load(config_path, cfg) != 0) {
        fprintf(stderr, "liveraid: failed to load config '%s'\n", config_path);
        free(cfg);
        free(fuse_argv);
        return 1;
    }

    /* ---- Initialize state ---- */
    lr_state *state = calloc(1, sizeof(lr_state));
    if (!state) {
        fprintf(stderr, "liveraid: out of memory\n");
        free(cfg);
        free(fuse_argv);
        return 1;
    }

    if (state_init(state, cfg) != 0) {
        fprintf(stderr, "liveraid: state_init failed\n");
        free(state);
        free(cfg);
        free(fuse_argv);
        return 1;
    }
    free(cfg); /* copied into state->cfg */

    /* ---- Load metadata ---- */
    if (metadata_load(state) != 0) {
        fprintf(stderr, "liveraid: warning: metadata_load failed (fresh start?)\n");
    }

    /* ---- Open parity files (if configured) ---- */
    if (state->cfg.parity_levels > 0) {
        lr_parity_handle *ph = calloc(1, sizeof(lr_parity_handle));
        if (ph && parity_open(ph, &state->cfg) == 0) {
            state->parity = ph;
        } else {
            fprintf(stderr, "liveraid: warning: could not open parity files, "
                            "running without parity\n");
            free(ph);
        }
    }

    /* ---- Start journal ---- */
    {
        lr_journal *j = calloc(1, sizeof(lr_journal));
        if (j && journal_init(j, state, 5000) == 0) {
            state->journal = j;

            /* Set persistent bitmap path from first content path */
            if (state->cfg.content_count > 0) {
                char bm_path[PATH_MAX + 8]; /* +8 for ".bitmap\0" */
                snprintf(bm_path, sizeof(bm_path), "%s.bitmap",
                         state->cfg.content_paths[0]);
                journal_set_bitmap_path(j, bm_path);
            }
        } else {
            fprintf(stderr, "liveraid: warning: journal_init failed\n");
            free(j);
        }
    }

    /* ---- Start control server (live rebuild socket) ---- */
    if (state->cfg.content_count > 0) {
        lr_ctrl *ctrl = calloc(1, sizeof(lr_ctrl));
        if (ctrl && ctrl_start(ctrl, state) == 0) {
            state->ctrl = ctrl;
        } else {
            fprintf(stderr, "liveraid: warning: ctrl_start failed, "
                            "live rebuild unavailable\n");
            free(ctrl);
        }
    }

    fprintf(stderr, "liveraid %s starting\n", lr_version);

    /* ---- Set global state and install signal handlers ---- */
    g_state = state;

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigusr1_handler;
        sigaction(SIGUSR1, &sa, NULL);
        sa.sa_handler = sigusr2_handler;
        sigaction(SIGUSR2, &sa, NULL);
    }

    /* ---- Run FUSE ---- */
    int ret = fuse_main(fuse_argc, fuse_argv, &lr_fuse_ops, NULL);

    /* ---- Cleanup (also called via destroy callback, but be safe) ---- */
    if (state->ctrl) {
        ctrl_stop(state->ctrl);
        free(state->ctrl);
        state->ctrl = NULL;
    }
    if (state->journal) {
        journal_flush(state->journal);
        journal_done(state->journal);
        free(state->journal);
        state->journal = NULL;
    }
    if (state->parity) {
        parity_close(state->parity);
        free(state->parity);
        state->parity = NULL;
    }
    metadata_save(state);
    state_done(state);
    free(state);
    free(fuse_argv);

    return ret;
}
