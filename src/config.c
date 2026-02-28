#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define DEFAULT_BLOCK_SIZE (256 * 1024)

/* Strip leading/trailing whitespace in-place, return pointer to start. */
static char *trim(char *s)
{
    char *end;
    while (isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

int config_load(const char *path, lr_config *cfg)
{
    FILE *f;
    char line[4096];
    int lineno = 0;

    memset(cfg, 0, sizeof(*cfg));
    cfg->block_size       = DEFAULT_BLOCK_SIZE;
    cfg->placement_policy = LR_PLACE_MOSTFREE;
    cfg->parity_threads   = 1;

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *p, *key, *rest;
        lineno++;

        /* Strip comments */
        p = strchr(line, '#');
        if (p)
            *p = '\0';

        p = trim(line);
        if (*p == '\0')
            continue;

        /* Split on first whitespace */
        key = p;
        rest = key;
        while (*rest && !isspace((unsigned char)*rest))
            rest++;
        if (*rest) {
            *rest++ = '\0';
            rest = trim(rest);
        }

        if (strcmp(key, "data") == 0) {
            /* data NAME DIR */
            char *name = rest;
            char *dir  = rest;
            while (*dir && !isspace((unsigned char)*dir))
                dir++;
            if (*dir) {
                *dir++ = '\0';
                dir = trim(dir);
            }
            if (!*name || !*dir) {
                fprintf(stderr, "config:%d: bad 'data' line\n", lineno);
                fclose(f);
                return -1;
            }
            if (cfg->drive_count >= LR_DRIVE_MAX) {
                fprintf(stderr, "config:%d: too many drives\n", lineno);
                fclose(f);
                return -1;
            }
            lr_drive_conf *dc = &cfg->drives[cfg->drive_count++];
            snprintf(dc->name, sizeof(dc->name), "%s", name);
            snprintf(dc->dir,  sizeof(dc->dir),  "%s", dir);

        } else if (strcmp(key, "parity") == 0) {
            /* parity LEVEL PATH  e.g.:  parity 1 /mnt/p1/liveraid.parity */
            char *endp;
            long level = strtol(rest, &endp, 10);
            char *path = trim(endp);
            if (level < 1 || level > LR_LEV_MAX || !*path) {
                fprintf(stderr, "config:%d: bad 'parity' line — expected: parity LEVEL(1-%d) PATH\n",
                        lineno, LR_LEV_MAX);
                fclose(f);
                return -1;
            }
            snprintf(cfg->parity_path[level - 1], PATH_MAX, "%s", path);

        } else if (strcmp(key, "content") == 0) {
            if (cfg->content_count >= 8) {
                fprintf(stderr, "config:%d: too many content paths\n", lineno);
                fclose(f);
                return -1;
            }
            snprintf(cfg->content_paths[cfg->content_count++],
                     PATH_MAX, "%s", rest);

        } else if (strcmp(key, "mountpoint") == 0) {
            snprintf(cfg->mountpoint, PATH_MAX, "%s", rest);

        } else if (strcmp(key, "blocksize") == 0) {
            long val = strtol(rest, NULL, 10);
            if (val <= 0 || val > (long)(UINT32_MAX / 1024) ||
                (val * 1024) % 64 != 0) {
                fprintf(stderr, "config:%d: bad blocksize (must be multiple of 64 bytes when in KiB)\n", lineno);
                fclose(f);
                return -1;
            }
            cfg->block_size = (uint32_t)(val * 1024);

        } else if (strcmp(key, "placement") == 0) {
            if (strcmp(rest, "mostfree") == 0)
                cfg->placement_policy = LR_PLACE_MOSTFREE;
            else if (strcmp(rest, "roundrobin") == 0)
                cfg->placement_policy = LR_PLACE_ROUNDROBIN;
            else if (strcmp(rest, "lfs") == 0)
                cfg->placement_policy = LR_PLACE_LFS;
            else if (strcmp(rest, "pfrd") == 0)
                cfg->placement_policy = LR_PLACE_PFRD;
            else {
                fprintf(stderr, "config:%d: unknown placement policy '%s'\n",
                        lineno, rest);
                fclose(f);
                return -1;
            }
        } else if (strcmp(key, "parity_threads") == 0) {
            long val = strtol(rest, NULL, 10);
            if (val < 1 || val > 64) {
                fprintf(stderr, "config:%d: parity_threads must be between 1 and 64\n", lineno);
                fclose(f);
                return -1;
            }
            cfg->parity_threads = (unsigned)val;

        } else {
            fprintf(stderr, "config:%d: unknown directive '%s'\n", lineno, key);
            /* non-fatal: ignore */
        }
    }

    fclose(f);

    /* Compute parity_levels from assigned slots; error on gaps */
    {
        int highest = -1, i;
        for (i = 0; i < LR_LEV_MAX; i++)
            if (cfg->parity_path[i][0] != '\0')
                highest = i;
        for (i = 0; i <= highest; i++) {
            if (cfg->parity_path[i][0] == '\0') {
                fprintf(stderr, "config: parity levels have a gap — "
                        "parity %d is missing\n", i + 1);
                return -1;
            }
        }
        cfg->parity_levels = (unsigned)(highest + 1);
    }

    /* Validate */
    if (cfg->drive_count == 0) {
        fprintf(stderr, "config: no data drives defined\n");
        return -1;
    }
    if (cfg->content_count == 0) {
        fprintf(stderr, "config: no content file defined\n");
        return -1;
    }
    if (cfg->mountpoint[0] == '\0') {
        fprintf(stderr, "config: no mountpoint defined\n");
        return -1;
    }

    return 0;
}

void config_dump(const lr_config *cfg)
{
    unsigned i;
    fprintf(stderr, "=== liveraid config ===\n");
    fprintf(stderr, "  block_size: %u bytes\n", cfg->block_size);
    fprintf(stderr, "  mountpoint: %s\n", cfg->mountpoint);
    for (i = 0; i < cfg->drive_count; i++)
        fprintf(stderr, "  drive[%u]: name=%s dir=%s\n",
                i, cfg->drives[i].name, cfg->drives[i].dir);
    for (i = 0; i < cfg->parity_levels; i++)
        fprintf(stderr, "  parity[%u]: %s\n", i, cfg->parity_path[i]);
    for (i = 0; i < cfg->content_count; i++)
        fprintf(stderr, "  content[%u]: %s\n", i, cfg->content_paths[i]);
    const char *placement = "mostfree";
    if (cfg->placement_policy == LR_PLACE_ROUNDROBIN) placement = "roundrobin";
    else if (cfg->placement_policy == LR_PLACE_LFS)   placement = "lfs";
    else if (cfg->placement_policy == LR_PLACE_PFRD)  placement = "pfrd";
    fprintf(stderr, "  placement: %s\n", placement);
}
