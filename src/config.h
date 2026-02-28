#ifndef LR_CONFIG_H
#define LR_CONFIG_H

#include <stdint.h>
#include <limits.h>

/* ISA-L gf_gen_cauchy1_matrix uses row indices 0..(nd+np-1) as GF(2^8) elements.
 * All indices must be distinct bytes, so nd + np <= 256.
 * With LR_LEV_MAX parity levels reserved, the max data drives is 256 - LR_LEV_MAX. */
#define LR_LEV_MAX    6
#define LR_DRIVE_MAX  (256 - LR_LEV_MAX)  /* = 250 */

#define LR_PLACE_MOSTFREE   0
#define LR_PLACE_ROUNDROBIN 1
#define LR_PLACE_LFS        2   /* least free space: fill fullest drive first */
#define LR_PLACE_PFRD       3   /* probabilistic: weighted random by free space */

typedef struct {
    char name[64];
    char dir[PATH_MAX];
} lr_drive_conf;

typedef struct {
    lr_drive_conf  drives[LR_DRIVE_MAX];
    unsigned       drive_count;

    char           parity_path[LR_LEV_MAX][PATH_MAX];
    unsigned       parity_levels;

    char           content_paths[8][PATH_MAX];
    unsigned       content_count;

    char           mountpoint[PATH_MAX];

    uint32_t       block_size;      /* bytes, multiple of 64 */
    int            placement_policy;
    unsigned       parity_threads;  /* parallel threads for parity drain (default 1) */
} lr_config;

/* Parse config file at path into cfg.  Returns 0 on success, -1 on error. */
int config_load(const char *path, lr_config *cfg);

/* Print config to stderr for debugging. */
void config_dump(const lr_config *cfg);

#endif /* LR_CONFIG_H */
